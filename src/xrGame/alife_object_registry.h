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

class CALifeObjectRegistry
{
public:
    typedef xr_map<ALife::_OBJECT_ID, CSE_ALifeDynamicObject*> OBJECT_REGISTRY;
    struct SaveState
    {
        xr_vector<ALife::_OBJECT_ID> pendingObjects;
        xr_vector<ALife::_OBJECT_ID> traversalObjects;
        xr_vector<u8> visitedObjects;
        u32 nextObjectIndex{};
        u32 rootScanPosition{};
        u32 objectCount{};
        u32 countPosition{};
        float averageObjectMilliseconds{0.005f};
        float maxBatchMilliseconds{};
        u32 maxBatchObjects{};
        u32 maxBatchBytes{};
        u8 planningPhase{};
        bool open{};
    };

protected:
    struct SaveCostEstimate
    {
        float milliseconds{0.005f};
        float packetBytes{256.f};
    };

    OBJECT_REGISTRY m_objects;

private:
    static xr_map<shared_str, SaveCostEstimate>& save_section_costs();
    static void load_save_costs();
    static void store_save_costs();
    void save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count);
    u32 save_object(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count);

public:
    static CSE_ALifeDynamicObject* get_object(IReader& file_stream);

public:
    CALifeObjectRegistry(LPCSTR section);
    virtual ~CALifeObjectRegistry();
    virtual void save(IWriter& memory_stream);
    void begin_save(IWriter& memory_stream, SaveState& state);
    bool continue_save(IWriter& memory_stream, SaveState& state, float budgetMilliseconds);
    void load(IReader& file_stream);
    IC void add(CSE_ALifeDynamicObject* object);
    IC void remove(const ALife::_OBJECT_ID& id, bool no_assert = false);
    IC CSE_ALifeDynamicObject* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
    IC const OBJECT_REGISTRY& objects() const;
    IC OBJECT_REGISTRY& objects();
};

#include "alife_object_registry_inline.h"
