// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai_profile.h"

#include <array>
#include <chrono>

// Tactics lane: weapon doctrine and engagement bands, the fire gate, accuracy, cover scoring, fall back, squad
// roles, suppression and flanking, grenades.

class CAI_Stalker;
class CCoverPoint;
class CEntityAlive;
class CGameObject;
class CScriptGameObject;
class IGameObject;
struct SHit;

template <typename _object_type>
class CPropertyEvaluator;
template <typename _object_type>
class CActionBase;

namespace DaAi
{
struct SquadState;
namespace Comms
{
struct Message;
}

// [ai:tactics] begin state
// A step's time share (P10): the first item of a step always runs, the next ones only while the step has taken less
// than its budget; the rest wait for the next update, so no geometry makes one step long.
class StepBudget
{
    using clock = std::chrono::steady_clock;

    clock::time_point m_start{clock::now()};
    float m_budget_us;
    bool m_started{};

public:
    explicit StepBudget(float budget_us) : m_budget_us(budget_us) {}

    bool next()
    {
        if (!m_started)
        {
            m_started = true;
            return true;
        }
        return std::chrono::duration<float, std::micro>(clock::now() - m_start).count() < m_budget_us;
    }
};

// The covers of a disk (or a ring: inner > 0) in square tiles, one tile per call of Tactics::next_cover_tile: a tile's
// covers come from the circle around its square and only those inside the square are kept, so every cover of the disk
// comes once, and a wide disk is walked over several steps.
struct CoverTiles
{
    Fvector centre{};
    float radius{};
    float inner{};
    float side{}; // of a square
    float query{}; // radius of a tile's circle
    Fvector corner{}; // of the grid, the lowest x and z
    u16 per_row{};
    u16 next{};
    bool active{};
};

// Weapon doctrine toward the selected enemy (human enemies only).
struct Doctrine
{
    bool active{}; // feature on, a human enemy and a firearm
    bool out_of_band{}; // outside [min, engage_max] and no covered approach
    bool too_far{}; // beyond engage_max with a covered approach the profile's aggression takes
    bool too_close{}; // closer than the weapon's min
    float distance{}; // known distance
    WeaponBand band{};
};

// Runtime only: reaction and settle timers of the selected enemy, the own last-seen point (the only suppression
// target, F5), the doctrine cache and the cover bookkeeping.
struct TacticsState
{
    // the fire gate of the object handler, decided on the main thread each frame (prepare_fire)
    bool fire_ok{true};

    // selected enemy and own sight of it
    u16 target{u16(-1)};
    bool seeing{};
    bool react_counted{};
    // the gate stayed shut for an order or the doctrine after the identification: not a reaction sample
    bool react_held{};
    bool suppress_roll{};
    u32 t_identified{};
    u32 t_acquired{};
    u32 t_fire_ready{};
    u32 t_last_seen{};
    u32 t_sample{};
    // the reaction the gate waited for at the identification (rolled base + add-ons) and its rolled base
    u32 react_due_ms{};
    u32 react_base_ms{};
    // An aimed fire command the gate refused only for the reaction still running: prepare_fire gives it in the frame
    // the reaction ends instead of at the next brain update (defer_fire). Cleared at every brain update.
    struct DeferredFire
    {
        u16 target{u16(-1)};
        u16 weapon{u16(-1)};
        u32 queue_size_min{};
        u32 queue_size_max{};
        u32 queue_interval_min{};
        u32 queue_interval_max{};
        float angle{};
    } deferred_fire;
    Fvector last_seen_pos{};
    Fvector last_seen_vel{};
    struct Seen
    {
        u16 id{u16(-1)};
        u32 time{};
        Fvector position{};
    };
    std::array<Seen, 4> recent_seen{}; // earlier targets: last own sight and where

    // doctrine, evaluated at most once per frame
    u32 doctrine_frame{u32(-1)};
    Doctrine doctrine{};
    bool approach_active{};
    u32 approach_run_time{};
    u32 approach_time{};
    Fvector approach_from{};
    Fvector approach_start{};
    float approach_floor{};
    const CCoverPoint* approach_cover{}; // plain cover points only: they live as long as the level

    // cover scoring context, once per frame
    u32 score_frame{u32(-1)};
    WeaponBand score_band{};
    float score_aggr{};
    float score_cover{};
    Fvector score_threat{};
    bool score_exposed{}; // the NPC stands exposed toward score_threat

