#include "StdAfx.h"
#include "save_extension_gameplay.h"

#include "Actor.h"
#include "ActorHelmet.h"
#include "Artefact.h"
#include "Weapon.h"
#include "xrEngine/IGame_Persistent.h"

#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace SaveExtensionGameplay
{
namespace
{
constexpr u32 actorAdrenalineChunkType = SaveExtensionChunkIds::ActorAdrenaline;
constexpr u32 artefactOverrideChunkType = SaveExtensionChunkIds::ArtefactOverrides;
constexpr u32 environmentSeasonChunkType = SaveExtensionChunkIds::EnvironmentSeason;
constexpr u16 chunkVersion = 1;
constexpr size_t countSize = sizeof(u32);
constexpr size_t weaponRecordSize = 2 * sizeof(u16) + 2 * sizeof(u32);
constexpr size_t actorRecordSize = 2 * sizeof(u16) + sizeof(u32) + sizeof(float);
constexpr size_t helmetRecordSize = 2 * sizeof(u16) + sizeof(u32);
constexpr size_t artefactRecordSize = 2 * sizeof(u16) + 2 * sizeof(u32) +
    artefactOverrideValueCount * sizeof(float);
constexpr u32 maxObjectRecords = std::numeric_limits<u16>::max();
constexpr u32 artefactKnownMask = (1u << artefactOverrideValueCount) - 1u;

template <typename T>
void write_value(u8*& cursor, const T& value)
{
    static_assert(std::is_unsigned_v<T>);
    for (size_t index = 0; index < sizeof(value); ++index)
        cursor[index] = static_cast<u8>(value >> (index * 8));
    cursor += sizeof(value);
}

void write_value(u8*& cursor, float value)
{
    write_value(cursor, std::bit_cast<u32>(value));
}

template <typename T>
void read_value(const u8*& cursor, T& value)
{
    static_assert(std::is_unsigned_v<T>);
    value = 0;
    for (size_t index = 0; index < sizeof(value); ++index)
        value |= static_cast<T>(cursor[index]) << (index * 8);
    cursor += sizeof(value);
}

void read_value(const u8*& cursor, float& value)
{
    u32 bits = 0;
    read_value(cursor, bits);
    value = std::bit_cast<float>(bits);
}

bool decode_count(const xr_vector<u8>& payload, size_t recordSize, u32& count, const u8*& cursor)
{
    if (payload.size() < countSize)
        return false;

    cursor = payload.data();
    read_value(cursor, count);
    return count <= maxObjectRecords &&
        payload.size() == countSize + static_cast<size_t>(count) * recordSize;
}

template <typename T>
u32 discard_duplicate_ids(xr_vector<T>& records)
{
    std::sort(records.begin(), records.end(), [](const T& left, const T& right)
    {
        return left.objectId < right.objectId;
    });

    size_t output = 0;
    u32 discarded = 0;
    for (size_t index = 0; index < records.size();)
    {
        size_t next = index + 1;
        while (next < records.size() && records[next].objectId == records[index].objectId)
            ++next;
        if (next == index + 1)
            records[output++] = std::move(records[index]);
        else
            discarded += static_cast<u32>(next - index);
        index = next;
    }
    records.resize(output);
    return discarded;
}

void report_skipped_records(pcstr name, u32 count)
{
    if (count)
        Msg("! Ignored %u semantically invalid records in save-extension chunk '%s'", count, name);
}

void add_mutation(ChunkMutationList& mutations, u32 type, xr_vector<u8> payload, bool present)
{
    mutations.push_back({{type, chunkVersion, 0, std::move(payload)}, chunkVersion, !present});
}

template <typename Record, typename Encoder>
bool continue_record_payload(const xr_vector<Record>& records, size_t recordSize,
    xr_vector<u8>& payload, size_t& index, float budgetMilliseconds, Encoder&& encoder, bool& failed)
{
    if (!(budgetMilliseconds > 0.f))
        return false;

    if (records.size() > maxObjectRecords)
    {
        failed = true;
        return true;
    }

    if (payload.empty())
    {
        payload.reserve(countSize + records.size() * recordSize);
        payload.resize(countSize);
        u8* cursor = payload.data();
        write_value(cursor, static_cast<u32>(records.size()));
    }

    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    CTimer budgetTimer;
    if (!unlimitedBudget)
        budgetTimer.Start();

    u32 batchRecords = 0;
    while (index < records.size())
    {
        const Record& record = records[index];
        if (record.objectId == u16(-1) || (index && record.objectId <= records[index - 1].objectId))
        {
            failed = true;
            return true;
        }

        const size_t offset = payload.size();
        payload.resize(offset + recordSize);
        u8* cursor = payload.data() + offset;
        if (!encoder(record, cursor))
        {
            failed = true;
            return true;
        }
        ++index;

        if (!unlimitedBudget && ++batchRecords == 16)
        {
            batchRecords = 0;
            if (budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
                return false;
        }
    }
    return true;
}

bool encode_weapon_record(const SWeaponExtendedSaveState& record, u8*& cursor)
{
    if (record.reserved || !record.extendedMask ||
        (record.extendedMask & ~u32(eWeaponConditionExtendedSaveMask)))
        return false;

    write_value(cursor, record.objectId);
    write_value(cursor, record.reserved);
    write_value(cursor, record.sectionChecksum);
    write_value(cursor, record.extendedMask);
    return true;
}

bool encode_actor_record(const SActorAdrenalineSaveState& record, u8*& cursor)
{
    if (!std::isfinite(record.remaining) || record.remaining <= 0.f)
        return false;

    write_value(cursor, record.objectId);
    write_value(cursor, u16{});
    write_value(cursor, record.sectionChecksum);
    write_value(cursor, record.remaining);
    return true;
}

bool decode_actor_chunk(const xr_vector<u8>& payload, xr_vector<SActorAdrenalineSaveState>& records)
{
    u32 count = 0;
    const u8* cursor = nullptr;
    if (!decode_count(payload, actorRecordSize, count, cursor))
        return false;

    records.clear();
    records.reserve(count);
    u32 skipped = 0;
    for (u32 i = 0; i < count; ++i)
    {
        SActorAdrenalineSaveState record;
        u16 reserved = 0;
        read_value(cursor, record.objectId);
        read_value(cursor, reserved);
        read_value(cursor, record.sectionChecksum);
        read_value(cursor, record.remaining);
        if (record.objectId == u16(-1) || reserved || !std::isfinite(record.remaining) || record.remaining <= 0.f)
        {
            ++skipped;
            continue;
        }
        records.push_back(record);
    }
    skipped += discard_duplicate_ids(records);
    report_skipped_records("ADR1", skipped);
    return true;
}

bool encode_helmet_record(const SHelmetFilterSaveState& record, u8*& cursor)
{
    write_value(cursor, record.objectId);
    write_value(cursor, record.elapsed);
    write_value(cursor, record.sectionChecksum);
    return true;
}

bool decode_helmet_chunk(const xr_vector<u8>& payload, xr_vector<SHelmetFilterSaveState>& records)
{
    u32 count = 0;
    const u8* cursor = nullptr;
    if (!decode_count(payload, helmetRecordSize, count, cursor))
        return false;

    records.clear();
    records.reserve(count);
    u32 skipped = 0;
    for (u32 i = 0; i < count; ++i)
    {
        SHelmetFilterSaveState record;
        read_value(cursor, record.objectId);
        read_value(cursor, record.elapsed);
        read_value(cursor, record.sectionChecksum);
        if (record.objectId == u16(-1))
        {
            ++skipped;
            continue;
        }
        records.push_back(record);
    }
    skipped += discard_duplicate_ids(records);
    report_skipped_records("HFT1", skipped);
    return true;
}

bool artefact_record_valid(const SArtefactOverrideSaveState& record)
{
    const u32 knownMask = record.changedMask & artefactKnownMask;
    if (record.objectId == u16(-1) || !knownMask)
        return false;

    for (u32 index = 0; index < artefactOverrideValueCount; ++index)
    {
        if ((knownMask & (1u << index)) && !std::isfinite(record.values[index]))
            return false;
    }
    return true;
}

bool encode_artefact_record(const SArtefactOverrideSaveState& record, u8*& cursor)
{
    if (!artefact_record_valid(record))
        return false;

    write_value(cursor, record.objectId);
    write_value(cursor, u16{});
    write_value(cursor, record.sectionChecksum);
    write_value(cursor, record.changedMask & artefactKnownMask);
    for (const float value : record.values)
        write_value(cursor, value);
    return true;
}

bool decode_artefact_chunk(const xr_vector<u8>& payload, xr_vector<SArtefactOverrideSaveState>& records)
{
    u32 count = 0;
    const u8* cursor = nullptr;
    if (!decode_count(payload, artefactRecordSize, count, cursor))
        return false;

    records.clear();
    records.reserve(count);
    u32 skipped = 0;
    for (u32 i = 0; i < count; ++i)
    {
        SArtefactOverrideSaveState record;
        u16 reserved = 0;
        read_value(cursor, record.objectId);
        read_value(cursor, reserved);
        read_value(cursor, record.sectionChecksum);
        read_value(cursor, record.changedMask);
        for (float& value : record.values)
            read_value(cursor, value);
        record.changedMask &= artefactKnownMask;
        if (reserved || !artefact_record_valid(record))
        {
            ++skipped;
            continue;
        }
        records.push_back(record);
    }
    skipped += discard_duplicate_ids(records);
    report_skipped_records("AFO1", skipped);
    return true;
}

const SaveExtensionContainer::Chunk* supported_chunk(
    const SaveExtensionContainer::ChunkList& chunks, u32 type)
{
    const SaveExtensionContainer::Chunk* chunk = SaveExtensionContainer::find_chunk(chunks, type);
    return chunk && chunk->version == chunkVersion && !chunk->flags ? chunk : nullptr;
}

void report_invalid_chunk(pcstr name)
{
    Msg("! Ignored semantically invalid save-extension chunk '%s'", name);
}
}

void begin_capture(CaptureState& state)
{
    state = {};
    state.mutations.reserve(5);
    CWeapon::BeginExtendedSaveCapture(state.weapon);
    CActor::BeginAdrenalineSaveCapture(state.actor);
    CHelmet::BeginFilterSaveCapture(state.helmet);
    CArtefact::BeginOverrideSaveCapture(state.artefact);
    state.initialized = true;
}

bool continue_capture(CaptureState& state, float budgetMilliseconds)
{
    if (!state.initialized)
    {
        state.failed = true;
        state.completed = true;
        return true;
    }
    if (state.completed)
        return true;
    if (!(budgetMilliseconds > 0.f))
        return false;

    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    CTimer budgetTimer;
    budgetTimer.Start();
    const auto budget_exhausted = [&]()
    {
        return !unlimitedBudget && budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds;
    };

    while (!state.completed)
    {
        if (budget_exhausted())
            return false;

        if (state.phase == 0)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!CWeapon::ContinueExtendedSaveCapture(state.weapon, remaining))
                return false;
            state.phase = 1;
            continue;
        }

        if (state.phase == 1)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!CActor::ContinueAdrenalineSaveCapture(state.actor, remaining))
                return false;
            state.phase = 2;
            continue;
        }

        if (state.phase == 2)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!CHelmet::ContinueFilterSaveCapture(state.helmet, remaining))
                return false;
            state.phase = 3;
            continue;
        }

        if (state.phase == 3)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!CArtefact::ContinueOverrideSaveCapture(state.artefact, remaining))
                return false;
            state.phase = 4;
            continue;
        }

        if (state.phase == 4)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!continue_record_payload(state.weapon.records, weaponRecordSize,
                state.payload, state.encodeIndex, remaining, encode_weapon_record, state.failed))
            {
                return false;
            }
            if (state.failed)
            {
                state.completed = true;
                return true;
            }
            add_mutation(state.mutations, weaponExtendedSaveChunkType,
                std::move(state.payload), !state.weapon.records.empty());
            state.payload.clear();
            state.encodeIndex = 0;
            state.phase = 5;
            continue;
        }

        if (state.phase == 5)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!continue_record_payload(state.actor.records, actorRecordSize,
                state.payload, state.encodeIndex, remaining, encode_actor_record, state.failed))
            {
                return false;
            }
            if (state.failed)
            {
                state.completed = true;
                return true;
            }
            add_mutation(state.mutations, actorAdrenalineChunkType,
                std::move(state.payload), !state.actor.records.empty());
            state.payload.clear();
            state.encodeIndex = 0;
            state.phase = 6;
            continue;
        }

        if (state.phase == 6)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!continue_record_payload(state.helmet.records, helmetRecordSize,
                state.payload, state.encodeIndex, remaining, encode_helmet_record, state.failed))
            {
                return false;
            }
            if (state.failed)
            {
                state.completed = true;
                return true;
            }
            add_mutation(state.mutations, SaveExtensionContainer::legacyHelmetFilterChunkType,
                std::move(state.payload), !state.helmet.records.empty());
            state.payload.clear();
            state.encodeIndex = 0;
            state.phase = 7;
            continue;
        }

        if (state.phase == 7)
        {
            const float elapsed = budgetTimer.GetElapsed_sec() * 1000.f;
            const float remaining = unlimitedBudget ? flt_max : budgetMilliseconds - elapsed;
            if (!continue_record_payload(state.artefact.records, artefactRecordSize,
                state.payload, state.encodeIndex, remaining, encode_artefact_record, state.failed))
            {
                return false;
            }
            if (state.failed)
            {
                state.completed = true;
                return true;
            }
            add_mutation(state.mutations, artefactOverrideChunkType,
                std::move(state.payload), !state.artefact.records.empty());
            state.payload.clear();
            state.encodeIndex = 0;
            state.phase = 8;
            continue;
        }

        if (state.phase == 8)
        {
            const float season = g_pGamePersistent ? g_pGamePersistent->Environment().GetSeason() : 0.f;
            if (!std::isfinite(season))
            {
                state.failed = true;
                state.completed = true;
                return true;
            }
            xr_vector<u8> seasonPayload(sizeof(season));
            u8* cursor = seasonPayload.data();
            write_value(cursor, season);
            add_mutation(state.mutations, environmentSeasonChunkType,
                std::move(seasonPayload), season != 0.f);
            state.phase = 9;
            state.completed = true;
        }
    }
    return true;
}

