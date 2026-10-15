// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "da_ai_knowledge.h"
#include "da_ai_npc.h"
#include "da_ai_profile.h"

#include "ai/stalker/ai_stalker.h"
#include "Level.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"

// .scov chunk AIK1 (plan 2.10): the beliefs of the online stalkers. Version 1 payload: u32 count, then per record
// u16 npcId, u16 targetId, u32 npcSectionCRC, f32 x, y, z, f32 err, u32 ageMs, u8 source, u8 confidence (0..255),
// u16 flags. A save without beliefs writes no chunk, which erases a stale one. On load the records are staged and
// each NPC takes its own when it spawns with the same id and section; a target resolves by id at first use.
namespace DaAi::Know
{
namespace
{
constexpr u16 flag_enemy = 1 << 0;
constexpr u16 flag_seen_own = 1 << 1; // the NPC had seen the target itself
constexpr u16 known_flags = flag_enemy | flag_seen_own;
constexpr size_t count_size = sizeof(u32);
constexpr size_t record_size = 2 * sizeof(u16) + sizeof(u32) + 4 * sizeof(float) + sizeof(u32) + 2 * sizeof(u8) +
    sizeof(u16);
constexpr u32 max_records = u32(u16(-1)) * 16;
constexpr u32 portion = 16; // NPCs encoded between two budget checks
constexpr float max_error = 10000.f;

struct Record
{
    u16 npc{};
    u16 target{};
    u32 section_crc{};
    Fvector pos{};
    float err{};
    u32 age_ms{};
    u8 source{};
    u8 confidence{};
    u16 flags{};
};

// Staged records by NPC id; they wait for the NPC's spawn and expire when nobody claims them.
xr_flat_hash_map<u16, xr_vector<Record>> g_staged;
u32 g_staged_since{};

template <typename T>
void write_le(xr_vector<u8>& out, T value)
{
    for (size_t index = 0; index < sizeof(T); ++index)
        out.push_back(u8(value >> (index * 8)));
}

template <typename T>
T read_le(const u8*& cursor)
{
    T value = 0;
    for (size_t index = 0; index < sizeof(T); ++index)
        value |= T(cursor[index]) << (index * 8);
    cursor += sizeof(T);
    return value;
}

void write_float(xr_vector<u8>& out, float value)
{
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    write_le<u32>(out, bits);
}

float read_float(const u8*& cursor)
{
    const u32 bits = read_le<u32>(cursor);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool target_exists(u16 id)
{
    const IGameObject* object = Level().Objects.net_Find(id);
    return object && !object->getDestroy();
}

// What the NPC knows now about each believed target (report_of: exact while seen, else the belief as it stands).
void encode_npc(const CAI_Stalker& npc, const KnowState& state, const SaveParams& params, xr_vector<u8>& out)
{
    const u32 time = Device.dwTimeGlobal;
    const u32 crc = section_crc(npc);
    u32 written = 0;
    for (const Belief& belief : state.beliefs)
    {
        if (written == params.beliefs_max)
            break;
        const u16 target = belief.e.target;
        if (target == u16(-1) || target == npc.ID() || !target_exists(target))
            continue;
        const Estimate estimate = report_of(npc, target);
        if (estimate.source == Source::none || !_valid(estimate.pos) || !_valid(estimate.err))
            continue;
        const u32 age = time - _min(time, estimate.t_observed);
        if (age > (belief.enemy ? params.forget_enemy_ms : params.forget_other_ms))
            continue;

        const u16 flags = u16((belief.enemy ? flag_enemy : 0) | (belief.t_seen_own ? flag_seen_own : 0));
        write_le<u16>(out, npc.ID());
        write_le<u16>(out, target);
        write_le<u32>(out, crc);
        write_float(out, estimate.pos.x);
        write_float(out, estimate.pos.y);
        write_float(out, estimate.pos.z);
        write_float(out, _min(estimate.err, max_error));
        write_le<u32>(out, age);
        write_le<u8>(out, u8(estimate.source));
        write_le<u8>(out, u8(iFloor(std::clamp(estimate.confidence, 0.f, 1.f) * 255.f + .5f)));
        write_le<u16>(out, flags);
        ++written;
    }
}

bool valid(const Record& record)
{
    return record.npc != u16(-1) && record.target != u16(-1) && record.npc != record.target && _valid(record.pos) &&
        _valid(record.err) && record.err >= 0.f && record.err <= max_error && record.source > u8(Source::none) &&
        record.source <= u8(Source::restored) && !(record.flags & ~known_flags);
}
} // namespace

void save_begin(ChunkCapture& state)
{
    state = {};
    state.begun = true;
    for (const CAI_Stalker* npc : online_npcs())
    {
        const NpcState* npc_state = npc ? DaAi::npc(*npc) : nullptr;
        if (npc_state && !npc_state->dead && npc_state->know && !npc_state->know->beliefs.empty())
            state.ids.push_back(npc->ID());
    }
    std::sort(state.ids.begin(), state.ids.end());
}

bool save_continue(ChunkCapture& state, float budget_ms)
{
    if (!state.begun)
        save_begin(state);
    if (!(budget_ms > 0.f))
        return false;

    if (state.payload.empty())
    {
        state.payload.reserve(count_size + state.ids.size() * 4 * record_size);
        write_le<u32>(state.payload, 0);
    }

    const SaveParams params = save_params();
    const bool unlimited = budget_ms == flt_max;
    CTimer timer;
    timer.Start();
    u32 batch = 0;
    while (state.index < state.ids.size())
    {
        // NPCs that went offline or died since the capture began are skipped.
        const NpcState* npc_state = DaAi::npc(state.ids[state.index++]);
        if (npc_state && !npc_state->dead && npc_state->know && npc_state->owner)
            encode_npc(*npc_state->owner, *npc_state->know, params, state.payload);

        if (!unlimited && ++batch == portion)
        {
            batch = 0;
            if (timer.GetElapsed_sec() * 1000.f >= budget_ms)
                return false;
        }
    }

    const u32 count = u32((state.payload.size() - count_size) / record_size);
    if (count > max_records)
    {
        state.failed = true;
        return true;
    }
    for (size_t index = 0; index < count_size; ++index)
        state.payload[index] = u8(count >> (index * 8));
    state.present = count > 0;
    return true;
}

bool stage(const xr_vector<u8>& payload)
{
    g_staged.clear();
    g_staged_since = 0;
    if (payload.size() < count_size)
        return false;
    const u8* cursor = payload.data();
    const u32 count = read_le<u32>(cursor);
    if (count > max_records || payload.size() != count_size + size_t(count) * record_size)
        return false;

    xr_vector<Record> records;
    records.reserve(count);
    u32 skipped = 0;
    for (u32 index = 0; index < count; ++index)
    {
        Record record;
        record.npc = read_le<u16>(cursor);
        record.target = read_le<u16>(cursor);
        record.section_crc = read_le<u32>(cursor);
        record.pos.x = read_float(cursor);
        record.pos.y = read_float(cursor);
        record.pos.z = read_float(cursor);
        record.err = read_float(cursor);
        record.age_ms = read_le<u32>(cursor);
        record.source = read_le<u8>(cursor);
        record.confidence = read_le<u8>(cursor);
        record.flags = read_le<u16>(cursor);
        if (valid(record))
            records.push_back(record);
        else
            ++skipped;
    }

    // Every copy of a duplicated (npc, target) pair is dropped, as in the other record chunks.
    std::sort(records.begin(), records.end(), [](const Record& left, const Record& right) {
        return left.npc != right.npc ? left.npc < right.npc : left.target < right.target;
    });
    for (size_t index = 0; index < records.size();)
    {
        size_t next = index + 1;
        while (next < records.size() && records[next].npc == records[index].npc &&
            records[next].target == records[index].target)
        {
            ++next;
        }
        if (next == index + 1)
            g_staged[records[index].npc].push_back(records[index]);
        else
            skipped += u32(next - index);
        index = next;
    }

    if (skipped)
        Msg("! Ignored %u semantically invalid records in save-extension chunk 'AIK1'", skipped);
    return true;
}

void clear_staged()
{
    g_staged.clear();
    g_staged_since = 0;
}

void forget_saved(u16 object_id)
{
    if (g_staged.empty())
        return;
    g_staged.erase(object_id);
    // A released target id may be reused by a new object: no staged belief may bind to it.
    for (auto& [npc, records] : g_staged)
    {
        records.erase(std::remove_if(records.begin(), records.end(),
                          [object_id](const Record& record) { return record.target == object_id; }),
            records.end());
    }
}

void apply_staged(const CAI_Stalker& npc, KnowState& state)
{
    if (g_staged.empty())
        return;
    const auto found = g_staged.find(npc.ID());
    if (found == g_staged.end())
        return;
    const xr_vector<Record> records = std::move(found->second);
    g_staged.erase(found);

    const SaveParams params = save_params();
    const u32 crc = section_crc(npc);
    const u32 time = Device.dwTimeGlobal;
    const NpcState* npc_state = DaAi::npc(npc);
    const int community = npc_state ? npc_state->community : -1;
    for (const Record& record : records)
    {
        if (record.section_crc != crc || state.beliefs.size() >= params.beliefs_max)
            continue;
        const bool enemy = record.flags & flag_enemy;
        if (record.age_ms > (enemy ? params.forget_enemy_ms : params.forget_other_ms))
            continue;

        // A restored belief is what the NPC remembers: never a sighting, whatever its saved source was (2.5).
        Belief belief{};
        Estimate& estimate = belief.e;
        estimate.pos = record.pos;
        estimate.err = record.err;
        estimate.target = record.target;
        estimate.reporter = npc.ID();
        estimate.source = Source::restored;
        estimate.confidence = float(record.confidence) / 255.f;

        // The age goes back from now as far as the timer reaches; the rest grows the error as age would.
        const u32 back = _min(record.age_ms, time > 1 ? time - 1 : 0);
        estimate.t_observed = time - back;
        if (const u32 missing = record.age_ms - back)
        {
            const float growth = params.age_growth * float(missing) / 1000.f;
            estimate.err += _min(growth, _max(0.f, params.age_cap - estimate.err));
        }

        if (ai().get_level_graph())
        {
            const CLevelGraph& graph = ai().level_graph();
            estimate.vertex = graph.vertex(u32(-1), estimate.pos);
            if (!graph.valid_vertex_id(estimate.vertex) || !graph.inside(estimate.vertex, estimate.pos))
                estimate.vertex = resolve_vertex(estimate.pos);
        }

        belief.t_received = time;
        belief.t_seen_own = record.flags & flag_seen_own ? estimate.t_observed : 0;
        belief.enemy = enemy;
        state.beliefs.push_back(belief);
        stat_add(Stat::k_restored, 1.f, community);
    }
}

void expire_staged()
{
    if (g_staged.empty())
        return;
    const u32 time = Device.dwTimeGlobal;
    if (!g_staged_since)
        g_staged_since = time ? time : 1;
    else if (time > g_staged_since + save_params().restore_grace_ms)
        g_staged.clear();
}
} // namespace DaAi::Know
