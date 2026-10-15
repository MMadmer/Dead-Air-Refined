// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_tactics.h"
#include "da_ai_npc.h"
#include "da_ai_knowledge.h"
#include "da_ai_senses.h"
#include "da_ai_comms.h"
#include "da_ai_morale.h"
#include "da_ai_search.h"

#include "ai/stalker/ai_stalker.h"
#include "ai/stalker/ai_stalker_impl.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "WeaponMagazined.h"
#include "EntityCondition.h"
#include "Hit.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_location_manager.h"
#include "danger_cover_location.h"
#include "danger_object_location.h"
#include "member_order.h"
#include "cover_point.h"
#include "stalker_planner.h"
#include "stalker_decision_space.h"
#include "Level.h"
#include "Explosive.h"
#include "ai_space.h"
#include "ai/stalker/ai_stalker_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/ai_object_location.h"

namespace DaAi::Tactics
{
namespace
{
constexpr pcstr config_section = "ai_tactics";

Config g_config;

float read(pcstr key, float fallback) { return READ_IF_EXISTS(pSettings, r_float, config_section, key, fallback); }
u32 read(pcstr key, u32 fallback) { return READ_IF_EXISTS(pSettings, r_u32, config_section, key, fallback); }

Range read(pcstr key, const Range& fallback)
{
    if (!pSettings->line_exist(config_section, key))
        return fallback;
    const pcstr value = pSettings->r_string(config_section, key);
    const int count = value ? _GetItemCount(value) : 0;
    if (!count)
        return fallback;
    string64 item;
    Range result;
    result.lo = float(atof(_GetItem(value, 0, item)));
    result.hi = count > 1 ? float(atof(_GetItem(value, 1, item))) : result.lo;
    return result;
}

u32 read_classes(pcstr key, u32 fallback)
{
    if (!pSettings->line_exist(config_section, key))
        return fallback;
    const pcstr value = pSettings->r_string(config_section, key);
    u32 result = 0;
    string64 item;
    for (int index = 0, count = value ? _GetItemCount(value) : 0; index < count; ++index)
    {
        _GetItem(value, index, item);
        for (u32 which = 0; which < u32(WeaponClass::count); ++which)
        {
            if (!xr_strcmp(item, weapon_class_name(WeaponClass(which))))
                result |= 1u << which;
        }
    }
    return result;
}

void load_config()
{
    Config& c = g_config;
    c = {};
    c.supp_classes = (1u << u32(WeaponClass::smg)) | (1u << u32(WeaponClass::rifle)) | (1u << u32(WeaponClass::mg));
    c.flank_classes = (1u << u32(WeaponClass::pistol)) | (1u << u32(WeaponClass::smg)) |
        (1u << u32(WeaponClass::shotgun)) | (1u << u32(WeaponClass::rifle_old)) | (1u << u32(WeaponClass::rifle));

    c.approach_min_gain_m = read("approach_min_gain_m", c.approach_min_gain_m);
    c.approach_aggr_min = read("approach_aggr_min", c.approach_aggr_min);
    c.approach_radius_m = read("approach_radius_m", c.approach_radius_m);
    c.approach_search_ms = read("approach_search_ms", c.approach_search_ms);
    c.approach_moved_m = read("approach_moved_m", c.approach_moved_m);
    c.approach_threat_moved_m = read("approach_threat_moved_m", c.approach_threat_moved_m);
    c.approach_cover_max = read("approach_cover_max", c.approach_cover_max);
    c.approach_max_ms = read("approach_max_ms", c.approach_max_ms);
    c.fire_on_order_max_s = read("fire_on_order_max_s", c.fire_on_order_max_s);
    c.reload_window_s = read("reload_window_s", c.reload_window_s);

    c.react_blink_ms = read("react_blink_ms", c.react_blink_ms);
    c.react_reacquire_s = read("react_reacquire_s", c.react_reacquire_s);
    c.react_reacquire_mult = read("react_reacquire_mult", c.react_reacquire_mult);
    c.react_dist_ms = read("react_dist_ms", c.react_dist_ms);
    c.react_peripheral_deg = read("react_peripheral_deg", c.react_peripheral_deg);
    c.react_peripheral_ms = read("react_peripheral_ms", c.react_peripheral_ms);
    c.react_fear_mult = read("react_fear_mult", c.react_fear_mult);
    c.react_fatigue_mult = read("react_fatigue_mult", c.react_fatigue_mult);
    c.alert_memory_s = read("alert_memory_s", c.alert_memory_s);
    c.settle_start_mult = read("settle_start_mult", c.settle_start_mult);
    c.settle_fear_mult = read("settle_fear_mult", c.settle_fear_mult);
    c.settle_fatigue_mult = read("settle_fatigue_mult", c.settle_fatigue_mult);
    c.acc_suppression_mult = read("acc_suppression_mult", c.acc_suppression_mult);
    c.acc_wound_mult = read("acc_wound_mult", c.acc_wound_mult);
    c.acc_fatigue_mult = read("acc_fatigue_mult", c.acc_fatigue_mult);
    c.acc_lateral_mult = read("acc_lateral_mult", c.acc_lateral_mult);
    c.acc_lateral_ref = _max(read("acc_lateral_ref", c.acc_lateral_ref), EPS_L);

    c.supp_classes = read_classes("supp_classes", c.supp_classes);
    c.supp_floor_m = read("supp_floor_m", c.supp_floor_m);
    c.supp_slope = read("supp_slope", c.supp_slope);
    c.supp_order_window_s = read("supp_order_window_s", c.supp_order_window_s);
    c.supp_precise_m = read("supp_precise_m", c.supp_precise_m);
    c.supp_near_m = read("supp_near_m", c.supp_near_m);

    c.cover_open_value = read("cover_open_value", c.cover_open_value);
    c.cover_min_enemy_m = read("cover_min_enemy_m", c.cover_min_enemy_m);
    c.lof_low_max = read("lof_low_max", c.lof_low_max);
    c.short_range_engage_max = read("short_range_engage_max", c.short_range_engage_max);
    c.exposure_open_limit_m = read("exposure_open_limit_m", c.exposure_open_limit_m);
    c.exposure_gap_m = read("exposure_gap_m", c.exposure_gap_m);
    c.open_fight_budget_m = read("open_fight_budget_m", c.open_fight_budget_m);
    c.step_hold_ms = read("step_hold_ms", c.step_hold_ms);
    c.exposure_unknown_share = read("exposure_unknown_share", c.exposure_unknown_share);
    c.score_line_of_fire = read("score_line_of_fire", c.score_line_of_fire);
    c.score_band = read("score_band", c.score_band);
    c.score_aggr = read("score_aggr", c.score_aggr);
    c.score_aggr_range_m = _max(read("score_aggr_range_m", c.score_aggr_range_m), 1.f);
    c.score_travel = read("score_travel", c.score_travel);
    c.score_exposure = read("score_exposure", c.score_exposure);
    c.at_cover_m = read("at_cover_m", c.at_cover_m);
    c.no_cover_memory_ms = read("no_cover_memory_ms", c.no_cover_memory_ms);
    c.hit_in_cover_ms = read("hit_in_cover_ms", c.hit_in_cover_ms);
    c.pressure_ms = read("pressure_ms", c.pressure_ms);
    c.worried_supp_min = read("worried_supp_min", c.worried_supp_min);
    c.reload_near_m = read("reload_near_m", c.reload_near_m);
    c.fallback_cooldown_ms = read("fallback_cooldown_ms", c.fallback_cooldown_ms);
    c.fallback_max_ms = read("fallback_max_ms", c.fallback_max_ms);
    c.return_fire_ms = read("return_fire_ms", c.return_fire_ms);
    c.return_fire_cooldown_ms = read("return_fire_cooldown_ms", c.return_fire_cooldown_ms);
    c.fallback_radius_m = read("fallback_radius_m", c.fallback_radius_m);
    c.fallback_min_gain_m = read("fallback_min_gain_m", c.fallback_min_gain_m);
    c.fallback_min_move_m = read("fallback_min_move_m", c.fallback_min_move_m);
    c.peek_hold_ms = read("peek_hold_ms", c.peek_hold_ms);
    c.peek_avoid_min = read("peek_avoid_min", c.peek_avoid_min);
    c.peek_reuse_m = read("peek_reuse_m", c.peek_reuse_m);
    c.danger_cover_ms = read("danger_cover_ms", c.danger_cover_ms);
    c.danger_cover_m = read("danger_cover_m", c.danger_cover_m);

    c.role_think_ms = read("role_think_ms", c.role_think_ms);
    c.role_hold_s = read("role_hold_s", c.role_hold_s);
    c.role_renew_s = read("role_renew_s", c.role_renew_s);
    c.flank_role_min = read("flank_role_min", c.flank_role_min);
    c.flank_short_bonus = read("flank_short_bonus", c.flank_short_bonus);
    c.flank_exo_mult = read("flank_exo_mult", c.flank_exo_mult);
    c.flank_repeat_s = read("flank_repeat_s", c.flank_repeat_s);
    c.flank_belief_s = read("flank_belief_s", c.flank_belief_s);
    c.flank_min_enemy_m = read("flank_min_enemy_m", c.flank_min_enemy_m);
    c.flank_radius_m = read("flank_radius_m", c.flank_radius_m);
    c.flank_open_limit_m = read("flank_open_limit_m", c.flank_open_limit_m);
    c.flank_via_radius_m = read("flank_via_radius_m", c.flank_via_radius_m);
    constexpr u32 candidates_max = TacticsState::FlankSearch::candidates_max;
    c.flank_checks = _min(read("flank_checks", c.flank_checks), candidates_max);
    c.flank_vias = _min(read("flank_vias", c.flank_vias), u32(TacticsState::FlankSearch::via_max));
    c.flank_agent_checks = std::clamp(read("flank_agent_checks", c.flank_agent_checks), 1u, candidates_max);
    c.flank_agent_per_update = _max(read("flank_agent_per_update", c.flank_agent_per_update), 1u);
    c.flank_walks_per_update = _max(read("flank_walks_per_update", c.flank_walks_per_update), 1u);
    c.flank_step_budget_us = _max(read("flank_step_budget_us", c.flank_step_budget_us), 1.f);
    c.flank_tile_m = _max(read("flank_tile_m", c.flank_tile_m), 1.f);
    c.flank_candidates =
        std::clamp(read("flank_candidates", c.flank_candidates), 1u, u32(TacticsState::flank_tried_max));
    c.flank_retry_s = read("flank_retry_s", c.flank_retry_s);
    c.flank_route_s = read("flank_route_s", c.flank_route_s);
    c.flank_grace_ms = read("flank_grace_ms", c.flank_grace_ms);
    c.flank_max_ms = read("flank_max_ms", c.flank_max_ms);
    c.flank_classes = read_classes("flank_classes", c.flank_classes);
    c.flank_angle_deg = read("flank_angle_deg", c.flank_angle_deg);
    c.supp_role_min = read("supp_role_min", c.supp_role_min);
    c.supp_mg_bonus = read("supp_mg_bonus", c.supp_mg_bonus);
    c.supp_order_s = read("supp_order_s", c.supp_order_s);
    c.cover_fire_msg_s = read("cover_fire_msg_s", c.cover_fire_msg_s);
    c.mate_scan_ms = read("mate_scan_ms", c.mate_scan_ms);
    c.mate_moving_speed = read("mate_moving_speed", c.mate_moving_speed);
    c.suppress_burst_ms = read("suppress_burst_ms", c.suppress_burst_ms);
    c.suppress_pause_ms = read("suppress_pause_ms", c.suppress_pause_ms);
    c.suppress_spread_m = read("suppress_spread_m", c.suppress_spread_m);
    c.order_range_mult = read("order_range_mult", c.order_range_mult);
    c.order_supp_min = read("order_supp_min", c.order_supp_min);
    c.bound_min_m = read("bound_min_m", c.bound_min_m);
    c.reload_push_aggr = read("reload_push_aggr", c.reload_push_aggr);

    c.gren_think_ms = read("gren_think_ms", c.gren_think_ms);
    c.grenadier_mult = read("grenadier_mult", c.grenadier_mult);
    c.flush_static_s = read("flush_static_s", c.flush_static_s);
    c.flush_move_m = read("flush_move_m", c.flush_move_m);
    c.flush_belief_extra_s = read("flush_belief_extra_s", c.flush_belief_extra_s);
    c.flush_cover_max = read("flush_cover_max", c.flush_cover_max);
    c.indoor_cover_max = read("indoor_cover_max", c.indoor_cover_max);
    c.grenade_min_m = read("grenade_min_m", c.grenade_min_m);
    c.grenade_max_m = read("grenade_max_m", c.grenade_max_m);
    c.grenade_warn_m = read("grenade_warn_m", c.grenade_warn_m);
    c.grenade_danger_m = read("grenade_danger_m", c.grenade_danger_m);
    c.grenade_danger_ms = read("grenade_danger_ms", c.grenade_danger_ms);

    c.stat_max_dt_ms = read("stat_max_dt_ms", c.stat_max_dt_ms);
    c.stat_teleport_m = read("stat_teleport_m", c.stat_teleport_m);
    c.loaded = true;
}

int community_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state ? state->community : -1;
}

u32 ms(float seconds) { return seconds > 0.f ? u32(iFloor(seconds * 1000.f)) : 0; }
u32 roll_ms(const Range& range)
{
    const float value = range.hi > range.lo ? ::Random.randF(range.lo, range.hi) : range.lo;
    return value > 0.f ? u32(iFloor(value)) : 0;
}

// Orders carry their addressee and a side in Message::extra: bit 0 = side, bits 1.. = addressee id + 1 (0 = nobody,
// an announcement such as "flanking!" or "cover me!").
u32 order_extra(u16 addressee, u8 side) { return ((u32(addressee) + 1) << 1) | (side & 1u); }
u16 order_addressee(u32 extra)
{
    const u32 id = extra >> 1;
    return id && id <= 0xffffu ? u16(id - 1) : u16(-1);
}

// A window is only ever extended: a later, shorter message must not cut an order's longer one.
void extend(u32& until, u32 to) { until = _max(until, to); }

bool role_active(const TacticsState& state)
{
    return state.role != Role::opportunist && Device.dwTimeGlobal < state.role_until;
}

void clear_flank(TacticsState& state)
{
    state.flank_target = nullptr;
    state.flank_via = nullptr;
    state.flank_searched = false;
    state.flank_running = false;
    state.flank_start = 0;
    state.flank_leg = {};
}

void assign_role(TacticsState& state, Role role, u8 side)
{
    if (state.role != role || !role_active(state))
        state.flank_counted = false;
    // A renewed order of a flank under way keeps its route.
    const bool renew = role == Role::flanker && state.role == role && role_active(state) && state.flank_running;
    state.role = role;
    extend(state.role_until, Device.dwTimeGlobal + ms(config().role_hold_s));
    if (renew)
        return;
    state.role_time = Device.dwTimeGlobal;
    state.flank_side = side;
    clear_flank(state);
    // An ordered man covers or goes around: a bound of his own under way ends.
    state.approach_active = false;
    state.approach_time = 0;
    state.doctrine_frame = u32(-1);
}

void drop_role(TacticsState& state)
{
    state.role = Role::opportunist;
    state.role_until = 0;
    clear_flank(state);
    state.doctrine_frame = u32(-1);
}

// The ordered flanker holds a covered route: the band does not stop him on the way (he fires only inside it).
bool flank_route(const TacticsState& state)
{
    return feature(Feature::roles) && role_active(state) && state.role == Role::flanker && state.flank_target;
}

bool holding(const TacticsState& state) { return feature(Feature::roles) && state.order == u8(Order::holding); }

u32 order_flags(const TacticsState& state) { return holding(state) ? u32(Comms::quiet) : 0u; }

bool is_leader_of_mind(const CAI_Stalker& npc) { return Morale::believed_leader(npc) == npc.ID(); }

// The firearm the NPC fights with: the one in its hands (none for a knife), else the one it last chose.
const CWeaponMagazined* combat_weapon(const CAI_Stalker& npc)
{
    return smart_cast<const CWeaponMagazined*>(held_weapon(npc));
}

bool reloading(const CAI_Stalker& npc)
{
    const CWeaponMagazined* weapon = combat_weapon(npc);
    return weapon && (weapon->GetState() == CWeapon::eReload || !weapon->GetAmmoElapsed());
}

// The cover the NPC holds right now (its agent member cover, within at_cover_m), else null.
const CCoverPoint* held_cover(CAI_Stalker& npc)
{
    const CMemberOrder* order = npc.agent_manager().member().get_member(npc.ID());
    const CCoverPoint* cover = order ? order->cover() : nullptr;
    return cover && cover->position().distance_to_sqr(npc.Position()) < _sqr(config().at_cover_m) ? cover : nullptr;
}

bool at_cover(CAI_Stalker& npc) { return !!held_cover(npc); }

// Remembers how well the held cover covered from the belief when the NPC reached it: a cover that was good and is
// open now has been outflanked.
void track_held_cover(CAI_Stalker& npc, TacticsState& state, const CEntityAlive* enemy)
{
    const CCoverPoint* cover = enemy ? held_cover(npc) : nullptr;
    if (cover == state.held_cover)
        return;
    state.held_cover = cover;
    if (cover)
        state.held_value = cover_toward(cover->level_vertex_id(), cover->position(), Know::known_position(npc, *enemy));
}

// Reaction time of an identification (F6): the profile's range, idle add-on, distance and peripheral add-ons, fear
// and fatigue; a re-acquisition of a target seen moments ago takes a fraction of it.
float reaction_ms(
    const CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy, bool reacquire, float& base)
{
    const Config& c = config();
    const Profile& p = profile(npc);
    const u32 now = Device.dwTimeGlobal;
    base = ::Random.randF(p.react_ms.lo, _max(p.react_ms.lo, p.react_ms.hi));
    float result = base;
    if (!state.alert_time || now > state.alert_time + ms(c.alert_memory_s))
        result += p.react_idle_ms;

    const Fvector position = Know::known_position(npc, enemy);
    result += c.react_dist_ms * npc.Position().distance_to(position);

    Fvector direction = Fvector().sub(position, npc.Position());
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    if (angle_difference(-yaw, npc.movement().head_orientation().current.yaw) > deg2rad(c.react_peripheral_deg))
        result += c.react_peripheral_ms;

    result *= 1.f + c.react_fear_mult * Morale::fear(npc);
    result *= 1.f + c.react_fatigue_mult * (1.f - Morale::stamina(npc));
    if (reacquire)
        result *= c.react_reacquire_mult;
    return result;
}

// Own sight of the selected enemy: identification, re-acquisition, the own last-seen point and velocity. The real
// position is read only while the target is in the NPC's own vision (F3).
void track_sight(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy, bool sees)
{
    const u32 now = Device.dwTimeGlobal;
    if (state.target != enemy.ID())
    {
        // Switching targets: a target seen moments ago is re-acquired, not identified anew.
        auto& recent = state.recent_seen;
        if (state.target != u16(-1) && state.t_last_seen)
        {
            const auto slot = std::min_element(recent.begin(), recent.end(),
                [](const TacticsState::Seen& a, const TacticsState::Seen& b) { return a.time < b.time; });
            *slot = {state.target, state.t_last_seen, state.last_seen_pos};
        }
        const auto known = std::find_if(recent.begin(), recent.end(),
            [&enemy](const TacticsState::Seen& entry) { return entry.id == enemy.ID(); });

        state.target = enemy.ID();
        state.seeing = false;
        state.react_counted = false;
        state.react_held = false;
        state.t_identified = 0;
        state.t_acquired = 0;
        state.t_fire_ready = 0;
        state.t_last_seen = known != recent.end() ? known->time : 0;
        if (known != recent.end())
            state.last_seen_pos = known->position;
        state.t_sample = 0;
        state.last_seen_vel.set(0.f, 0.f, 0.f);
    }

    if (!sees)
    {
        if (state.seeing)
        {
            state.seeing = false;
            state.suppress_roll = ::Random.randF() < profile(npc).supp;
        }
        return;
    }

    const Fvector position = Know::known_position(npc, enemy);
    // A blink (the target passing a post, a vision tick missed) keeps the acquisition.
    if (!state.seeing && state.t_last_seen && now <= state.t_last_seen + config().react_blink_ms)
        state.seeing = true;

    if (!state.seeing)
    {
        const bool reacquire = state.t_last_seen && now < state.t_last_seen + ms(config().react_reacquire_s);
        float base;
        const u32 due = u32(iFloor(reaction_ms(npc, state, enemy, reacquire, base)));
        state.t_fire_ready = now + due;
        state.t_acquired = now;
        if (!reacquire)
        {
            state.t_identified = now;
            state.react_counted = false;
            state.react_held = false;
            state.react_due_ms = due;
            state.react_base_ms = u32(iFloor(base));
        }
        state.seeing = true;
        state.t_sample = now;
        state.last_seen_pos = position;
        state.last_seen_vel.set(0.f, 0.f, 0.f);
    }
    else if (now >= state.t_sample + 100)
    {
        const float dt = float(now - state.t_sample) / 1000.f;
        Fvector velocity = Fvector().sub(position, state.last_seen_pos).div(dt);
        state.last_seen_vel.lerp(state.last_seen_vel, velocity, .5f);
        state.last_seen_pos = position;
        state.t_sample = now;
    }
    state.t_last_seen = now;
}

// The suppressor or rearguard role covers a mate it knows to be exposed.
bool covering(const TacticsState& state)
{
    return feature(Feature::roles) && role_active(state) &&
        (state.role == Role::suppressor || state.role == Role::rearguard) &&
        Device.dwTimeGlobal < state.cover_fire_until;
}

// Storey test of a line of fire, centre to centre (know's positions carry the centre height): a height difference
// that grows with the distance is terrain, a steep one close by is another floor.
bool same_floor(const CAI_Stalker& npc, const Fvector& point)
{
    const Config& c = config();
    Fvector centre;
    npc.Center(centre);
    return _abs(centre.y - point.y) <= _max(c.supp_floor_m, npc.Position().distance_to_xz(point) * c.supp_slope);
}

// F5: bursts at the own last-seen point only, within the profile's window (an ordered cover keeps it open for
// supp_order_window_s), with an automatic weapon or rifle.
bool suppress_target_ok(const CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy)
{
    if (!feature(Feature::suppression))
        return false;
    const Profile& p = profile(npc);
    if (p.supp_window_s <= 0.f || state.target != enemy.ID() || state.seeing || !state.t_last_seen)
        return false;
    const Config& c = config();
    const float window = covering(state) ? _max(p.supp_window_s, c.supp_order_window_s) : p.supp_window_s;
    if (Device.dwTimeGlobal > state.t_last_seen + ms(window))
        return false;

    const CWeaponMagazined* weapon = combat_weapon(npc);
    if (!weapon || !(c.supp_classes & (1u << u32(weapon_class(*weapon)))))
        return false;
    if (!same_floor(npc, state.last_seen_pos))
        return false;
    return npc.Position().distance_to(state.last_seen_pos) <= band(npc).engage_max;
}

// Unordered suppression is rolled per sight loss against the profile's supp; a covering role always suppresses.
bool suppression_allowed(const CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy)
{
    return (state.suppress_roll || covering(state)) && suppress_target_ok(npc, state, enemy);
}

// Evidence that the enemy knows of the NPC: a hit, a near miss, a mate reported being fired on, or the enemy seen
// looking its way.
bool detected(CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if ((state.hit_time && now < state.hit_time + c.pressure_ms) ||
        (state.fired_on_time && now < state.fired_on_time + c.pressure_ms))
        return true;
    if (Sense::suppression(npc) >= c.order_supp_min)
        return true;
    return Know::sees_now(npc, &enemy) && Know::enemy_sees_me(npc, enemy);
}

void release_fire(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy)
{
    state.order = u8(Order::released);
    // The leader's order goes by his channels; a squad whose leader is out of reach opens up raggedly.
    if (is_leader_of_mind(npc))
        Comms::request(npc, Comms::Kind::attack, enemy.ID());
}

// Fire on order (T4): an undetected fire_on_order squad holds until its leader's attack order, being fired on or
// found out, or [ai_tactics] fire_on_order_max_s. The leader opens once the enemy is inside his band.
void update_order(CAI_Stalker& npc, TacticsState& state, const CEntityAlive* enemy)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (!enemy)
    {
        if (state.order != u8(Order::none) && now > state.engage_time + ms(c.alert_memory_s))
            state.order = u8(Order::none);
        return;
    }
    state.engage_time = now;
    if (state.order == u8(Order::released))
        return;
    if (!feature(Feature::roles) || !profile(npc).fire_on_order || !enemy->human_being() || !engine_combat(npc))
    {
        if (state.order == u8(Order::holding))
            state.order = u8(Order::released);
        return;
    }

