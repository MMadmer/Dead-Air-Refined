// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_senses.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"

#include "Actor.h"
#include "actor_defs.h"
#include "ai/stalker/ai_stalker.h"
#include "danger_object.h"
#include "enemy_manager.h"
#include "Hit.h"
#include "memory_manager.h"
#include "memory_space.h"
#include "stalker_movement_manager_smart_cover.h"
#include "visual_memory_manager.h"
#include "xrEngine/Environment.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "GamePersistent.h"

#include <atomic>
#include <mutex>

namespace DaAi::Sense
{
namespace
{
Config g_config;
collide::rq_results g_ray_storage;

// Near misses arrive from the bullet manager, which may run off the main thread; drained by the main thread only.
struct NearMiss
{
    Fvector point;
    Fvector direction;
    u32 time;
    u16 shooter;
    u16 victim;
};
std::mutex g_near_miss_lock;
xr_vector<NearMiss> g_near_misses;
xr_vector<NearMiss> g_near_miss_batch;
// The queue cap the bullet thread reads; written by the main thread when the config loads.
std::atomic<u32> g_near_miss_cap{256};
u32 g_drained_frame{u32(-1)};

// The last rendered brightness of each body and the light of the environment when it last changed.
struct Lit
{
    float value{-1.f};
    float environment{};
};
xr_flat_hash_map<u16, Lit> g_lit;

float environment_light()
{
    const CEnvDescriptorMixer& env = GamePersistent().Environment().CurrentEnv;
    return _max(env.ambient.x + env.hemi_color.x + env.sun_color.x,
        _max(env.ambient.y + env.hemi_color.y + env.sun_color.y, env.ambient.z + env.hemi_color.z + env.sun_color.z));
}

void read(pcstr section, pcstr key, float& value) { value = READ_IF_EXISTS(pSettings, r_float, section, key, value); }
void read(pcstr section, pcstr key, u32& value) { value = READ_IF_EXISTS(pSettings, r_u32, section, key, value); }

void read(pcstr section, pcstr key, float& first, float& second)
{
    if (!pSettings->line_exist(section, key))
        return;
    float a = first, b = second;
    if (sscanf_s(pSettings->r_string(section, key), "%f , %f", &a, &b) == 2)
    {
        first = a;
        second = b;
    }
}

void read(pcstr section, pcstr key, Linear& value) { read(section, key, value.e0, value.k); }

void read(pcstr section, pcstr key, float* values, size_t count)
{
    if (!pSettings->line_exist(section, key))
        return;
    pcstr text = pSettings->r_string(section, key);
    if (_GetItemCount(text) != int(count))
        return;
    string64 item;
    for (size_t i = 0; i < count; ++i)
        values[i] = float(atof(_GetItem(text, int(i), item)));
}

void load_config()
{
    Config& c = g_config;
    c = {};

    constexpr pcstr awareness = "ai_awareness";
    read(awareness, "identify", c.identify);
    read(awareness, "suspicious", c.suspicious);
    read(awareness, "crouch_mult", c.crouch_mult);
    read(awareness, "rate_danger", c.rate_danger);
    read(awareness, "rate_free", c.rate_free);
    read(awareness, "band_rate", c.band_rate, std::size(c.band_rate));
    read(awareness, "peripheral_mult", c.peripheral_mult);
    read(awareness, "close", c.close_m, c.close_gain);
    read(awareness, "decay", c.decay_danger, c.decay_free);
    read(awareness, "reacquire", c.reacquire_floor, c.reacquire_s);
    read(awareness, "suspect_err", c.suspect_err_far, c.suspect_err_near);
    read(awareness, "suspect_period_ms", c.suspect_period_ms);
    read(awareness, "opaque_below", c.opaque_below);
    read(awareness, "fog_sight", c.fog_sight);
    read(awareness, "daylight_lum", c.daylight_lum);
    read(awareness, "intensity_cache", c.intensity_cache_ms, c.intensity_cache_m);
    read(awareness, "touch_m", c.touch_m);
    read(awareness, "forget_ms", c.forget_ms);
    c.identify = _max(c.identify, EPS_L);
    c.suspicious = _min(c.suspicious, c.identify);

    constexpr pcstr noise = "ai_noise";
    read(noise, "occlusion_floor", c.occlusion_floor);
    read(noise, "occlusion_hits", c.occlusion_hits);
    read(noise, "occlusion_cache_ms", c.occlusion_cache_ms);
    read(noise, "occlusion_cache_far_ms", c.occlusion_cache_far_ms);
    read(noise, "occlusion_move_m", c.occlusion_move_m);
    read(noise, "occlusion_move_k", c.occlusion_move_k);
    float repeat_ms = float(c.heard_repeat_ms);
    read(noise, "heard_repeat", repeat_ms, c.heard_repeat_m);
    c.heard_repeat_ms = u32(_max(repeat_ms, 0.f));
    repeat_ms = float(c.heard_repeat_far_ms);
    read(noise, "heard_repeat_far", repeat_ms, c.heard_repeat_far_m);
    c.heard_repeat_far_ms = u32(_max(repeat_ms, 0.f));
    read(noise, "pre_rays", c.pre_rays);
    read(noise, "pre_budget_us", c.pre_budget_us);
    read(noise, "rain_mask", c.rain_mask);
    read(noise, "detached_m", c.detached_m);
    read(noise, "gait", c.gait, std::size(c.gait));
    read(noise, "shuffle", c.shuffle);
    read(noise, "landing", c.landing);
    read(noise, "step_range", c.step_range);
    read(noise, "noise_range", c.noise_range);
    read(noise, "cull_mult", c.cull_mult);
    read(noise, "edge_power", c.edge_power);
    read(noise, "load", c.load_free_kg, c.load_step_mult);
    read(noise, "reload", c.reload);
    read(noise, "torch_click", c.torch_click);
    read(noise, "inbox_max", c.inbox_max);
    read(noise, "stimulus_ttl_ms", c.stimulus_ttl_ms);
    read(noise, "stimuli_per_update", c.stimuli_per_update);
    read(noise, "near_miss_suppression", c.near_miss_suppression);
    read(noise, "near_miss_radius", c.near_miss_radius, c.near_miss_min);
    read(noise, "suppression_decay", c.suppression_decay);
    read(noise, "near_miss_estimate_ms", c.near_miss_estimate_ms);
    read(noise, "near_miss_hit_ms", c.near_miss_hit_ms);
    read(noise, "near_miss_queue_max", c.near_miss_queue_max);
    c.inbox_max = _max(c.inbox_max, 1u);
    c.pre_rays = _max(c.pre_rays, 1u);
    c.cull_mult = _max(c.cull_mult, 1.f);
    g_near_miss_cap = c.near_miss_queue_max;

    constexpr pcstr light = "ai_light";
    read(light, "flash_period_ms", c.flash_period_ms);
    read(light, "ambient", c.ambient_day, c.ambient_night);
    read(light, "night_hours", c.night_hours, 4);
    read(light, "muzzle_range", c.muzzle_range_day, c.muzzle_range_night);
    read(light, "muzzle_silenced", c.muzzle_silenced);
    read(light, "flash_rate", c.flash_rate);
    read(light, "flash_cue_ms", c.flash_cue_ms);
    read(light, "flash_day_mult", c.flash_day_mult);
    read(light, "torch_range", c.torch_range_day, c.torch_range_night);
    read(light, "torch_strength", c.torch_strength_min, c.torch_strength_max);
    read(light, "torch_rate", c.torch_rate);
    read(light, "torch_notice", c.torch_notice);
    read(light, "torch_period_ms", c.torch_period_ms);
    read(light, "torch_scan_rays", c.torch_scan_rays);
    read(light, "torch_scan_checks", c.torch_scan_checks);
    read(light, "spot_darkness", c.spot_darkness);
    read(light, "spot_range", c.spot_range);
    read(light, "spot_err_k", c.spot_err_k);

    constexpr pcstr errors = "ai_errors";
    read(errors, "heard_shot", c.heard_shot);
    read(errors, "heard_step", c.heard_step);
    read(errors, "heard_other", c.heard_other);
    read(errors, "heard_silenced_mult", c.heard_silenced_mult);
    read(errors, "heard_occlusion_mult", c.heard_occlusion_mult);
    read(errors, "heard_skill_base", c.heard_skill_base);
    read(errors, "hit_cone_deg", c.hit_cone_deg);
    read(errors, "hit_distance_err", c.hit_distance_err);
    read(errors, "hit_distance_prior", c.hit_distance_prior_min, c.hit_distance_prior_max);
    read(errors, "near_miss_cone_deg", c.near_miss_cone_deg);
    read(errors, "near_miss_distance_prior", c.near_miss_distance_prior_min, c.near_miss_distance_prior_max);
    read(errors, "muzzle_flash", c.muzzle_flash);
    read(errors, "flashlight", c.flashlight);

    c.loaded = true;
}

SenseState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && !state->dead ? state->sense : nullptr;
}

int community_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state ? state->community : -1;
}

