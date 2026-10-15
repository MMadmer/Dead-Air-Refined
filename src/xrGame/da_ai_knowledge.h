// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_ai.h"

// Knowledge lane: per-NPC beliefs about other objects, the honest replacements of the ground-truth reads, the
// EnemySeeMe estimate and the .scov chunk AIK1. With the knowledge feature off (or for an NPC without AI state)
// every function answers what the stock engine answered.

class CAI_Stalker;
class CCustomMonster;
class CEntityAlive;
class CGameObject;
class CScriptGameObject;
class IGameObject;

template <typename _object_type>
class CPropertyEvaluator;

namespace MemorySpace
{
struct CVisibleObject;
struct CSoundObject;
struct CHitObject;
struct CNotYetVisibleObject;
struct CMemoryInfo;
} // namespace MemorySpace

namespace DaAi
{
// Serialized in AIK1 and returned to Lua (ai_belief_source): values are append-only.
enum class Source : u8
{
    none,
    seen,
    heard_shot,
    heard_step,
    heard_reload,
    heard_item,
    heard_voice,
    hit,
    near_miss,
    muzzle_flash,
    flashlight,
    report_voice,
    report_hushed,
    report_radio,
    report_gesture,
    report_pda,
    report_hq,
    witness,
    script,
    restored
};

struct Estimate
{
    Fvector pos{}; // realised position (truth + error at observation time), on a valid level vertex
    float err{}; // radius the NPC itself assumes
    u32 t_observed{};
    u16 target{u16(-1)};
    u16 reporter{u16(-1)};
    Source source{Source::none};
    float confidence{};
    u32 vertex{u32(-1)}; // level vertex of pos; observe() resolves it when the sender leaves it invalid
};

struct Belief
{
    Estimate e;
    Fvector vel{}; // last seen velocity, extrapolated for [ai_errors] extrap_s after the target left own sight
    u32 t_received{};
    u32 t_seen_own{}; // last time this NPC's own vision confirmed the target (0 = never)
    u32 t_seen_frame{};
    bool enemy{};
};

struct KnowScratch;

// [ai:know] begin state
struct KnowState
{
    xr_vector<Belief> beliefs; // at most [ai_errors] beliefs_max, creatures only
    // Object id -> time this NPC's own vision last confirmed it; the "I see him" bit of the shared visual memory is
    // cleared from this, never from the group's shared time (W2).
    xr_flat_hash_map<u16, u32> own_seen;
    u32 own_seen_stamp{};
    // Aim roll per target: the bone and the lead error stay stable for a while instead of changing every frame.
    u16 aim_target{u16(-1)};
    u32 aim_roll_time{};
    bool aim_head{};
    float aim_lead{1.f};
    u16 aim_fallback_target{u16(-1)};
    // enemy_sees_me: the last LOS ray from the enemy's eyes to this NPC's head, reused for [ai_errors] see_me_ray_s.
    u16 see_me_target{u16(-1)};
    u32 see_me_time{};
    bool see_me_los{};
    // The script's enemy callback per target, reused until [ai_errors] enemy_callback_ms runs out (a Lua call per
    // candidate and memory update otherwise; the stock checks before it still run every time).
    struct Callback
    {
        u32 until{};
        u16 id{u16(-1)};
        bool useful{};
    };
    xr_vector<Callback> callbacks;
    u32 relations_time{}; // next refresh of the beliefs' enemy flags ([ai_errors] relation_refresh_ms)
    u32 maintain_time{}; // next ageing pass over the beliefs ([ai_errors] maintain_ms)
    KnowScratch* scratch{}; // Lua list getters, created on first use
};
// [ai:know] end state

namespace Know
{
KnowState* create(CAI_Stalker& npc);
void destroy(KnowState* state);
void update(CAI_Stalker& npc, float dt);
void on_death(CAI_Stalker& npc, IGameObject* who);
void level_unload();
// Level-wide data the knowledge lane builds once per level (the vertex search's column index), at the first spawn.
void level_prepare();

// True while knowledge decides for this NPC: the feature is on and the NPC has live AI state.
bool active(const CAI_Stalker& npc);

void on_own_shot(CAI_Stalker& npc);
void observe(CAI_Stalker& npc, const CGameObject* target, const Estimate& estimate);
// danger_type / perceive_type: CDangerObject::EDangerType / EDangerPerceiveType. Builds a danger object of the NPC
// at the estimate (never at the object's live position); the one way a sense turns a stimulus into a danger.
void suspect(CAI_Stalker& npc, const Estimate& estimate, u32 danger_type, u32 perceive_type);
const Belief* belief(const CAI_Stalker& npc, u16 id);
bool sees_now(const CAI_Stalker& npc, const CGameObject* object);
// True when the NPC sees a live creature or holds a belief about it; things and corpses are world state (true).
bool knows(const CAI_Stalker& npc, const CGameObject* object);
// Position the NPC believes of a creature (the exact one while it sees it; x, z of the position and y of the centre,
// as the stock memory entries); things and unbelieved corpses answer their position. An unknown live creature: the
// NPC's own position, known_distance -1, known_direction zero.
Fvector known_position(const CAI_Stalker& npc, const CGameObject& object);
float known_distance(const CAI_Stalker& npc, const CGameObject& object);
Fvector known_direction(const CAI_Stalker& npc, const CGameObject& object);
u32 last_seen_time(const CAI_Stalker& npc, const CGameObject* object);
// The NPC's own eyes saw the object now or within `ms`.
bool seen_own_within(const CAI_Stalker& npc, const CGameObject& object, u32 ms);
// Time of the last hit the NPC itself took from the enemy (0 = none). The agent's hit entry is shared by the group
// and refreshed by every member's hit, so a mate's wound reaches this NPC only as a message (F2).
u32 hit_time(const CAI_Stalker& npc, const CEntityAlive* enemy);
// The sender's own belief degraded by its age, the payload of a message (source none = no belief).
Estimate report_of(const CAI_Stalker& sender, u16 id);
void rumour(CAI_Stalker& npc, const CEntityAlive* object, float err, Source source);
// The NPC's own estimate of "the enemy sees me" (W4): only an enemy in own sight shows where it looks; true when this
// NPC is inside its facing cone and a ray from its eyes reaches this NPC's head. An enemy out of own sight answers
// [ai_errors] see_me_unknown (cautious by default).
bool enemy_sees_me(const CAI_Stalker& npc, const CEntityAlive& enemy);
void forget(CAI_Stalker& npc, u16 id);
// Lua ai_inform: an honest scripted report at a position.
void inform(CAI_Stalker& npc, const CGameObject* target, const Fvector& position, float err, Source source);

// Error model of [ai_errors] (plan 2.5), shared by the senses and comms: err = e0 + k * distance for a source (for
// report_* the describe_k part of the channel); realise() places an estimate of a true position with that assumed
// error and resolves its level vertex. Estimate::err is the error at t_observed: effective_error() adds the age
// growth up to now, so nobody adds it twice. report_grid() is the quantisation step of a channel, metres.
// anchor: where a spot off the graph is snapped back toward (resolve_vertex), the observer when null; resolve false
// leaves the vertex invalid for a caller that moves the spot again before it resolves it.
float source_error(Source source, float distance);
float effective_error(const Estimate& estimate);
float report_grid(Source channel);
Estimate realise(const CAI_Stalker& observer, const CGameObject* target, const Fvector& truth, Source source,
    float err, float clarity = 1.f, const Fvector* anchor = nullptr, bool resolve = true);
// A spot on the level graph for any position: the vertex containing it, else the nearest one (pos is moved there).
// anchor: a point on the graph (the observer) toward which a spot far off the graph is snapped at a bounded cost.
u32 resolve_vertex(Fvector& pos, const Fvector* anchor = nullptr);
// The vertex under a position, else the nearest one, without moving it. No graph cell within [ai_errors]
// vertex_search_m: the first cell on the way back to the anchor, else the stock nearest-vertex search.
u32 find_vertex(const Fvector& pos, const Fvector* anchor = nullptr);

// The script's enemy callback of CEnemyManager::useful: true = result holds this NPC's recent answer for the target.
bool cached_enemy_callback(const CAI_Stalker& npc, u16 id, bool& result);
void store_enemy_callback(const CAI_Stalker& npc, u16 id, bool result);
void forget_enemy_callback(const CAI_Stalker& npc, u16 id);

// Hook lines of the visual memory manager: return true when knowledge decided the result. last_update is the time of
// the NPC's last visual update, where its visible bits were last judged.
bool hook_visible_now(const CAI_Stalker& npc, const CGameObject* object, u32 last_update, bool& result);
bool hook_visible_right_now(const CAI_Stalker& npc, const CGameObject* object, u32 last_update, bool& result);
bool hook_time_last_seen(const CAI_Stalker& npc, const IGameObject* object, u32& result);
// The still-visible clear of the visual memory update; true = the entry was handled.
bool hook_make_invisible(const CAI_Stalker& npc, MemorySpace::CVisibleObject& object, u64 mask);
// Every object the NPC's own vision (or a fictitious addition) puts into the visual memory.
void on_add_visible(CAI_Stalker& npc, const CGameObject& object, bool fictitious);

// Memory lookups of CMemoryManager for a stalker: true = knowledge answered (the result may be "unknown").
bool memory_info(const CAI_Stalker& npc, const IGameObject* object, MemorySpace::CMemoryInfo& result);
bool memory_time(const CAI_Stalker& npc, const IGameObject* object, u32& result);
bool memory_position(const CAI_Stalker& npc, const IGameObject* object, Fvector& result);
// Enemy candidates of one memory update from the NPC's beliefs; out of combat only the kinds the stock engine took
// from its visual memory (own sight and the told ones), in combat every kind.
void add_candidates(CAI_Stalker& npc, bool in_combat);
// Believed live creatures, for the agent's enemy distribution.
void known_objects(const CAI_Stalker& npc, xr_vector<const CEntityAlive*>& result);
// While the hearing sense is off, a heard or hit entry of the NPC's own (its mask bit) stands in for the estimate
// hearing would make: the stock exactness, only for this NPC.
void observe_pooled(CAI_Stalker& npc, const MemorySpace::CSoundObject& entry);
void observe_pooled(CAI_Stalker& npc, const MemorySpace::CHitObject& entry);

// Aim (W1, W10): true = knowledge chose the point. A target out of own sight is aimed at the belief, a seen actor
// or live stalker at the profile's rolled bone (head with aim_head, else the body) unless a script set the bone.
bool aim_point(const CAI_Stalker& npc, const CGameObject& target, Fvector& result);
// Look point of an object sight: true only when the target is out of own sight (the belief, never the object).
bool look_point(const CAI_Stalker& npc, const CGameObject& target, Fvector& result);
// Velocity lead factor of the current target roll (1 + lead error sample of the profile).
float lead_factor(const CAI_Stalker& npc, const CGameObject& target);
// The virtual finishing hit of the kill-wounded action needs the victim within reach of what the NPC knows.
bool may_finish_unseen(const CAI_Stalker& npc, const CEntityAlive& victim, float reach);

// Lua list getters (memory_visible_objects etc.): the NPC's filtered list, null = the stock list.
const xr_vector<MemorySpace::CVisibleObject>* script_visible_objects(CCustomMonster& monster);
const xr_vector<MemorySpace::CSoundObject>* script_sound_objects(CCustomMonster& monster);
const xr_vector<MemorySpace::CHitObject>* script_hit_objects(CCustomMonster& monster);
const xr_vector<MemorySpace::CNotYetVisibleObject>* script_not_yet_visible_objects(CCustomMonster& monster);

// .scov chunk AIK1 (da_ai_know_save.cpp)
void save_begin(ChunkCapture& state);
bool save_continue(ChunkCapture& state, float budget_ms);
bool stage(const xr_vector<u8>& payload);
void clear_staged();
void forget_saved(u16 object_id);
// create() moves the staged records of the new NPC (id and section CRC match) into its state; update() calls
// expire_staged(), which drops the records nobody claimed once the level has run [ai_errors] restore_grace_s.
void apply_staged(const CAI_Stalker& npc, KnowState& state);
void expire_staged();

// [ai_errors] values the AIK1 code needs.
struct SaveParams
{
    u32 forget_enemy_ms{};
    u32 forget_other_ms{};
    u32 beliefs_max{};
    u32 restore_grace_ms{};
    float age_growth{};
    float age_cap{};
};
SaveParams save_params();

// planner (replaces the stock EnemySeeMe evaluator of the combat planner)
CPropertyEvaluator<CScriptGameObject>* make_enemy_see_me_evaluator(CAI_Stalker* object, pcstr name);
} // namespace Know
} // namespace DaAi