    const bool found_out = detected(npc, state, *enemy);
    if (state.order == u8(Order::none))
    {
        state.order_start = now;
        if (found_out)
            release_fire(npc, state, *enemy);
        else
            state.order = u8(Order::holding);
        return;
    }

    bool open = found_out || now >= state.order_start + ms(c.fire_on_order_max_s);
    if (!open && is_leader_of_mind(npc) && Know::sees_now(npc, enemy))
    {
        const WeaponBand weapon_band = band(npc);
        open = weapon_band.effective > 0.f &&
            Know::known_distance(npc, *enemy) <= weapon_band.effective * c.order_range_mult;
    }
    if (open)
        release_fire(npc, state, *enemy);
}

// The mates the NPC believes alive: morale's list, else its online squad. Callback(NpcState&).
template <typename Callback>
void for_each_believed_mate(const CAI_Stalker& npc, Callback&& callback)
{
    const NpcState* self = npc.da_ai();
    const auto visit = [&](u16 id) {
        NpcState* other = DaAi::npc(id);
        if (id != npc.ID() && other && other->owner && other->tactics)
            callback(*other);
    };
    if (feature(Feature::morale) && self && self->morale)
    {
        for (const MoraleState::Mate& mate : self->morale->mates)
            visit(mate.id);
        return;
    }
    if (const SquadState* squad = squad_of(npc))
    {
        for (const u16 id : squad->members)
            visit(id);
    }
}

