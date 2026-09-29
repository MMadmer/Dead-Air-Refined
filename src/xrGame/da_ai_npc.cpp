// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"
#include "da_ai_knowledge.h"
#include "da_ai_senses.h"
#include "da_ai_comms.h"
#include "da_ai_tactics.h"
#include "da_ai_morale.h"
#include "da_ai_search.h"

#include "ai/stalker/ai_stalker.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"

namespace DaAi
{
namespace
{
xr_vector<CAI_Stalker*> g_npcs;
xr_flat_hash_map<u16, NpcState*> g_states;
xr_flat_hash_map<u16, SquadState*> g_squads;

// The sim squad of an online stalker (CSE_ALifeMonsterAbstract::m_group_id); an NPC without one is a squad of one.
u16 sim_squad_id(const CAI_Stalker& npc)
{
    if (!ai().get_alife())
        return npc.ID();
    const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(npc.ID(), true));
    return monster && monster->m_group_id != u16(-1) ? monster->m_group_id : npc.ID();
}

// Engine commander: CSE_ALifeOnlineOfflineGroup::commander_id(), the lowest id alive of the whole sim squad;
// without a server squad the lowest online id.
u16 engine_commander(const SquadState& squad)
{
    if (ai().get_alife())
    {
        if (auto* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(ai().alife().objects().object(squad.id, true)))
            return group->commander_id();
    }
    return squad.members.empty() ? u16(-1) : *std::min_element(squad.members.begin(), squad.members.end());
}

void destroy_squad_state(SquadState* squad)
{
    Search::destroy_squad(squad->search);
    Morale::destroy_squad(squad->morale);
    Tactics::destroy_squad(squad->tactics);
    Comms::destroy_squad(squad->comms);
    xr_delete(squad);
}

void destroy_lanes(NpcState& state)
{
    Search::destroy(state.search);
    Morale::destroy(state.morale);
    Tactics::destroy(state.tactics);
    Comms::destroy(state.comms);
    Sense::destroy(state.sense);
    Know::destroy(state.know);
    state.search = nullptr;
    state.morale = nullptr;
    state.tactics = nullptr;
    state.comms = nullptr;
    state.sense = nullptr;
    state.know = nullptr;
}

void update_leadership(SquadState& squad)
{
    squad.commander = engine_commander(squad);

    // Highest band alive; ties by rank value, then by the engine order (lowest id first).
    const NpcState* best = nullptr;
    for (const u16 id : squad.members)
    {
        const NpcState* state = npc(id);
        if (!state || state->dead)
            continue;
        if (!best || state->band > best->band || (state->band == best->band && state->rank > best->rank) ||
            (state->band == best->band && state->rank == best->rank && state->id < best->id))
        {
            best = state;
        }
    }
    squad.leader = best ? best->id : u16(-1);
    if (best)
        squad.community = u8(best->community);
}

void join_squad(NpcState& state, u16 squad_id)
{
    state.squad_id = squad_id;
    SquadState*& squad = g_squads[squad_id];
    if (!squad)
    {
        squad = xr_new<SquadState>();
        squad->id = squad_id;
        squad->comms = Comms::create_squad(*squad);
        squad->tactics = Tactics::create_squad(*squad);
        squad->morale = Morale::create_squad(*squad);
        squad->search = Search::create_squad(*squad);
    }
    squad->members.push_back(state.id);
    update_leadership(*squad);
}

void leave_squad(NpcState& state)
{
    const auto found = g_squads.find(state.squad_id);
    state.squad_id = u16(-1);
    if (found == g_squads.end())
        return;

    SquadState* squad = found->second;
    const auto member = std::find(squad->members.begin(), squad->members.end(), state.id);
    if (member != squad->members.end())
        squad->members.erase(member);

    if (squad->members.empty())
    {
        g_squads.erase(found);
        destroy_squad_state(squad);
        return;
    }
    update_leadership(*squad);
}
} // namespace

NpcState* npc(const CAI_Stalker& npc) { return npc.da_ai(); }

NpcState* npc(u16 id)
{
    const auto found = g_states.find(id);
    return found == g_states.end() ? nullptr : found->second;
}

SquadState* squad(u16 id)
{
    const auto found = g_squads.find(id);
    return found == g_squads.end() ? nullptr : found->second;
}

