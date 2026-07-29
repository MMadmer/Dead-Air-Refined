////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_object_registry.h"
#include "ai_debug.h"
#include "xrServerEntities/xrMessages.h"

#include <fstream>
#include <unordered_map>
CALifeObjectRegistry::CALifeObjectRegistry(LPCSTR section) {}
CALifeObjectRegistry::~CALifeObjectRegistry()
{
    store_save_costs();

    OBJECT_REGISTRY::iterator const B = m_objects.begin();
    OBJECT_REGISTRY::iterator I = B;
    OBJECT_REGISTRY::iterator const E = m_objects.end();
    for (; I != E; ++I)
        (*I).second->on_unregister();

    for (I = B; I != E; ++I)
        xr_delete((*I).second);
}

xr_map<shared_str, CALifeObjectRegistry::SaveCostEstimate>&
CALifeObjectRegistry::save_section_costs()
{
    // The cache intentionally lives until process shutdown to avoid shared-string teardown order issues.
    static auto* costs = new xr_map<shared_str, SaveCostEstimate>();
    return *costs;
}

void CALifeObjectRegistry::load_save_costs()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;

    string_path path;
    FS.update_path(path, "$app_data_root$", "async_save_costs.bin");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return;

    u32 magic = 0;
    u32 version = 0;
    u32 count = 0;
    stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    stream.read(reinterpret_cast<char*>(&version), sizeof(version));
    stream.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!stream || magic != 0x43565341 || version != 4 || count > 100000)
        return;

    auto& costs = save_section_costs();
    for (u32 index = 0; index < count; ++index)
    {
        u16 length = 0;
        stream.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!stream || !length || length >= 4096)
            return;

        std::string section(length, '\0');
        SaveCostEstimate estimate;
        stream.read(section.data(), length);
        stream.read(reinterpret_cast<char*>(&estimate.milliseconds), sizeof(estimate.milliseconds));
        stream.read(reinterpret_cast<char*>(&estimate.packetBytes), sizeof(estimate.packetBytes));
        if (!stream || !std::isfinite(estimate.milliseconds) || !std::isfinite(estimate.packetBytes) ||
            estimate.milliseconds <= 0.f || estimate.milliseconds > 100.f ||
            estimate.packetBytes <= 0.f || estimate.packetBytes > float(std::numeric_limits<u16>::max() * 2u))
        {
            return;
        }

        costs[shared_str(section.c_str())] = estimate;
    }
}

void CALifeObjectRegistry::store_save_costs()
{
    auto& costs = save_section_costs();
    if (costs.empty())
        return;

    string_path path;
    FS.update_path(path, "$app_data_root$", "async_save_costs.bin");
    string_path temporaryPath;
    strconcat(sizeof(temporaryPath), temporaryPath, path, ".tmp");

    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
        return;

    const u32 magic = 0x43565341;
    const u32 version = 4;
    u32 count = 0;
    for (const auto& [section, estimate] : costs)
    {
        const size_t sectionLength = xr_strlen(section.c_str());
        if (sectionLength && sectionLength < std::numeric_limits<u16>::max())
            ++count;
    }
    stream.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [section, estimate] : costs)
    {
        const size_t sectionLength = xr_strlen(section.c_str());
        if (!sectionLength || sectionLength >= std::numeric_limits<u16>::max())
            continue;

        const u16 length = static_cast<u16>(sectionLength);
        stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
        stream.write(section.c_str(), length);
        stream.write(reinterpret_cast<const char*>(&estimate.milliseconds), sizeof(estimate.milliseconds));
        stream.write(reinterpret_cast<const char*>(&estimate.packetBytes), sizeof(estimate.packetBytes));
    }
    stream.flush();
    stream.close();
    if (!stream)
        return;

#if defined(XR_PLATFORM_WINDOWS)
    MoveFileExA(temporaryPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    std::rename(temporaryPath, path);
#endif
}

void CALifeObjectRegistry::save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count)
{
    save_object(memory_stream, object, object_count);

    for (const ALife::_OBJECT_ID childId : object->children)
    {
        CSE_ALifeDynamicObject* child = this->object(childId, true);
        if (child && child->can_save())
            save(memory_stream, child, object_count);
    }
}