// The enemy fights from a spot (the NPC's own memory of him: position and valid vertex) that it believes he has held
// for flush_static_s and that covers him from the NPC: a grenade flush target, and a position to go around even while
// his head shows over it.
bool dug_in(const CAI_Stalker& npc, const TacticsState& state, u16 enemy, u32 vertex, const Fvector& position)
{
    const Config& c = config();
    return state.static_target == enemy && Device.dwTimeGlobal >= state.static_since + ms(c.flush_static_s) &&
        cover_toward(vertex, position, npc.Position()) <= c.flush_cover_max;
}

bool dug_in(CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy)
{
    const MemorySpace::CMemoryInfo memory = npc.memory().memory(&enemy);
    const u32 vertex = memory.m_object_params.m_level_vertex_id;
    return memory.m_object && ai().level_graph().valid_vertex_id(vertex) &&
        dug_in(npc, state, enemy.ID(), vertex, memory.m_object_params.m_position);
}

// The open metres a flanker's route may cross: flank_open_limit_m; a short gun its gap, the open limit only while a
// mate suppresses.
float flank_limit(const CAI_Stalker& flanker, bool suppressed)
{
    const Config& c = config();
    if (!short_range(band(flanker)))
        return c.flank_open_limit_m;
    return _min(c.flank_open_limit_m, suppressed ? c.exposure_open_limit_m : c.exposure_gap_m);
}

struct FlankRoute
{
    const CCoverPoint* end{};
    const CCoverPoint* via{};
    float open{};
    u8 side{};
};

// One step of the leader's search for his flanker's covered route, looked for on the ground around the man from the
// leader's own belief: the side the man stands on, the other side, then two legs through a cover on either side.
// A leg's covers are found by geometry tile by tile; the agent's checks and the walks over the open ground to them run
// after it, a few per update, and every step stops once it has taken [ai_tactics] flank_step_budget_us (P10: a leg in
// one update was the tactics part's worst update, ~0.5 ms; one gather at night 0.66 ms). True once decided (route.end
// null: none).
bool flank_search_step(TacticsState::FlankSearch& search, CAI_Stalker& flanker, FlankRoute& route)
{
    const u32 walks = config().flank_walks_per_update;
    const u32 checks = config().flank_agent_per_update;
    StepBudget budget(config().flank_step_budget_us);
    const WeaponBand weapon_band = band(flanker);
    const bool first_side = search.stage == 1 || search.stage == 3;
    const u8 side = first_side ? search.side : u8(search.side ^ 1u);
    TacticsState::FlankSearch::Leg& leg = search.leg;
    if (search.stage <= 2)
    {
        if (!leg.gathered)
        {
            if (!gather_flank_leg(flanker, search.threat, side, weapon_band, nullptr, leg, budget) || leg.count)
                return false;
        }
        else if (!judge_flank_leg(flanker, search.threat, search.limit, nullptr, leg, walks, checks, budget))
            return false;
        const CCoverPoint* end = leg.best;
        const float open = leg.best_open;
        leg = {};
        if (end)
        {
            route = {end, nullptr, open, side};
            return true;
        }
        ++search.stage;
        search.via_count = u8(-1);
        return false;
    }

    // Two legs: the first leg's covers, then a second leg from each of them.
    if (search.via_count == u8(-1))
    {
        if (!leg.gathered)
        {
            if (!gather_flank_vias(flanker, search.threat, side, weapon_band, leg, budget) || leg.count)
                return false;
        }
        else if (!judge_flank_vias(flanker, search.threat, search.limit, leg, walks, checks, budget, search.via,
                     search.via_open, u8(config().flank_vias)))
        {
            return false;
        }
        search.via_count = leg.found;
        search.next = 0;
        leg = {};
        if (search.via_count)
            return false;
    }
    else if (search.next < search.via_count)
    {
        const u8 index = search.next;
        const CCoverPoint* from = search.via[index];
        const float left = search.limit - search.via_open[index];
        if (!leg.gathered)
        {
            if (!gather_flank_leg(flanker, search.threat, side, weapon_band, from, leg, budget) || leg.count)
                return false;
        }
        else if (!judge_flank_leg(flanker, search.threat, left, from, leg, walks, checks, budget))
            return false;
        const CCoverPoint* end = leg.best;
        const float open = leg.best_open;
        leg = {};
        ++search.next;
        // The via cover itself is no end of the route.
        if (end && end != from)
        {
            route = {end, from, search.via_open[index] + open, side};
            return true;
        }
        if (search.next < search.via_count)
            return false;
    }
    if (search.stage >= 4)
    {
        route = {};
        return true;
    }
    ++search.stage;
    search.via_count = u8(-1);
    return false;
}

// No covered flank route: the leader cracks the position another way and says which. A dug-in enemy within his throw
// gets his own grenade (the next throw needs no roll); with a suppressor covering and men who bound, the suppression
// order goes out again so they advance under it; otherwise the squad holds.
void flank_alternative(CAI_Stalker& npc, TacticsState& state, TacticsSquad& plan, const CEntityAlive& enemy,
    const Fvector& threat, const NpcState* suppressor, u32 flags)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const int community = community_of(npc);
    const float distance = npc.Position().distance_to(threat);
    if (feature(Feature::grenades) && profile(npc).gren > 0.f && npc.inventory().ItemFromSlot(GRENADE_SLOT) &&
        distance >= c.grenade_min_m && distance <= c.grenade_max_m && dug_in(npc, state, enemy))
    {
        state.flush_order_until = now + ms(c.flush_static_s + c.flush_belief_extra_s);
        stat_add(Stat::t_alt_grenade, 1.f, community);
        return;
    }

    bool bounders = profile(npc).bounding;
    for_each_believed_mate(npc, [&](NpcState& mate) {
        bounders = bounders || (&mate != suppressor && profile(*mate.owner).bounding);
    });
    if (suppressor && bounders)
    {
        // Renewed now unless it just went out in this decision.
        if (plan.suppress_time != now &&
            Comms::request(npc, Comms::Kind::cover_me, enemy.ID(), order_extra(suppressor->id, 0), flags))
        {
            plan.suppressor = suppressor->id;
            plan.suppress_time = now;
            extend(state.suppressor_until, now + ms(c.supp_order_s));
        }
        stat_add(Stat::t_alt_advance, 1.f, community);
        return;
    }
    stat_add(Stat::t_alt_hold, 1.f, community);
}