// An estimate already placed where the NPC believes it (no further random offset), on the level graph.
Estimate estimate_at(const CAI_Stalker& npc, const CGameObject* target, const Fvector& position, float err,
    Source source, float confidence)
{
    Estimate result{};
    result.pos = position;
    result.vertex = Know::resolve_vertex(result.pos, &npc.Position());
    result.err = err;
    result.t_observed = Device.dwTimeGlobal;
    result.target = target ? target->ID() : u16(-1);
    result.reporter = npc.ID();
    result.source = source;
    result.confidence = clampr(confidence, 0.f, 1.f);
    return result;
}

// 0 on the view axis .. 1 at the edge of the field of view.
float angle_ratio(const CAI_Stalker& npc, const CGameObject& object)
{
    Fvector to;
    object.Center(to);
    to.sub(npc.eye_matrix.c);
    to.normalize_safe();
    const float half_fov = deg2rad(npc.eye_fov) * .5f;
    if (half_fov <= EPS)
        return 0.f;
    return clampr(acosf(clampr(view_direction(npc).dotproduct(to), -.99999f, .99999f)) / half_fov, 0.f, 1.f);
}

bool crouched(const CGameObject& object)
{
    if (const CActor* actor = smart_cast<const CActor*>(&object))
        return !!(actor->MovingState() & mcCrouch);
    if (const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(&object))
        return stalker->movement().body_state() == MonsterSpace::eBodyStateCrouch;
    return false;
}

