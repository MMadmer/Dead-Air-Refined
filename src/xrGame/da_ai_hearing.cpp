// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_senses.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_tactics.h"

#include "Actor.h"
#include "actor_defs.h"
#include "ai/stalker/ai_stalker.h"
#include "danger_object.h"
#include "GamePersistent.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "material_manager.h"
#include "memory_manager.h"
#include "Missile.h"
#include "sound_memory_manager.h"
#include "stalker_movement_manager_smart_cover.h"
// CGrenade and CTorch inherit engine interfaces by dominance; the C4250 notes belong to their headers.
#pragma warning(push)
#pragma warning(disable : 4250)
#include "Grenade.h"
#include "Torch.h"
#pragma warning(pop)
#include "Weapon.h"
#include "xrEngine/Environment.h"
#include "xrEngine/da_snow_field.h"
#include "xrMaterialSystem/GameMtlLib.h"

namespace DaAi::Sense
{
namespace
{
// A fight is heard from more than eight sources at once: fewer slots evicted each other's rays before they aged.
constexpr size_t occlusion_slots = 16;
constexpr size_t repeat_slots = 8;

collide::rq_results g_occlusion_storage;

bool is_type(u32 type, u32 mask) { return (type & mask) == mask; }

Noise kind_of(u32 type)
{
    if (is_type(type, SOUND_TYPE_WEAPON_SHOOTING))
        return Noise::shot;
    if (is_type(type, SOUND_TYPE_WEAPON_BULLET_HIT))
        return Noise::impact;
    if (is_type(type, SOUND_TYPE_WEAPON_RECHARGING))
        return Noise::reload;
    if (is_type(type, SOUND_TYPE_OBJECT_EXPLODING))
        return Noise::explosion;
    if (is_type(type, SOUND_TYPE_MONSTER_STEP))
        return Noise::step;
    if (is_type(type, SOUND_TYPE_MONSTER))
        return Noise::voice;
    if (is_type(type, SOUND_TYPE_ITEM) || is_type(type, SOUND_TYPE_WEAPON))
        return Noise::item;
    if (is_type(type, SOUND_TYPE_WORLD_OBJECT_BREAKING) || is_type(type, SOUND_TYPE_WORLD_OBJECT_COLLIDING))
        return Noise::impact;
    return Noise::count;
}

// One source per stimulus kind (C12): a human's steps, weapon reloads and torch clicks reach the AI only through the
// noise bus. The grenade pin and other item sounds have no bus hook and stay engine sounds.
bool bus_owned(u32 type, const IGameObject* who)
{
    if (is_type(type, SOUND_TYPE_MONSTER_STEP))
        return true;
    if (is_type(type, SOUND_TYPE_WEAPON_RECHARGING))
        return !!smart_cast<const CWeapon*>(who);
    if (is_type(type, SOUND_TYPE_ITEM_USING))
        return !!smart_cast<const CTorch*>(who);
    return false;
}

Source source_of(Noise kind)
{
    switch (kind)
    {
    case Noise::shot:
    case Noise::explosion: return Source::heard_shot;
    case Noise::step:
    case Noise::landing: return Source::heard_step;
    case Noise::reload:
    case Noise::grenade_draw: return Source::heard_reload;
    case Noise::voice: return Source::heard_voice;
    default: return Source::heard_item;
    }
}

const Linear& error_model(Noise kind)
{
    const Config& c = config();
    switch (kind)
    {
    case Noise::shot:
    case Noise::explosion: return c.heard_shot;
    case Noise::step:
    case Noise::landing: return c.heard_step;
    default: return c.heard_other;
    }
}

bool silenced(const CEntityAlive& shooter)
{
    const CInventoryOwner* owner = smart_cast<const CInventoryOwner*>(&shooter);
    const CWeapon* weapon = owner ? smart_cast<const CWeapon*>(owner->inventory().ActiveItem()) : nullptr;
    return weapon && weapon->IsSilencerAttached();
}

struct OcclusionParam
{
    float factor;
    u32 hits;
    u32 max_hits;
    float floor;
};

bool occlusion_callback(collide::rq_result& result, LPVOID params)
{
    OcclusionParam& param = *static_cast<OcclusionParam*>(params);
    if (result.O)
        return true;
    const CDB::TRI* tri = Level().ObjectSpace.GetStaticTris() + result.element;
    param.factor *= GMLib.GetMaterialByIdx(tri->material)->fSndOcclusionFactor;
    return ++param.hits < param.max_hits && param.factor > param.floor;
}

// The cached ray toward a source near that spot; fresh = still within its time. A source that moved a little tells
// nothing new about the walls between: the farther it is, the more it may move (the line barely turns), and a hearer
// far from the player keeps its rays longer.
SenseState::Occlusion* occlusion_slot(
    const CAI_Stalker& hearer, SenseState& state, const Fvector& position, u16 source, float range, bool& fresh)
{
    const Config& c = config();
    const float move = c.occlusion_move_m + c.occlusion_move_k * range;
    fresh = false;
    for (SenseState::Occlusion& entry : state.occlusion)
    {
        if (entry.source == source && entry.position.distance_to_sqr(position) < _sqr(move))
        {
            const NpcState* self = hearer.da_ai();
            fresh = Device.dwTimeGlobal < entry.time + (self && self->distant ? c.occlusion_cache_far_ms :
                                                                              c.occlusion_cache_ms);
            return &entry;
        }
    }
    return nullptr;
}

// True when hearing a sound from there needs no new ray.
bool occlusion_known(const CAI_Stalker& hearer, SenseState& state, const Fvector& position, u16 source)
{
    Fvector to = position;
    to.y += .3f;
    const float range = hearer.eye_matrix.c.distance_to(to);
    bool fresh;
    return range < 1.f || (occlusion_slot(hearer, state, position, source, range, fresh) && fresh);
}

// How much of a sound gets through the walls between the source and the hearer's head: the product of the
// materials' sound occlusion along one static ray, floored because sound also bends around corners.
float occlusion_factor(CAI_Stalker& hearer, SenseState& state, const Fvector& position, u16 source)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;

