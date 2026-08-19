#include "StdAfx.h"

// XMS spawn composer: applies module .xspawn layers on top of the freshly
// loaded base spawn graph. Base spawn ids never move; module vertices live in
// per-module persistent id ranges. See docs/dead-air/XMS_ARCHITECTURE.md p.7.

#include "alife_spawn_registry.h"
#include "xrServerEntities/xrServer_Objects.h"		// CSE_Shape: restrictors carry their geometry
#include "xms_game.h"
#include "xrCore/XMS/xms_core.h"

namespace
{
struct OpSection
{
    xr_string name; // section name without the "obj:" prefix
    xr_vector<std::pair<xr_string, xr_string>> keys;

    pcstr value(pcstr key) const
    {
        for (const auto& [k, v] : keys)
            if (0 == xr_strcmp(k.c_str(), key))
                return v.c_str();
        return nullptr;
    }
};

bool parse_fvector(pcstr text, Fvector& out)
{
    return text && 3 == sscanf(text, "%f , %f , %f", &out.x, &out.y, &out.z);
}

GameGraph::_GRAPH_ID nearest_game_vertex(const CGameGraph& graph, GameGraph::_LEVEL_ID level_id, const Fvector& position)
{
    GameGraph::_GRAPH_ID best = GameGraph::_GRAPH_ID(-1);
    float best_distance = flt_max;
    for (GameGraph::_GRAPH_ID i = 0, count = GameGraph::_GRAPH_ID(graph.header().vertex_count()); i < count; ++i)
    {
        const CGameGraph::CGameVertex* vertex = graph.vertex(i);
        if (vertex->level_id() != level_id)
            continue;
        const float distance = vertex->level_point().distance_to_sqr(position);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

// custom_data value: inline ltx text, or @relative\path.ltx inside the module
bool resolve_custom_data(const XMS::Module& m, pcstr value, shared_str& out)
{
    if (!value || !value[0])
        return false;
    if (value[0] != '@')
    {
        out = value;
        return true;
    }
    string_path path;
    xr_sprintf(path, "%s%s", m.root.c_str(), value + 1);
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        Msg("! XMS: custom_data file not found [%s]", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    xr_vector<char> buffer(size_t(std::max(0l, len)) + 1, 0);
    const size_t got = len > 0 ? fread(buffer.data(), 1, size_t(len), f) : 0;
    fclose(f);
    buffer[got] = 0;
    out = buffer.data();
    return true;
}
} // namespace

void CALifeSpawnRegistry::xms_compose()
{
    if (!XMS::Active() || !m_game_graph)
        return;

    // stable addressing of base objects: "<level>/<name_replace>" -> spawn id
    xr_map<xr_string, ALife::_SPAWN_ID> base_by_key;
    for (const auto& [vertex_id, vertex] : m_spawns.vertices())
    {
        // graph location lives on the ALife layer of the entity
        const CSE_ALifeObject* object = smart_cast<const CSE_ALifeObject*>(&vertex->data()->object());
        if (!object)
            continue;
        const auto* graph_vertex = m_game_graph->vertex(object->m_tGraphID);
        if (!graph_vertex || !m_game_graph->header().level_exist(graph_vertex->level_id()))
            continue;
        xr_string key = m_game_graph->header().level(graph_vertex->level_id()).name().c_str();
        key += "/";
        key += object->name_replace();
        base_by_key.emplace(std::move(key), vertex_id);
    }

    // ledger attribution: who touched a base object last
    xr_map<xr_string, xr_string> last_toucher;
    u32 added = 0, modified = 0, removed = 0, failed = 0;

    for (const XMS::Module& m : XMS::Modules())
    {
        // mode-gated modules contribute spawns only when their mode is active
        if (!XMS::ModuleApplies(m))
            continue;

        xr_vector<xr_string> files;
        string_path spawn_dir;
        xr_sprintf(spawn_dir, "%sspawn" DELIMITER, m.root.c_str());
        XMS::ListFiles(spawn_dir, "*.xspawn", files);
        if (files.empty())
            continue;

        // gather op sections preserving file order
        xr_vector<OpSection> ops;
        for (const xr_string& file : files)
        {
            xr_vector<XMS::IniRecord> records;
            XMS::ReadIniRecords(file.c_str(), records);
            for (XMS::IniRecord& record : records)
            {
                if (0 != strncmp(record.section.c_str(), "obj:", 4))
                    continue;
                const xr_string name = record.section.substr(4);
                if (ops.empty() || ops.back().name != name)
                {
                    OpSection section;
                    section.name = name;
                    ops.emplace_back(std::move(section));
                }
                ops.back().keys.emplace_back(std::move(record.key), std::move(record.value));
            }
        }

        for (const OpSection& op : ops)
        {
            // per-op mode gate on top of the module-level one
            if (pcstr op_mode = op.value("mode"))
                if (!XMS::ModeActive(op_mode))
                    continue;
            pcstr op_kind = op.value("op");
            if (!op_kind)
                op_kind = "add";
            const bool is_base_target = 0 == strncmp(op.name.c_str(), "base:", 5);

            // ---- modify / remove of a base (or earlier-module) object ------
            if (is_base_target)
            {
                const xr_string target_key = op.name.substr(5);
                const auto found = base_by_key.find(target_key);
                if (found == base_by_key.end())
                {
                    Msg("! XMS: [%s] target not found: %s", m.id.c_str(), target_key.c_str());
                    ++failed;
                    continue;
                }
                SPAWN_GRAPH::CVertex* vertex = m_spawns.vertex(found->second);
                if (!vertex)
                {
                    // removed by an earlier module - the declared order wins
                    Msg("! XMS: [%s] target already removed: %s", m.id.c_str(), target_key.c_str());
                    XMS::AddConflict(XMS::ConflictKind::Other, target_key.c_str(),
                        (m.id + " (target missing)").c_str(), last_toucher[target_key].c_str());
                    ++failed;
                    continue;
                }

                const auto toucher = last_toucher.find(target_key);
                if (toucher != last_toucher.end())
                    XMS::AddConflict(XMS::ConflictKind::Other, ("spawn:" + target_key).c_str(),
                        toucher->second.c_str(), m.id.c_str());
                last_toucher[target_key] = m.id;

                if (0 == xr_strcmp(op_kind, "remove"))
                {
                    // strip incoming edges first so the graph stays coherent
                    for (const auto& [other_id, other_vertex] : m_spawns.vertices())
                    {
                        if (other_id == found->second)
                            continue;
                        if (m_spawns.edge(other_id, found->second))
                            m_spawns.remove_edge(other_id, found->second);
                    }
                    m_spawns.remove_vertex(found->second);
                    ++removed;
                    continue;
                }

                if (0 != xr_strcmp(op_kind, "modify"))
                {
                    Msg("! XMS: [%s] bad op '%s' for base target %s", m.id.c_str(), op_kind, target_key.c_str());
                    ++failed;
                    continue;
                }

                CSE_Abstract& object = vertex->data()->object();
                Fvector vec;
                if (parse_fvector(op.value("position"), vec))
                    object.o_Position = vec;
                if (parse_fvector(op.value("direction"), vec))
                    object.o_Angle = vec;
                if (pcstr visual = op.value("visual"))
                    if (CSE_Visual* visual_object = smart_cast<CSE_Visual*>(&object))
                        visual_object->set_visual(visual);
                if (pcstr profile = op.value("character_profile"))
                    if (CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(&object))
                        trader->set_character_profile(profile);
                if (pcstr custom = op.value("custom_data"))
                {
                    shared_str data;
                    if (resolve_custom_data(m, custom, data))
                        if (CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(&object))
                            alife_object->m_ini_string = data;
                }
                if (pcstr disabled = op.value("disabled"))
                    object.m_spawn_flags.set(CSE_Abstract::flSpawnEnabled, 0 != xr_strcmp(disabled, "true"));
                ++modified;
                continue;
            }

            // ---- add a module-owned object ---------------------------------
            if (0 != xr_strcmp(op_kind, "add"))
            {
                Msg("! XMS: [%s] op '%s' needs a base: target (%s)", m.id.c_str(), op_kind, op.name.c_str());
                ++failed;
                continue;
            }

            pcstr section = op.value("section");
            pcstr level_name = op.value("level");
            Fvector position;
            if (!section || !level_name || !parse_fvector(op.value("position"), position))
            {
                Msg("! XMS: [%s] add %s: section/level/position are required", m.id.c_str(), op.name.c_str());
                ++failed;
                continue;
            }
            if (!m_game_graph->header().level_exist(level_name))
            {
                Msg("! XMS: [%s] add %s: unknown level [%s]", m.id.c_str(), op.name.c_str(), level_name);
                ++failed;
                continue;
            }

            u32 range_base = 0;
            if (!XMS::SpawnRange(m.id.c_str(), m.budget_spawns, range_base))
            {
                ++failed;
                continue;
            }
            u32 spawn_id = 0;
            if (!XMS::SpawnLocalId(m.id.c_str(), op.name.c_str(), spawn_id))
            {
                ++failed;
                continue;
            }
            if (m_spawns.vertex(ALife::_SPAWN_ID(spawn_id)))
            {
                Msg("! XMS: [%s] add %s: spawn id %u already taken", m.id.c_str(), op.name.c_str(), spawn_id);
                ++failed;
                continue;
            }

            CSE_Abstract* abstract = F_entity_Create(section, true);
            CSE_ALifeObject* alife_object = abstract ? smart_cast<CSE_ALifeObject*>(abstract) : nullptr;
            if (!alife_object)
            {
                Msg("! XMS: [%s] add %s: section [%s] is not spawnable (no ALife entity)", m.id.c_str(),
                    op.name.c_str(), section);
                if (abstract)
                    F_entity_Destroy(abstract);
                ++failed;
                continue;
            }

            const GameGraph::_LEVEL_ID level_id = m_game_graph->header().level(level_name).id();
            GameGraph::_GRAPH_ID game_vertex = GameGraph::_GRAPH_ID(-1);
            if (pcstr explicit_vertex = op.value("game_vertex"))
                if (0 != xr_strcmp(explicit_vertex, "auto"))
                    game_vertex = GameGraph::_GRAPH_ID(atoi(explicit_vertex));
            if (game_vertex == GameGraph::_GRAPH_ID(-1))
                game_vertex = nearest_game_vertex(*m_game_graph, level_id, position);
            if (game_vertex == GameGraph::_GRAPH_ID(-1) || !m_game_graph->vertex(game_vertex))
            {
                Msg("! XMS: [%s] add %s: no game vertex on level [%s]", m.id.c_str(), op.name.c_str(), level_name);
                F_entity_Destroy(abstract);
                ++failed;
                continue;
            }

            string256 full_name;
            xr_sprintf(full_name, "%s.%s", m.id.c_str(), op.name.c_str());
            abstract->set_name_replace(full_name);
            abstract->o_Position = position;
            Fvector direction;
            if (parse_fvector(op.value("direction"), direction))
                abstract->o_Angle = direction;
            alife_object->m_tGraphID = GameGraph::_GRAPH_ID(game_vertex);
            alife_object->m_tNodeID = m_game_graph->vertex(game_vertex)->level_vertex_id();
            if (pcstr level_vertex = op.value("level_vertex"))
                alife_object->m_tNodeID = u32(atoll(level_vertex));
            abstract->m_tSpawnID = ALife::_SPAWN_ID(spawn_id);
            abstract->ID = 0xffff;
            abstract->ID_Parent = 0xffff;
            abstract->m_spawn_flags.set(CSE_Abstract::flSpawnEnabled, TRUE);

            // Overriding the visual on a generic section is the cheap way to place a
            // model: no generated config section per prop, and two modules dropping
            // different meshes stay independent ops.
            if (pcstr visual = op.value("visual"))
                if (CSE_Visual* visual_object = smart_cast<CSE_Visual*>(abstract))
                    visual_object->set_visual(visual);

            // A restrictor without its shape is a zone nothing can ever be inside: the
            // marker points at it correctly and walking in does nothing, because the
            // shape list the editor drew is not part of the section.
            if (CSE_Shape* shape_object = smart_cast<CSE_Shape*>(abstract))
            {
                const int shape_count = op.value("shapes") ? atoi(op.value("shapes")) : 0;
                xr_vector<CShapeData::shape_def> shapes;
                for (int si = 0; si < shape_count; ++si)
                {
                    string64 key;
                    xr_sprintf(key, "shape%d", si);
                    pcstr text = op.value(key);
                    if (!text)
                        continue;
                    CShapeData::shape_def def{};
                    if (0 == strncmp(text, "sphere", 6))
                    {
                        def.type = CShapeData::cfSphere;
                        if (4 != sscanf(text + 6, " , %f , %f , %f , %f", &def.data.sphere.P.x,
                                &def.data.sphere.P.y, &def.data.sphere.P.z, &def.data.sphere.R))
                            continue;
                    }
                    else if (0 == strncmp(text, "box", 3))
                    {
                        def.type = CShapeData::cfBox;
                        Fmatrix& b = def.data.box;
                        b.identity();
                        if (12 != sscanf(text + 3, " , %f , %f , %f , %f , %f , %f , %f , %f , %f , %f , %f , %f",
                                &b.i.x, &b.i.y, &b.i.z, &b.j.x, &b.j.y, &b.j.z,
                                &b.k.x, &b.k.y, &b.k.z, &b.c.x, &b.c.y, &b.c.z))
                            continue;
                    }
                    else
                        continue;
                    shapes.push_back(def);
                }
                if (!shapes.empty())
                    shape_object->assign_shapes(&shapes[0], u32(shapes.size()));
            }
            if (pcstr profile = op.value("character_profile"))
                if (CSE_ALifeTraderAbstract* trader = smart_cast<CSE_ALifeTraderAbstract*>(abstract))
                    trader->set_character_profile(profile);

            if (pcstr custom = op.value("custom_data"))
            {
                shared_str data;
                if (resolve_custom_data(m, custom, data))
                    alife_object->m_ini_string = data;
            }
            if (pcstr story = op.value("story_id"))
                alife_object->m_story_id = (u32(m.ns) << 16) | (u32(atoi(story)) & 0xFFFF);

            m_spawns.add_vertex(xr_new<CServerEntityWrapper>(abstract), ALife::_SPAWN_ID(spawn_id));
            ++added;
        }
    }

    if (added || modified || removed || failed)
    {
        Msg("* XMS: spawn layers composed: +%u added, %u modified, %u removed, %u failed", added, modified, removed,
            failed);
        XMS::WriteReport();
    }
}
