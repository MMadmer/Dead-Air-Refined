// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Per-NPC and per-squad state of the human NPC AI. Runtime only: nothing here is saved except what the know lane
// writes to AIK1 and the trait table writes to AIT1. Offline NPCs have no state.

class CAI_Stalker;

namespace DaAi
{
struct Profile;
// Lane state, defined in the lane headers.
struct KnowState;
struct SenseState;
struct CommsState;
struct TacticsState;
struct MoraleState;
struct SearchState;
struct CommsSquad;
struct TacticsSquad;
struct MoraleSquad;
struct SearchSquad;

struct NpcState
{
    const Profile* profile{}; // cached, re-resolved when the community index changes
    u16 squad_id{u16(-1)}; // sim squad (m_group_id) or own id
    u8 band{}; // rank band 1..4
    bool has_radio{}; // rolled once per lifetime (Traits)
    KnowState* know{};
    SenseState* sense{};
    CommsState* comms{};
    TacticsState* tactics{};
    MoraleState* morale{};
    SearchState* search{};

    // core bookkeeping
    CAI_Stalker* owner{};
    u16 id{u16(-1)};
    int community{-1}; // community index the profile was resolved for
    int rank{}; // rank value at resolution (the real rating)
    u32 squad_check_time{};
    float pre_us{}; // time of this update's pre-memory hook, for p_max_us
    // Level of detail, judged at the start of each update: beyond [ai_config] lod_near_m of the player's camera, and
    // without a selected enemy. Lanes coarsen only per-target refresh work by them, never a first reaction.
    bool distant{};
    bool calm{true};
    bool dead{};
};

struct SquadState // keyed by sim squad id, level lifetime
{
    u16 id{};
    u16 commander{u16(-1)}; // engine order: lowest id alive
    u8 community{};
    u16 leader{u16(-1)}; // highest band alive; orders, radio bonus, leader death
    xr_vector<u16> members; // online alive stalkers of the squad
    CommsSquad* comms{};
    TacticsSquad* tactics{};
    MoraleSquad* morale{};
    SearchSquad* search{};

    u32 last_tick{};
};

// Null for monsters and for NPCs without state (dead at spawn, offline).
NpcState* npc(const CAI_Stalker& npc);
NpcState* npc(u16 id);
SquadState* squad(u16 id);
SquadState* squad_of(const CAI_Stalker& npc);
// AI leader of a squad (C22): the alive member with the highest band; ties by rank value, then engine order.
u16 leader(const SquadState& squad);
bool is_leader(const CAI_Stalker& npc);
// Per-squad step, at most every [ai_config] squad_tick_ms; called from da_ai_post_memory.
void squad_tick(CAI_Stalker& npc);
// Online stalkers with state (alive and dead ones until they are destroyed).
const xr_vector<CAI_Stalker*>& online_npcs();
u32 npc_count();
u32 squad_count();

// Lifetime of the state, driven by the hooks of da_ai.cpp (core only).
namespace Registry
{
NpcState* add(CAI_Stalker& npc);
void remove(CAI_Stalker& npc);
void died(CAI_Stalker& npc);
// Re-reads the sim squad and the community every [ai_config] squad_refresh_ms.
void refresh(CAI_Stalker& npc);
void clear();
} // namespace Registry
} // namespace DaAi