    Fvector from = hearer.eye_matrix.c, to = position;
    to.y += .3f;
    Fvector dir;
    dir.sub(to, from);
    const float range = dir.magnitude();
    if (range < 1.f)
        return 1.f;

    bool fresh;
    SenseState::Occlusion* slot = occlusion_slot(hearer, state, position, source, range, fresh);
    if (slot && fresh)
        return slot->factor;
    // Deferred by the ray budget: the last result for this source, else only the engine's own occlusion.
    if (!RayBudget::take(1))
        return slot ? slot->factor : 1.f;
    stat_add(Stat::s_occlusion_rays, 1.f, hearer.da_ai()->community);

    dir.div(range);
    OcclusionParam param{1.f, 0, _max(c.occlusion_hits, 1u), c.occlusion_floor};
    const collide::ray_defs ray(from, dir, range - .3f, CDB::OPT_CULL, collide::rqtStatic);
    Level().ObjectSpace.RayQuery(g_occlusion_storage, ray, occlusion_callback, &param, nullptr, nullptr);
    const float factor = clampr(param.factor, c.occlusion_floor, 1.f);

    if (!slot)
    {
        if (state.occlusion.size() < occlusion_slots)
            slot = &state.occlusion.emplace_back();
        else
        {
            slot = &*std::min_element(state.occlusion.begin(), state.occlusion.end(),
                [](const SenseState::Occlusion& a, const SenseState::Occlusion& b) { return a.time < b.time; });
        }
    }
    slot->position = position;
    slot->source = source;
    slot->time = now;
    slot->factor = factor;
    return factor;
}

const SenseState::Repeat* repeat_of(
    const CAI_Stalker& hearer, const SenseState& state, u16 source, Noise kind, const Fvector& position)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    // A calm NPC far from the player takes a distant fight's gunfire as fewer observations; its first shot counts.
    const NpcState* self = hearer.da_ai();
    const bool coarse = self && self->distant && self->calm;
    const u32 period = coarse ? _max(c.heard_repeat_far_ms, c.heard_repeat_ms) : c.heard_repeat_ms;
    const float reach = coarse ? _max(c.heard_repeat_far_m, c.heard_repeat_m) : c.heard_repeat_m;
    for (const SenseState::Repeat& entry : state.repeats)
    {
        if (entry.source == source && entry.kind == kind && now < entry.time + period &&
            entry.position.distance_to_sqr(position) <= _sqr(reach))
        {
            return &entry;
        }
    }
    return nullptr;
}

