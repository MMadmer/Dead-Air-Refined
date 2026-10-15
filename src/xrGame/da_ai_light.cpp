// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_senses.h"
#include "da_ai_knowledge.h"
#include "da_ai_npc.h"
#include "da_ai_tactics.h"

#include "Actor.h"
#include "actor_defs.h"
#include "ai/stalker/ai_stalker.h"
#include "danger_object.h"
#include "Level.h"
// CTorch inherits engine interfaces by dominance; the C4250 notes belong to its header, not to this file.
#pragma warning(push)
#pragma warning(disable : 4250)
#include "Torch.h"
#pragma warning(pop)
#include "Weapon.h"
#include "xrEngine/CameraBase.h"

namespace DaAi::Sense
{
namespace
{
// A lit lamp in someone's hands. The spot its beam lights is traced at most once per [ai_light] torch_period_ms and
// shared by every observer.
struct Lamp
{
    Fvector spot{};
    u32 spot_time{};
    u16 torch{u16(-1)};
    u16 carrier{u16(-1)};
    bool spot_valid{};
};

constexpr size_t flash_shooters_max = 256;

xr_vector<Lamp> g_lamps;
xr_flat_hash_map<u16, u32> g_flash_time; // shooter -> last muzzle flash event
u32 g_lamps_checked{u32(-1)}; // frame of the last removal of dark or lost lamps

SenseState* light_state(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && !state->dead ? state->sense : nullptr;
}

int community_of(const IGameObject* object)
{
    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(object);
    const NpcState* state = stalker ? stalker->da_ai() : nullptr;
    return state ? state->community : -1;
}

const CTorch* lit_torch(const Lamp& lamp)
{
    const CTorch* torch = smart_cast<const CTorch*>(Level().Objects.net_Find(lamp.torch));
    return torch && torch->torch_active() && torch->H_Parent() && torch->H_Parent()->ID() == lamp.carrier ? torch :
                                                                                                             nullptr;
}

float mix(float from, float to, float t) { return from + (to - from) * t; }

// A body's head: a stalker's eye, the actor's camera in first person, else two thirds up its bounding sphere.
Fvector head_of(const CGameObject& body)
{
    if (const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(&body))
        return stalker->eye_matrix.c;
    const CActor* actor = smart_cast<const CActor*>(&body);
    if (actor && const_cast<CActor*>(actor)->active_cam() == eacFirstEye && Level().CurrentViewEntity() == actor)
        return Device.vCameraPosition;
    Fvector position;
    body.Center(position);
    position.y += body.Radius() * 2.f / 3.f;
    return position;
}

// Where a carried lamp shines from and where it points: the carrier's head, a stalker's gaze, the actor's camera as
// the torch follows it, else the carrier's heading.
void lamp_pose(const CGameObject& carrier, Fvector& position, Fvector& direction)
{
    position = head_of(carrier);
    if (const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(&carrier))
    {
        direction = view_direction(*stalker);
        return;
    }

    direction = carrier.XFORM().k;
    if (CActor* actor = smart_cast<CActor*>(const_cast<CGameObject*>(&carrier)))
    {
        const CCameraBase* camera = actor->active_cam() == eacLookAt ? actor->cam_Active() : actor->cam_FirstEye();
        if (camera)
            direction.setHP(-camera->yaw, -camera->pitch);
    }
}

// How bright a lamp looks from an eye: brightest along its beam, dim side-on, gone when it points away, fading over
// the range light carries by day or by night.
float glow_at(const Fvector& eye, const Fvector& lamp, const Fvector& beam, float dark)
{
    const Config& c = config();
    Fvector to;
    to.sub(eye, lamp);
    const float distance = to.magnitude();
    const float range = mix(c.torch_range_day, c.torch_range_night, dark);
    if (range <= 0.f || distance >= range)
        return 0.f;

    const float facing = distance > EPS_L ? beam.dotproduct(to.div(distance)) : 1.f;
    const float strength = facing >= 0.f ? mix(c.torch_strength_min, c.torch_strength_max, facing) :
                                           c.torch_strength_min * (1.f + facing);
    return strength * (1.f - distance / range);
}

void drop_dark_lamps()
{
    if (g_lamps_checked == Device.dwFrame)
        return;
    g_lamps_checked = Device.dwFrame;
    g_lamps.erase(std::remove_if(g_lamps.begin(), g_lamps.end(), [](const Lamp& lamp) { return !lit_torch(lamp); }),
        g_lamps.end());
}

// The spot the beam lights: one static ray per lamp and period, for everybody.
bool lamp_spot(Lamp& lamp, const Fvector& position, const Fvector& direction, float dark)
{
    const u32 now = Device.dwTimeGlobal;
    if (now < lamp.spot_time)
        return lamp.spot_valid;
    if (!RayBudget::take(1))
        return false;

    const Config& c = config();
    lamp.spot_time = now + c.torch_period_ms;
    collide::rq_result result;
    lamp.spot_valid = ray_pick(position, direction, mix(c.torch_range_day, c.torch_range_night, dark),
        collide::rqtStatic, result, nullptr);
    if (lamp.spot_valid)
        lamp.spot.mad(position, direction, result.range);
    return lamp.spot_valid;
}

// A light points at someone: a belief about the carrier with the flashlight error, a danger for an enemy.
void notice(CAI_Stalker& npc, SenseState& state, const CEntityAlive& carrier, float err, float confidence)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    stat_add(Stat::s_torch, 1.f, community_of(&npc));