// The leader's plan (T4): a suppressor with an automatic weapon, then a flanker with the best flank, sent as orders;
// a mate takes a role only when the order reaches him. Untrained profiles are never picked.
void lead(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    TacticsState::FlankSearch& search = state.flank_search;
    // A route search under way, or the next man's after one that found none, goes on at every update.
    if (!search.stage && !state.flank_tried_count && now < state.lead_time + c.role_think_ms)
        return;
    state.lead_time = now;
    const auto stop_search = [&state, &search] {
        search.stage = 0;
        state.flank_tried_count = 0;
    };
    SquadState* squad = squad_of(npc);
    if (!squad || !squad->tactics || !is_leader_of_mind(npc))
    {
        stop_search();
        return;
    }

    TacticsSquad& plan = *squad->tactics;
    if (plan.planner != npc.ID())
    {
        plan = {};
        plan.planner = npc.ID();
    }

    // Orders about a ghost help nobody: the leader acts on a fresh belief of his own or of his men's reports.
    const bool sees = Know::sees_now(npc, &enemy);
    const Belief* belief = Know::belief(npc, enemy.ID());
    if (!sees && (!belief || now > belief->e.t_observed + ms(c.flank_belief_s)))
    {
        stop_search();
        return;
    }
    const Fvector threat = Know::known_position(npc, enemy);

    // The suppressor: an automatic weapon and the will to use it; the man already covering keeps the job. A man in the
    // fight goes first (his leader hears him fire): an ordered cover fires only at its own last sight, which a man who
    // has not fired for supp_order_window_s may never have had. The man ordered keeps the job until the order is due
    // again (role_renew_s), so the flank search is not restarted each time somebody fires.
    NpcState* suppressor = nullptr;
    bool suppressor_engaged = false;
    float best_supp = c.supp_role_min;
    for_each_believed_mate(npc, [&](NpcState& mate) {
        const Profile& p = profile(*mate.owner);
        const CWeaponMagazined* weapon = combat_weapon(*mate.owner);
        const WeaponClass mate_class = weapon ? weapon_class(*weapon) : WeaponClass::none;
        if (!weapon || !p.has_role(Role::suppressor) || !(c.supp_classes & (1u << u32(mate_class))))
            return;
        const bool current = mate.id == plan.suppressor;
        const u32 shot = mate.tactics->t_shot;
        const bool engaged = (current && now < plan.suppress_time + ms(c.role_renew_s)) ||
            (shot && now < shot + ms(c.supp_order_window_s));
        const float score = p.supp + (mate_class == WeaponClass::mg ? c.supp_mg_bonus : 0.f) + (current ? 1.f : 0.f);
        if (score < c.supp_role_min || (suppressor_engaged && !engaged))
            return;
        if (!suppressor || (engaged && !suppressor_engaged) || score >= best_supp)
        {
            best_supp = score;
            suppressor = &mate;
            suppressor_engaged = engaged;
        }
    });

    // The flanker: the best flank of the rest, a weapon of flank_classes, fresh legs, no exo suit. A man the enemy is
    // within reach of goes first (the leader's own picture: where his man stands, where he believes the enemy); one
    // beyond his band is sent only when nobody is, and goes only by a covered route. A man whose route the leader
    // already looked for in vain in this decision is passed over.
    const u16* tried = state.flank_tried;
    const u16* tried_end = tried + state.flank_tried_count;
    NpcState* flanker = nullptr;
    bool flanker_in_band = false;
    float best_flank = c.flank_role_min;
    for_each_believed_mate(npc, [&](NpcState& mate) {
        const CAI_Stalker& man = *mate.owner;
        const Profile& p = profile(man);
        const CWeaponMagazined* weapon = combat_weapon(man);
        if (&mate == suppressor || !weapon || !p.has_role(Role::flanker) || Morale::tired(man) ||
            !(c.flank_classes & (1u << u32(weapon_class(*weapon)))) ||
            std::find(tried, tried_end, mate.id) != tried_end)
        {
            return;
        }
        const WeaponBand mate_band = DaAi::weapon_band(*weapon);
        const bool in_band =
            mate_band.engage_max <= 0.f || man.Position().distance_to(threat) <= mate_band.engage_max;
        // A short gun makes the better flanker once the enemy is within its reach, not before.
        float score = p.flank +
            (in_band && mate_band.engage_max <= c.short_range_engage_max ? c.flank_short_bonus : 0.f);
        if (armour_class(man) == ArmourClass::exo)
            score *= c.flank_exo_mult;
        if (score < c.flank_role_min || (flanker && flanker_in_band && !in_band))
            return;
        if (!flanker || (in_band && !flanker_in_band) || score >= best_flank)
        {
            best_flank = score;
            flanker = &mate;
            flanker_in_band = in_band;
        }
    });

    const u32 flags = order_flags(state);
    if (suppressor && (plan.suppressor != suppressor->id || now >= plan.suppress_time + ms(c.role_renew_s)) &&
        Comms::request(npc, Comms::Kind::cover_me, enemy.ID(), order_extra(suppressor->id, 0), flags))
    {
        plan.suppressor = suppressor->id;
        plan.suppress_time = now;
        extend(state.suppressor_until, now + ms(c.supp_order_s));
    }

    // Flank an enemy that holds a position out of sight or dug in behind cover, never one standing in the open or
    // already close, and only by a covered route: the leader looks at the ground around his man first and, with no
    // such route for any man he may send, picks another tactic instead of ordering the flank.
    if ((plan.flank_time && now < plan.flank_time + ms(c.flank_repeat_s)) ||
        (plan.flank_fail_time && now < plan.flank_fail_time + ms(c.flank_retry_s)) ||
        npc.Position().distance_to(threat) < c.flank_min_enemy_m || (sees && !dug_in(npc, state, enemy)))
    {
        stop_search();
        return;
    }
    const auto no_route = [&] {
        stop_search();
        plan.flank_fail_time = now;
        flank_alternative(npc, state, plan, enemy, threat, suppressor, flags);
    };
    if (!flanker)
    {
        // Every man worth sending was looked at in vain.
        if (state.flank_tried_count)
            no_route();
        else
            search.stage = 0;
        return;
    }
    if (!search.stage || search.flanker != flanker->id)
    {
        search = {};
        search.flanker = flanker->id;
        search.stage = 1;
        search.side = side_of(threat, npc.Position(), flanker->owner->Position());
        search.threat = threat;
        search.limit = flank_limit(*flanker->owner, suppressor || now < state.suppressor_until);
    }
    FlankRoute route;
    bool decided;
    {
        const ScopedTimer step(Stat::p_max_flank_us);
        decided = flank_search_step(search, *flanker->owner, route);
    }
    if (!decided)
        return;
    search.stage = 0;
    if (!route.end)
    {
        // Another man may have a covered way where this one has none (a short gun is allowed far less open ground than
        // a rifle): the next one is looked at, up to flank_candidates men per decision, before another tactic.
        if (state.flank_tried_count + 1u < c.flank_candidates)
            state.flank_tried[state.flank_tried_count++] = flanker->id;
        else
            no_route();
        return;
    }
    state.flank_tried_count = 0;
    if (Comms::request(npc, Comms::Kind::flanking, enemy.ID(), order_extra(flanker->id, route.side), flags))
    {
        plan.flanker = flanker->id;
        plan.flank_time = now;
        plan.flank_target = route.end;
        plan.flank_via = route.via;
        plan.flank_open = route.open;
    }
}

// Cover fire by sight: a mate this NPC sees moving over open ground toward or around the enemy needs it.
void scan_mates(CAI_Stalker& npc, TacticsState& state, const Fvector& threat)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (!feature(Feature::roles) || now < state.mate_scan_time + c.mate_scan_ms)
        return;
    state.mate_scan_time = now;
    const SquadState* squad = squad_of(npc);
    if (!squad)
        return;
    const CLevelGraph& graph = ai().level_graph();
    for (const u16 id : squad->members)
    {
        const NpcState* mate = DaAi::npc(id);
        if (id == npc.ID() || !mate || !mate->owner || !Know::sees_now(npc, mate->owner))
            continue;
        CAI_Stalker& other = *mate->owner;
        const u32 vertex = other.ai_location().level_vertex_id();
        if (other.movement().speed() < c.mate_moving_speed || !graph.valid_vertex_id(vertex) ||
            cover_toward(vertex, other.Position(), threat) <= c.cover_open_value)
            continue;
        extend(state.cover_fire_until, now + ms(c.cover_fire_msg_s));
        return;
    }
}

// A trained NPC that starts reloading in the open asks for cover.
void ask_cover(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy, const Fvector& threat)
{
    const bool reload_now = reloading(npc);
    if (reload_now && !state.was_reloading && feature(Feature::roles) && trained(npc) && exposed(npc, threat))
        Comms::request(npc, Comms::Kind::cover_me, enemy.ID(), 0, order_flags(state));
    state.was_reloading = reload_now;
}

// The order names the place: the route the leader chose for this man (within the open metres the leader allowed him,
// knowing whom he ordered to cover), taken while fresh. A short gun refuses a route with more open ground than is
// left of its fight's budget.
void take_route(CAI_Stalker& npc, TacticsState& state)
{
    const SquadState* squad = squad_of(npc);
    const TacticsSquad* plan = squad ? squad->tactics : nullptr;
    if (state.flank_running || !plan || plan->flanker != npc.ID() || !plan->flank_target ||
        Device.dwTimeGlobal > plan->flank_time + ms(config().flank_route_s))
    {
        return;
    }
    state.flank_searched = true;
    if (short_range(band(npc)) && plan->flank_open > config().open_fight_budget_m - state.open_spent_m)
    {
        stat_add(Stat::t_flank_noroute, 1.f, community_of(npc));
        drop_role(state);
        return;
    }
    state.flank_target = plan->flank_target;
    state.flank_via = plan->flank_via;
    state.doctrine_frame = u32(-1);
}

// An order without the leader's route (he changed his plan since): the flanker looks for its covered route once (its
// own belief, the ordered side), in the steps of the leader's search (its covers in one update, then at most walks
// open-ground walks and checks agent's checks per update, within flank_step_budget_us; u32(-1) = all of it now); with
// none he cannot flank without crossing open ground and drops the role.
void plan_flank(CAI_Stalker& npc, TacticsState& state, const Fvector& threat, u32 walks, u32 checks)
{
    if (state.flank_searched || !role_active(state) || state.role != Role::flanker)
        return;
    const bool now = walks == u32(-1);
    StepBudget budget(now ? flt_max : config().flank_step_budget_us);
    TacticsState::FlankSearch::Leg& leg = state.flank_leg;
    if (!leg.gathered)
    {
        if (!gather_flank_leg(npc, threat, state.flank_side, band(npc), nullptr, leg, budget) || (leg.count && !now))
            return;
    }
    if (!judge_flank_leg(npc, threat, flank_limit(npc, Device.dwTimeGlobal < state.suppressor_until), nullptr, leg,
            walks, checks, budget))
    {
        return;
    }
    state.flank_searched = true;
    state.flank_target = leg.best;
    leg = {};
    state.flank_via = nullptr;
    state.doctrine_frame = u32(-1);
    if (!state.flank_target)
    {
        stat_add(Stat::t_flank_noroute, 1.f, community_of(npc));
        drop_role(state);
    }
}

// The belief of the enemy staying put: how long it has not moved beyond [ai_tactics] flush_move_m.
void track_static(TacticsState& state, const CEntityAlive& enemy, const Fvector& threat)
{
    if (state.static_target == enemy.ID() && state.static_pos.distance_to(threat) <= config().flush_move_m)
        return;
    state.static_target = enemy.ID();
    state.static_pos = threat;
    state.static_since = Device.dwTimeGlobal;
}

// Bounding overwatch: a trained, confident NPC closes on an enemy it cannot see while a mate covers the squad.
bool bounding(CAI_Stalker& npc, const TacticsState& state, const CEntityAlive& enemy, float distance)
{
    if (!feature(Feature::roles) || !profile(npc).bounding)
        return false;
    // The ordered suppressor covers and the ordered flanker goes around: neither bounds.
    if (role_active(state) && (state.role == Role::suppressor || state.role == Role::flanker))
        return false;
    if (state.approach_active)
        return true;
    const Morale::Level level = Morale::level(npc);
    if (level != Morale::Level::confident && level != Morale::Level::heroic)
        return false;
    if (Device.dwTimeGlobal >= state.suppressor_until || Know::sees_now(npc, &enemy))
        return false;
    const Config& c = config();
    return distance > c.bound_min_m + c.approach_min_gain_m;
}

void expire_approach(TacticsState& state)
{
    if (state.approach_active && Device.dwTimeGlobal > state.approach_run_time + config().approach_max_ms)
        state.approach_active = false;
}

// The next bound toward the belief, searched again when the NPC, the belief or the floor changed.
bool approach(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy, const Doctrine& current, float floor)
{
    if (state.approach_active)
        return true;
    const Config& c = config();
    const Fvector threat = Know::known_position(npc, enemy);
    const u32 now = Device.dwTimeGlobal;
    const bool fresh = state.approach_time && now < state.approach_time + c.approach_search_ms &&
        state.approach_start.distance_to_sqr(npc.Position()) < _sqr(c.approach_moved_m) &&
        state.approach_from.distance_to_sqr(threat) < _sqr(c.approach_threat_moved_m) &&
        fsimilar(state.approach_floor, floor);
    if (!fresh)
    {
        state.approach_time = now;
        state.approach_start = npc.Position();
        state.approach_from = threat;
        state.approach_floor = floor;
        state.approach_cover = find_approach_cover(npc, threat, current, floor);
    }
    return !!state.approach_cover;
}