float mirror(const SenseState::Target& target, float threshold)
{
    return target.identified ? threshold : _min(target.awareness / config().identify, .999f) * threshold;
}

void decay(SenseState::Target& target, bool danger, float dt)
{
    const Config& c = config();
    float floor = 0.f;
    // An enemy identified moments ago is recognised again quickly when he reappears.
    if (target.t_identified && Device.dwTimeGlobal < target.t_identified + u32(c.reacquire_s * 1000.f))
        floor = _min(c.reacquire_floor * c.identify, target.awareness);
    target.awareness = _max(target.awareness - (danger ? c.decay_danger : c.decay_free) * dt, floor);
    if (target.awareness <= 0.f)
        target.awareness = 0.f;
}

// sighted: the rays reach the object inside the view distance now; only then may a suspicion describe where it is.
void transitions(CAI_Stalker& npc, const CGameObject& object, SenseState::Target& target, bool sighted)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const int community = community_of(npc);
    const auto enemy = [&npc, &object] {
        const CEntityAlive* entity = smart_cast<const CEntityAlive*>(&object);
        return entity && entity->g_Alive() && npc.is_relation_enemy(entity);
    };

    if (target.awareness >= c.identify)
    {
        if (!target.counted)
        {
            target.counted = true;
            if (enemy())
            {
                stat_add(Stat::s_identified, 1.f, community);
                stat_add(Stat::s_ident_ms, float(now - _min(target.t_first, now)), community);
            }
        }
        target.identified = true;
        target.suspicious = false;
        if (sighted)
            target.t_identified = now;
        return;
    }

    target.identified = false;
    if (target.awareness < c.suspicious)
    {
        target.suspicious = false;
        return;
    }
    if (!sighted)
        return;

    if (!target.suspicious)
    {
        if (!enemy())
            return;
        target.suspicious = true;
        stat_add(Stat::s_suspicious, 1.f, community);
    }
    if (target.t_suspect && now < target.t_suspect + c.suspect_period_ms)
        return;

    // A glimpse: a danger at a rough spot, sharper the longer the NPC has been looking.
    target.t_suspect = now;
    const float ratio = target.awareness / c.identify;
    const float err = c.suspect_err_far + (c.suspect_err_near - c.suspect_err_far) * ratio;
    const Estimate estimate = make_estimate(npc, &object, object.Position(), err, Source::seen, ratio);
    Know::suspect(npc, estimate, CDangerObject::eDangerTypeEnemySound, CDangerObject::eDangerPerceiveTypeVisual);
}

struct LosParam
{
    float vis;
    float threshold;
};

bool los_callback(collide::rq_result& result, LPVOID params)
{
    LosParam& param = *static_cast<LosParam*>(params);
    float transparency = 0.f;
    if (!result.O)
    {
        const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + result.element;
        transparency = GMLib.GetMaterialByIdx(tri->material)->fVisTransparencyFactor;
    }
    param.vis *= transparency;
    return param.vis > param.threshold;
}

// The transparency product along the static ray, stopped once the line counts as blocked.
float trace_vis(const Fvector& from, const Fvector& dir, float range, const IGameObject* ignore)
{
    LosParam param{1.f, config().opaque_below};
    const collide::ray_defs ray(from, dir, range, ray_flags(), collide::rqtStatic);
    Level().ObjectSpace.RayQuery(g_ray_storage, ray, los_callback, &param, nullptr, const_cast<IGameObject*>(ignore));
    return param.vis;
}

