////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_spawn_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife spawn registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_spawn_registry.h"
#include "xms_game.h"
#include "Common/object_broker.h"
#include "game_base.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"

namespace
{
constexpr size_t spawnUpdateBatchWords = 16 * 1024;
} // namespace

CALifeSpawnRegistry::CALifeSpawnRegistry(LPCSTR section)
{
    m_spawn_name = "";
    seed(u32(CPU::QPC() & 0xffffffff));
    m_game_graph = nullptr;
    m_chunk = nullptr;
    m_file = nullptr;
}

CALifeSpawnRegistry::~CALifeSpawnRegistry()
{
    xr_delete(m_game_graph);
    if (m_chunk)
        m_chunk->close();
    if (m_file)
        FS.r_close(m_file);
}

void CALifeSpawnRegistry::save(IWriter& memory_stream)
{
    SaveState state;
    begin_save(memory_stream, state);
    continue_save(memory_stream, state, flt_max);
}

void CALifeSpawnRegistry::begin_save(IWriter& memory_stream, SaveState& state)
{
    Msg("* Saving spawns...");
    memory_stream.open_chunk(SPAWN_CHUNK_DATA);

    memory_stream.open_chunk(0);
    memory_stream.w_stringZ(m_spawn_name);
    memory_stream.w(&header().guid(), sizeof(header().guid()));
    memory_stream.close_chunk();

    memory_stream.open_chunk(1);
    state.updateWordOffset = 0;
}

bool CALifeSpawnRegistry::continue_save(IWriter& memory_stream, SaveState& state, float budgetMilliseconds)
{
    CTimer budgetTimer;
    budgetTimer.Start();

    while (state.updateWordOffset < m_save_update_words.size())
    {
        const size_t batchWords = std::min(
            spawnUpdateBatchWords, m_save_update_words.size() - state.updateWordOffset);

        // Spawn updates are empty in this save format, so copy their prebuilt chunk headers in large batches.
        memory_stream.w(m_save_update_words.data() + state.updateWordOffset, batchWords * sizeof(u32));
        state.updateWordOffset += batchWords;

        if (state.updateWordOffset != m_save_update_words.size() && budgetMilliseconds != flt_max &&
            budgetTimer.GetElapsed_sec() * 1000.f >= budgetMilliseconds)
        {
            return false;
        }
    }

    memory_stream.close_chunk();
    memory_stream.close_chunk();
    return true;
}

void CALifeSpawnRegistry::load(IReader& file_stream, LPCSTR game_name)
{
    R_ASSERT(FS.exist(game_name));

    IReader *chunk, *chunk0;
    Msg("* Loading spawn registry...");
    R_ASSERT2(file_stream.find_chunk(SPAWN_CHUNK_DATA), "Cannot find chunk SPAWN_CHUNK_DATA!");
    chunk0 = file_stream.open_chunk(SPAWN_CHUNK_DATA);

    xrGUID guid;
    chunk = chunk0->open_chunk(0);
    VERIFY(chunk);
    chunk->r_stringZ(m_spawn_name);
    chunk->r(&guid, sizeof(guid));
    chunk->close();

    string_path file_name;
    bool file_exists = !!FS.exist(file_name, "$game_spawn$", m_spawn_name.c_str(), ".spawn");
    R_ASSERT3(file_exists, "Can't find spawn file:", m_spawn_name.c_str());

    VERIFY(!m_file);
    m_file = FS.r_open(file_name);
    load(*m_file, &guid);

    chunk0->close();
}

void CALifeSpawnRegistry::load(LPCSTR spawn_name)
{
    Msg("* Loading spawn registry...");
    m_spawn_name = spawn_name;
    string_path file_name;
    R_ASSERT3(FS.exist(file_name, "$game_spawn$", m_spawn_name.c_str(), ".spawn"), "Can't find spawn file:", m_spawn_name.c_str());

    VERIFY(!m_file);
    m_file = FS.r_open(file_name);
    load(*m_file);
}