void remember_repeat(SenseState& state, u16 source, Noise kind, const Fvector& position, const Estimate& estimate)
{
    if (!config().heard_repeat_ms)
        return;
    auto slot = std::find_if(state.repeats.begin(), state.repeats.end(),
        [source, kind](const SenseState::Repeat& entry) { return entry.source == source && entry.kind == kind; });
    if (slot == state.repeats.end())
    {
        if (state.repeats.size() < repeat_slots)
            slot = state.repeats.emplace(state.repeats.end());
        else
        {
            slot = std::min_element(state.repeats.begin(), state.repeats.end(),
                [](const SenseState::Repeat& a, const SenseState::Repeat& b) { return a.time < b.time; });
        }
    }
    *slot = {estimate, position, Device.dwTimeGlobal, source, kind};
}

// The danger the stock danger manager derives from a sound entry (CDangerManager::add(CSoundObject)).
u32 danger_type(const CAI_Stalker& hearer, u32 type, const CEntityAlive* source)
{
    if (is_type(type, SOUND_TYPE_BULLET_HIT))
        return CDangerObject::eDangerTypeBulletRicochet;
    if (is_type(type, SOUND_TYPE_WEAPON_SHOOTING))
        return CDangerObject::eDangerTypeAttackSound;
    if (is_type(type, SOUND_TYPE_INJURING))
    {
        if (source && smart_cast<const CActor*>(source) && !hearer.is_relation_enemy(source))
            return CDangerObject::eDangerTypeDummy;
        return CDangerObject::eDangerTypeEntityAttacked;
    }
    if (is_type(type, SOUND_TYPE_DYING))
        return CDangerObject::eDangerTypeEntityDeath;
    if (source && hearer.is_relation_enemy(source))
        return CDangerObject::eDangerTypeEnemySound;
    return CDangerObject::eDangerTypeDummy;
}

SenseState* hearing_state(const CAI_Stalker& hearer)
{
    const NpcState* state = hearer.da_ai();
    return feature(Feature::hearing) && state && !state->dead ? state->sense : nullptr;
}

// The bus runs only together with the hearing it feeds; with either off the engine sounds stay the only source.
bool bus_on() { return feature(Feature::noise) && feature(Feature::hearing); }

int community_of(const IGameObject* object)
{
    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(object);
    const NpcState* state = stalker ? stalker->da_ai() : nullptr;
    return state ? state->community : -1;
}

// The localisation error of a heard sound: worse with distance, walls, a suppressor and a poor ear.
float heard_error(const CAI_Stalker& hearer, Noise kind, float distance, float occlusion, bool silenced)
{
    const Config& c = config();
    float err = error_model(kind).at(distance);
    if (silenced)
        err *= c.heard_silenced_mult;
    err *= 1.f + c.heard_occlusion_mult * occlusion;
    err *= _max(c.heard_skill_base - profile(hearer).hear_skill, .1f);
    return err;
}

// [ai_noise_surface]: step loudness by the ground's material name (its last path part), cached per material index.
xr_vector<float> g_surface;

float surface_factor(u16 material)
{
    if (g_surface.empty())
    {
        constexpr pcstr section = "ai_noise_surface";
        const bool present = pSettings->section_exist(section);
        const float fallback = present ? READ_IF_EXISTS(pSettings, r_float, section, "default", 1.f) : 1.f;
        g_surface.assign(GMLib.CountMaterial(), fallback);
        for (u32 i = 0; present && i < g_surface.size(); ++i)
        {
            const SGameMtl* mtl = GMLib.GetMaterialByIdx(u16(i));
            pcstr name = mtl ? mtl->m_Name.c_str() : nullptr;
            if (!name)
                continue;
            if (pcstr slash = strrchr(name, '\\'))
                name = slash + 1;
            if (pSettings->line_exist(section, name))
                g_surface[i] = pSettings->r_float(section, name);
        }
    }
    return material < g_surface.size() ? g_surface[material] : 1.f;
}

// Carried weight above the free load makes every step louder (the old script guessed it through walls).
float load_factor(const CGameObject& walker)
{
    const Config& c = config();
    const CInventoryOwner* owner = smart_cast<const CInventoryOwner*>(&walker);
    if (!owner)
        return 1.f;
    return 1.f + c.load_step_mult * _max(owner->inventory().TotalWeight() - c.load_free_kg, 0.f);
}