// The thrower of a grenade in flight or on the ground, null for one in a hand or not armed by anyone.
const CEntityAlive* grenade_thrower(const CGameObject* object)
{
    const auto* explosive = smart_cast<const CExplosive*>(object);
    if (!object || !explosive || object->getDestroy() || object->H_Parent() || explosive->CurrentParentID() == u16(-1))
        return nullptr;
    return smart_cast<const CEntityAlive*>(Level().Objects.net_Find(explosive->CurrentParentID()));
}

// The NPC ducks away from a grenade it believes at the spot, and its cover choice keeps away from the grenade too
// (the agent's danger area gets this member's bit).
void grenade_danger(CAI_Stalker& npc, const CGameObject& grenade, const CEntityAlive& thrower, const Fvector& spot)
{
    const Config& c = config();
    npc.memory().danger().add(CDangerObject(&thrower, spot, Device.dwTimeGlobal, CDangerObject::eDangerTypeGrenade,
        CDangerObject::eDangerPerceiveTypeSound, &grenade));
    npc.agent_manager().location().add(xr_new<CDangerObjectLocation>(&grenade, Device.dwTimeGlobal,
        c.grenade_danger_ms, c.grenade_danger_m, npc.agent_manager().member().mask(&npc)));
}

// A warning shout of a grenade: the hearer ducks away from where the warning puts it, if that is near.
void warned_of_grenade(CAI_Stalker& npc, const Comms::Message& message)
{
    if (!feature(Feature::grenades) || message.target == u16(-1))
        return;
    const auto* object = smart_cast<const CGameObject*>(Level().Objects.net_Find(message.target));
    const CEntityAlive* thrower = grenade_thrower(object);
    if (!thrower)
        return;

    Source source = Source::report_voice;
    switch (message.channel)
    {
    case Channel::hushed: source = Source::report_hushed; break;
    case Channel::radio: source = Source::report_radio; break;
    case Channel::gesture: source = Source::report_gesture; break;
    case Channel::pda: source = Source::report_pda; break;
    case Channel::hq: source = Source::report_hq; break;
    default: break;
    }
    // Only the spot counts (the danger is placed there, never walked to): no level vertex is resolved for it, which
    // could search the graph inside the comms taking-in.
    const float distance = npc.Position().distance_to(object->Position());
    const Estimate spot = Know::realise(
        npc, object, object->Position(), source, Know::source_error(source, distance), 1.f, nullptr, false);
    if (npc.Position().distance_to(spot.pos) > config().grenade_warn_m)
        return;
    grenade_danger(npc, *object, *thrower, spot.pos);
}

// H12: the sender's cover becomes dangerous for this receiver once the message is understood.
void mark_danger_cover(CAI_Stalker& npc, const Comms::Message& message)
{
    if (!feature(Feature::cover_score))
        return;
    const auto* sender = smart_cast<CAI_Stalker*>(Level().Objects.net_Find(message.sender));
    if (!sender || sender == &npc || !sender->g_Alive() || &sender->agent_manager() != &npc.agent_manager())
        return;
    const CMemberOrder* order = npc.agent_manager().member().get_member(sender->ID());
    const CCoverPoint* cover = order ? order->cover() : nullptr;
    if (!cover)
        return;

    const Config& c = config();
    npc.agent_manager().location().add(xr_new<CDangerCoverLocation>(
        cover, Device.dwTimeGlobal, c.danger_cover_ms, c.danger_cover_m, npc.agent_manager().member().mask(&npc)));
}

void update_counters(CAI_Stalker& npc, TacticsState& state, const CEntityAlive* enemy)
{
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const u32 dt = state.stat_time ? _min(now - state.stat_time, c.stat_max_dt_ms) : 0;
    const Fvector moved = Fvector().sub(npc.Position(), state.stat_pos);
    state.stat_time = now;
    state.stat_pos = npc.Position();
    if (!enemy || !enemy->human_being() || !engine_combat(npc))
        return;

    const int community = community_of(npc);
    const Fvector threat = Know::known_position(npc, *enemy);
    const bool open = exposed(npc, threat);
    stat_add(Stat::t_combat_ms, float(dt), community);
    if (!open)
        stat_add(Stat::t_cover_ms, float(dt), community);

    // Metres moved in the open toward the enemy by short-range weapon holders.
    const WeaponBand weapon_band = band(npc);
    if (!open || weapon_band.engage_max <= 0.f || weapon_band.engage_max > c.short_range_engage_max)
        return;
    const Fvector to_threat = Fvector().sub(threat, npc.Position());
    const float distance = to_threat.magnitude();
    const float step = moved.magnitude();
    if (distance < EPS_L || step > c.stat_teleport_m)
        return;
    if (const float toward = moved.dotproduct(to_threat) / distance; toward > 0.f)
    {
        stat_add(Stat::t_open_m, toward, community);
        state.open_spent_m += toward;
    }
}
} // namespace

const Config& config()
{
    if (!g_config.loaded)
        load_config();
    return g_config;
}

TacticsState* state_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state && !state->dead ? state->tactics : nullptr;
}

const CEntityAlive* selected_enemy(const CAI_Stalker& npc) { return npc.memory().enemy().selected(); }

bool engine_combat(const CAI_Stalker& npc)
{
    const CStalkerPlanner& brain = npc.brain();
    return brain.initialized() && brain.current_action_id() == StalkerDecisionSpace::eWorldOperatorCombatPlanner;
}

TacticsState* create(CAI_Stalker& npc)
{
    config();
    return xr_new<TacticsState>();
}

void destroy(TacticsState* state) { xr_delete(state); }
TacticsSquad* create_squad(SquadState& squad) { return xr_new<TacticsSquad>(); }
void destroy_squad(TacticsSquad* state) { xr_delete(state); }

void update(CAI_Stalker& npc, float dt)
{
    TacticsState* state = state_of(npc);
    if (!state)
        return;

    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    // A fire command kept from the last brain update is the actions' to give again now.
    state->deferred_fire = {};
    const CEntityAlive* enemy = selected_enemy(npc);
    // Identification first: the alert time of this update must not hide an idle NPC's extra reaction.
    if (enemy)
        track_sight(npc, *state, *enemy, Know::sees_now(npc, enemy));
    else
        state->seeing = false;

    if (enemy || npc.memory().danger().selected())
        state->alert_time = Device.dwTimeGlobal;
    track_held_cover(npc, *state, enemy);

    if (state->role != Role::opportunist && !role_active(*state))
        drop_role(*state);
    update_order(npc, *state, enemy);
    if (enemy && enemy->human_being() && engine_combat(npc))
    {
        // A fight ends after alert_memory_s without a human enemy: the next one starts a new open-ground budget.
        const u32 now = Device.dwTimeGlobal;
        if (!state->fight_time || now > state->fight_time + ms(config().alert_memory_s))
            state->open_spent_m = 0.f;
        state->fight_time = now;
        const Fvector threat = Know::known_position(npc, *enemy);
        {
            const ScopedTimer step(Stat::p_max_flank_us);
            plan_flank(npc, *state, threat, config().flank_walks_per_update, config().flank_agent_per_update);
        }
        track_static(*state, *enemy, threat);
        scan_mates(npc, *state, threat);
        ask_cover(npc, *state, *enemy, threat);
        if (feature(Feature::roles))
            lead(npc, *state, *enemy);
    }
    update_counters(npc, *state, enemy);
}

// Roles live in the members' own state (applied on delivery) and the plan in the leader's decisions.
void squad_update(SquadState& squad) {}

void on_hit(CAI_Stalker& npc, const SHit& hit)
{
    TacticsState* state = state_of(npc);
    const IGameObject* initiator = hit.initiator();
    if (!state || !initiator || initiator->ID() == npc.ID())
        return;

    state->hit_time = Device.dwTimeGlobal;
    if (at_cover(npc))
        state->hit_in_cover_time = Device.dwTimeGlobal;
}

void on_death(CAI_Stalker& npc, IGameObject* who) {}

void on_message(CAI_Stalker& npc, const Comms::Message& message)
{
    TacticsState* state = state_of(npc);
    if (!state || message.origin == npc.ID())
        return;

    using Comms::Kind;
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    const bool roles = feature(Feature::roles);
    switch (message.kind)
    {
    case Kind::taking_fire:
        state->fired_on_time = now;
        mark_danger_cover(npc, message);
        break;
    case Kind::flanking:
    case Kind::cover_me:
    {
        if (!roles)
            break;
        const u16 addressee = order_addressee(message.extra);
        if (addressee == u16(-1))
        {
            // A mate moves over open ground or asks for cover.
            extend(state->cover_fire_until, now + ms(c.cover_fire_msg_s));
            break;
        }
        if (addressee != npc.ID())
        {
            // Overheard order: a mate is sent around the flank, or a mate is told to cover the squad.
            // A suppressor ordered covers the squad for the order's window (the men who bound move under it).
            if (message.kind == Kind::flanking)
                extend(state->cover_fire_until, now + ms(c.cover_fire_msg_s));
            else
                extend(state->suppressor_until, now + ms(c.supp_order_s));
            break;
        }
        // An order to this NPC counts only from the leader it follows and only for a role it was trained for.
        const Role wanted = message.kind == Kind::flanking ? Role::flanker : Role::suppressor;
        if (message.origin != Morale::believed_leader(npc) || !profile(npc).has_role(wanted))
            break;
        assign_role(*state, wanted, u8(message.extra & 1u));
        if (wanted == Role::flanker)
        {
            stat_add(Stat::t_flank_role, 1.f, community_of(npc));
            take_route(npc, *state);
        }
        // "Suppress!" covers the squad until the leader's next renewal of the order, not one mate's move.
        if (wanted == Role::suppressor)
            extend(state->cover_fire_until, now + ms(c.supp_order_s));
        break;
    }
    case Kind::reloading:
        if (roles)
            extend(state->cover_fire_until, now + ms(c.cover_fire_msg_s));
        break;
    case Kind::fall_back:
        if (!roles)
            break;
        extend(state->cover_fire_until, now + ms(c.cover_fire_msg_s));
        // A rearguard covers the withdrawal instead of joining it at once.
        if (profile(npc).has_role(Role::rearguard) && !Morale::panicking(npc) && !role_active(*state))
            assign_role(*state, Role::rearguard, 0);
        break;
    case Kind::attack:
        if (state->order == u8(Order::holding))
            state->order = u8(Order::released);
        break;
    case Kind::grenade: warned_of_grenade(npc, message); break;
    default: break;
    }
}

void level_unload() { g_config.loaded = false; }

void on_own_shot(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    if (!state)
        return;

    const int community = community_of(npc);
    stat_add(Stat::t_shots, 1.f, community);
    state->t_shot = Device.dwTimeGlobal;
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!enemy)
        return;

    const bool suppressing = !Know::sees_now(npc, enemy) && suppression_allowed(npc, *state, *enemy);
    if (suppressing)
        stat_add(Stat::t_shots_supp, 1.f, community);

    // The band holds for every living target (a buckshot round does not reach a dog at 47 m either); a suppression
    // round goes at the own last-seen point, an aimed one at the known target.
    if (enemy->g_Alive())
    {
        const WeaponBand weapon_band = band(npc);
        const float distance = suppressing ? npc.Position().distance_to(state->last_seen_pos) :
                                             Know::known_distance(npc, *enemy);
        if (weapon_band.engage_max > 0.f && distance > weapon_band.engage_max)
            stat_add(Stat::t_shots_oob, 1.f, community);
    }
}