float current_suppression(const SenseState& state)
{
    if (state.suppression <= 0.f)
        return 0.f;
    const float elapsed = float(Device.dwTimeGlobal - state.suppression_time) / 1000.f;
    return _max(state.suppression - config().suppression_decay * elapsed, 0.f);
}

// Suppression is physical: every bullet counts at once, whatever the NPC's update schedule; only the estimate of the
// shooter waits for the NPC's own update. Runs on the main thread, once per frame.
void drain_near_misses()
{
    if (g_drained_frame == Device.dwFrame)
        return;
    g_drained_frame = Device.dwFrame;
    {
        std::lock_guard lock(g_near_miss_lock);
        if (g_near_misses.empty())
            return;
        g_near_miss_batch.swap(g_near_misses);
    }
    if (!feature(Feature::near_miss))
    {
        g_near_miss_batch.clear();
        return;
    }

    const Config& c = config();
    for (const NearMiss& miss : g_near_miss_batch)
    {
        NpcState* state = npc(miss.victim);
        if (!state || state->dead || !state->sense || !state->owner)
            continue;

        CAI_Stalker& victim = *state->owner;
        // A mate's line of fire is expected; fire from anyone else frightens.
        const CEntityAlive* shooter = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(miss.shooter));
        if (!shooter || victim.tfGetRelationType(shooter) == ALife::eRelationTypeFriend)
            continue;

        // Closer to the head is more frightening: the distance of the bullet line from the eyes.
        Fvector to_eye;
        to_eye.sub(victim.eye_matrix.c, miss.point);
        Fvector closest;
        closest.mad(miss.point, miss.direction, to_eye.dotproduct(miss.direction));
        const float passed = victim.eye_matrix.c.distance_to(closest);
        const float closeness = c.near_miss_radius > 0.f ?
            clampr(1.f - passed / c.near_miss_radius, c.near_miss_min, 1.f) : 1.f;

        SenseState& sense = *state->sense;
        sense.suppression = _min(current_suppression(sense) + c.near_miss_suppression * closeness, 1.f);
        sense.suppression_time = Device.dwTimeGlobal;
        stat_add(Stat::s_near_miss, 1.f, state->community);

        SenseState::Stimulus stimulus;
        stimulus.position = miss.point;
        stimulus.direction = miss.direction;
        stimulus.power = closeness;
        stimulus.time = miss.time;
        stimulus.who = miss.shooter;
        stimulus.kind = Noise::shot;
        stimulus.cue = SenseState::Cue::near_miss;
        queue(victim, stimulus);
    }
    g_near_miss_batch.clear();
}

bool process(CAI_Stalker& npc, SenseState& state, SenseState::Stimulus& stimulus, u32 rays)
{
    switch (stimulus.cue)
    {
    case SenseState::Cue::noise: return process_noise(npc, state, stimulus, rays > 0);
    case SenseState::Cue::flash: return process_flash(npc, state, stimulus, rays);
    case SenseState::Cue::near_miss: return process_near_miss(npc, state, stimulus);
    }
    return true;
}
} // namespace

Timer::Timer(int community, bool outside_hooks)
    : m_timer(Stat::p_sense_us, community), m_community(community), m_outside_hooks(outside_hooks)
{
}

Timer::~Timer()
{
    if (m_outside_hooks)
        stat_add(Stat::p_total_us, m_timer.elapsed_us(), m_community);
}

const Config& config()
{
    if (!g_config.loaded)
        load_config();
    return g_config;
}

Fvector view_direction(const CAI_Stalker& npc)
{
    const MonsterSpace::SBoneRotation& head = npc.movement().m_head;
    Fvector view;
    view.setHP(-head.current.yaw, -head.current.pitch);
    return view;
}

bool in_view(const CAI_Stalker& npc, const Fvector& point)
{
    Fvector to;
    to.sub(point, npc.eye_matrix.c);
    const float distance = to.magnitude();
    if (distance < EPS_L)
        return true;
    to.div(distance);
    return view_direction(npc).dotproduct(to) >= _cos(deg2rad(npc.eye_fov) * .5f);
}

float darkness()
{
    const Config& c = config();
    const float brightness = environment_light();
    const float weather = c.ambient_day <= c.ambient_night ?
        (brightness < c.ambient_day ? 1.f : 0.f) :
        clampr((c.ambient_day - brightness) / (c.ambient_day - c.ambient_night), 0.f, 1.f);

    // By the clock as well: Dead Air's nights keep a bright blue sky colour (as bright as an overcast day), so the
    // weather alone left a night half dark.
    const float hour = GamePersistent().Environment().GetGameTime() / 3600.f;
    const auto [dusk, dark, dawn, light] = c.night_hours;
    float clock = 0.f;
    if (hour >= dark || hour <= dawn)
        clock = 1.f;
    else if (hour > dusk && hour < dark)
        clock = (hour - dusk) / _max(dark - dusk, EPS_L);
    else if (hour > dawn && hour < light)
        clock = (light - hour) / _max(light - dawn, EPS_L);
    return _max(weather, clock);
}