// Loudness of one step by the walker's gait; landings of the actor count as their own kind.
float gait_loudness(const CEntityAlive& walker, bool& landing)
{
    const Config& c = config();
    landing = false;
    if (const CActor* actor = smart_cast<const CActor*>(&walker))
    {
        const u32 state = actor->MovingState();
        if (state & (mcLanding | mcLanding2))
        {
            landing = true;
            return c.landing;
        }
        if (state & mcCrouch)
            return c.gait[0];
        if (state & mcSprint)
            return c.gait[3];
        if (!(state & mcAnyMove))
            return c.shuffle;
        return isActorAccelerated(state, actor->IsZoomAimingMode()) ? c.gait[2] : c.gait[1];
    }

    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(&walker);
    if (!stalker)
        return 0.f;
    const auto& movement = stalker->movement();
    if (movement.body_state() == MonsterSpace::eBodyStateCrouch)
        return c.gait[0];
    switch (movement.movement_type())
    {
    case MonsterSpace::eMovementTypeWalk: return c.gait[1];
    case MonsterSpace::eMovementTypeRun:
        return movement.mental_state() == MonsterSpace::eMentalStatePanic ? c.gait[3] : c.gait[2];
    default: return c.shuffle;
    }
}

// A human walker, alive: steps of mutants stay engine sounds.
const CEntityAlive* human(const IGameObject* object)
{
    const CEntityAlive* entity = smart_cast<const CEntityAlive*>(object);
    if (!entity || !entity->g_Alive())
        return nullptr;
    return smart_cast<const CActor*>(entity) || smart_cast<const CAI_Stalker*>(entity) ? entity : nullptr;
}
} // namespace

bool engine_sound(CAI_Stalker& hearer, const IGameObject* who, u32 sound_type, const Fvector& position, float& power,
    float threshold, Heard& heard)
{
    if (!hearing_state(hearer))
        return false;

    Timer timer(hearer.da_ai()->community, true);
    heard = {};
    heard.kind = kind_of(sound_type);

    // Own sounds keep the stock handling: they only raise the NPC's own masking threshold.
    const IGameObject* root = who ? who->H_Root() : nullptr;
    if (root && root->ID() == hearer.ID())
        return true;

    const CEntityAlive* source = smart_cast<const CEntityAlive*>(root);
    if (source && source->human_being() && feature(Feature::noise) && bus_owned(sound_type, who))
    {
        power = 0.f;
        return true;
    }
    if (heard.kind == Noise::reload && smart_cast<const CMissile*>(who))
        heard.kind = Noise::grenade_draw;

    // A sound playing away from its source object (a ricochet, a bullet whine) tells where it plays, not who caused it.
    if (root && heard.kind != Noise::count &&
        root->Position().distance_to_sqr(position) > _sqr(config().detached_m))
    {
        heard.kind = Noise::impact;
    }

    power = receive(hearer, position, power, root ? root->ID() : u16(-1), threshold, &heard.occlusion);
    heard.silenced = heard.kind == Noise::shot && source && silenced(*source);
    return true;
}

bool on_heard(CAI_Stalker& hearer, const IGameObject* who, u32 sound_type, const Fvector& position, float power,
    Heard& heard)
{
    if (!hearing_state(hearer) || heard.kind == Noise::count)
        return false;

    const int community = hearer.da_ai()->community;
    Timer timer(community, true);
    const IGameObject* root = who ? who->H_Root() : nullptr;
    const CEntityAlive* source = smart_cast<const CEntityAlive*>(root);

    if (heard.kind == Noise::shot)
        stat_add(heard.silenced ? Stat::s_noise_shot_sil : Stat::s_noise_shot, 1.f, community);

    // The next round of a burst (or its impact) from about the same spot: the sound entry takes the estimate this
    // burst already gave; the danger and the belief it made stay as they are.
    SenseState& state = *hearing_state(hearer);
    const bool repeating = root && (heard.kind == Noise::shot || heard.kind == Noise::impact);
    if (repeating)
    {
        if (const SenseState::Repeat* repeat = repeat_of(hearer, state, root->ID(), heard.kind, position))
        {
            heard.estimate = repeat->estimate;
            heard.store = true;
            return true;
        }
    }

    const float err =
        heard_error(hearer, heard.kind, hearer.Position().distance_to(position), heard.occlusion, heard.silenced);
    stat_add(Stat::s_heard_err, err, community);

    heard.estimate = make_estimate(hearer, smart_cast<const CGameObject*>(root), position, err,
        source_of(heard.kind), clampr(1.f - err / 60.f, .1f, 1.f));
    heard.store = !!root;

    if (const u32 danger = danger_type(hearer, sound_type, source); danger != u32(CDangerObject::eDangerTypeDummy))
        Know::suspect(hearer, heard.estimate, danger, CDangerObject::eDangerPerceiveTypeSound);

    // A thrown grenade heard landing: a grenade danger at the heard spot and a warning, as for one seen (plan 2.6).
    if (const auto* grenade = smart_cast<const CGrenade*>(root); grenade && !Know::sees_now(hearer, grenade))
        Tactics::on_grenade_heard(hearer, *grenade, heard.estimate.pos);

    // An impact or an explosion is heard where it happens, not where the shooter or thrower stands.
    if (source && heard.kind != Noise::impact && heard.kind != Noise::explosion &&
        hearer.tfGetRelationType(source) != ALife::eRelationTypeFriend && !Know::sees_now(hearer, source))
    {
        Know::observe(hearer, source, heard.estimate);
    }
    if (repeating)
        remember_repeat(state, root->ID(), heard.kind, position, heard.estimate);
    return true;
}

