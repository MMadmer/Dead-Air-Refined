////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_object_registry.h"
#include "xms_game.h"
#include "ai_debug.h"
#include "xrServerEntities/xrMessages.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <bit>
#if defined(XR_PLATFORM_WINDOWS)
#include <intrin.h>
#endif

namespace
{
u64 read_save_profile_cycles()
{
#if defined(XR_PLATFORM_WINDOWS)
    return __rdtsc();
#else
    return 0;
#endif
}
}

CALifeObjectRegistry::CALifeObjectRegistry(LPCSTR section) {}
CALifeObjectRegistry::~CALifeObjectRegistry()
{
    OBJECT_REGISTRY::iterator const B = m_objects.begin();
    OBJECT_REGISTRY::iterator I = B;
    OBJECT_REGISTRY::iterator const E = m_objects.end();
    for (; I != E; ++I)
        (*I).second->on_unregister();

    for (I = B; I != E; ++I)
        xr_delete((*I).second);
}

void CALifeObjectRegistry::save(
    IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count, NET_Packet& packet)
{
    save_object(memory_stream, object, object_count, packet);

    for (const ALife::_OBJECT_ID childId : object->children)
    {
        CSE_ALifeDynamicObject* child = this->object(childId, true);
        if (child && child->can_save())
            save(memory_stream, child, object_count, packet);
    }
}

u32 CALifeObjectRegistry::save_object(
    IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count, NET_Packet& packet)
{
    ++object_count;

    // A reused packet must match the fresh per-object cursor state.
    packet.r_pos = 0;

    // Spawn
    object->Spawn_Write(packet, TRUE);
    const u32 spawnBytes = packet.B.count;
    const u16 spawnSize = static_cast<u16>(spawnBytes);
    memory_stream.w(&spawnSize, sizeof(spawnSize));
    memory_stream.w(packet.B.data, spawnBytes);

    // Update
    packet.w_begin(M_UPDATE);
    object->UPDATE_Write(packet);

    const u32 updateBytes = packet.B.count;
    const u16 updateSize = static_cast<u16>(updateBytes);
    memory_stream.w(&updateSize, sizeof(updateSize));
    memory_stream.w(packet.B.data, updateBytes);
    return u32(2 * sizeof(u16)) + spawnBytes + updateBytes;
}

u32 CALifeObjectRegistry::save_object(
    CMemoryWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count,
    NET_Packet& packet, SaveObjectProfile* profile)
{
    ++object_count;

    // A reused packet must match the fresh per-object cursor state.
    packet.r_pos = 0;

    const u64 spawnStarted = read_save_profile_cycles();
    object->Spawn_Write(packet, TRUE);
    const u64 spawnEnded = read_save_profile_cycles();
    const u32 spawnBytes = packet.B.count;
    memory_writer_write_packet(memory_stream, static_cast<u16>(spawnBytes), packet.B.data);
    const u64 spawnCopyEnded = read_save_profile_cycles();

    packet.w_begin(M_UPDATE);
    object->UPDATE_Write(packet);
    const u64 updateEnded = read_save_profile_cycles();
    const u32 updateBytes = packet.B.count;
    memory_writer_write_packet(memory_stream, static_cast<u16>(updateBytes), packet.B.data);
    const u64 updateCopyEnded = read_save_profile_cycles();
    if (profile)
    {
        profile->spawnCycles = spawnEnded - spawnStarted;
        profile->spawnCopyCycles = spawnCopyEnded - spawnEnded;
        profile->updateCycles = updateEnded - spawnCopyEnded;
        profile->updateCopyCycles = updateCopyEnded - updateEnded;
    }
    return u32(2 * sizeof(u16)) + spawnBytes + updateBytes;
}

void CALifeObjectRegistry::save(IWriter& memory_stream)
{
    SaveState state;
    begin_save(memory_stream, state);
    continue_save(memory_stream, state, flt_max);
}