SenseState* create(CAI_Stalker& npc)
{
    config();
    return xr_new<SenseState>();
}

void destroy(SenseState* state) { xr_delete(state); }

void pre_memory(CAI_Stalker& npc, float dt)
{
    SenseState* state = state_of(npc);
    if (!state)
        return;

    Timer timer(community_of(npc), false);
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    drain_near_misses();

    if (now >= state->cleanup_time)
    {
        state->cleanup_time = now + c.forget_ms / 2;
        for (auto it = state->targets.begin(); it != state->targets.end();)
        {
            if (now > it->second.t_update + c.forget_ms)
                it = state->targets.erase(it);
            else
                ++it;
        }
    }

    // One update casts at most pre_rays new rays for its stimuli and its look for lamps (K11: a night step with eight
    // stimuli and a look took 1.2 ms); a look that falls due keeps one of them. After the first stimulus the step goes
    // on only within pre_budget_us (r2n: one step took 0.41 ms, the only update of the run over 0.4); the rest waits.
    const auto started = std::chrono::steady_clock::now();
    const auto within_budget = [&c, started] {
        return std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - started).count() <
            c.pre_budget_us;
    };
    const bool look = feature(Feature::light) && now >= state->torch_time;
    const u32 rays_start = RayBudget::granted();
    const auto rays_left = [&c, rays_start](u32 reserve) {
        const u32 used = RayBudget::granted() - rays_start;
        return used + reserve < c.pre_rays ? c.pre_rays - used - reserve : 0u;
    };

    // Stimuli of the noise bus, muzzle flashes and near misses; one waiting for the ray budget stays until it expires.
    // At most stimuli_per_update are taken per update, oldest first: the rest wait for the next one.
    if (!state->inbox.empty())
    {
        xr_vector<SenseState::Stimulus>& inbox = state->inbox;
        size_t kept = 0;
        u32 taken = 0;
        for (size_t i = 0; i < inbox.size(); ++i)
        {
            SenseState::Stimulus stimulus = inbox[i];
            if (now > stimulus.time + c.stimulus_ttl_ms)
            {
                if (stimulus.cue == SenseState::Cue::flash)
                    stat_add(Stat::s_flash_x_wait, 1.f, community_of(npc));
                continue;
            }
            bool done = false;
            if (taken < c.stimuli_per_update && (!taken || within_budget()))
            {
                ++taken;
                done = process(npc, *state, stimulus, rays_left(look && c.pre_rays > 1 ? 1 : 0));
            }
            if (!done)
                inbox[kept++] = stimulus;
        }
        inbox.resize(kept);
    }

    // A look without a ray or time left waits for the next update.
    if (look && within_budget())
    {
        if (const u32 rays = rays_left(0))
        {
            state->torch_time = now + c.torch_period_ms;
            scan_lamps(npc, *state, rays);
        }
    }
}

void queue(CAI_Stalker& npc, const SenseState::Stimulus& stimulus)
{
    SenseState* state = state_of(npc);
    if (!state)
        return;

    // One pending stimulus per source and kind: the strongest one stands for the rest until the NPC's next update.
    xr_vector<SenseState::Stimulus>& inbox = state->inbox;
    if (stimulus.who != u16(-1))
    {
        for (SenseState::Stimulus& pending : inbox)
        {
            if (pending.cue == stimulus.cue && pending.who == stimulus.who && pending.kind == stimulus.kind)
            {
                if (stimulus.power >= pending.power)
                    pending = stimulus;
                return;
            }
        }
    }
    if (inbox.size() >= config().inbox_max)
    {
        if (inbox.front().cue == SenseState::Cue::flash)
            stat_add(Stat::s_flash_x_wait, 1.f, community_of(npc));
        inbox.erase(inbox.begin());
    }
    inbox.push_back(stimulus);
}

void on_death(CAI_Stalker& npc, IGameObject* who)
{
    if (SenseState* state = state_of(npc))
    {
        state->targets.clear();
        state->occlusion.clear();
        state->repeats.clear();
        state->inbox.clear();
        state->suppression = 0.f;
    }
}

void level_unload()
{
    {
        std::lock_guard lock(g_near_miss_lock);
        g_near_misses.clear();
    }
    g_near_miss_batch.clear();
    g_drained_frame = u32(-1);
    g_lit.clear();
    hearing_level_unload();
    light_level_unload();
    g_config.loaded = false;
}