void on_fire_decision(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy || state->react_counted || state->target != enemy->ID() || !state->t_identified)
        return;
    state->react_counted = true;
    // Identification -> the first command to fire at him, over an unbroken sight whose gate waited for the reaction
    // alone (a fire-on-order hold or an out-of-band target is a decision, not a reaction). A fight with a mutant runs
    // the stock combat, whose moves come before the first shot (r2m: a bandit's shotgun at a dog 4 s after): only a
    // human enemy is a sample of the reaction.
    if (state->react_held || state->t_acquired != state->t_identified || !enemy->human_being())
        return;
    // Next to the reaction the gate required (rolled base + distance, peripheral, fear and fatigue add-ons) and its
    // rolled base.
    const int community = community_of(npc);
    stat_add(Stat::t_react_ms, float(Device.dwTimeGlobal - state->t_identified), community);
    stat_add(Stat::t_react_due_ms, float(state->react_due_ms), community);
    stat_add(Stat::t_react_base_ms, float(state->react_base_ms), community);
}

FireMode fire_mode(const CAI_Stalker& npc, const CEntityAlive* enemy)
{
    TacticsState* state = state_of(npc);
    if (!state || !enemy)
        return FireMode::aimed;

    const bool reaction = feature(Feature::reaction);
    const bool doctrine_on = feature(Feature::doctrine);
    const bool suppression = feature(Feature::suppression);
    const bool hold = holding(*state);
    if (!reaction && !doctrine_on && !suppression && !hold)
        return FireMode::aimed;

    auto& stalker = const_cast<CAI_Stalker&>(npc);
    const bool sees = Know::sees_now(npc, enemy);
    track_sight(stalker, *state, *enemy, sees);
    // Fire on order: an undetected squad holds its fire until the order comes.
    if (hold)
    {
        state->react_held = state->react_held || sees;
        return FireMode::none;
    }
    if (sees)
    {
        if (reaction && Device.dwTimeGlobal < state->t_fire_ready)
            return FireMode::none;
        if (beyond_band(npc, *enemy))
        {
            state->react_held = true;
            return FireMode::none;
        }
        return FireMode::aimed;
    }

    if (!suppression)
        return FireMode::aimed;
    return suppression_allowed(npc, *state, *enemy) ? FireMode::suppress : FireMode::none;
}

bool beyond_band(const CAI_Stalker& npc, const CEntityAlive& enemy)
{
    // Every living target: the doctrine's movement is for human enemies only, its no-fire rule is not.
    if (!feature(Feature::doctrine) || !state_of(npc) || !enemy.g_Alive())
        return false;
    const WeaponBand weapon_band = band(npc);
    return weapon_band.engage_max > 0.f && Know::known_distance(npc, enemy) > weapon_band.engage_max;
}

float accuracy_mult(const CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    if (!state)
        return 1.f;

    float result = Morale::accuracy_mult(npc);
    if (!feature(Feature::settle))
        return result;

    const Config& c = config();
    const float stamina = Morale::stamina(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (enemy && state->target == enemy->ID())
    {
        if (state->seeing)
        {
            // Aim settles from settle_start_mult to the tier value after each (re)acquisition.
            const float settle_s = profile(npc).settle_s * (1.f + c.settle_fear_mult * Morale::fear(npc)) *
                (1.f + c.settle_fatigue_mult * (1.f - stamina));
            const float elapsed = float(Device.dwTimeGlobal - state->t_acquired) / 1000.f;
            const float left = settle_s > EPS_L ? clampr(1.f - elapsed / settle_s, 0.f, 1.f) : 0.f;
            result *= 1.f + (c.settle_start_mult - 1.f) * left;

            const Fvector to_target = Fvector().sub(state->last_seen_pos, npc.Position());
            if (const float distance = to_target.magnitude(); distance > EPS_L)
            {
                const Fvector direction = Fvector(to_target).div(distance);
                const Fvector& velocity = state->last_seen_vel;
                const Fvector lateral = Fvector().mad(velocity, direction, -velocity.dotproduct(direction));
                result *= 1.f + c.acc_lateral_mult * _min(1.f, lateral.magnitude() / c.acc_lateral_ref);
            }
        }
        else
            result *= c.settle_start_mult;
    }

    result *= 1.f + c.acc_suppression_mult * Sense::suppression(npc);
    result *= 1.f + c.acc_wound_mult * (1.f - clampr(npc.conditions().GetHealth(), 0.f, 1.f));
    result *= 1.f + c.acc_fatigue_mult * (1.f - stamina);
    return result;
}

Role role(const CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::roles))
        return Role::opportunist;
    if (role_active(*state))
        return state->role;
    // A trained grenadier with a grenade on him is one without an order.
    if (profile(npc).has_role(Role::grenadier) && npc.inventory().ItemFromSlot(GRENADE_SLOT))
        return Role::grenadier;
    return Role::opportunist;
}

WeaponBand band(const CAI_Stalker& npc)
{
    // The band as carried (scope, slugs) of the firearm the NPC fights with.
    const CWeaponMagazined* weapon = combat_weapon(npc);
    return weapon ? weapon_band(*weapon) : WeaponBand{};
}

bool hook_fire_make_sense(CAI_Stalker& npc, bool& result)
{
    TacticsState* state = state_of(npc);
    if (state && holding(*state))
    {
        result = false;
        return true;
    }
    if (!state || !feature(Feature::suppression))
        return false;

    result = false;
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!enemy)
        return true;

    const bool sees = Know::sees_now(npc, enemy);
    track_sight(npc, *state, *enemy, sees);
    if (!sees && !suppression_allowed(npc, *state, *enemy))
        return true;

    // The stock checks, against the known position while seen and the own last-seen point otherwise.
    const Config& c = config();
    const Fvector target = sees ? Know::known_position(npc, *enemy) : state->last_seen_pos;
    const float pick = npc.pick_distance();
    if (pick + c.supp_precise_m < npc.Position().distance_to(target) || pick < c.supp_near_m)
        return true;
    result = same_floor(npc, target);
    return true;
}

bool hook_too_far(CAI_Stalker& npc, const Fvector& position, bool& result)
{
    if (!state_of(npc) || !feature(Feature::doctrine))
        return false;

    const WeaponBand weapon_band = band(npc);
    result = weapon_band.engage_max > 0.f && npc.Position().distance_to(position) > weapon_band.engage_max;
    return true;
}

const Doctrine& doctrine(CAI_Stalker& npc)
{
    static const Doctrine inactive{};
    TacticsState* state = state_of(npc);
    if (!state)
        return inactive;
    if (state->doctrine_frame == Device.dwFrame)
        return state->doctrine;

    state->doctrine_frame = Device.dwFrame;
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    Doctrine& result = state->doctrine;
    result = {};
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!feature(Feature::doctrine) || !enemy || !enemy->human_being())
        return result;

    result.band = band(npc);
    if (result.band.engage_max <= 0.f)
        return result;

    result.distance = Know::known_distance(npc, *enemy);
    if (result.distance < 0.f)
        return result;
    result.active = true;
    // Contact lost: no band rule holds the NPC back or bounds it on toward a stale belief; the search walks to the
    // last known position instead (the search sub-planner requires DaOutOfBand and TooFarToKillEnemy false).
    if (Search::contact_lost(npc))
        return result;
    result.too_close = result.band.min > 0.f && result.distance < result.band.min;
    expire_approach(*state);
    // The ordered flanker goes around by his covered route: no bound and no ambush on the way.
    if (flank_route(*state))
    {
        result.out_of_band = result.too_close;
        return result;
    }
    if (result.distance <= result.band.engage_max)
    {
        result.out_of_band = result.too_close;
        // Inside the band a trained squad bounds from cover to cover while a mate covers it (T4).
        if (!result.too_close && bounding(npc, *state, *enemy, result.distance))
            result.too_far = approach(npc, *state, *enemy, result, config().bound_min_m);
        return result;
    }

    // Beyond the band: close in through covers when the profile's aggression takes it and a bound exists that cuts
    // the distance, else hold (the natural ambush of short guns). A bound under way is kept until the run ends.
    const bool keen = state->approach_active || Morale::aggression(npc) >= config().approach_aggr_min;
    result.too_far = keen && approach(npc, *state, *enemy, result, result.band.min);
    result.out_of_band = !result.too_far;
    return result;
}

bool approach_begin(CAI_Stalker& npc, const CCoverPoint*& cover)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::doctrine))
        return false;

    const Doctrine& current = doctrine(npc);
    cover = current.too_far ? state->approach_cover : nullptr;
    state->approach_active = !!cover;
    state->approach_run_time = Device.dwTimeGlobal;
    if (!cover)
        return true;
    stat_add(Stat::t_advance, 1.f, community_of(npc));
    // A trained NPC asks for cover before it leaves its own.
    if (const CEntityAlive* enemy = selected_enemy(npc); enemy && feature(Feature::roles) && trained(npc))
        Comms::request(npc, Comms::Kind::cover_me, enemy->ID(), 0, order_flags(*state));
    return true;
}

bool approach_running(const CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::doctrine))
        return true;
    return state->approach_active && Device.dwTimeGlobal <= state->approach_run_time + config().approach_max_ms;
}

void approach_end(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
    {
        state->approach_active = false;
        state->approach_time = 0;
        state->doctrine_frame = u32(-1);
    }
}

bool should_fall_back(CAI_Stalker& npc, bool in_cover)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score))
        return false;
    if (state->fallback_active)
        return true;
    if (state->fallback_frame == Device.dwFrame)
        return state->fallback_result;

    // The doctrine first: its own timer must not run inside this one.
    const Doctrine& current = doctrine(npc);
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    state->fallback_frame = Device.dwFrame;
    state->fallback_result = false;
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!enemy || !enemy->human_being())
        return false;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (state->fallback_done_time && now < state->fallback_done_time + c.fallback_cooldown_ms)
        return false;

    const Fvector threat = Know::known_position(npc, *enemy);
    const bool open = exposed(npc, threat);
    const bool pressed = (state->hit_time && now < state->hit_time + c.pressure_ms) ||
        Sense::suppression(npc) >= c.worried_supp_min;

    bool result = false;
    // The enemy is inside the weapon's minimum distance.
    if (current.active && current.too_close)
        result = true;
    // Cover compromised: the belief moved outside the arc of a cover that covered from it, or a hit found the NPC
    // there.
    else if (in_cover && at_cover(npc) &&
        ((open && state->held_cover && state->held_value <= c.cover_open_value) ||
            now < state->hit_in_cover_time + c.hit_in_cover_ms))
        result = true;
    // Exposed under fire with no acceptable cover in reach.
    else if (open && pressed && state->no_cover_time && now < state->no_cover_time + c.no_cover_memory_ms)
        result = true;
    // Reloading in the open with the enemy near.
    else if (const float distance = Know::known_distance(npc, *enemy);
             open && distance >= 0.f && distance < c.reload_near_m && reloading(npc))
        result = true;
    // Worried and exposed toward the belief (worried and under fire in a cover that covers him: he stays in it).
    else if (Morale::level(npc) == Morale::Level::worried && open)
        result = true;

    state->fallback_result = result;
    return result;
}

