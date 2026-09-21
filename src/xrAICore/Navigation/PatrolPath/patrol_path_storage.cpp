////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_path_storage.cpp
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol path storage
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"
#include "patrol_path_storage.h"
#include "patrol_path.h"
#include "patrol_point.h"
#include "xrAICore/Navigation/level_graph.h"
#include "Common/LevelGameDef.h"

// The registry may hold the same CPatrolPath* under several keys: add_alias_if_exist()
// maps an extra name onto an existing object, and such an entry references rather than
// owns it. delete_data() frees every pair in turn, so the allocator receives the same
// block twice. Collect the pointers, deduplicate, then delete each exactly once.
void CPatrolPathStorage::destroy_registry()
{
    xr_vector<CPatrolPath*> owned;
    owned.reserve(m_registry.size());
    for (auto& it : m_registry)
        if (it.second)
            owned.push_back(it.second);

    std::sort(owned.begin(), owned.end());
    owned.erase(std::unique(owned.begin(), owned.end()), owned.end());

    for (CPatrolPath* path : owned)
        xr_delete(path);

    m_registry.clear();
    m_runtime.clear();
}

CPatrolPathStorage::~CPatrolPathStorage() { destroy_registry(); }
void CPatrolPathStorage::load_raw(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph, IReader& stream)
{
    ZoneScoped;

    IReader* chunk = stream.open_chunk(WAY_PATROLPATH_CHUNK);

    if (!chunk)
        return;

    u32 chunk_iterator;
    for (IReader* sub_chunk = chunk->open_chunk_iterator(chunk_iterator); sub_chunk;
         sub_chunk = chunk->open_chunk_iterator(chunk_iterator, sub_chunk))
    {
        R_ASSERT(sub_chunk->find_chunk(WAYOBJECT_CHUNK_VERSION));
        R_ASSERT(sub_chunk->r_u16() == WAYOBJECT_VERSION);
        R_ASSERT(sub_chunk->find_chunk(WAYOBJECT_CHUNK_NAME));

        shared_str patrol_name;
        sub_chunk->r_stringZ(patrol_name);
        VERIFY3(m_registry.find(patrol_name) == m_registry.end(), "Duplicated patrol path found", patrol_name.c_str());
        m_registry.emplace(
            patrol_name, &(xr_new<CPatrolPath>(patrol_name))->load_raw(level_graph, cross, game_graph, *sub_chunk)
		);
    }

    chunk->close();
}

void CPatrolPathStorage::load(IReader& stream)
{
    ZoneScoped;

    IReader* chunk = stream.open_chunk(0);
    const u32 size = chunk->r_u32();
    chunk->close();

    // destroy_registry(), not clear(): the registry holds owning pointers. There is no
    // leak today because AISpaceBase always recreates the storage before load(), but this
    // keeps ownership consistent with the destructor should load() ever run twice.
    destroy_registry();

    chunk = stream.open_chunk(1);
    for (u32 i = 0; i < size; ++i)
    {
        PATROL_REGISTRY::value_type pair{};

        IReader* chunk1 = chunk->open_chunk(i);
        IReader* chunk2 = chunk1->open_chunk(0);

        load_data(pair.first, *chunk2);
        chunk2->close();

        chunk2 = chunk1->open_chunk(1);
        load_data(pair.second, *chunk2);
        chunk2->close();

        chunk1->close();

        // insert() below overwrites the mapped value on a duplicate key
        // (AssociativeVector::insert: *I = value) instead of rejecting it, so the displaced
        // pointer used to fall out of the map and leak. Dead Air data really does contain
        // duplicate names. Update the mapping first, release the displaced path afterwards;
        // it cannot be aliased because the registry was emptied above and every entry loaded
        // here is a fresh allocation.
        CPatrolPath* displaced = nullptr;

        const_iterator I = m_registry.find(pair.first);
        if (I != m_registry.end())
        {
            Log("~ Duplicated patrol path found ", pair.first.c_str());
            displaced = I->second;
        }

#ifdef DEBUG
        pair.second->name(pair.first);
#endif

        m_registry.insert(pair);

        if (displaced && displaced != pair.second)
            xr_delete(displaced);
    }

    chunk->close();
}

void CPatrolPathStorage::save(IWriter& stream)
{
    stream.open_chunk(0);
    stream.w_u32(m_registry.size());
    stream.close_chunk();

    stream.open_chunk(1);

    PATROL_REGISTRY::iterator I = m_registry.begin();
    PATROL_REGISTRY::iterator E = m_registry.end();
    for (int i = 0; I != E; ++I, ++i)
    {
        stream.open_chunk(i);

        stream.open_chunk(0);
        save_data((*I).first, stream);
        stream.close_chunk();

        stream.open_chunk(1);
        save_data((*I).second, stream);
        stream.close_chunk();

        stream.close_chunk();
    }

    stream.close_chunk();
}

bool CPatrolPathStorage::runtime(shared_str patrol_name) const
{
    return std::find(m_runtime.begin(), m_runtime.end(), patrol_name) != m_runtime.end();
}

bool CPatrolPathStorage::set_runtime_point(shared_str patrol_name, const Fvector& position, float max_snap,
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph)
{
    if (!level_graph || !cross || !game_graph || !patrol_name.size())
        return false;

    // The navmesh decides where a stalker can stand. An exact hit first; a
    // crate on a table or an item in the grass is off the mesh by a little, so
    // the nearest cell is the honest destination - within reason.
    // (with no current vertex to start from, vertex() tries the exact cell and
    // falls back to the nearest one)
    const u32 vertex_id = level_graph->vertex(u32(-1), position);
    if (!level_graph->valid_vertex_id(vertex_id))
        return false;
    if (level_graph->distance(vertex_id, position) > max_snap)
        return false;

    const_iterator it = patrol_paths().find(patrol_name);
    if (it != patrol_paths().end())
    {
        // a name the level shipped is never ours to move
        if (!runtime(patrol_name))
            return false;
        CPatrolPath* path = it->second;
        CPatrolPath::CVertex* point = path->vertex(0);
        if (!point)
            return false;
        point->data(CPatrolPoint(level_graph, cross, game_graph, path, position, vertex_id, 0, "wp00"));
        return true;
    }

    CPatrolPath* path = xr_new<CPatrolPath>(patrol_name);
    path->add_vertex(CPatrolPoint(level_graph, cross, game_graph, path, position, vertex_id, 0, "wp00"), 0);
    m_registry.emplace(patrol_name, path);
    m_runtime.push_back(patrol_name);
    return true;
}

const CPatrolPath* CPatrolPathStorage::add_alias_if_exist(shared_str patrol_name, shared_str duplicate_name)
{
    const_iterator it = patrol_paths().find(patrol_name);
    if (it == patrol_paths().end())
        return nullptr;

    // The alias entry references the path, it does not own it (see destroy_registry).
    // Keep the pointer, not the iterator: emplace can reallocate the underlying vector.
    CPatrolPath* path = it->second;
    m_registry.emplace(duplicate_name, path);
    return path;
}