bool ray_pick(const Fvector& start, const Fvector& dir, float range, collide::rq_target target,
    collide::rq_result& result, IGameObject* ignore)
{
    result.O = nullptr;
    result.range = range;
    result.element = -1;

    const collide::ray_defs ray(start, dir, range, ray_flags() | CDB::OPT_ONLYNEAREST, target);
    if (!Level().ObjectSpace.RayQuery(g_ray_storage, ray, nullptr, nullptr, nullptr, ignore))
        return false;
    result = *g_ray_storage.r_begin();
    return result.element >= 0;
}

bool los(const Fvector& from, const Fvector& to, const IGameObject* ignore, float* vis)
{
    if (vis)
        *vis = 0.f;

    Fvector dir;
    dir.sub(to, from);
    const float range = dir.magnitude();
    if (range < EPS_L)
    {
        if (vis)
            *vis = 1.f;
        return true;
    }
    if (!RayBudget::take(1))
        return false;

    dir.div(range);
    const float result = trace_vis(from, dir, range, ignore);
    if (vis)
        *vis = result;
    return result > config().opaque_below;
}

float line_vis(const Fvector& from, const Fvector& to)
{
    Fvector dir;
    dir.sub(to, from);
    const float range = dir.magnitude();
    return range < EPS_L ? 1.f : trace_vis(from, dir.div(range), range, nullptr);
}

bool metered(const CAI_Stalker& npc) { return feature(Feature::awareness) && state_of(npc); }

float vision_seen(CAI_Stalker& npc, const CGameObject& object, const Glimpse& glimpse, float threshold, float dt)
{
    SenseState* state = state_of(npc);
    if (!state)
        return 0.f;

    Timer timer(community_of(npc), true);
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    SenseState::Target& target = state->targets[object.ID()];
    target.t_update = now;

    if (!glimpse.in_range)
        decay(target, glimpse.danger, dt);
    // In range the meter never falls, so an identified target stays identified whatever this glimpse's rate is.
    else if (target.awareness < c.identify)
    {
        float rate = 0.f;
        if (glimpse.instant)
            rate = flt_max;
        else
        {
            if (glimpse.intensity > 0.f)
            {
                rate = glimpse.intensity / _max(threshold, EPS_L) * (glimpse.danger ? c.rate_danger : c.rate_free);
                if (crouched(object))
                    rate *= c.crouch_mult;
            }
            // A lamp the target carries and a muzzle flash that drew the eye there; crouching hides neither.
            if (feature(Feature::light))
            {
                rate += lamp_glow(npc, object) * c.torch_rate;
                if (now < target.cue_until)
                    rate += target.cue_rate;
            }
            if (rate > 0.f)
            {
                const NpcState* ai = npc.da_ai();
                rate *= profile(npc).detect_rate * c.band_rate[clampr(int(ai->band) - 1, 0, 3)];
                rate *= 1.f + (c.peripheral_mult - 1.f) * angle_ratio(npc, object);
                if (glimpse.object_distance < c.close_m && c.close_m > 0.f)
                    rate *= 1.f + c.close_gain * (1.f - glimpse.object_distance / c.close_m);
                // Foliage and fences only slow the recognition down; the vision rays stop at "fully blocked".
                if (feature(Feature::concealment))
                    rate *= npc.feel_vision_transparency(&object);
            }
        }

        if (rate > 0.f)
        {
            if (target.awareness <= 0.f)
            {
                target.t_first = now;
                target.counted = false;
            }
            target.awareness = rate == flt_max ? c.identify : _min(target.awareness + rate * dt, c.identify);
        }
    }

    transitions(npc, object, target, glimpse.in_range);
    return mirror(target, threshold);
}

float vision_unseen(CAI_Stalker& npc, const CGameObject& object, bool danger, float threshold, float dt)
{
    SenseState* state = state_of(npc);
    if (!state)
        return 0.f;
    const auto found = state->targets.find(object.ID());
    if (found == state->targets.end())
        return 0.f;

    Timer timer(community_of(npc), true);
    SenseState::Target& target = found->second;
    decay(target, danger, dt);
    transitions(npc, object, target, false);
    return mirror(target, threshold);
}

float fog_sight_factor(float full_range)
{
    if (full_range <= EPS_L)
        return 1.f;
    const auto& env = GamePersistent().Environment().CurrentEnv;
    const float sight = env.fog_near + config().fog_sight * _max(env.fog_far - env.fog_near, 0.f);
    return clampr(sight / full_range, 0.f, 1.f);
}