u32 CALifeObjectRegistry::save_object(
    IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count)
{
    const u32 startPosition = memory_stream.tell();
    ++object_count;

    NET_Packet tNetPacket;
    // Spawn
    object->Spawn_Write(tNetPacket, TRUE);
    memory_stream.w_u16(u16(tNetPacket.B.count));
    memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);

    // Update
    tNetPacket.w_begin(M_UPDATE);
    object->UPDATE_Write(tNetPacket);

    memory_stream.w_u16(u16(tNetPacket.B.count));
    memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);
    return memory_stream.tell() - startPosition;
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
    struct BatchSample
    {
        SaveCostEstimate* cost{};
        u32 packetBytes{};
    };

    VERIFY(state.open);
    CTimer saveTimer;
    saveTimer.Start();
    const auto budget_exhausted = [&]()
    {
        return budgetMilliseconds != flt_max &&
            saveTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds;
    };
    load_save_costs();
    auto& sectionCosts = save_section_costs();
    // Interned section-name pointers stay stable while the shared-string cache entry exists.
    static auto* fastSectionCosts = new std::unordered_map<pcstr, SaveCostEstimate*>();
    const u32 planningObjectLimit = budgetMilliseconds == flt_max ? u32(-1) : 64u;

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

    while (state.nextObjectIndex < state.pendingObjects.size())
    {
        float predictedWeight = 0.f;
        const float remainingMilliseconds = budgetMilliseconds == flt_max ? flt_max :
            std::max(0.01f, budgetMilliseconds - saveTimer.GetElapsed_sec() * 1000.f);
        const float weightBudget =
            budgetMilliseconds == flt_max ? flt_max : remainingMilliseconds;
        xr_vector<BatchSample> samples;
        samples.reserve(32);
        u32 batchBytes = 0;
        CTimer batchTimer;
        batchTimer.Start();

        while (state.nextObjectIndex < state.pendingObjects.size() && samples.size() < 32)
        {
            const ALife::_OBJECT_ID objectId = state.pendingObjects[state.nextObjectIndex];
            CSE_ALifeDynamicObject* object = this->object(objectId, true);
            if (!object || !object->can_save())
            {
                ++state.nextObjectIndex;
                continue;
            }

            pcstr sectionName = object->name();
            auto fastCost = fastSectionCosts->find(sectionName);
            if (fastCost == fastSectionCosts->end())
            {
                const auto cost =
                    sectionCosts.emplace(shared_str(sectionName), SaveCostEstimate{}).first;
                fastCost = fastSectionCosts->emplace(sectionName, &cost->second).first;
            }
            SaveCostEstimate* cost = fastCost->second;
            const float estimatedMilliseconds =
                std::max(0.001f, cost ? cost->milliseconds : state.averageObjectMilliseconds);
            if (!samples.empty() && predictedWeight + estimatedMilliseconds > weightBudget)
                break;

            ++state.nextObjectIndex;
            const u32 packetBytes = save_object(memory_stream, object, state.objectCount);
            predictedWeight += estimatedMilliseconds;
            batchBytes += packetBytes;
            samples.push_back({cost, packetBytes});
        }

        const float batchMilliseconds = batchTimer.GetElapsed_sec() * 1000.f;
        if (batchMilliseconds > state.maxBatchMilliseconds)
        {
            state.maxBatchMilliseconds = batchMilliseconds;
            state.maxBatchObjects = static_cast<u32>(samples.size());
            state.maxBatchBytes = batchBytes;
        }

        if (!samples.empty())
        {
            const float perObjectShare = 0.25f / float(samples.size());
            const float safeBatchBytes = float(std::max(1u, batchBytes));
            for (const BatchSample& sample : samples)
            {
                const float byteShare = 0.75f * float(sample.packetBytes) / safeBatchBytes;
                const float observedMilliseconds = std::clamp(
                    batchMilliseconds * (perObjectShare + byteShare), 0.001f, 100.f);
                if (!sample.cost)
                    continue;

                sample.cost->milliseconds =
                    sample.cost->milliseconds * 0.9f + observedMilliseconds * 0.1f;
                sample.cost->packetBytes =
                    sample.cost->packetBytes * 0.9f + float(sample.packetBytes) * 0.1f;
            }
            state.averageObjectMilliseconds =
                std::max(0.001f, state.averageObjectMilliseconds * 0.95f +
                    batchMilliseconds / float(samples.size()) * 0.05f);
        }

        if (budget_exhausted())
            return false;
    }

    const u32 last_position = memory_stream.tell();
    memory_stream.seek(state.countPosition);
    memory_stream.w_u32(state.objectCount);
    memory_stream.seek(last_position);

    memory_stream.close_chunk();
    state.open = false;

    Msg("* %d objects are successfully saved", state.objectCount);
    if (state.maxBatchMilliseconds >= 1.f)
    {
        Msg("* Heaviest save batch: %u objects, %u bytes, %.3f ms",
            state.maxBatchObjects, state.maxBatchBytes, state.maxBatchMilliseconds);
    }
    return true;
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::get_object(IReader& file_stream)
{
    NET_Packet tNetPacket;
    u16 u_id;
    // Spawn
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_SPAWN == u_id, "Invalid packet ID (!= M_SPAWN)");

    string64 s_name;
    tNetPacket.r_stringZ(s_name);
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("Loading object %s [%d]b", s_name, tNetPacket.B.count);
    }
#endif
    // create entity
    CSE_Abstract* tpSE_Abstract = F_entity_Create(s_name);
    R_ASSERT2(tpSE_Abstract, "Can't create entity.");
    CSE_ALifeDynamicObject* tpALifeDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(tpSE_Abstract);
    R_ASSERT2(tpALifeDynamicObject, "Non-ALife object in the saved game!");
    tpALifeDynamicObject->Spawn_Read(tNetPacket);

    // Update
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_UPDATE == u_id, "Invalid packet ID (!= M_UPDATE)");
    tpALifeDynamicObject->UPDATE_Read(tNetPacket);

    return (tpALifeDynamicObject);
}

void CALifeObjectRegistry::load(IReader& file_stream)
{
    Msg("* Loading objects...");
    R_ASSERT2(file_stream.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");

    m_objects.clear();

    u32 count = file_stream.r_u32();
    for (u32 index = 0; index < count; ++index)
    {
        CSE_ALifeDynamicObject* object = get_object(file_stream);
        add(object);
    }

    Msg("* %d objects are successfully loaded", count);
}