bool fall_back_begin(CAI_Stalker& npc, Fvector& position, u32& vertex, bool& cover)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy)
        return false;

    // The doctrine first: its own timer must not run inside this one.
    const Doctrine& current = doctrine(npc);
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    const Fvector threat = Know::known_position(npc, *enemy);
    // Straight away over open ground only from an enemy inside the weapon's minimum; otherwise a cover or nothing (the
    // cooldown starts and take cover picks the best cover).
    const bool away = current.active && current.too_close;
    if (!find_fall_back(npc, threat, profile(npc).fanatic, away, position, vertex, cover))
    {
        fall_back_end(npc);
        return false;
    }
    state->fallback_active = true;
    stat_add(Stat::t_fallback, 1.f, community_of(npc));
    if (feature(Feature::roles) && trained(npc))
        Comms::request(npc, Comms::Kind::cover_me, enemy->ID(), 0, order_flags(*state));
    return true;
}

void fall_back_end(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
    {
        state->fallback_active = false;
        state->fallback_done_time = Device.dwTimeGlobal;
        state->fallback_frame = u32(-1);
    }
}

bool return_fire(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score))
        return false;
    const float range = profile(npc).return_fire_m;
    const CEntityAlive* enemy = selected_enemy(npc);
    if (range <= 0.f || !enemy || !enemy->human_being() || holding(*state) || reloading(npc))
        return false;

    const u32 now = Device.dwTimeGlobal;
    const bool running = now < state->return_fire_until;
    if (!running && now < state->return_fire_next)
        return false;
    // Only at a target it has identified and sees now, inside the profile's range.
    if (!state->seeing || state->target != enemy->ID() || !Know::sees_now(npc, enemy))
        return false;
    const float distance = Know::known_distance(npc, *enemy);
    if (distance < 0.f || distance > range)
        return false;
    if (running)
        return true;

    // A new exchange: he has found it out (a hit, near misses, his gaze). In the open it shoots back before it runs; in
    // the cover it already has it shoots back from there instead of leaving it for a better one first.
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    const Config& c = config();
    const bool found_out = (state->hit_time && now < state->hit_time + c.pressure_ms) ||
        Sense::suppression(npc) >= c.order_supp_min || Know::enemy_sees_me(npc, *enemy);
    if (!found_out)
        return false;
    state->return_fire_until = now + roll_ms(c.return_fire_ms);
    state->return_fire_next = state->return_fire_until + c.return_fire_cooldown_ms;
    stat_add(Stat::t_return_fire, 1.f, community_of(npc));
    return true;
}

void on_ambush(CAI_Stalker& npc)
{
    if (state_of(npc))
        stat_add(Stat::t_ambush, 1.f, community_of(npc));
}

bool reaction_running(const CAI_Stalker& npc, const CEntityAlive& enemy)
{
    const TacticsState* state = state_of(npc);
    return state && feature(Feature::reaction) && !holding(*state) && state->target == enemy.ID() && state->seeing &&
        Device.dwTimeGlobal < state->t_fire_ready;
}

void defer_fire(CAI_Stalker& npc, const CEntityAlive& enemy, const CGameObject* weapon, u32 queue_size_min,
    u32 queue_size_max, u32 queue_interval_min, u32 queue_interval_max, float angle)
{
    TacticsState* state = state_of(npc);
    if (!state || !weapon || !feature(Feature::reaction) || holding(*state) || state->target != enemy.ID() ||
        !state->seeing)
    {
        return;
    }
    state->deferred_fire = {enemy.ID(), weapon->ID(), queue_size_min, queue_size_max, queue_interval_min,
        queue_interval_max, angle};
}

void hold_fire(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (state && enemy && state->target == enemy->ID() && state->seeing)
        state->react_held = true;
}

namespace
{
// The reaction ran out between two brain updates: the action's aimed fire command goes now, under the action's own
// angle test. A gate shut for anything but the reaction (a hold, the band, a lost sight) drops the command.
void give_deferred_fire(CAI_Stalker& npc, TacticsState& state, const CEntityAlive& enemy, FireMode mode)
{
    TacticsState::DeferredFire& deferred = state.deferred_fire;
    if (deferred.target == u16(-1))
        return;
    if (deferred.target != enemy.ID() || mode == FireMode::suppress ||
        (mode == FireMode::none && !reaction_running(npc, enemy)))
    {
        deferred = {};
        return;
    }
    if (mode != FireMode::aimed)
        return;

    float yaw, pitch;
    Fvector().sub(Know::known_position(npc, enemy), npc.Position()).getHP(yaw, pitch);
    if (angle_difference(-yaw, npc.movement().head_orientation().current.yaw) > deferred.angle)
        return;
    const TacticsState::DeferredFire order = deferred;
    deferred = {};
    auto* weapon = smart_cast<CInventoryItem*>(Level().Objects.net_Find(order.weapon));
    if (!weapon || weapon->object().H_Parent() != &npc)
        return;
    on_fire_decision(npc);
    npc.CObjectHandler::set_goal(MonsterSpace::eObjectActionFire1, weapon, order.queue_size_min,
        order.queue_size_max, order.queue_interval_min, order.queue_interval_max);
}
} // namespace

void prepare_fire(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    if (!state)
        return;
    const bool combat = engine_combat(npc);
    const CEntityAlive* enemy = combat ? selected_enemy(npc) : nullptr;
    if (!enemy)
    {
        // No round in engine combat without a selected enemy; scripted scenes outside it keep the stock fire.
        state->fire_ok = !combat || !Know::active(npc);
        state->deferred_fire = {};
        return;
    }
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    const FireMode mode = fire_mode(npc, enemy);
    state->fire_ok = mode != FireMode::none;
    give_deferred_fire(npc, *state, *enemy, mode);
}

bool fire_continues(CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    return !state || state->fire_ok;
}

bool suppress_point(const CAI_Stalker& npc, Fvector& point)
{
    const TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy || state->target != enemy->ID() || !state->t_last_seen)
        return false;
    // The burst of DaSuppressFire may go at a cover edge beside the point.
    point = Fvector().add(state->last_seen_pos, state->suppress_offset);
    return true;
}

bool cover_distances(const CAI_Stalker& npc, float& min_distance, float& max_distance)
{
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state_of(npc) || !feature(Feature::doctrine) || !enemy || !enemy->human_being())
        return false;

    const WeaponBand weapon_band = band(npc);
    if (weapon_band.engage_max <= 0.f)
        return false;

    // The stock filter only forbids moving the wrong way (closer inside the minimum, farther back beyond the maximum);
    // the weapon band replaces its fixed metres. Farther back than engage_max the enemy would be out of reach (the
    // score's band term already prefers the effective range); an NPC outranged already loses nothing by a cover farther
    // back, and holding its ambush in the open because no cover lies closer was the cost (DaHoldAmbush never covered).
    min_distance = _max(config().cover_min_enemy_m, weapon_band.min);
    max_distance = Know::known_distance(npc, *enemy) > weapon_band.engage_max ?
        flt_max :
        _max(min_distance, weapon_band.engage_max);
    return true;
}

void on_cover_search(CAI_Stalker& npc, bool found)
{
    if (TacticsState* state = state_of(npc))
        state->no_cover_time = found ? 0 : Device.dwTimeGlobal;
}

bool cover_enemy_position(const CAI_Stalker& npc, const CEntityAlive* enemy, bool& known, Fvector& position)
{
    if (!state_of(npc) || !feature(Feature::knowledge) || !enemy)
        return false;

    // F2: the member's own belief, never the pool entry.
    known = Know::knows(npc, enemy);
    if (known)
        position = Know::known_position(npc, *enemy);
    return true;
}

bool take_cover_run(CAI_Stalker& npc)
{
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state_of(npc) || !feature(Feature::cover_score) || !enemy || !enemy->human_being())
        return false;
    return may_run(npc) && exposed(npc, Know::known_position(npc, *enemy));
}

u32 hold_time_ms(const CAI_Stalker& npc, u32 stock)
{
    if (!state_of(npc) || !feature(Feature::cover_score))
        return stock;

    // Trained profiles stay exposed for a shorter time.
    const Range& hold = config().peek_hold_ms;
    const float base = hold.hi + (hold.lo - hold.hi) * clampr(profile(npc).peek, 0.f, 1.f);
    return u32(iFloor(base * ::Random.randF(.75f, 1.25f)));
}

bool peek_stance(CAI_Stalker& npc, bool& crouch)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score))
        return false;

    // Disciplined peeks do not repeat the last stance.
    crouch = ::Random.randF() < profile(npc).peek ? !state->peek_crouch : ::Random.randI(2) != 0;
    state->peek_crouch = crouch;
    return true;
}

bool peek_avoid(const CAI_Stalker& npc, Fvector& position, float& radius)
{
    const TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::cover_score) || !state->peeked || profile(npc).peek < config().peek_avoid_min)
        return false;

    position = state->last_peek;
    radius = config().peek_reuse_m;
    return true;
}

void on_peek(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
    {
        state->last_peek = npc.Position();
        state->peeked = true;
    }
}

bool danger_cover_own_only(const CAI_Stalker& npc) { return state_of(npc) && feature(Feature::cover_score); }

bool held_cover_from_cover(const CAI_Stalker& npc) { return state_of(npc) && feature(Feature::cover_score); }

bool can_detour(const CAI_Stalker& npc, bool& result)
{
    const TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::roles))
        return false;
    if (reload_push(npc))
    {
        result = true;
        return true;
    }
    // An untrained profile keeps the stock half-squad quota; a trained one flanks only on the leader's order.
    if (!profile(npc).has_role(Role::flanker))
        return false;
    result = flank_route(*state);
    return true;
}

bool cover_detouring(const CAI_Stalker& npc, bool& result)
{
    const TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::roles))
        return false;
    // A mate is known to be exposed only from his message or from own sight, never from the agent's detour list.
    result = Device.dwTimeGlobal < state->cover_fire_until;
    return true;
}

bool quiet_cover_fire(const CAI_Stalker& npc) { return state_of(npc) && feature(Feature::roles); }

bool reload_push(const CAI_Stalker& npc)
{
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state_of(npc) || !feature(Feature::roles) || !enemy || !enemy->human_being())
        return false;
    const Belief* belief = Know::belief(npc, enemy->ID());
    if (!belief || belief->e.source != Source::heard_reload ||
        Device.dwTimeGlobal > belief->e.t_observed + ms(config().reload_window_s))
        return false;
    return Morale::aggression(npc) >= config().reload_push_aggr;
}

bool suppress_role(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy || !enemy->human_being() || holding(*state) || !covering(*state))
        return false;
    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    return suppress_target_ok(npc, *state, *enemy);
}

void suppress_begin(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
    {
        state->burst_until = 0;
        state->pause_until = 0;
        state->suppress_offset.set(0.f, 0.f, 0.f);
        stat_add(Stat::t_suppress, 1.f, community_of(npc));
    }
}

bool suppress_step(CAI_Stalker& npc, Fvector& point)
{
    TacticsState* state = state_of(npc);
    if (!state || !state->t_last_seen)
        return false;

    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (now >= state->pause_until)
    {
        // Next burst: the last-seen point or a cover edge beside it, across the line of fire.
        Fvector across = Fvector().sub(state->last_seen_pos, npc.Position());
        across.y = 0.f;
        if (const float length = across.magnitude(); length > EPS_L)
            across.set(-across.z / length, 0.f, across.x / length);
        else
            across.set(0.f, 0.f, 0.f);
        state->suppress_offset.mul(across, ::Random.randF(-c.suppress_spread_m, c.suppress_spread_m));
        state->burst_until = now + roll_ms(c.suppress_burst_ms);
        state->pause_until = state->burst_until + roll_ms(c.suppress_pause_ms);
    }
    point = Fvector().add(state->last_seen_pos, state->suppress_offset);
    return now < state->burst_until;
}