float receive(CAI_Stalker& hearer, const Fvector& position, float power, u16 source, float threshold,
    float* occlusion)
{
    if (occlusion)
        *occlusion = 0.f;
    SenseState* state = hearing_state(hearer);
    if (!state)
        return power;
    if (power <= 0.f)
        return 0.f;

    const Config& c = config();
    power *= profile(hearer).hear_gain;
    // Rain is a noise floor: quiet sounds drown in it, gunfire does not. Falling snow is nearly silent
    // and fresh snow swallows sound, so it raises the floor only a little.
    const float liquid = clampr(da_precip().liquid, 0.f, 1.f);
    const float rain =
        c.rain_mask * GamePersistent().Environment().CurrentEnv.rain_density * (liquid + 0.15f * (1.f - liquid));
    if (power - rain < threshold)
        return _max(power - rain, 0.f);

    const float factor = occlusion_factor(hearer, *state, position, source);
    if (occlusion)
        *occlusion = 1.f - factor;
    if (factor < 1.f)
        stat_add(Stat::s_occluded, 1.f, hearer.da_ai()->community);
    return _max(power * factor - rain, 0.f);
}

namespace
{
// hear() with the occlusion of the ray, which the estimate needs.
float hear_through(CAI_Stalker& hearer, const Fvector& position, float power, u16 source, float& occlusion)
{
    const float threshold = hearer.memory().sound().masking_threshold();
    const float received = receive(hearer, position, power, source, threshold, &occlusion);
    return received >= threshold ? received : 0.f;
}
} // namespace

float hear(CAI_Stalker& hearer, const Fvector& position, float power, Noise kind, const CGameObject* who)
{
    if (!hearing_state(hearer))
        return power;

    Timer timer(hearer.da_ai()->community, false);
    float occlusion = 0.f;
    return hear_through(hearer, position, power, who ? who->ID() : u16(-1), occlusion);
}

void emit_noise(const Fvector& position, float loudness, Noise kind, const CGameObject* who)
{
    if (!bus_on() || loudness <= 0.f || kind >= Noise::count || !_valid(position))
        return;

    const IGameObject* root = who ? who->H_Root() : nullptr;
    const CEntityAlive* source = smart_cast<const CEntityAlive*>(root);
    Timer timer(community_of(root), true);
    const Config& c = config();
    const float radius = loudness * (kind == Noise::step || kind == Noise::landing ? c.step_range : c.noise_range);
    const float reach_sqr = _sqr(radius * c.cull_mult);

    SenseState::Stimulus stimulus;
    stimulus.position = position;
    stimulus.time = Device.dwTimeGlobal;
    stimulus.who = root ? root->ID() : u16(-1);
    stimulus.kind = kind;
    stimulus.cue = SenseState::Cue::noise;
    for (CAI_Stalker* hearer : online_npcs())
    {
        if (!hearer || (root && hearer->ID() == root->ID()) || !hearing_state(*hearer))
            continue;
        const float distance_sqr = hearer->Position().distance_to_sqr(position);
        if (distance_sqr > reach_sqr)
            continue;
        // A mate's footsteps and reloads are expected and tell nothing.
        if (source && hearer->tfGetRelationType(source) == ALife::eRelationTypeFriend)
            continue;
        // Inverse square: in silence the noise is heard exactly within its audible radius; masking by recent loud
        // sounds, walls, rain and a poor ear shrink that radius in hear().
        stimulus.power = c.edge_power * _sqr(radius) / _max(distance_sqr, 1.f);
        queue(*hearer, stimulus);
    }
}