    const Estimate estimate =
        make_estimate(npc, &carrier, carrier.Position(), err, Source::flashlight, clampr(confidence, .1f, 1.f));
    Know::observe(npc, &carrier, estimate);

    SenseState::Target& target = state.targets[carrier.ID()];
    target.t_update = _max(target.t_update, now);
    if (npc.is_relation_enemy(&carrier) && (!target.t_suspect || now >= target.t_suspect + c.suspect_period_ms))
    {
        target.t_suspect = now;
        Know::suspect(npc, estimate, CDangerObject::eDangerTypeEnemySound, CDangerObject::eDangerPerceiveTypeVisual);
    }
}
} // namespace

void muzzle_flash(CWeapon& weapon)
{
    if (!feature(Feature::light))
        return;
    const CEntityAlive* shooter = smart_cast<const CEntityAlive*>(weapon.H_Parent());
    if (!shooter || !shooter->g_Alive())
        return;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (g_flash_time.size() > flash_shooters_max)
    {
        for (auto it = g_flash_time.begin(); it != g_flash_time.end();)
        {
            if (now >= it->second + c.flash_period_ms)
                it = g_flash_time.erase(it);
            else
                ++it;
        }
    }
    // One flash event per shooter and period: an automatic burst costs one set of rays, not one per round.
    u32& last = g_flash_time[shooter->ID()];
    if (last && now < last + c.flash_period_ms)
    {
        stat_add(Stat::s_flash_x_period, 1.f, community_of(shooter));
        return;
    }
    last = now;

    const float mult = weapon.IsSilencerAttached() ? c.muzzle_silenced : 1.f;
    if (mult <= 0.f)
        return;

    Timer timer(community_of(shooter), true);
    const float dark = darkness();
    const float range = mix(c.muzzle_range_day, c.muzzle_range_night, dark) * mult;
    // Observers another hour's light would reach count as rejected by the range; farther ones never could see it.
    const float reach = _max(c.muzzle_range_day, c.muzzle_range_night) * mult;
    SenseState::Stimulus stimulus;
    stimulus.position = weapon.get_LastFP();
    stimulus.head = head_of(*shooter);
    stimulus.power = mix(c.flash_day_mult, 1.f, dark);
    stimulus.time = now;
    stimulus.who = shooter->ID();
    stimulus.kind = Noise::shot;
    stimulus.cue = SenseState::Cue::flash;
    for (CAI_Stalker* npc : online_npcs())
    {
        if (!npc || npc->ID() == shooter->ID() || !light_state(*npc))
            continue;
        const float distance_sqr = npc->eye_matrix.c.distance_to_sqr(stimulus.position);
        if (distance_sqr > _sqr(reach))
            continue;
        // The first rule that rejects the flash for this observer names the reason.
        const int community = npc->da_ai()->community;
        if (distance_sqr > _sqr(range))
            stat_add(Stat::s_flash_x_range, 1.f, community);
        else if (!in_view(*npc, stimulus.position))
            stat_add(Stat::s_flash_x_view, 1.f, community);
        else if (Know::sees_now(*npc, shooter))
            stat_add(Stat::s_flash_x_seen, 1.f, community);
        else
        {
            stat_add(Stat::s_flash_q, 1.f, community);
            queue(*npc, stimulus);
        }
    }
}