void suppress_end(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
        state->suppress_offset.set(0.f, 0.f, 0.f);
}

bool on_detour_begin(CAI_Stalker& npc) { return flank_begin(npc); }

bool flank_begin(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy || !flank_route(*state))
        return false;
    const u32 now = Device.dwTimeGlobal;
    if (!state->flank_running)
        state->flank_start = now;
    state->flank_running = true;
    // The run keeps the role until flank_max_ms from its start (flank_expired ends it there).
    extend(state->role_until, state->flank_start + _max(ms(config().role_hold_s), config().flank_max_ms));
    if (!state->flank_counted)
    {
        state->flank_counted = true;
        stat_add(Stat::t_flank, 1.f, community_of(npc));
    }
    // The side travels in the message; the covering mates take it as their cue.
    return Comms::request(npc, Comms::Kind::flanking, enemy->ID(), state->flank_side, order_flags(*state),
        StalkerSpace::eStalkerSoundDetour);
}

bool flank_go(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!state || !enemy || !enemy->human_being() || !flank_route(*state))
        return false;
    // A burst under way when the order came ends first, within flank_grace_ms of the delivery.
    if (!state->flank_running && Device.dwTimeGlobal < state->role_time + config().flank_grace_ms)
    {
        const CWeaponMagazined* weapon = combat_weapon(npc);
        if (weapon && weapon->GetState() == CWeapon::eFire)
            return false;
    }
    return true;
}

const CCoverPoint* flank_waypoint(const CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    if (!state || !flank_route(*state))
        return nullptr;
    return state->flank_via ? state->flank_via : state->flank_target;
}

bool flank_reached(CAI_Stalker& npc, const CCoverPoint& point)
{
    TacticsState* state = state_of(npc);
    if (!state)
        return true;
    if (&point == state->flank_via)
    {
        state->flank_via = nullptr;
        return false;
    }
    // One order, one flank: the end reached, the role is done.
    drop_role(*state);
    return true;
}

bool flank_expired(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    if (!state || !state->flank_running || Device.dwTimeGlobal <= state->flank_start + config().flank_max_ms)
        return false;
    drop_role(*state);
    return true;
}

bool flank_fire(const CAI_Stalker& npc) { return profile(npc).flank_fire; }

void on_detour_end(CAI_Stalker& npc)
{
    TacticsState* state = state_of(npc);
    if (!state || !state->flank_running)
        return;
    state->flank_running = false;
    // Reached: the order is done. Cut short (he saw the enemy on the way): the route stays for the next detour.
    const CCoverPoint* target = state->flank_target;
    if (target && target->position().distance_to_sqr(npc.Position()) < _sqr(config().at_cover_m))
        drop_role(*state);
}

bool flank_cover(CAI_Stalker& npc, const Fvector& threat, const CCoverPoint*& cover)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::roles) || !role_active(*state) || state->role != Role::flanker)
        return false;
    // The route is looked for once per order and kept until reached, so the flank ends at it; with none this detour
    // stays where it is (never a flank over open ground). The detour needs it now: a search under way is finished.
    if (!state->flank_searched)
    {
        ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
        plan_flank(npc, *state, threat, u32(-1), u32(-1));
    }
    cover = state->flank_target;
    return true;
}

Order order(const CAI_Stalker& npc)
{
    const TacticsState* state = state_of(npc);
    return state && feature(Feature::roles) ? Order(state->order) : Order::none;
}

bool grenade_decision(CAI_Stalker& npc, bool started, bool positioned, bool& result)
{
    TacticsState* state = state_of(npc);
    if (!state || !feature(Feature::grenades) || !Know::active(npc))
        return false;

    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    result = started;
    if (started || !positioned)
        return true;

    // The stock conditions: not too often, a grenade on him, a human enemy out of own sight.
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    if (npc.last_throw_time() + npc.throw_time_interval() >= now || !npc.inventory().ItemFromSlot(GRENADE_SLOT))
        return true;
    const CEntityAlive* enemy = selected_enemy(npc);
    if (!enemy || !enemy->human_being() || Know::sees_now(npc, enemy))
        return true;

    // The target is an own sighting or hearing of him, fresh enough; never a report (F5).
    const Belief* belief = Know::belief(npc, enemy->ID());
    if (!belief || belief->e.source < Source::seen || belief->e.source > Source::heard_voice ||
        now > belief->e.t_observed + ms(c.flush_static_s + c.flush_belief_extra_s))
        return true;
    const MemorySpace::CMemoryInfo memory = npc.memory().memory(enemy);
    if (!memory.m_object)
        return true;
    const Fvector& position = memory.m_object_params.m_position;
    const u32 vertex = memory.m_object_params.m_level_vertex_id;
    const float distance = npc.Position().distance_to(position);
    if (distance < c.grenade_min_m || distance > c.grenade_max_m)
        return true;

    // Flush: he sits still behind cover, is indoors, or where the NPC cannot go.
    const CLevelGraph& graph = ai().level_graph();
    if (!graph.valid_vertex_id(vertex))
        return true;
    const bool indoors = graph.vertex_high_cover(vertex) <= c.indoor_cover_max;
    if (!dug_in(npc, *state, enemy->ID(), vertex, position) && !indoors && npc.movement().accessible(vertex))
        return true;

    // The leader's own flush instead of a flank without a covered route needs no roll.
    if (now < state->flush_order_until)
        state->gren_roll_ok = true;
    else if (now >= state->gren_roll_time + c.gren_think_ms)
    {
        state->gren_roll_time = now;
        const float chance = profile(npc).gren * (role(npc) == Role::grenadier ? c.grenadier_mult : 1.f);
        state->gren_roll_ok = ::Random.randF() < chance;
    }
    if (!state->gren_roll_ok || !npc.agent_manager().member().can_throw_grenade(position))
        return true;

    npc.throw_target(position, vertex, const_cast<CEntityAlive*>(enemy));
    // A grenade already in hand cannot be put back; otherwise the trajectory must be clear.
    result = npc.inventory().ItemFromSlot(GRENADE_SLOT) == npc.inventory().ActiveItem() || npc.throw_enabled();
    return true;
}

bool grenade_danger_own_only() { return feature(Feature::grenades); }

void on_grenade_heard(CAI_Stalker& npc, const CGameObject& grenade, const Fvector& spot)
{
    TacticsState* state = state_of(npc);
    const Config& c = config();
    const u32 now = Device.dwTimeGlobal;
    // Every bounce and every re-emitted sound event of the same grenade is heard again: one reaction per grenade.
    if (!state || !feature(Feature::grenades) ||
        (state->heard_grenade == grenade.ID() && now < state->heard_grenade_time + c.grenade_danger_ms))
    {
        return;
    }
    const CEntityAlive* thrower = grenade_thrower(&grenade);
    if (!thrower || npc.Position().distance_to(spot) > c.grenade_warn_m)
        return;

    state->heard_grenade = grenade.ID();
    state->heard_grenade_time = now;
    grenade_danger(npc, grenade, *thrower, spot);
    // The warning the one who sees it land gives (react_on_grenades): an enemy's grenade, in a group.
    if (npc.is_relation_enemy(thrower) && npc.agent_manager().member().group_behaviour() &&
        !Comms::request(npc, Comms::Kind::grenade, grenade.ID(), 0, Comms::urgent,
            StalkerSpace::eStalkerSoundGrenadeAlarm))
    {
        npc.sound().play(StalkerSpace::eStalkerSoundGrenadeAlarm);
    }
}

void on_grenade_throw(CAI_Stalker& npc)
{
    if (TacticsState* state = state_of(npc))
    {
        state->gren_roll_ok = false;
        state->flush_order_until = 0;
        stat_add(Stat::t_grenade, 1.f, community_of(npc));
    }
}

bool trained(const CAI_Stalker& npc) { return (profile(npc).roles & ~(1u << u32(Role::opportunist))) != 0; }

bool open_limit(const CAI_Stalker& npc, bool approaching, float& limit, bool exposed)
{
    const TacticsState* state = state_of(npc);
    if (!state || !short_range(band(npc)))
        return false;
    const Config& c = config();
    if (!approaching)
    {
        // Out of the open away or across, any cover is a gain: staying costs as much open ground as the run.
        if (exposed)
            return false;
        limit = c.exposure_open_limit_m;
        return true;
    }
    // A gap under a mate's suppression for the trained, a step for the rest, and never past the fight's budget.
    const bool suppressed = trained(npc) && Device.dwTimeGlobal < state->suppressor_until;
    limit = _min(suppressed ? c.exposure_open_limit_m : c.exposure_gap_m,
        _max(c.open_fight_budget_m - state->open_spent_m, 0.f));
    return true;
}

Step covered_step(CAI_Stalker& npc, const Fvector& goal, Fvector& position, u32& vertex)
{
    TacticsState* state = state_of(npc);
    float limit;
    if (!state || !feature(Feature::doctrine) || !open_limit(npc, true, limit))
        return Step::direct;
    const Config& c = config();
    if (npc.Position().distance_to_xz(goal) <= c.at_cover_m)
        return Step::direct;

    ScopedTimer timer(Stat::p_tactics_us, community_of(npc));
    const u32 now = Device.dwTimeGlobal;
    const bool same_goal = state->step_goal.distance_to_sqr(goal) < _sqr(c.approach_threat_moved_m);
    const Step last = Step(state->step_kind);
    const auto bound = [&](const CCoverPoint& cover) {
        position = cover.position();
        vertex = cover.level_vertex_id();
        return Step::bound;
    };
    // A bound under way is kept until reached (or approach_max_ms); a decision stands for approach_search_ms while
    // the NPC stays put.
    if (same_goal && last == Step::bound && state->step_cover && now < state->step_time + c.approach_max_ms &&
        state->step_cover->position().distance_to_sqr(npc.Position()) >= _sqr(c.at_cover_m))
    {
        return bound(*state->step_cover);
    }
    const bool fresh = same_goal && last != Step::bound && now < state->step_time + c.approach_search_ms &&
        state->step_from.distance_to_sqr(npc.Position()) < _sqr(c.approach_moved_m);
    if (!fresh)
    {
        const CEntityAlive* enemy = selected_enemy(npc);
        const Fvector threat = enemy ? Know::known_position(npc, *enemy) : goal;
        const bool held = same_goal && (last == Step::hold || last == Step::given_up);
        state->step_goal = goal;
        state->step_from = npc.Position();
        state->step_time = now;
        state->step_cover = nullptr;
        Step kind = Step::direct;
        if (path_exposure(npc, goal, threat, limit) > limit)
        {
            state->step_cover = find_step_cover(npc, goal, threat, limit);
            kind = state->step_cover ? Step::bound : Step::hold;
        }
        if (kind == Step::hold && !held)
            state->step_hold_since = now;
        state->step_kind = u8(kind);
    }

    switch (Step(state->step_kind))
    {
    case Step::bound: return bound(*state->step_cover);
    case Step::hold:
    case Step::given_up:
        // Held long enough without a covered way: the goal counts as watched from here.
        if (now >= state->step_hold_since + c.step_hold_ms)
            state->step_kind = u8(Step::given_up);
        return Step(state->step_kind);
    default: return Step::direct;
    }
}

bool may_run(const CAI_Stalker& npc, bool flank)
{
    if (!state_of(npc))
        return true;
    if (Morale::winded(npc))
        return false;
    // No running flank for the tired or for an exo suit.
    return !flank || (!Morale::tired(npc) && armour_class(npc) != ArmourClass::exo);
}
} // namespace DaAi::Tactics