float target_luminocity(const CGameObject& target, float rendered)
{
    // The renderer refreshes a body's light only while the camera draws it: a value that has not changed since the
    // environment darkened (a time skip, dusk out of the player's sight) darkens with it.
    const float environment = environment_light();
    Lit& lit = g_lit[target.ID()];
    if (lit.value != rendered || lit.environment <= 0.f)
        lit = {rendered, environment};
    else if (environment < lit.environment)
        rendered *= environment / lit.environment;
    return _max(rendered, config().daylight_lum * (1.f - darkness()));
}

bool cached_intensity(CAI_Stalker& npc, const CGameObject& object, bool danger, float distance, float& result)
{
    const SenseState* state = state_of(npc);
    if (!state)
        return false;
    const auto found = state->targets.find(object.ID());
    if (found == state->targets.end())
        return false;
    const Config& c = config();
    const SenseState::Target& target = found->second;
    if (!target.intensity_time || target.intensity_danger != danger ||
        float(Device.dwTimeGlobal - target.intensity_time) > c.intensity_cache_ms ||
        _abs(distance - target.intensity_d) > c.intensity_cache_m)
    {
        return false;
    }
    result = target.intensity;
    return true;
}

bool identified(const CAI_Stalker& npc, const CGameObject& object)
{
    const SenseState* state = state_of(npc);
    if (!state)
        return false;
    const auto found = state->targets.find(object.ID());
    return found != state->targets.end() && found->second.awareness >= config().identify;
}

void store_intensity(CAI_Stalker& npc, const CGameObject& object, bool danger, float distance, float value)
{
    SenseState* state = state_of(npc);
    if (!state)
        return;
    SenseState::Target& target = state->targets[object.ID()];
    target.intensity = value;
    target.intensity_d = distance;
    target.intensity_time = Device.dwTimeGlobal;
    target.intensity_danger = danger;
}

float vision_threshold(const CAI_Stalker& npc, float stock)
{
    return feature(Feature::concealment) && metered(npc) ? _min(stock, config().opaque_below) : stock;
}

void retrace(CAI_Stalker& npc, float dt)
{
    if (!feature(Feature::full_los) || !state_of(npc))
        return;
    const CEntityAlive* enemy = npc.memory().enemy().selected();
    if (!enemy)
        return;

    const auto in_view = std::find_if(npc.feel_visible.begin(), npc.feel_visible.end(),
        [enemy](const Feel::Vision::feel_visible_Item& item) { return item.O == enemy; });
    if (in_view == npc.feel_visible.end())
        return;

    Timer timer(community_of(npc), true);
    if (!RayBudget::take(1))
        return;
    const float threshold = vision_threshold(npc, npc.memory().visual().transparency_threshold());
    npc.feel_vision_retrace(enemy, npc.eye_matrix.c, dt, threshold, ray_flags());
    stat_add(Stat::s_retrace, 1.f, community_of(npc));
}

float awareness(const CAI_Stalker& npc, const CGameObject* object)
{
    if (!object)
        return 0.f;
    if (!metered(npc))
        return npc.memory().visual().visible_now(object) ? 1.f : 0.f;

    const SenseState* state = state_of(npc);
    const auto found = state->targets.find(object->ID());
    return found == state->targets.end() ? 0.f : clampr(found->second.awareness / config().identify, 0.f, 1.f);
}

float suppression(const CAI_Stalker& npc)
{
    const SenseState* state = state_of(npc);
    return state && feature(Feature::near_miss) ? current_suppression(*state) : 0.f;
}

Estimate make_estimate(const CAI_Stalker& npc, const CGameObject* target, const Fvector& truth, float err,
    Source source, float confidence)
{
    Estimate result = Know::realise(npc, target, truth, source, err);
    result.confidence = clampr(confidence, 0.f, 1.f);
    return result;
}

Estimate direction_guess(const CAI_Stalker& npc, const CGameObject& shooter, const Fvector& bullet_dir,
    float cone_deg, float prior_min, float prior_max, float distance_err, Source source)
{
    Fvector back = bullet_dir;
    back.invert();
    if (back.square_magnitude() < EPS_L)
        return {};
    back.normalize();

    // The distance comes from the NPC's own heard-shot belief of the shooter, else it is a guess within the prior.
    float prior = ::Random.randF(prior_min, prior_max);
    float spread = (prior_max - prior_min) * .5f;
    if (const Belief* belief = Know::belief(npc, shooter.ID());
        belief && belief->e.source == Source::heard_shot && belief->e.err > 0.f)
    {
        prior = npc.Position().distance_to(belief->e.pos);
        spread = 0.f;
    }
    const float guess = prior * (1.f + ::Random.randF(-distance_err, distance_err));
    const float cone = deg2rad(cone_deg);

    float yaw, pitch;
    back.getHP(yaw, pitch);
    back.setHP(yaw + ::Random.randF(-cone, cone), pitch + ::Random.randF(-cone, cone) * .5f);

    Fvector spot;
    spot.mad(npc.Position(), back, guess);
    const float err = _sqrt(_sqr(guess * _sin(cone)) + _sqr(_max(guess * distance_err, spread)));
    return estimate_at(npc, &shooter, spot, err, source, clampr(1.f - err / 60.f, .1f, 1.f));
}