bool process_flash(CAI_Stalker& npc, SenseState& state, SenseState::Stimulus& stimulus, u32 rays)
{
    if (!feature(Feature::light))
        return true;
    const CEntityAlive* shooter = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(stimulus.who));
    if (!shooter)
        return true;
    const int community = community_of(&npc);
    if (Know::sees_now(npc, shooter))
    {
        stat_add(Stat::s_flash_x_seen, 1.f, community);
        return true;
    }

    // The flash shows at the muzzle and on the shooter's head it lights: the muzzle's exact point alone was hidden
    // from every observer of a shooter in clear view (r1m: 8 of 8, the line to his weapon clear at 151 m). Each line
    // waits for the ray budget; los() alone cannot tell "blocked" from "deferred".
    const Fvector& eye = npc.eye_matrix.c;
    if (!stimulus.muzzle_hidden)
    {
        if (!rays || !RayBudget::left())
            return false;
        --rays;
        stimulus.muzzle_hidden = !los(eye, stimulus.position, &npc);
    }
    if (stimulus.muzzle_hidden)
    {
        if (!rays || !RayBudget::left())
            return false;
        if (!los(eye, stimulus.head, &npc))
        {
            stat_add(Stat::s_flash_x_los, 1.f, community);
            return true;
        }
    }

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    stat_add(Stat::s_flash, 1.f, community);
    const Fvector& seen = stimulus.muzzle_hidden ? stimulus.head : stimulus.position;
    const float err = c.muzzle_flash.at(eye.distance_to(seen));
    const Estimate estimate =
        make_estimate(npc, shooter, seen, err, Source::muzzle_flash, clampr(1.f - err / 60.f, .1f, 1.f));
    Know::suspect(npc, estimate, CDangerObject::eDangerTypeAttackSound, CDangerObject::eDangerPerceiveTypeVisual);
    if (npc.tfGetRelationType(shooter) != ALife::eRelationTypeFriend)
        Know::observe(npc, shooter, estimate);

    // The eye is drawn to the flash: the shooter is recognised faster for a moment.
    SenseState::Target& target = state.targets[shooter->ID()];
    target.t_update = _max(target.t_update, now);
    target.cue_until = now + c.flash_cue_ms;
    target.cue_rate = c.flash_rate * stimulus.power;
    return true;
}

void scan_lamps(CAI_Stalker& npc, SenseState& state, u32 ray_cap)
{
    drop_dark_lamps();
    if (g_lamps.empty())
        return;

    const Config& c = config();
    const float dark = darkness();
    const Fvector& eye = npc.eye_matrix.c;
    // A look casts at most torch_scan_rays rays (the beams' shared spots included) and asks who carries a lamp
    // (relation, own sight) at most torch_scan_checks times. At night every NPC lit its torch: one look at all of them
    // was a millisecond (K9: 0.87-0.96 ms), most of it the spot rays of every lamp falling due in one update.
    // The geometry of every lamp comes first and costs little. The lamps facing the NPC are judged first, brightest
    // first: the round robin of the spots had spent the caps on other lamps' beams before it reached the one shining
    // at the NPC. The spots take what is left, round robin from where the last look stopped.
    struct Seen
    {
        Fvector position;
        Fvector direction;
        const CEntityAlive* carrier;
        u32 lamp;
        float glow;
        bool judged; // the carrier was asked about (friend or seen: done)
        bool done;
    };
    // Main thread only (the scheduler is serial).
    static xr_vector<Seen> seen;
    static xr_vector<u32> facing;
    seen.clear();
    facing.clear();

    const size_t count = g_lamps.size();
    const size_t start = state.lamp_cursor % count;
    for (size_t visited = 0, index = start; visited < count; ++visited, index = (index + 1) % count)
    {
        const Lamp& lamp = g_lamps[index];
        if (lamp.carrier == npc.ID())
            continue;
        const CTorch* torch = lit_torch(lamp);
        const CEntityAlive* carrier = torch ? smart_cast<const CEntityAlive*>(torch->H_Parent()) : nullptr;
        if (!carrier || !carrier->g_Alive())
            continue;
        Seen& item = seen.emplace_back();
        lamp_pose(*carrier, item.position, item.direction);
        item.carrier = carrier;
        item.lamp = u32(index);
        item.glow = glow_at(eye, item.position, item.direction, dark);
        item.judged = item.done = false;
        // The lamp itself, once it faces the NPC enough to stand out.
        if (item.glow >= c.torch_notice && in_view(npc, item.position))
            facing.push_back(u32(seen.size() - 1));
    }

    const u32 rays = _min(_max(c.torch_scan_rays, 1u), _max(ray_cap, 1u));
    const u32 checks = _max(c.torch_scan_checks, 1u);
    u32 traced = 0;
    u32 checked = 0;
    const auto carrier_ignored = [&npc](const CEntityAlive& carrier) {
        return npc.tfGetRelationType(&carrier) == ALife::eRelationTypeFriend || Know::sees_now(npc, &carrier);
    };

    std::sort(facing.begin(), facing.end(), [](u32 a, u32 b) { return seen[a].glow > seen[b].glow; });
    const size_t first = state.facing_skip < facing.size() ? state.facing_skip : 0;
    state.facing_skip = 0;
    for (size_t k = 0; k < facing.size(); ++k)
    {
        const size_t slot = (first + k) % facing.size();
        Seen& item = seen[facing[slot]];
        if (checked >= checks || traced >= rays)
        {
            // The caps ran out: the next look starts with the lamps this one left.
            state.facing_skip = u32(slot);
            break;
        }
        ++checked;
        item.judged = true;
        if (carrier_ignored(*item.carrier))
        {
            item.done = true;
            continue;
        }
        ++traced;
        if (los(eye, item.position, &npc))
        {
            notice(npc, state, *item.carrier, c.flashlight.at(eye.distance_to(item.position)), item.glow);
            item.done = true;
        }
    }

    // At night the patch of light the beam throws on a wall or the ground gives the carrier away.
    if (dark < c.spot_darkness)
        return;
    size_t index = 0;
    for (; index < seen.size(); ++index)
    {
        Seen& item = seen[index];
        if (item.done)
            continue;
        Lamp& lamp = g_lamps[item.lamp];
        const bool due = Device.dwTimeGlobal >= lamp.spot_time;
        if (due && traced >= rays)
            break;
        traced += due ? 1 : 0;
        if (!lamp_spot(lamp, item.position, item.direction, dark))
            continue;
        const float spot_distance = eye.distance_to(lamp.spot);
        if (spot_distance > c.spot_range || spot_distance < EPS_L || !in_view(npc, lamp.spot))
            continue;

        if (!item.judged)
        {
            if (checked >= checks)
                break;
            ++checked;
            item.judged = true;
            if (carrier_ignored(*item.carrier))
                continue;
        }
        if (traced >= rays)
            break;
        // The ray stops short of the lit surface, which would otherwise block it.
        Fvector target;
        target.lerp(lamp.spot, eye, _min(.3f / spot_distance, 1.f));
        ++traced;
        if (!los(eye, target, &npc))
            continue;
        const float err = c.flashlight.at(spot_distance) + c.spot_err_k * item.position.distance_to(lamp.spot);
        notice(npc, state, *item.carrier, err, 1.f - err / 60.f);
    }
    // The next look goes on from the lamp this one stopped at (every lamp done: from the same start).
    state.lamp_cursor = index < seen.size() ? seen[index].lamp : u32(start);
}