    // cover and fall back
    u32 alert_time{};
    u32 hit_time{};
    u32 hit_in_cover_time{};
    u32 no_cover_time{};
    const CCoverPoint* held_cover{}; // compared only, never dereferenced
    float held_value{}; // its cover value toward the belief when the NPC reached it
    u32 fallback_frame{u32(-1)};
    bool fallback_result{};
    bool fallback_active{};
    u32 fallback_done_time{};
    bool peeked{};
    bool peek_crouch{};
    Fvector last_peek{};

    // role given by the believed leader, applied on delivery of his order
    Role role{Role::opportunist};
    u32 role_until{};
    u32 role_time{}; // delivery of the current order
    u8 flank_side{}; // 0 left, 1 right of the line from the belief to the flanker
    bool flank_counted{};
    const CCoverPoint* flank_target{}; // kept once chosen, so the flank ends at it
    const CCoverPoint* flank_via{}; // the first leg's cover of a two-leg route, null once passed or for one leg
    bool flank_searched{}; // the covered route of the current order was looked for
    bool flank_running{}; // the ordered flank is under way (detour started toward flank_target)
    u32 flank_start{};

    // the leader's search for a covered flank route, one step per update
    struct FlankSearch
    {
        static constexpr u8 via_max = 4;
        static constexpr u8 candidates_max = 32;
        u16 flanker{u16(-1)};
        u8 stage{}; // 0 = idle
        u8 side{}; // the side tried first
        float limit{};
        Fvector threat{};
        const CCoverPoint* via[via_max]{};
        float via_open[via_max]{};
        u8 via_count{}; // u8(-1) = the covers of the first leg not looked for yet
        u8 next{}; // the next of them to try a second leg from
        // The leg under way: its covers found by geometry tile by tile, best first; the agent's checks and the walks
        // over the open ground to them run a few per update after it ([ai_tactics] flank_agent_per_update,
        // flank_walks_per_update), every step within flank_step_budget_us.
        struct Leg
        {
            const CCoverPoint* cover[candidates_max]{};
            float value[candidates_max]{}; // geometry terms; the agent's danger weight is applied when judged
            CoverTiles tiles; // the gather under way
            Fvector origin{}; // the search centre: the NPC, or the cover the route passes first
            Fvector anchor{}; // the NPC when the gather began
            WeaponBand band{};
            u8 count{};
            u8 next{}; // the next cover to judge
            u8 judged{}; // covers that passed the agent's checks (at most flank_checks)
            u8 found{}; // vias kept so far
            bool gathered{};
            const CCoverPoint* best{};
            float best_value{};
            float best_open{};
        } leg;
    } flank_search;
    // an ordered flanker's own search for its route (plan_flank), in the same steps
    FlankSearch::Leg flank_leg;
    // men of the current flank decision whose covered route the leader looked for in vain
    static constexpr u8 flank_tried_max = 4;
    u16 flank_tried[flank_tried_max]{};
    u8 flank_tried_count{};
    // the leader chose a grenade flush instead of a flank: his next throw needs no roll
    u32 flush_order_until{};
    // a mate is exposed (flanking, asking for cover, reloading, withdrawing): told or seen
    u32 cover_fire_until{};
    // a mate was ordered to cover the squad
    u32 suppressor_until{};
    u32 mate_scan_time{};
    // suppression bursts of DaSuppressFire
    u32 burst_until{};
    u32 pause_until{};
    Fvector suppress_offset{};

    // the NPC's last round: its leader hears which of his men are in the fight
    u32 t_shot{};

    // fire on order: an undetected squad holds its fire until the leader's order
    u8 order{}; // Tactics::Order
    u32 order_start{};
    u32 engage_time{};
    u32 fired_on_time{}; // a mate reported being fired on
    u32 lead_time{}; // the leader's last role decision
    bool was_reloading{};

    // return fire: caught in the open at close range, it shoots back before it moves to cover
    u32 return_fire_until{};
    u32 return_fire_next{};

    // grenades: the belief of the selected enemy stays put (a flush target)
    u16 static_target{u16(-1)};
    u32 static_since{};
    Fvector static_pos{};
    u32 gren_roll_time{};
    bool gren_roll_ok{};
    u16 heard_grenade{u16(-1)}; // the last grenade heard landing, reacted to once
    u32 heard_grenade_time{};

