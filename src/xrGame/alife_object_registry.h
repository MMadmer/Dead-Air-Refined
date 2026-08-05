////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrServer_Objects_ALife.h"
#include "xrEngine/profiler.h"

#include <array>

class CMemoryWriter;

enum class ESaveExtensionObjectType : u8
{
    Weapon,
    Actor,
    Helmet,
    Artefact,
    Count,
};

class CALifeObjectRegistry
{
public:
    typedef xr_map<ALife::_OBJECT_ID, CSE_ALifeDynamicObject*> OBJECT_REGISTRY;
    struct SaveObjectProfile
    {
        u64 spawnCycles{};
        u64 spawnCopyCycles{};
        u64 updateCycles{};
        u64 updateCopyCycles{};
    };

    struct SaveState
    {
        xr_vector<ALife::_OBJECT_ID> pendingObjects;
        xr_vector<ALife::_OBJECT_ID> traversalObjects;
        xr_vector<u8> visitedObjects;
        u32 nextObjectIndex{};
        u32 rootScanPosition{};
        u32 objectCount{};
        u32 countPosition{};
        SaveObjectProfile heaviestObjectProfile;
        u64 heaviestObjectCycles{};
        ALife::_OBJECT_ID heaviestObjectId{};
        u8 planningPhase{};
        bool open{};
    };

protected:
    OBJECT_REGISTRY m_objects;
    static constexpr size_t ObjectIndexSize =
        size_t(std::numeric_limits<ALife::_OBJECT_ID>::max()) + 1;
    static constexpr size_t SaveExtensionObjectWordCount =
        (ObjectIndexSize + std::numeric_limits<u64>::digits - 1) / std::numeric_limits<u64>::digits;
    static constexpr size_t SaveExtensionObjectTypeCount =
        static_cast<size_t>(ESaveExtensionObjectType::Count);
    std::array<CSE_ALifeDynamicObject*, ObjectIndexSize> m_objectIndex{};
    std::array<std::array<u64, SaveExtensionObjectWordCount>, SaveExtensionObjectTypeCount>
        m_saveExtensionObjectTypes{};

private:
    void save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count, NET_Packet& packet);
    u32 save_object(
        IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count, NET_Packet& packet);
    u32 save_object(
        CMemoryWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count,
        NET_Packet& packet, SaveObjectProfile* profile = nullptr);
    static CSE_ALifeDynamicObject* get_object(IReader& file_stream, NET_Packet& packet);
    void index_save_extension_object(CSE_ALifeDynamicObject* object);

public:
    static CSE_ALifeDynamicObject* get_object(IReader& file_stream);

public:
    CALifeObjectRegistry(LPCSTR section);
    virtual ~CALifeObjectRegistry();
    virtual void save(IWriter& memory_stream);
    void begin_save(IWriter& memory_stream, SaveState& state);
    bool continue_save(IWriter& memory_stream, SaveState& state, float budgetMilliseconds);
    bool continue_save(
        IWriter& memory_stream, SaveState& state, float budgetMilliseconds, NET_Packet& packet);
    void load(IReader& file_stream);
    CSE_ALifeDynamicObject* next_save_extension_object(
        ESaveExtensionObjectType type, u32& nextObjectId) const;
    IC void add(CSE_ALifeDynamicObject* object);
    IC void remove(const ALife::_OBJECT_ID& id, bool no_assert = false);
    IC CSE_ALifeDynamicObject* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
    IC const OBJECT_REGISTRY& objects() const;
    IC OBJECT_REGISTRY& objects();
};

#include "alife_object_registry_inline.h"