float lamp_glow(const CAI_Stalker& observer, const CGameObject& carrier)
{
    if (g_lamps.empty())
        return 0.f;
    const auto lamp = std::find_if(
        g_lamps.begin(), g_lamps.end(), [&carrier](const Lamp& item) { return item.carrier == carrier.ID(); });
    if (lamp == g_lamps.end() || !lit_torch(*lamp))
        return 0.f;

    Fvector position, direction;
    lamp_pose(carrier, position, direction);
    return glow_at(observer.eye_matrix.c, position, direction, darkness());
}

void light_level_unload()
{
    g_lamps.clear();
    g_flash_time.clear();
    g_lamps_checked = u32(-1);
}
} // namespace DaAi::Sense

void da_ai_sense_on_shot(CWeapon& weapon)
{
    // Shot accounting of the shooter's own lanes: knowledge counts blind shots, tactics the shot counters.
    if (CAI_Stalker* shooter = smart_cast<CAI_Stalker*>(weapon.H_Parent()); shooter && shooter->g_Alive())
    {
        DaAi::Know::on_own_shot(*shooter);
        DaAi::Tactics::on_own_shot(*shooter);
    }
    DaAi::Sense::muzzle_flash(weapon);
}

void da_ai_sense_on_torch(CTorch& torch, bool light_on)
{
    using namespace DaAi::Sense;
    const CEntityAlive* carrier = smart_cast<const CEntityAlive*>(torch.H_Parent());
    const bool lit = light_on && carrier && carrier->g_Alive();
    const auto found =
        std::find_if(g_lamps.begin(), g_lamps.end(), [&torch](const Lamp& lamp) { return lamp.torch == torch.ID(); });
    if (lit == (found != g_lamps.end()))
    {
        if (lit)
            found->carrier = carrier->ID();
        return;
    }

    if (lit)
        g_lamps.push_back({{}, 0, torch.ID(), carrier->ID(), false});
    else
        g_lamps.erase(found);

    // The switch clicks; a lamp restored by a load or a spawn does not.
    if (carrier && carrier->g_Alive() && !Device.dwPrecacheFrame)
    {
        Fvector center;
        carrier->Center(center);
        emit_noise(center, config().torch_click, DaAi::Noise::torch_click, carrier);
    }
}