bool process_noise(CAI_Stalker& npc, SenseState& state, const SenseState::Stimulus& stimulus, bool may_trace)
{
    if (!bus_on() || !hearing_state(npc))
        return true;

    const CGameObject* who = nullptr;
    if (stimulus.who != u16(-1))
    {
        who = smart_cast<const CGameObject*>(Level().Objects.net_Find(stimulus.who));
        if (!who)
            return true;
    }
    const CEntityAlive* source = smart_cast<const CEntityAlive*>(who);
    if (source && Know::sees_now(npc, source))
        return true;
    // The update's rays are spent: the noise waits for the next one.
    if (!may_trace && !occlusion_known(npc, state, stimulus.position, stimulus.who))
        return false;

    float occlusion = 0.f;
    if (hear_through(npc, stimulus.position, stimulus.power, stimulus.who, occlusion) <= 0.f)
        return true;

    const int community = npc.da_ai()->community;
    if (stimulus.kind == Noise::step || stimulus.kind == Noise::landing)
        stat_add(Stat::s_noise_step, 1.f, community);
    else if (stimulus.kind == Noise::reload || stimulus.kind == Noise::grenade_draw)
        stat_add(Stat::s_noise_reload, 1.f, community);

    const float err = heard_error(npc, stimulus.kind, npc.Position().distance_to(stimulus.position), occlusion, false);
    stat_add(Stat::s_heard_err, err, community);
    const Estimate estimate = make_estimate(npc, who, stimulus.position, err, source_of(stimulus.kind),
        clampr(1.f - err / 60.f, .1f, 1.f));

    // A noise nobody can be put to (a scripted one) is a possible enemy; a known source is judged like its sounds.
    if (!source || npc.is_relation_enemy(source))
        Know::suspect(npc, estimate, CDangerObject::eDangerTypeEnemySound, CDangerObject::eDangerPerceiveTypeSound);
    if (source && npc.tfGetRelationType(source) != ALife::eRelationTypeFriend)
        Know::observe(npc, source, estimate);
    return true;
}

void hearing_level_unload() { g_surface.clear(); }

bool noise_by_name(pcstr name, Noise& result)
{
    static constexpr std::pair<pcstr, Noise> names[]{{"step", Noise::step}, {"landing", Noise::landing},
        {"reload", Noise::reload}, {"grenade_draw", Noise::grenade_draw}, {"item", Noise::item},
        {"torch_click", Noise::torch_click}, {"impact", Noise::impact}, {"voice", Noise::voice},
        {"shot", Noise::shot}, {"explosion", Noise::explosion}};

    if (!name)
        return false;
    for (const auto& [text, noise] : names)
    {
        if (!xr_strcmp(text, name))
        {
            result = noise;
            return true;
        }
    }
    return false;
}
} // namespace DaAi::Sense

void da_ai_sense_on_reload(CWeapon& weapon)
{
    using namespace DaAi::Sense;
    if (!bus_on())
        return;
    if (const CEntityAlive* carrier = human(weapon.H_Parent()))
    {
        Fvector center;
        carrier->Center(center);
        emit_noise(center, config().reload, DaAi::Noise::reload, carrier);
    }
}

void da_ai_sense_on_step(IGameObject& walker, float power, SGameMtlPair* mtl_pair)
{
    using namespace DaAi::Sense;
    if (!bus_on() || power <= 0.f)
        return;
    const CEntityAlive* entity = human(&walker);
    if (!entity)
        return;

    // Gait from the movement state: the step power of the animation data is uneven (turning in place outweighs a
    // run), so it only tells that a foot landed.
    bool landing = false;
    float loudness = gait_loudness(*entity, landing);
    if (loudness <= 0.f)
        return;
    loudness *= surface_factor(entity->material().last_material_idx()) * load_factor(*entity);
    emit_noise(entity->Position(), loudness, landing ? DaAi::Noise::landing : DaAi::Noise::step, entity);
}