void on_hit(CAI_Stalker& npc, const SHit& hit)
{
    SenseState* state = state_of(npc);
    if (!state || !feature(Feature::hearing) || !hit.is_valide())
        return;
    const CEntityAlive* who = smart_cast<const CEntityAlive*>(hit.initiator());
    if (!who || who->ID() == npc.ID())
        return;

    const int community = community_of(npc);
    Timer timer(community, true);
    const Config& c = config();
    const float distance = npc.Position().distance_to(who->Position());

    Estimate estimate;
    if (Know::sees_now(npc, who))
        estimate = make_estimate(npc, who, who->Position(), 0.f, Source::hit, 1.f);
    else if (distance <= c.touch_m)
        estimate = make_estimate(npc, who, who->Position(), c.touch_m, Source::hit, 1.f);
    else if (hit.type() == ALife::eHitTypeFireWound)
    {
        // Only where the bullet came from: a direction cone and a guessed distance, never the shooter himself.
        estimate = direction_guess(npc, *who, hit.direction(), c.hit_cone_deg.at(1.f - profile(npc).hear_skill),
            c.hit_distance_prior_min, c.hit_distance_prior_max, c.hit_distance_err, Source::hit);
        if (estimate.source == Source::none)
            return;
    }
    else
        return;

    state->hit = {estimate, Device.dwFrame, Device.dwTimeGlobal, who->ID()};
    Know::observe(npc, who, estimate);
    Know::suspect(npc, estimate, CDangerObject::eDangerTypeAttacked, CDangerObject::eDangerPerceiveTypeHit);
}

bool process_near_miss(CAI_Stalker& npc, SenseState& state, const SenseState::Stimulus& stimulus)
{
    if (!feature(Feature::near_miss))
        return true;
    const CEntityAlive* shooter = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(stimulus.who));
    if (!shooter || Know::sees_now(npc, shooter))
        return true;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    // A bullet crossing the NPC's sphere also reports the hit it made; the hit already placed the shooter.
    if (state.hit.who == stimulus.who && _abs(int(stimulus.time) - int(state.hit.time)) <= int(c.near_miss_hit_ms))
        return true;

    SenseState::Target& target = state.targets[stimulus.who];
    target.t_update = _max(target.t_update, now);
    if (target.t_near_miss && now < target.t_near_miss + c.near_miss_estimate_ms)
        return true;
    target.t_near_miss = now;

    // Where the crack came from: the reversed bullet direction and a guessed distance.
    const Estimate estimate = direction_guess(npc, *shooter, stimulus.direction, c.near_miss_cone_deg,
        c.near_miss_distance_prior_min, c.near_miss_distance_prior_max, c.hit_distance_err, Source::near_miss);
    if (estimate.source == Source::none)
        return true;
    Know::observe(npc, shooter, estimate);
    Know::suspect(npc, estimate, CDangerObject::eDangerTypeAttackSound, CDangerObject::eDangerPerceiveTypeSound);
    return true;
}

void apply_hit_estimate(const CAI_Stalker& npc, MemorySpace::CHitObject& entry)
{
    const SenseState* state = state_of(npc);
    if (!state || state->hit.frame != Device.dwFrame || !entry.m_object || entry.m_object->ID() != state->hit.who)
        return;
    entry.m_object_params.m_position = state->hit.estimate.pos;
    if (state->hit.estimate.vertex != u32(-1))
        entry.m_object_params.m_level_vertex_id = state->hit.estimate.vertex;
}
} // namespace DaAi::Sense

void da_ai_sense_queue_near_miss(u16 shooter, u16 victim, const Fvector& point, const Fvector& direction)
{
    using namespace DaAi::Sense;
    // Any thread: only ids and vectors go in; the main thread resolves the objects when it drains the queue.
    if (shooter == victim || shooter == u16(-1) || !_valid(point) || !_valid(direction))
        return;
    std::lock_guard lock(g_near_miss_lock);
    if (g_near_misses.size() < g_near_miss_cap)
        g_near_misses.push_back({point, direction, Device.dwTimeGlobal, shooter, victim});
}