void CALifeObjectRegistry::begin_save(IWriter& memory_stream, SaveState& state)
{
    Msg("* Saving objects...");
    memory_stream.open_chunk(OBJECT_CHUNK_DATA);

    state = {};
    state.open = true;
    state.countPosition = memory_stream.tell();
    memory_stream.w_u32(u32(-1));
    state.pendingObjects.reserve(m_objects.size());
    state.traversalObjects.reserve(m_objects.size());
    state.visitedObjects.resize(std::numeric_limits<ALife::_OBJECT_ID>::max() + 1u, 0);
}

bool CALifeObjectRegistry::continue_save(
    IWriter& memory_stream, SaveState& state, float budgetMilliseconds)
{
    NET_Packet packet;
    return continue_save(memory_stream, state, budgetMilliseconds, packet);
}

void CALifeObjectRegistry::index_save_extension_object(CSE_ALifeDynamicObject* object)
{
    ESaveExtensionObjectType type = ESaveExtensionObjectType::Count;
    if (smart_cast<CSE_ALifeItemWeapon*>(object))
        type = ESaveExtensionObjectType::Weapon;
    else if (smart_cast<CSE_ALifeCreatureActor*>(object))
        type = ESaveExtensionObjectType::Actor;
    else if (smart_cast<CSE_ALifeItemHelmet*>(object))
        type = ESaveExtensionObjectType::Helmet;
    else if (smart_cast<CSE_ALifeItemArtefact*>(object))
        type = ESaveExtensionObjectType::Artefact;

    constexpr size_t bitsPerWord = std::numeric_limits<u64>::digits;
    const size_t typeWord = object->ID / bitsPerWord;
    const u64 typeBit = u64{1} << (object->ID % bitsPerWord);
    for (auto& typeWords : m_saveExtensionObjectTypes)
        typeWords[typeWord] &= ~typeBit;

    if (type != ESaveExtensionObjectType::Count)
        m_saveExtensionObjectTypes[static_cast<size_t>(type)][typeWord] |= typeBit;
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::next_save_extension_object(
    ESaveExtensionObjectType type, u32& nextObjectId) const
{
    const size_t typeIndex = static_cast<size_t>(type);
    if (typeIndex >= m_saveExtensionObjectTypes.size())
        return nullptr;

    constexpr u32 bitsPerWord = std::numeric_limits<u64>::digits;
    const auto& typeWords = m_saveExtensionObjectTypes[typeIndex];
    while (nextObjectId < std::numeric_limits<ALife::_OBJECT_ID>::max())
    {
        const size_t typeWord = nextObjectId / bitsPerWord;
        const u32 bitOffset = nextObjectId % bitsPerWord;
        const u64 candidates = typeWords[typeWord] & (~u64{0} << bitOffset);
        if (!candidates)
        {
            nextObjectId = static_cast<u32>((typeWord + 1) * bitsPerWord);
            continue;
        }

        const ALife::_OBJECT_ID objectId = static_cast<ALife::_OBJECT_ID>(
            typeWord * bitsPerWord + std::countr_zero(candidates));
        nextObjectId = static_cast<u32>(objectId) + 1;
        if (CSE_ALifeDynamicObject* object = m_objectIndex[objectId])
            return object;
    }
    return nullptr;
}

bool CALifeObjectRegistry::continue_save(
    IWriter& memory_stream, SaveState& state, float budgetMilliseconds, NET_Packet& savePacket)
{
    constexpr size_t maxBatchObjects = 8;
    using SaveClock = CTimerBase::Clock;
    using SaveDuration = SaveClock::duration;

    VERIFY(state.open);
    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    if (!unlimitedBudget && budgetMilliseconds <= 0.f)
        return false;

    const auto saveStartedAt = SaveClock::now();
    const SaveDuration workBudget = unlimitedBudget ? SaveDuration::max() :
        std::chrono::duration_cast<SaveDuration>(
            std::chrono::duration<float, std::milli>(budgetMilliseconds));
    const auto budget_exhausted_at = [&](const SaveClock::time_point current)
    {
        return !unlimitedBudget && current - saveStartedAt >= workBudget;
    };
    const auto budget_exhausted = [&]()
    {
        return budget_exhausted_at(SaveClock::now());
    };
    CMemoryWriter* fastMemoryStream = dynamic_cast<CMemoryWriter*>(&memory_stream);
    const u32 planningObjectLimit = unlimitedBudget ? u32(-1) : 64u;

    if (state.planningPhase == 0)
    {
        auto iterator = state.rootScanPosition > std::numeric_limits<ALife::_OBJECT_ID>::max() ?
            m_objects.end() :
            m_objects.lower_bound(static_cast<ALife::_OBJECT_ID>(state.rootScanPosition));
        while (iterator != m_objects.end())
        {
            u32 processedObjects = 0;
            while (iterator != m_objects.end() && processedObjects < planningObjectLimit)
            {
                const auto& [id, object] = *iterator++;
                state.rootScanPosition = u32(id) + 1u;
                ++processedObjects;
                if (!object->can_save() || object->redundant() || object->ID_Parent != 0xffff)
                    continue;

                state.pendingObjects.push_back(id);
            }
            if (budget_exhausted())
                return false;
        }

        state.traversalObjects.assign(state.pendingObjects.rbegin(), state.pendingObjects.rend());
        state.pendingObjects.clear();
        state.planningPhase = 1;
        if (budget_exhausted())
            return false;
    }

    if (state.planningPhase == 1)
    {
        while (!state.traversalObjects.empty())
        {
            u32 processedObjects = 0;
            while (!state.traversalObjects.empty() && processedObjects < planningObjectLimit)
            {
                const ALife::_OBJECT_ID objectId = state.traversalObjects.back();
                state.traversalObjects.pop_back();
                ++processedObjects;
                if (state.visitedObjects[objectId])
                    continue;
                state.visitedObjects[objectId] = 1;

                CSE_ALifeDynamicObject* object = this->object(objectId, true);
                if (!object || !object->can_save())
                    continue;

                state.pendingObjects.push_back(objectId);
                for (auto iterator = object->children.rbegin(); iterator != object->children.rend(); ++iterator)
                    state.traversalObjects.push_back(*iterator);
            }
            if (budget_exhausted())
                return false;
        }

        state.visitedObjects.clear();
        state.planningPhase = 2;
        if (budget_exhausted())
            return false;
    }

    auto schedulerTime = SaveClock::now();
    if (budget_exhausted_at(schedulerTime))
        return false;

    while (state.nextObjectIndex < state.pendingObjects.size())
    {
        size_t batchObjectCount = 0;

        while (state.nextObjectIndex < state.pendingObjects.size() && batchObjectCount < maxBatchObjects)
        {
            const ALife::_OBJECT_ID objectId = state.pendingObjects[state.nextObjectIndex];
            CSE_ALifeDynamicObject* object = this->object(objectId, true);
            if (!object || !object->can_save())
            {
                ++state.nextObjectIndex;
                if (!unlimitedBudget)
                {
                    schedulerTime = SaveClock::now();
                    if (budget_exhausted_at(schedulerTime))
                        break;
                }
                continue;
            }

            ++state.nextObjectIndex;
            if (fastMemoryStream)
            {
                SaveObjectProfile profile;
                save_object(*fastMemoryStream, object, state.objectCount, savePacket, &profile);
                const u64 objectCycles = profile.spawnCycles + profile.spawnCopyCycles +
                    profile.updateCycles + profile.updateCopyCycles;
                if (objectCycles > state.heaviestObjectCycles)
                {
                    state.heaviestObjectCycles = objectCycles;
                    state.heaviestObjectId = objectId;
                    state.heaviestObjectProfile = profile;
                }
            }
            else
                save_object(memory_stream, object, state.objectCount, savePacket);
            ++batchObjectCount;

            schedulerTime = SaveClock::now();
            if (budget_exhausted_at(schedulerTime))
                break;
        }

        if (budget_exhausted_at(schedulerTime))
            return false;
    }

    const u32 last_position = memory_stream.tell();
    memory_stream.seek(state.countPosition);
    memory_stream.w_u32(state.objectCount);
    memory_stream.seek(last_position);

    memory_stream.close_chunk();
    state.open = false;

    return true;
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::get_object(IReader& file_stream)
{
    NET_Packet packet;
    return get_object(file_stream, packet);
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::get_object(IReader& file_stream, NET_Packet& packet)
{
    u16 u_id;
    // Spawn
    packet.B.count = file_stream.r_u16();
    CHECK_OR_EXIT(packet.B.count < NET_PacketSizeLimit,
        make_string("Invalid spawn packet size: %u", packet.B.count));
    file_stream.r(packet.B.data, packet.B.count);
    packet.B.data[packet.B.count] = 0;
    packet.r_begin(u_id);
    R_ASSERT2(M_SPAWN == u_id, "Invalid packet ID (!= M_SPAWN)");

    string64 s_name;
    packet.r_stringZ(s_name);
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("Loading object %s [%d]b", s_name, packet.B.count);
    }
#endif
    // XMS: an object whose section vanished (its module was removed) is
    // skipped instead of killing the load; the u16 length prefixes keep the
    // stream framing intact
    const bool known_section = pSettings->section_exist(s_name) && pSettings->line_exist(s_name, "class");
    CSE_Abstract* tpSE_Abstract = known_section ? F_entity_Create(s_name, true) : nullptr;
    if (!tpSE_Abstract)
    {
        XmsGame::NoteSkippedObject(s_name);
        const u32 update_size = file_stream.r_u16();
        CHECK_OR_EXIT(update_size < NET_PacketSizeLimit,
            make_string("Invalid update packet size: %u", update_size));
        file_stream.advance(update_size);
        return nullptr;
    }
    CSE_ALifeDynamicObject* tpALifeDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(tpSE_Abstract);
    R_ASSERT2(tpALifeDynamicObject, "Non-ALife object in the saved game!");
    tpALifeDynamicObject->Spawn_Read(packet);

    // Update
    packet.B.count = file_stream.r_u16();
    CHECK_OR_EXIT(packet.B.count < NET_PacketSizeLimit,
        make_string("Invalid update packet size: %u", packet.B.count));
    file_stream.r(packet.B.data, packet.B.count);
    packet.B.data[packet.B.count] = 0;
    packet.r_begin(u_id);
    R_ASSERT2(M_UPDATE == u_id, "Invalid packet ID (!= M_UPDATE)");
    tpALifeDynamicObject->UPDATE_Read(packet);

    return (tpALifeDynamicObject);
}

void CALifeObjectRegistry::load(IReader& file_stream)
{
    Msg("* Loading objects...");
    R_ASSERT2(file_stream.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");

    m_objects.clear();
    m_objectIndex.fill(nullptr);
    for (auto& typeWords : m_saveExtensionObjectTypes)
        typeWords.fill(0);

    u32 count = file_stream.r_u32();
    // Reuse one packet to avoid clearing 16 KiB for every loaded object.
    NET_Packet loadPacket;
    XmsGame::ResetSkippedObjects();
    for (u32 index = 0; index < count; ++index)
    {
        CSE_ALifeDynamicObject* object = get_object(file_stream, loadPacket);
        if (object)
            add(object);
    }

    if (const u32 skipped = XmsGame::SkippedObjectCount())
        Msg("! XMS: %u of %u saved object(s) skipped (their modules are not installed)", skipped, count);
    Msg("* %d objects are successfully loaded", count - XmsGame::SkippedObjectCount());
}