    // short guns: open metres crossed toward the enemy in this fight (the per-fight budget)
    float open_spent_m{};
    u32 fight_time{};
    // a short gun's covered way to a goal of the search (covered_step)
    u8 step_kind{};
    u32 step_time{};
    u32 step_hold_since{};
    Fvector step_goal{};
    Fvector step_from{};
    const CCoverPoint* step_cover{};

    // counters
    u32 stat_time{};
    Fvector stat_pos{};
};

// The leader's plan of the squad; members act only on the orders delivered to them.
struct TacticsSquad
{
    u16 planner{u16(-1)}; // the leader who made the plan
    u16 flanker{u16(-1)};
    u32 flank_time{};
    // the covered route the leader chose for the flanker (the order names the place to go)
    const CCoverPoint* flank_target{};
    const CCoverPoint* flank_via{};
    float flank_open{}; // open metres of the whole route
    u32 flank_fail_time{}; // no covered route: another tactic was chosen
    u16 suppressor{u16(-1)};
    u32 suppress_time{};
};
// [ai:tactics] end state

namespace Tactics
{
enum class FireMode : u8
{
    none,
    aimed,
    suppress
};

// Fire on order of an engagement: none (not decided yet or not a fire_on_order profile), holding, released.
enum class Order : u8
{
    none,
    holding,
    released
};

TacticsState* create(CAI_Stalker& npc);
void destroy(TacticsState* state);
TacticsSquad* create_squad(SquadState& squad);
void destroy_squad(TacticsSquad* state);
void update(CAI_Stalker& npc, float dt);
void squad_update(SquadState& squad);
void on_hit(CAI_Stalker& npc, const SHit& hit);
void on_death(CAI_Stalker& npc, IGameObject* who);
void on_message(CAI_Stalker& npc, const Comms::Message& message);
void level_unload();

void on_own_shot(CAI_Stalker& npc);
// A combat action commands aimed fire at the target it sees: the end of the reaction (t_react_ms).
void on_fire_decision(CAI_Stalker& npc);
// A combat action's aimed fire command at the enemy it sees that waits only for the reaction to run out
// (reaction_running) or the head to turn to it: prepare_fire gives it in the frame both hold (the head within angle of
// the known position) rather than at the next brain update, which added up to a schedule period to the profile's
// reaction (B2).
bool reaction_running(const CAI_Stalker& npc, const CEntityAlive& enemy);
void defer_fire(CAI_Stalker& npc, const CEntityAlive& enemy, const CGameObject* weapon, u32 queue_size_min,
    u32 queue_size_max, u32 queue_interval_min, u32 queue_interval_max, float angle);
// A deliberate hold of fire at a seen enemy (an ordered flanker without flank_fire): not a reaction sample.
void hold_fire(CAI_Stalker& npc);
FireMode fire_mode(const CAI_Stalker& npc, const CEntityAlive* enemy);
// The doctrine's no-fire rule alone: a human target known farther than the active weapon's engage_max.
bool beyond_band(const CAI_Stalker& npc, const CEntityAlive& enemy);
float accuracy_mult(const CAI_Stalker& npc);
Role role(const CAI_Stalker& npc);
// Engagement band of the active weapon.
WeaponBand band(const CAI_Stalker& npc);

// Hook blocks of ai_stalker_fire.cpp: return true when tactics decided the result.
bool hook_fire_make_sense(CAI_Stalker& npc, bool& result);
bool hook_too_far(CAI_Stalker& npc, const Fvector& position, bool& result);

// planner: replaced stock evaluators, new evaluators and operators of the combat planner
CPropertyEvaluator<CScriptGameObject>* make_too_far_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_should_throw_grenade_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_out_of_band_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_suppress_role_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_should_fall_back_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_return_fire_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_flank_role_evaluator(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_suppress_fire_action(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_fall_back_action(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_hold_ambush_action(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_return_fire_action(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_flank_action(CAI_Stalker* object, pcstr name);

// Lane internals, used by the stock files the lane owns (combat actions, cover evaluators, object handler, agent
// location manager) and by its own files.

// [ai_tactics] of dead_air_x64_ai.ltx; the initial values are the compiled-in defaults.
struct Config
{
    // doctrine
    float approach_min_gain_m{5.f};
    float approach_aggr_min{.35f};
    float approach_radius_m{30.f};
    u32 approach_search_ms{1500};
    float approach_moved_m{2.f};
    float approach_threat_moved_m{5.f};
    float approach_cover_max{.5f};
    u32 approach_max_ms{20000};
    float fire_on_order_max_s{8.f};
    float reload_window_s{2.f};

    // reaction, settle, accuracy
    u32 react_blink_ms{400};
    float react_reacquire_s{4.f};
    float react_reacquire_mult{.35f};
    float react_dist_ms{1.5f};
    float react_peripheral_deg{35.f};
    float react_peripheral_ms{150.f};
    float react_fear_mult{.5f};
    float react_fatigue_mult{.2f};
    float alert_memory_s{10.f};
    float settle_start_mult{3.f};
    float settle_fear_mult{1.f};
    float settle_fatigue_mult{.3f};
    float acc_suppression_mult{1.f};
    float acc_wound_mult{.5f};
    float acc_fatigue_mult{.3f};
    float acc_lateral_mult{.5f};
    float acc_lateral_ref{5.f};

    // suppression (F5)
    u32 supp_classes{}; // bit per WeaponClass
    float supp_floor_m{2.f};
    float supp_slope{.2f};
    float supp_order_window_s{12.f};
    float supp_precise_m{2.5f};
    float supp_near_m{2.5f};

    // cover
    float cover_open_value{.6f};
    float cover_min_enemy_m{3.f};
    float lof_low_max{.4f};
    float short_range_engage_max{100.f};
    float exposure_open_limit_m{6.f};
    float exposure_gap_m{2.f};
    float open_fight_budget_m{12.f};
    u32 step_hold_ms{15000};
    float exposure_unknown_share{.5f};
    float score_line_of_fire{.5f};
    float score_band{2.f};
    float score_aggr{1.f};
    float score_aggr_range_m{30.f};
    float score_travel{.02f};
    float score_exposure{.15f};
    float at_cover_m{1.5f};
    u32 no_cover_memory_ms{3000};
    u32 hit_in_cover_ms{1500};
    u32 pressure_ms{3000};
    float worried_supp_min{.3f};
    float reload_near_m{15.f};
    u32 fallback_cooldown_ms{8000};
    u32 fallback_max_ms{15000};
    Range return_fire_ms{2000.f, 4000.f};
    u32 return_fire_cooldown_ms{10000};
    float fallback_radius_m{30.f};
    float fallback_min_gain_m{3.f};
    float fallback_min_move_m{3.f};
    Range peek_hold_ms{1000.f, 3000.f}; // hold exposure: trained (peek 1) .. untrained (peek 0)
    float peek_avoid_min{.5f};
    float peek_reuse_m{2.f};
    u32 danger_cover_ms{120000};
    float danger_cover_m{3.f};

    // roles, cover fire, suppression bursts
    u32 role_think_ms{2000};
    float role_hold_s{20.f};
    float role_renew_s{12.f};
    float flank_role_min{.4f};
    float flank_short_bonus{.2f};
    float flank_exo_mult{.5f};
    float flank_repeat_s{30.f};
    float flank_belief_s{15.f};
    float flank_min_enemy_m{15.f};
    float flank_radius_m{55.f};
    float flank_open_limit_m{20.f};
    float flank_via_radius_m{25.f};
    u32 flank_checks{12}; // at most TacticsState::FlankSearch::candidates_max
    u32 flank_vias{4}; // at most TacticsState::FlankSearch::via_max
    u32 flank_agent_checks{32}; // at most TacticsState::FlankSearch::candidates_max
    u32 flank_agent_per_update{8};
    u32 flank_walks_per_update{4};
    float flank_step_budget_us{120.f};
    float flank_tile_m{20.f};
    u32 flank_candidates{2}; // at most TacticsState::flank_tried_max
    float flank_retry_s{10.f};
    float flank_route_s{10.f};
    u32 flank_grace_ms{1500};
    u32 flank_max_ms{30000};
    u32 flank_classes{}; // bit per WeaponClass
    Range flank_angle_deg{35.f, 110.f}; // off the line from the belief to the flanker
    float supp_role_min{.3f};
    float supp_mg_bonus{.3f};
    float supp_order_s{15.f};
    float cover_fire_msg_s{6.f};
    u32 mate_scan_ms{500};
    float mate_moving_speed{1.2f};
    Range suppress_burst_ms{400.f, 1200.f};
    Range suppress_pause_ms{700.f, 1800.f};
    float suppress_spread_m{2.f};
    float order_range_mult{1.f};
    float order_supp_min{.05f};
    float bound_min_m{15.f};
    float reload_push_aggr{.5f};

    // grenades
    u32 gren_think_ms{4000};
    float grenadier_mult{2.f};
    float flush_static_s{4.f};
    float flush_move_m{3.f};
    float flush_belief_extra_s{5.f};
    float flush_cover_max{.5f};
    float indoor_cover_max{.35f};
    float grenade_min_m{10.f};
    float grenade_max_m{40.f};
    float grenade_warn_m{15.f};
    float grenade_danger_m{10.f};
    u32 grenade_danger_ms{6000};

    // counters
    u32 stat_max_dt_ms{1000};
    float stat_teleport_m{20.f};

    bool loaded{};
};

const Config& config();
TacticsState* state_of(const CAI_Stalker& npc);
const CEntityAlive* selected_enemy(const CAI_Stalker& npc);
// The combat planner of the engine runs (not a script combat scheme).
bool engine_combat(const CAI_Stalker& npc);

const Doctrine& doctrine(CAI_Stalker& npc);
// Get-distance run: true when tactics decides; cover null = stay put (no point, no move).
bool approach_begin(CAI_Stalker& npc, const CCoverPoint*& cover);
// false once tactics dropped the run (no bound, or approach_max_ms passed): the run ends where the NPC stands.
bool approach_running(const CAI_Stalker& npc);
void approach_end(CAI_Stalker& npc);

// DaShouldFallBack / DaFallBack.
bool should_fall_back(CAI_Stalker& npc, bool in_cover);
// DaReturnFire: a profile with return_fire_m, found out by an enemy it identified and sees within that range, shoots
// back from where it stands (in the open or behind the cover it already has) for [ai_tactics] return_fire_ms before
// the planner moves it to cover.
bool return_fire(CAI_Stalker& npc);
// cover = the destination is a cover point (a spot straight away over open ground is not).
bool fall_back_begin(CAI_Stalker& npc, Fvector& position, u32& vertex, bool& cover);
void fall_back_end(CAI_Stalker& npc);
void on_ambush(CAI_Stalker& npc);

// The fire gate for the object handler, which runs on a worker thread: prepare_fire decides it on the main thread
// every frame before the object handler is queued (da_ai_update_cl), fire_continues only reads it (false stops a
// running queue). Nothing the object handler calls may run Lua or fill a shared cache.
void prepare_fire(CAI_Stalker& npc);
bool fire_continues(CAI_Stalker& npc);
// Own last-seen point of the selected enemy, the only suppression target (F5).
bool suppress_point(const CAI_Stalker& npc, Fvector& point);

// Cover search.
bool cover_distances(const CAI_Stalker& npc, float& min_distance, float& max_distance);
void on_cover_search(CAI_Stalker& npc, bool found);
// Extra terms of CCoverEvaluatorBest on top of the stock value (lower is better); false rejects the cover. From a start
// on the cover itself only the terms of the cover against the threat count (travel and path exposure are zero).
bool cover_terms(const CAI_Stalker& npc, const CCoverPoint& cover, const Fvector& threat, const Fvector& start,
    float threat_distance, float current_distance, float bound, float& value);
// Agent location manager: true when tactics decides the enemy position a member keeps its cover away from;
// known = false skips an enemy the member has no belief about.
bool cover_enemy_position(const CAI_Stalker& npc, const CEntityAlive* enemy, bool& known, Fvector& position);

// Roles (T4). The agent member manager's detour quota and cover fire for a member: true when tactics decides.
bool can_detour(const CAI_Stalker& npc, bool& result);
bool cover_detouring(const CAI_Stalker& npc, bool& result);
// Holders covering a mate stay quiet (the one who needs cover asks for it).
bool quiet_cover_fire(const CAI_Stalker& npc);
// A heard reload of the enemy opens a short window to push or peek ([ai_tactics] reload_window_s).
bool reload_push(const CAI_Stalker& npc);
// DaSuppressRole and the bursts of DaSuppressFire (point = own last-seen point plus the burst's offset).
bool suppress_role(CAI_Stalker& npc);
void suppress_begin(CAI_Stalker& npc);
bool suppress_step(CAI_Stalker& npc, Fvector& point);
void suppress_end(CAI_Stalker& npc);
// Detour: the flanker's run and cover; false = the stock detour cover choice.
// true when the flanker announced its move through comms (the caller skips the stock bark).
bool on_detour_begin(CAI_Stalker& npc);
// The ordered flank ends with the detour once its cover is reached (the role with it).
void on_detour_end(CAI_Stalker& npc);
// true for the ordered flanker: cover = its covered route's end, null = no covered route (it stays).
bool flank_cover(CAI_Stalker& npc, const Fvector& threat, const CCoverPoint*& cover);
// DaFlankRole: the ordered flanker with a covered route leaves at once (a burst under way ends first, within
// [ai_tactics] flank_grace_ms); it outranks fighting from where it stands and the search while the order holds.
bool flank_go(CAI_Stalker& npc);
// DaFlank: the start of the move (count it, announce it: true when comms took the announcement), the next cover of
// the route (the via cover, then the end), its arrival (true = the end: the order is done), the time limit and the
// profile's fire on the move.
bool flank_begin(CAI_Stalker& npc);
const CCoverPoint* flank_waypoint(const CAI_Stalker& npc);
bool flank_reached(CAI_Stalker& npc, const CCoverPoint& point);
bool flank_expired(CAI_Stalker& npc);
bool flank_fire(const CAI_Stalker& npc);
// Fire on order.
Order order(const CAI_Stalker& npc);
// Grenades: ShouldThrowGrenade (true when tactics decides), the throw itself.
bool grenade_decision(CAI_Stalker& npc, bool started, bool positioned, bool& result);
void on_grenade_throw(CAI_Stalker& npc);
// The agent's danger area of a thrown grenade marks only the members who saw it or were warned.
bool grenade_danger_own_only();
// A thrown grenade heard landing near (plan 2.6): a grenade danger at the heard spot and a warning when an enemy threw
// it; once per grenade.
void on_grenade_heard(CAI_Stalker& npc, const CGameObject& grenade, const Fvector& spot);
// Fatigue: a winded NPC walks; flank = the run of a flank, which an exo suit also slows to a walk.
bool may_run(const CAI_Stalker& npc, bool flank = false);

// Combat actions.
bool take_cover_run(CAI_Stalker& npc);
u32 hold_time_ms(const CAI_Stalker& npc, u32 stock);
bool peek_stance(CAI_Stalker& npc, bool& crouch);
bool peek_avoid(const CAI_Stalker& npc, Fvector& position, float& radius);
void on_peek(CAI_Stalker& npc);
// H12: a hit marks the cover dangerous for the hit member only; mates learn of it through taking_fire.
bool danger_cover_own_only(const CAI_Stalker& npc);
// The value of the held best cover is taken from the cover itself, so the holder's own walk (look out, detour) does
// not make it worse: the scored terms that depend on the start would otherwise reset the combat chain.
bool held_cover_from_cover(const CAI_Stalker& npc);

// Short guns (shotgun, pistol: engage_max <= short_range_engage_max) never cross open ground toward the enemy.
bool short_range(const WeaponBand& weapon_band);
// A profile trained for more than opportunist fire.
bool trained(const CAI_Stalker& npc);
// The open metres a short gun may cross on one move (false: another weapon, no limit). Toward the enemy: a gap of
// exposure_gap_m, exposure_open_limit_m for a trained one while a mate suppresses, never more than is left of the
// fight's open_fight_budget_m; away or across: exposure_open_limit_m from a covered spot, no limit out of an exposed
// one (exposed: the NPC stands in the open toward the threat, where staying costs as much as the run).
bool open_limit(const CAI_Stalker& npc, bool approaching, float& limit, bool exposed = false);
// A short gun's way to a goal of the search (the stock walk is straight): direct, the next covered bound toward it
// (position, vertex), or hold and watch; given_up once it held step_hold_ms without a covered way.
enum class Step : u8
{
    direct,
    bound,
    hold,
    given_up
};
Step covered_step(CAI_Stalker& npc, const Fvector& goal, Fvector& position, u32& vertex);

// Geometry (da_ai_cover.cpp). Cover values: 0 covered .. 1 open, as the level graph stores them.
float cover_toward(u32 vertex_id, const Fvector& from, const Fvector& threat);
bool exposed(const CAI_Stalker& npc, const Fvector& threat);
// Open metres on the straight level-graph line to a point; the walk stops once stop_at is exceeded. The overload
// starts at a given vertex and position (the leg of a route from a cover).
float path_exposure(const CAI_Stalker& npc, const Fvector& to, const Fvector& threat, float stop_at);
float path_exposure(u32 start_vertex, const Fvector& start, const Fvector& to, const Fvector& threat, float stop_at);
// A short-range weapon (shotgun, pistol) does not cross more open ground toward the threat than exposure_open_limit_m:
// the stock cover searches (look out, detour, and the flanker's fallback to them) skip such a cover.
bool open_ground_refused(const CAI_Stalker& npc, const Fvector& to, const Fvector& threat);
// floor: no bound closer to the belief than this.
const CCoverPoint* find_approach_cover(CAI_Stalker& npc, const Fvector& threat, const Doctrine& doctrine, float floor);
// away: with no cover behind, a straight line away over open ground is allowed; cover = a cover point was found.
bool find_fall_back(CAI_Stalker& npc, const Fvector& threat, bool fanatic, bool away, Fvector& position, u32& vertex,
    bool& cover);
// A flank leg in steps: a cover on the given side of the line from the belief to the NPC, off it by [ai_tactics]
// flank_angle_deg, reached over at most limit open metres; from = a cover the route passes first (the leg starts
// there), null = the NPC. gather_flank_leg finds the covers by geometry, tiles of flank_tile_m while the step's budget
// lasts, and is true once every tile is done (best first, at most flank_agent_checks, in leg.count); judge_flank_leg
// gives at most checks of them the agent's checks and walks the open ground to at most walks per call (flank_checks
// of them in all), both within the budget, and is true once the leg is decided (leg.best, null = none; leg.best_open =
// its open metres). The via itself may come out as leg.best: it is no end of the route.
bool gather_flank_leg(CAI_Stalker& npc, const Fvector& threat, u8 side, const WeaponBand& weapon_band,
    const CCoverPoint* from, TacticsState::FlankSearch::Leg& leg, StepBudget& budget);
bool judge_flank_leg(CAI_Stalker& npc, const Fvector& threat, float limit, const CCoverPoint* from,
    TacticsState::FlankSearch::Leg& leg, u32 walks, u32 checks, StepBudget& budget);
// The first leg of a two-leg flank in steps: covers near the NPC on the side, covered toward the belief and not closer
// to it than the weapon allows; judge_flank_vias keeps up to capacity of them reached over at most limit open metres,
// best first, in covers/open (leg.found of them) and is true once done.
bool gather_flank_vias(CAI_Stalker& npc, const Fvector& threat, u8 side, const WeaponBand& weapon_band,
    TacticsState::FlankSearch::Leg& leg, StepBudget& budget);
bool judge_flank_vias(CAI_Stalker& npc, const Fvector& threat, float limit, TacticsState::FlankSearch::Leg& leg,
    u32 walks, u32 checks, StepBudget& budget, const CCoverPoint** covers, float* open, u8 capacity);
// Square tiles of flank_tile_m / plan_tile_m over a disk or ring (tile_m: the radius of a tile's circle); the disk in
// one query when it is no wider than tile_m.
void start_cover_tiles(CoverTiles& tiles, const Fvector& centre, float radius, float inner, float tile_m);
// The covers (smart covers left out) of the next tile that meets the disk or ring; false once every tile is done.
bool next_cover_tile(CoverTiles& tiles, xr_vector<CCoverPoint*>& covers);
// A short gun's next covered bound toward a goal of the search, null = none.
const CCoverPoint* find_step_cover(CAI_Stalker& npc, const Fvector& goal, const Fvector& threat, float limit);
// Side (0 left, 1 right) of a point relative to the line from the threat through the anchor.
u8 side_of(const Fvector& threat, const Fvector& anchor, const Fvector& point);
} // namespace Tactics
} // namespace DaAi