static bool ignore_save_incompatibility() { return (!!strstr(Core.Params, "-ignore_save_incompatibility")); }
void CALifeSpawnRegistry::load(IReader& file_stream, xrGUID* save_guid)
{
    IReader* chunk;
    chunk = file_stream.open_chunk(0);
    m_header.load(*chunk);
    chunk->close();
    R_ASSERT2(!save_guid || (*save_guid == header().guid()) || ignore_save_incompatibility(),
        "Saved game doesn't correspond to the spawn : DELETE SAVED GAME!");

    chunk = file_stream.open_chunk(1);
    m_spawns.load(*chunk);
    chunk->close();

    chunk = file_stream.open_chunk(2);
    load_data(m_artefact_spawn_positions, *chunk);
    chunk->close();

    chunk = file_stream.open_chunk(3);
    R_ASSERT2(chunk, "Spawn version mismatch - REBUILD SPAWN!");
    ai().patrol_path_storage(*chunk);
    chunk->close();

    VERIFY(!m_chunk);
    if (header().version() >= XRAI_VERSION_PRIQUEL)
        m_chunk = file_stream.open_chunk(4);
    else // XRAI_VERSION_SOC and below
    {
        string_path file_name;
        FS.update_path(file_name, "$game_data$", GRAPH_NAME);
        m_chunk = FS.r_open(file_name);
    }
    R_ASSERT2(m_chunk, "Spawn version mismatch - REBUILD SPAWN!");

    // XMS P5: module levels join the graph here; base ids and guid unchanged
    m_chunk = XmsGame::ComposeGameGraph(m_chunk);

    VERIFY(!m_game_graph);
    m_game_graph = xr_new<CGameGraph>(*m_chunk);
    ai().SetGameGraph(m_game_graph);

    R_ASSERT2((header().graph_guid() == ai().game_graph().header().guid()) || ignore_save_incompatibility(),
        "Spawn doesn't correspond to the graph : REBUILD SPAWN!");

    // module spawn layers go on top of the untouched base file; the derived
    // indices below pick the added vertices up automatically
    xms_compose();

    build_story_spawns();

    build_root_spawns();

    Msg("* %d spawn points are successfully loaded", m_spawns.vertex_count());
}

void CALifeSpawnRegistry::save_updates(IWriter& stream)
{
    if (!m_save_update_words.empty())
        stream.w(m_save_update_words.data(), m_save_update_words.size() * sizeof(u32));
}

void CALifeSpawnRegistry::load_updates(IReader& stream)
{
    u32 vertex_id;
    for (IReader* chunk = stream.open_chunk_iterator(vertex_id); chunk;
         chunk = stream.open_chunk_iterator(vertex_id, chunk))
    {
        VERIFY(u32(ALife::_SPAWN_ID(-1)) > vertex_id);
        const SPAWN_GRAPH::CVertex* vertex = m_spawns.vertex(ALife::_SPAWN_ID(vertex_id));
        VERIFY(vertex);
        vertex->data()->load_update(*chunk);
    }
}

void CALifeSpawnRegistry::build_root_spawns()
{
    m_temp0.clear();
    m_temp1.clear();

    {
        SPAWN_GRAPH::const_vertex_iterator I = m_spawns.vertices().begin();
        SPAWN_GRAPH::const_vertex_iterator E = m_spawns.vertices().end();
        for (; I != E; ++I)
            m_temp0.push_back((*I).second->vertex_id());
    }

    {
        SPAWN_GRAPH::const_vertex_iterator I = m_spawns.vertices().begin();
        SPAWN_GRAPH::const_vertex_iterator E = m_spawns.vertices().end();
        for (; I != E; ++I)
        {
            SPAWN_GRAPH::const_iterator i = (*I).second->edges().begin();
            SPAWN_GRAPH::const_iterator e = (*I).second->edges().end();
            for (; i != e; ++i)
                m_temp1.push_back((*i).vertex_id());
        }
    }

    process_spawns(m_temp0);
    process_spawns(m_temp1);

    m_spawn_roots.resize(m_temp0.size() + m_temp1.size());
    xr_vector<ALife::_SPAWN_ID>::iterator I =
        std::set_difference(m_temp0.begin(), m_temp0.end(), m_temp1.begin(), m_temp1.end(), m_spawn_roots.begin());

    m_spawn_roots.erase(I, m_spawn_roots.end());
}

void CALifeSpawnRegistry::build_story_spawns()
{
    m_save_update_words.clear();
    m_save_update_words.reserve(m_spawns.vertex_count() * 2);

    SPAWN_GRAPH::const_vertex_iterator I = m_spawns.vertices().begin();
    SPAWN_GRAPH::const_vertex_iterator E = m_spawns.vertices().end();
    for (; I != E; ++I)
    {
        m_save_update_words.emplace_back(static_cast<u32>((*I).second->vertex_id()));
        m_save_update_words.emplace_back(0);

        CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(&(*I).second->data()->object());
        VERIFY(object);
        if (object->m_spawn_story_id == INVALID_SPAWN_STORY_ID)
            continue;

        m_spawn_story_ids.emplace(object->m_spawn_story_id, (*I).first);
    }
}
