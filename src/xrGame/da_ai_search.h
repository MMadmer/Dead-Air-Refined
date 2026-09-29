// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai_knowledge.h"

// Search lane: lost-contact search from the last known or reported position, the squad spot pool, pairs and
// overwatch, quiet search, staggered give-up, investigation of suspicion.

class CAI_Stalker;
class CScriptGameObject;
class IGameObject;
class CPropertyStorage;

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

// [ai:search] begin state
enum class SearchRole : u8
{
    none,
    sweep, // walk to the spot and look into it
    overwatch // cover the partner who sweeps the spot
};

// A place near the last known position where the target could hide.
struct SearchSpot
{
    Fvector pos{};
    u32 vertex{u32(-1)};
    float order{}; // lower = searched earlier (escape direction, distance)
    // Pool bookkeeping of the coordinator: who it sent there and when.
    u32 t_assigned{};
    u16 sweeper{u16(-1)};
    u16 overwatch{u16(-1)};
    u8 tries{};
    bool cleared{};
};

// Spots around one last known position; a new position or a wider ring is a new generation.
struct SearchPlan
{
    xr_vector<SearchSpot> spots;
    Fvector lkp{};
    Fvector escape{}; // estimated escape direction (horizontal unit vector, zero = unknown)
    float radius{};
    u32 t_lkp{};
    u8 generation{};
    u8 widened{};
    bool lkp_spot{}; // spots[0] is the last known position itself
};

struct SearchTask
{
    Fvector pos{};
    Fvector post{}; // overwatch post
    u32 vertex{u32(-1)};
    u32 post_vertex{u32(-1)};
    u32 t_given{};
    u16 target{u16(-1)};
    u16 coordinator{u16(-1)}; // u16(-1) = a spot of the NPC's own plan
    u8 generation{};
    u8 index{};
    SearchRole role{};
    bool lkp{}; // the spot is the last known position
};

enum class InvestigateRole : u8
{
    none,
    go, // walks to the spot and looks into it
    cover, // a mate who goes asked it to cover him
    hold // stays in cover and watches: a mate goes, or the profile waits and calls out
};

// Investigation of the NPC's selected danger (a suspicion); the spot is the NPC's own estimate of the source.
struct Investigation
{
    Fvector spot{};
    u32 vertex{u32(-1)};
    u32 t_start{};
    u32 t_go{}; // the NPC left its cover for the spot
    u32 t_mate{}; // cover and hold: since when it waits for its mate
    u32 t_torch{}; // last torch check
    u16 object{u16(-1)}; // the entity the danger names, u16(-1) = none
    u16 mate{u16(-1)}; // go: the partner asked to cover; cover and hold: the mate who goes or gave the order
    InvestigateRole role{};
    bool active{};
    bool called_out{};
    bool ordered{}; // a leader already sent two men
    bool torch_counted{};
};

// What a mate said about an investigation (search_assign with the investigation code); it applies to the NPC's own
// danger while it is fresh.
struct InvestigateNews
{
    u32 time{};
    u16 origin{u16(-1)}; // the mate who goes or gave the order
    bool cover{}; // this NPC is to cover
    bool go{}; // this NPC is sent to look
};

struct SearchState
{
    Investigation investigation; // danger state; kept when a search ends
    InvestigateNews news;
    SearchPlan own; // own plan, searched while no assignment reaches the NPC
    SearchTask task;
    SearchTask pending; // assignment delivered before the NPC's search began
    Fvector lkp{};
    Fvector escape{};
    u32 lkp_vertex{u32(-1)};
    float lkp_err{};
    u32 t_lkp{}; // observation time of the last known or reported position
    u32 t_start{};
    u32 duration_ms{}; // search_s (sworn enemies longer) plus the NPC's own give-up stagger
    u32 t_idle{}; // last task finished: waiting for the next assignment
    u32 t_unselected{}; // the target stopped being the selected enemy
    u32 t_given_up{};
    u32 t_torch{}; // last torch check of this search
    u16 given_up{u16(-1)}; // the last target the NPC gave up on: searched again only after newer information
    u16 engaged{u16(-1)}; // the selected enemy of the last engine-combat update, for r_drop
    u16 target{u16(-1)};
    Source lkp_source{};
    u8 ray_cursor{};
    bool active{};
    bool torch_counted{};
    bool coordinated{}; // tasks come from the squad pool (an assignment arrived, or the NPC coordinates)
    bool lkp_checked{}; // the NPC's own vision cleared the last known position
};