bool apply_mutations(SaveExtensionContainer::ChunkList& chunks, ChunkMutationList mutations)
{
    for (size_t index = 0; index < mutations.size(); ++index)
    {
        const auto duplicate = std::find_if(mutations.begin(), mutations.begin() + index,
            [&mutation = mutations[index]](const ChunkMutation& candidate)
            {
                return candidate.chunk.type == mutation.chunk.type;
            });
        if (duplicate != mutations.begin() + index)
            return false;
    }

    for (ChunkMutation& mutation : mutations)
    {
        const SaveExtensionContainer::UpdateResult result = mutation.erase ?
            SaveExtensionContainer::erase_supported_chunk(
                chunks, mutation.chunk.type, mutation.newestSupportedVersion) :
            SaveExtensionContainer::upsert_supported_chunk(
                chunks, std::move(mutation.chunk), mutation.newestSupportedVersion);
        if (result != SaveExtensionContainer::UpdateResult::Updated &&
            result != SaveExtensionContainer::UpdateResult::PreservedNewer)
        {
            return false;
        }
    }
    return true;
}

void stage_chunks(const SaveExtensionContainer::ChunkList& chunks)
{
    clear_state();

    if (const auto* chunk = supported_chunk(chunks, actorAdrenalineChunkType))
    {
        xr_vector<SActorAdrenalineSaveState> records;
        if (decode_actor_chunk(chunk->payload, records))
            CActor::StageAdrenalineSaveState(records);
        else
            report_invalid_chunk("ADR1");
    }

    if (const auto* chunk = supported_chunk(chunks, SaveExtensionContainer::legacyHelmetFilterChunkType))
    {
        xr_vector<SHelmetFilterSaveState> records;
        if (decode_helmet_chunk(chunk->payload, records))
            CHelmet::StageFilterSaveState(records);
        else
            report_invalid_chunk("HFT1");
    }

    if (const auto* chunk = supported_chunk(chunks, artefactOverrideChunkType))
    {
        xr_vector<SArtefactOverrideSaveState> records;
        if (decode_artefact_chunk(chunk->payload, records))
            CArtefact::StageOverrideSaveState(records);
        else
            report_invalid_chunk("AFO1");
    }

    if (const auto* chunk = supported_chunk(chunks, weaponExtendedSaveChunkType))
    {
        xr_vector<SWeaponExtendedSaveState> records;
        if (!CWeapon::DecodeExtendedSaveState(chunk->payload, records) ||
            !CWeapon::StageExtendedSaveState(records))
        {
            report_invalid_chunk("WEX1");
        }
    }

    if (const auto* chunk = supported_chunk(chunks, environmentSeasonChunkType))
    {
        float season = 0.f;
        if (chunk->payload.size() == sizeof(season))
        {
            const u8* cursor = chunk->payload.data();
            read_value(cursor, season);
        }
        if (chunk->payload.size() == sizeof(season) && std::isfinite(season) && g_pGamePersistent)
            g_pGamePersistent->Environment().SetSeason(season);
        else
            report_invalid_chunk("SEA1");
    }
}

void clear_state()
{
    CActor::ClearAdrenalineSaveState();
    CHelmet::ClearFilterSaveState();
    CArtefact::ClearOverrideSaveState();
    CWeapon::ClearExtendedSaveState();
    if (g_pGamePersistent)
        g_pGamePersistent->Environment().SetSeason(0.f);
}

void forget_object(const CSE_Abstract& serverObject)
{
    CActor::ForgetAdrenalineSaveState(serverObject);
    CHelmet::ForgetFilterSaveState(serverObject);
    CArtefact::ForgetOverrideSaveState(serverObject);
    CWeapon::ForgetExtendedSaveState(serverObject);
}
}