SquadState* squad_of(const CAI_Stalker& npc)
{
    const NpcState* state = npc.da_ai();
    return state ? squad(state->squad_id) : nullptr;
}

u16 leader(const SquadState& squad) { return squad.leader; }

bool is_leader(const CAI_Stalker& npc)
{
    const SquadState* squad = squad_of(npc);
    return squad && squad->leader == npc.ID();
}

void squad_tick(CAI_Stalker& npc)
{
    SquadState* squad = squad_of(npc);
    if (!squad)
        return;
    // The search lane's step runs at the member update after the others' (P10: all of them in one update were the squad
    // part's worst): the same cadence, the cost of a tick split over two updates.
    // The worst of each lane's squad step (P10: what p_max_squad_us is made of).
    const auto timed = [](Stat stat, auto&& step) {
        const auto start = std::chrono::steady_clock::now();
        step();
        stat_add(stat, std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - start).count());
    };
    if (squad->search && squad->search->due)
    {
        squad->search->due = false;
        timed(Stat::p_max_sq_search_us, [squad] { Search::squad_update(*squad); });
        return;
    }
    if (squad->last_tick && Device.dwTimeGlobal < squad->last_tick + g_config.squad_tick_ms)
        return;

    squad->last_tick = Device.dwTimeGlobal;
    timed(Stat::p_max_sq_comms_us, [squad] { Comms::squad_update(*squad); });
    timed(Stat::p_max_sq_tactics_us, [squad] { Tactics::squad_update(*squad); });
    timed(Stat::p_max_sq_morale_us, [squad] { Morale::squad_update(*squad); });
    if (squad->search)
        squad->search->due = true;
}

const xr_vector<CAI_Stalker*>& online_npcs() { return g_npcs; }
u32 npc_count() { return u32(g_npcs.size()); }
u32 squad_count() { return u32(g_squads.size()); }

namespace Registry
{
NpcState* add(CAI_Stalker& npc)
{
    if (NpcState* existing = npc.da_ai())
        return existing;

    NpcState* state = xr_new<NpcState>();
    state->owner = &npc;
    state->id = npc.ID();
    state->squad_check_time = Device.dwTimeGlobal;
    npc.da_ai(state);
    g_npcs.push_back(&npc);
    g_states[state->id] = state;

    resolve_profile(npc);
    join_squad(*state, sim_squad_id(npc));
    // After the squad is known: the leader bonus of the roll needs SquadState::leader.
    Traits::roll_radio(npc);

    state->know = Know::create(npc);
    state->sense = Sense::create(npc);
    state->comms = Comms::create(npc);
    state->tactics = Tactics::create(npc);
    state->morale = Morale::create(npc);
    state->search = Search::create(npc);
    return state;
}

void remove(CAI_Stalker& npc)
{
    NpcState* state = npc.da_ai();
    if (!state)
        return;

    destroy_lanes(*state);
    leave_squad(*state);
    const auto found = std::find(g_npcs.begin(), g_npcs.end(), &npc);
    if (found != g_npcs.end())
        g_npcs.erase(found);
    g_states.erase(state->id);
    npc.da_ai(nullptr);
    xr_delete(state);
}

void died(CAI_Stalker& npc)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead)
        return;

    state->dead = true;
    leave_squad(*state);
}

void refresh(CAI_Stalker& npc)
{
    NpcState* state = npc.da_ai();
    if (!state || state->dead || Device.dwTimeGlobal < state->squad_check_time + g_config.squad_refresh_ms)
        return;

    state->squad_check_time = Device.dwTimeGlobal;
    if (state->community != npc.Community())
    {
        resolve_profile(npc);
        if (SquadState* squad = squad_of(npc))
            update_leadership(*squad);
    }

    if (const u16 squad_id = sim_squad_id(npc); squad_id != state->squad_id)
    {
        leave_squad(*state);
        join_squad(*state, squad_id);
    }
}

void clear()
{
    // Level teardown: no squad bookkeeping (the ALife registry may already be gone), everything is freed at once.
    for (CAI_Stalker* npc : g_npcs)
    {
        NpcState* state = npc->da_ai();
        destroy_lanes(*state);
        npc->da_ai(nullptr);
        xr_delete(state);
    }
    for (auto& [id, squad] : g_squads)
        destroy_squad_state(squad);
    g_squads.clear();
    g_states.clear();
    g_npcs.clear();
}
} // namespace Registry
} // namespace DaAi