struct SearchPair
{
    u16 a{u16(-1)};
    u16 b{u16(-1)};
    bool flip{}; // the members swap sweeper and overwatch every spot
};

// The squad's spot pool for one target: the plan of its coordinator, handed out by search_assign messages.
struct SearchPool
{
    SearchPlan plan;
    xr_vector<SearchPair> pairs;
    u32 t_coordinator_lost{};
    u16 target{u16(-1)};
    u16 coordinator{u16(-1)};
};

struct SearchGiveUp
{
    u32 first{};
    u32 last{};
    u16 target{u16(-1)};
    u16 count{};
};

struct SearchSquad
{
    xr_vector<SearchPool> pools;
    xr_vector<SearchGiveUp> give_ups;
    bool due{}; // the squad step of this lane runs at the member update after the squad tick (squad_tick)
};
// [ai:search] end state

namespace Search
{
SearchState* create(CAI_Stalker& npc);
void destroy(SearchState* state);
SearchSquad* create_squad(SquadState& squad);
void destroy_squad(SearchSquad* state);
void update(CAI_Stalker& npc, float dt);
void squad_update(SquadState& squad);
void on_death(CAI_Stalker& npc, IGameObject* who);
void level_unload();

// Lua ai_searching: a lost-contact search, or an investigation the NPC walks or covers.
bool active(const CAI_Stalker& npc);
// DaContactLost: the selected enemy is out of own sight and the NPC has had no newer own, heard or told estimate of
// him for its profile's search_delay_s.
bool contact_lost(const CAI_Stalker& npc);
// The handover from the ambush chain: starts (or refreshes) the search on that enemy.
void on_contact_lost(CAI_Stalker& npc, u16 id);
void on_message(CAI_Stalker& npc, const Comms::Message& message);
// quiet_search profile while searching or investigating: no search barks, orders hushed.
bool quiet(const CAI_Stalker& npc);
// Pause after a search bark of a loud searcher ([ai_search] bark_pause_s), ms.
void bark_pause(u32& max_stop, u32& min_stop);
// A quiet searcher or investigator close to its spot sneaks up crouched.
bool sneak(const CAI_Stalker& npc, const Fvector& spot);

// The search sub-planner and the stock search actions.
void begin(CAI_Stalker& npc); // the search sub-planner starts or resumes on the selected enemy
const SearchState* state(const CAI_Stalker& npc);
const SearchTask* task(const CAI_Stalker& npc); // null without a task
bool wants_sweep(const CAI_Stalker& npc);
bool wants_overwatch(const CAI_Stalker& npc);
bool holds_in_place(const CAI_Stalker& npc); // search_style none: wait where it is
bool lkp_checked(const CAI_Stalker& npc);
// The stock ambush hold is over; true = the search decides the give-up, not the stock timer.
bool on_hold_complete(CAI_Stalker& npc);

// Investigation of a suspicion: the actions of the danger-in-direction sub-planner. With the search feature off every
// function answers the stock value.
// Take cover and look out are skipped: the NPC advances straight at the source or is on its way already.
bool investigate_skip_cover(CAI_Stalker& npc);
// The hold after looking out: stock_leave = the stock rule (time is up and the agent allows a detour); true = go.
bool investigate_leave(CAI_Stalker& npc, bool completed, bool stock_leave);
// The detour around the source is skipped (nothing to detour around, or advancing straight).
bool investigate_skip_detour(CAI_Stalker& npc);
// The NPC's investigation while it walks to the spot, null otherwise.
const Investigation* investigating(CAI_Stalker& npc);
// The walker checks the spot by its own eyes; true = done (its danger is dropped, the mates are told).
bool investigate_check(CAI_Stalker& npc);

// planner: DaContactLost and DaSearchHandover of the combat planner, the evaluators and operators of the search
// sub-planner (stalker_search_planner.cpp)
CPropertyEvaluator<CScriptGameObject>* make_contact_lost_evaluator(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_search_handover_action(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_search_spot_evaluator(CAI_Stalker* object, pcstr name);
CPropertyEvaluator<CScriptGameObject>* make_search_cover_evaluator(CAI_Stalker* object, pcstr name);
CActionBase<CScriptGameObject>* make_search_sweep_action(
    CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name);
CActionBase<CScriptGameObject>* make_search_overwatch_action(
    CAI_Stalker* object, CPropertyStorage* combat_storage, pcstr name);
} // namespace Search
} // namespace DaAi
