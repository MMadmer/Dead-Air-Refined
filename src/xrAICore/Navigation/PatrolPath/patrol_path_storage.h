////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_path_storage.h
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol path storage
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/object_interfaces.h"
#include "xrCore/Containers/AssociativeVector.hpp"
#include "xrCore/xrstring.h"

class CPatrolPath;
class CLevelGraph;
class CGameLevelCrossTable;
class CGameGraph;

class XRAICORE_API CPatrolPathStorage : public ISerializable
{
private:
    typedef ISerializable inherited;

public:
    typedef AssociativeVector<shared_str, CPatrolPath*> PATROL_REGISTRY;
    typedef PATROL_REGISTRY::iterator iterator;
    typedef PATROL_REGISTRY::const_iterator const_iterator;

protected:
    PATROL_REGISTRY m_registry;

public:
    IC CPatrolPathStorage();
    virtual ~CPatrolPathStorage();
    // Frees the registry honouring aliases: one path can be mapped under several
    // names, so every pointer must be deleted exactly once. Details in the .cpp.
    void destroy_registry();
    virtual void load(IReader& stream);
    virtual void save(IWriter& stream);

public:
    void load_raw(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph,
        IReader& stream);
    IC const CPatrolPath* path(shared_str patrol_name, bool no_assert = false) const;
    IC const PATROL_REGISTRY& patrol_paths() const;

    const CPatrolPath* add_alias_if_exist(shared_str patrol_name, shared_str duplicate_name);

    // Runtime one-point paths (XMS behaviour graphs): a destination nobody drew
    // in the level editor. Creates the path, or MOVES its single point when the
    // name is already a runtime one. The position is snapped to the nearest
    // navmesh cell; false when there is no cell within `max_snap` metres, when
    // the name belongs to a path the level shipped, or without a level graph.
    //
    // A runtime path is never freed or replaced before the storage itself dies:
    // CPatrolPathManager keeps a raw CPatrolPath* plus point INDICES, so moving
    // the point in place is safe where deleting the object would not be.
    // Nothing of this is serialized - the storage is level data, not a save.
    bool set_runtime_point(shared_str patrol_name, const Fvector& position, float max_snap,
        const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph);

    // A whole runtime path - a waypoint path a MODULE drew in the level editor and
    // ships in its spawn layer, registered once the level's navmesh is loaded. Same
    // rules as a runtime point: every point is snapped to the nearest navmesh cell
    // within `max_snap` (a point that has none refuses the whole path), a name the
    // level shipped is never taken, and an existing runtime path of that name is left
    // as it is - an NPC may already be walking it.
    struct RuntimePoint
    {
        Fvector position;
        u32 flags;
        shared_str name;
    };
    struct RuntimeLink
    {
        u32 from, to;
        float weight;
    };
    bool set_runtime_path(shared_str patrol_name, const xr_vector<RuntimePoint>& points,
        const xr_vector<RuntimeLink>& links, float max_snap, const CLevelGraph* level_graph,
        const CGameLevelCrossTable* cross, const CGameGraph* game_graph);
    bool runtime(shared_str patrol_name) const;

private:
    xr_vector<shared_str> m_runtime; // names created through set_runtime_point / set_runtime_path
};

#include "xrAICore/Navigation/PatrolPath/patrol_path_storage_inline.h"
