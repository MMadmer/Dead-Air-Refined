#include "pch_script.h"

#include "xms_game.h"
#include "save_extension_gameplay.h"
#include "xrCore/XMS/xms_core.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"
#include "alife_simulator_base.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "xrServerEntities/xrServer_Objects_ALife.h"

#include "xrCommon/xr_unordered_map.h"

#include <io.h>

namespace XmsGame
{
namespace
{
struct BlobState
{
    xr_unordered_map<u16, xr_vector<u8>> pending; // set by Lua this session
    xr_unordered_map<u16, xr_vector<u8>> loaded;  // last loaded .scov snapshot
    // applied-spawn ledgers, per module ns: sorted spawn ids this playthrough
    // has instantiated. pending_ledgers is what the next save writes; the
    // loaded set only feeds it (a loaded-only chunk would be dropped from the
    // next save, because the capture walks pending state exclusively).
    xr_unordered_map<u16, xr_vector<u16>> pending_ledgers;
    xr_unordered_map<u16, xr_vector<u16>> loaded_ledgers;
    u32 skipped_objects{0};
};

BlobState& bs()
{
    static BlobState state;
    return state;
}

// manifest payload v1: u32 count, then per module
// { stringZ id, stringZ version, u16 ns, u32 spawn_base, u32 spawn_size }
// v1 tail extensions (readers stop early safely):
//   stringZ active_modes_csv
//   u32 level_count { stringZ level_name, u8 level_id }
void build_manifest(xr_vector<u8>& out)
{
    CMemoryWriter writer;
    u32 count = 0;
    for (const XMS::Module& m : XMS::Modules())
        if (!m.disabled)
            ++count;
    writer.w_u32(count);
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled)
            continue;
        writer.w_stringZ(m.id.c_str());
        writer.w_stringZ(m.version.c_str());
        writer.w_u16(m.ns);
        u32 base = 0;
        const bool has_range = XMS::SpawnRange(m.id.c_str(), m.budget_spawns, base);
        writer.w_u32(has_range ? base : 0);
        writer.w_u32(has_range ? m.budget_spawns : 0);
    }
    // active game modes of this session
    writer.w_stringZ(XMS::ActiveModesCsv());
    // module level ids (self-healing for the local registry)
    xr_vector<std::pair<xr_string, u8>> levels;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled)
            continue;
        string_path dir;
        xr_sprintf(dir, "%sgamedata" DELIMITER "levels" DELIMITER, m.root.c_str());
        _finddata_t entry;
        string_path mask;
        strconcat(mask, dir, "*");
        const intptr_t handle = _findfirst(mask, &entry);
        if (handle == -1)
            continue;
        do
        {
            if (!(entry.attrib & _A_SUBDIR) || 0 == xr_strcmp(entry.name, ".") || 0 == xr_strcmp(entry.name, ".."))
                continue;
            u8 id = 0;
            if (XMS::LevelId(entry.name, id))
                levels.emplace_back(xr_string(entry.name), id);
        } while (_findnext(handle, &entry) == 0);
        _findclose(handle);
    }
    writer.w_u32(u32(levels.size()));
    for (const auto& [name, id] : levels)
    {
        writer.w_stringZ(name.c_str());
        writer.w_u8(id);
    }
    out.assign(writer.pointer(), writer.pointer() + writer.size());
}

void parse_manifest(const xr_vector<u8>& payload)
{
    IReader reader(const_cast<u8*>(payload.data()), payload.size());
    const u32 count = reader.r_u32();
    u32 missing = 0;
    for (u32 i = 0; i < count && !reader.eof(); ++i)
    {
        shared_str id, version;
        reader.r_stringZ(id);
        reader.r_stringZ(version);
        const u16 ns = reader.r_u16();
        const u32 base = reader.r_u32();
        const u32 size = reader.r_u32();
        if (base)
            XMS::AdoptSpawnRange(id.c_str(), base, size); // self-healing when the local registry is gone
        const XMS::Module* current = XMS::FindModule(id.c_str());
        if (!current || current->disabled)
        {
            ++missing;
            Msg("! XMS: save was made with module [%s %s] which is no longer installed", id.c_str(), version.c_str());
            XMS::AddConflict(XMS::ConflictKind::Other, id.c_str(), "present in savegame", "missing at load");
        }
        else if (current->version != version.c_str())
            Msg("* XMS: module [%s] version changed since the save: %s -> %s", id.c_str(), version.c_str(),
                current->version.c_str());
        (void)ns;
    }
    // tail extensions (absent in the earliest saves)
    if (!reader.eof())
    {
        shared_str modes;
        reader.r_stringZ(modes);
        XMS::SetActiveModes(modes.c_str());
    }
    if (!reader.eof())
    {
        const u32 level_count = reader.r_u32();
        for (u32 i = 0; i < level_count && !reader.eof(); ++i)
        {
            shared_str level_name;
            reader.r_stringZ(level_name);
            const u8 level_id = reader.r_u8();
            XMS::AdoptLevelId(level_name.c_str(), level_id);
        }
    }
    if (missing)
        XMS::WriteReport();
}
} // namespace

void SetPendingBlob(u16 ns, const void* data, size_t size)
{
    xr_vector<u8>& blob = bs().pending[ns];
    blob.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
}

const xr_vector<u8>* GetLoadedBlob(u16 ns)
{
    // pending overrides loaded so a mod reads back what it just wrote
    const auto pending_it = bs().pending.find(ns);
    if (pending_it != bs().pending.end())
        return &pending_it->second;
    const auto it = bs().loaded.find(ns);
    return it != bs().loaded.end() ? &it->second : nullptr;
}

void CollectSaveMutations(xr_vector<SaveExtensionGameplay::ChunkMutation>& mutations)
{
    if (!XMS::Active())
        return;

    SaveExtensionGameplay::ChunkMutation manifest;
    manifest.chunk.type = kManifestChunkId;
    manifest.chunk.version = 1;
    manifest.newestSupportedVersion = 1;
    build_manifest(manifest.chunk.payload);
    mutations.emplace_back(std::move(manifest));

    for (auto& [ns, blob] : bs().pending)
    {
        SaveExtensionGameplay::ChunkMutation mutation;
        mutation.chunk.type = kModuleChunkBase | ns;
        mutation.chunk.version = 1;
        mutation.newestSupportedVersion = 1;
        mutation.chunk.payload = blob;
        mutation.erase = blob.empty();
        mutations.emplace_back(std::move(mutation));
    }

    // ledger v1 payload: u32 count, count * u16 spawn id (sorted)
    for (auto& [ns, ids] : bs().pending_ledgers)
    {
        SaveExtensionGameplay::ChunkMutation mutation;
        mutation.chunk.type = kLedgerChunkBase | ns;
        mutation.chunk.version = 1;
        mutation.newestSupportedVersion = 1;
        CMemoryWriter writer;
        writer.w_u32(u32(ids.size()));
        for (const u16 id : ids)
            writer.w_u16(id);
        mutation.chunk.payload.assign(writer.pointer(), writer.pointer() + writer.size());
        mutation.erase = ids.empty();
        mutations.emplace_back(std::move(mutation));
    }
}

void OnSnapshotLoaded(const SaveExtensionContainer::ChunkList& chunks)
{
    BlobState& state = bs();
    state.pending.clear();
    state.loaded.clear();
    state.pending_ledgers.clear();
    state.loaded_ledgers.clear();
    state.skipped_objects = 0;

    // The mode set is process-global and outlives "quit to menu, load another
    // save", so it is cleared HERE rather than only overwritten by a manifest
    // that may not be there: a save written before this module existed, one
    // whose .scov went stale, or one made with -no_xms carries no manifest at
    // all, and inheriting the previous session's modes would let mode-gated
    // content into a save that never asked for it.
    XMS::SetActiveModes("");
    if (!XMS::Active() && chunks.empty())
        return;

    for (const SaveExtensionContainer::Chunk& chunk : chunks)
    {
        if (chunk.type == kManifestChunkId && chunk.version == 1)
            parse_manifest(chunk.payload);
        else if ((chunk.type & 0xFFFF0000) == kModuleChunkBase && chunk.version == 1)
            state.loaded[u16(chunk.type & 0xFFFF)] = chunk.payload;
        else if ((chunk.type & 0xFFFF0000) == kLedgerChunkBase && chunk.version == 1)
        {
            if (chunk.payload.size() < sizeof(u32))
                continue;
            IReader reader(const_cast<u8*>(chunk.payload.data()), int(chunk.payload.size()));
            const u32 count = reader.r_u32();
            if (chunk.payload.size() < sizeof(u32) + size_t(count) * sizeof(u16))
                continue;
            xr_vector<u16>& ids = state.loaded_ledgers[u16(chunk.type & 0xFFFF)];
            ids.resize(count);
            for (u32 i = 0; i < count; ++i)
                ids[i] = reader.r_u16();
            std::sort(ids.begin(), ids.end());
        }
    }
}

void SyncModesFromNewGameOptions()
{
    // A fresh game inherits nothing from the session before it: the mode set,
    // and the per-module blobs a loaded save left behind, are both process
    // global.
    BlobState& state = bs();
    state.pending.clear();
    state.loaded.clear();
    state.pending_ledgers.clear();
    state.loaded_ledgers.clear();
    state.skipped_objects = 0;
    if (!XMS::Active())
        return;

    // Always publish a set, even an empty one: XMS::SetActiveModes writes
    // process-global state that survives "quit to menu, start another game",
    // so skipping the call would carry the previous session's modes over.
    string_path options;
    FS.update_path(options, "$game_config$", "axr_options.ltx");
    if (!FS.exist(options))
    {
        XMS::SetActiveModes("");
        return;
    }

    CInifile ini(options, TRUE /*read only*/);
    constexpr pcstr section = "character_creation";
    if (!ini.section_exist(section))
    {
        XMS::SetActiveModes("");
        return;
    }

    // The screen writes one key per checkbox and leaves the unticked ones
    // empty. new_game_metro_mode -> "metro", matching the ids the editor
    // offers and a module's [module] mode.
    //
    // Both halves of the name are required. That section holds plenty of other
    // new_game_* keys the same screen writes as booleans - good_wpn, good_loot,
    // unlocked_guide, loadout_code - and none of them is a mode. The value has
    // to read exactly "true" for the same reason: that is what the game's own
    // is_*_mode() helpers compare against, and a mode active in C++ but not in
    // Lua is worse than one that is off in both.
    constexpr pcstr suffix = "_mode";
    const size_t suffix_len = xr_strlen(suffix);
    xr_string csv;
    for (const auto& item : ini.r_section(section).Data)
    {
        pcstr key = item.first.c_str();
        if (0 != _strnicmp(key, "new_game_", 9))
            continue;
        pcstr value = item.second.c_str();
        if (!value || 0 != _stricmp(value, "true"))
            continue;

        xr_string id = key + 9;
        if (id.size() <= suffix_len || 0 != _stricmp(id.c_str() + id.size() - suffix_len, suffix))
            continue;
        id.erase(id.size() - suffix_len);
        if (id.empty())
            continue;
        xr_strlwr(&id[0]);
        if (!csv.empty())
            csv += ",";
        csv += id;
    }

    XMS::SetActiveModes(csv.c_str());
    if (!csv.empty())
        Msg("* XMS: new game modes: %s", csv.c_str());
}

u32 SkippedObjectCount() { return bs().skipped_objects; }
void ResetSkippedObjects() { bs().skipped_objects = 0; }

void NoteSkippedObject(pcstr section)
{
    ++bs().skipped_objects;
    Msg("! XMS: skipped saved object with unknown section [%s]", section ? section : "?");
}

u32 LateSpawnCompose(CALifeSimulatorBase& sim)
{
    if (!XMS::Active())
        return 0;
    BlobState& state = bs();

    // every loaded ledger is re-emitted even when its module is now absent or
    // gated off: the capture walks pending state only, so a loaded-only chunk
    // would silently fall out of the next save
    for (const auto& [ns, ids] : state.loaded_ledgers)
        if (state.pending_ledgers.find(ns) == state.pending_ledgers.end())
            state.pending_ledgers[ns] = ids;

    // spawn ids already embodied by loaded (or freshly spawned) objects - the
    // secondary guard, and the whole seed on the new-game path
    xr_vector<bool> occupied(65536, false);
    for (const auto& [id, object] : sim.objects().objects())
        if (object->m_tSpawnID != ALife::_SPAWN_ID(-1))
            occupied[object->m_tSpawnID] = true;

    u32 created_total = 0;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled || !XMS::ModuleApplies(m))
            continue;
        u32 base = 0;
        if (!XMS::SpawnRange(m.id.c_str(), m.budget_spawns, base))
            continue;

        xr_vector<u16>& ledger = state.pending_ledgers[m.ns];
        u32 created = 0;
        const u32 top = std::min(base + m.budget_spawns, 65535u);
        for (u32 id = base; id < top; ++id)
        {
            const ALife::_SPAWN_ID spawn_id = ALife::_SPAWN_ID(id);
            if (!sim.spawns().spawns().vertex(spawn_id))
                continue;
            const bool in_ledger = std::binary_search(ledger.begin(), ledger.end(), u16(id));
            if (!in_ledger && !occupied[id])
            {
                // same recipe as CALifeSurgeManager::spawn_new_spawns, one
                // vertex at a time: clone the composed prototype, take a
                // fresh object id, register offline
                CSE_ALifeDynamicObject* prototype =
                    smart_cast<CSE_ALifeDynamicObject*>(&sim.spawns().spawns().vertex(spawn_id)->data()->object());
                if (!prototype)
                    continue;
                CSE_ALifeDynamicObject* object = nullptr;
                sim.create(object, prototype, spawn_id);
                ++created;
            }
            if (!in_ledger)
                ledger.insert(std::upper_bound(ledger.begin(), ledger.end(), u16(id)), u16(id));
        }
        if (created)
            Msg("* XMS: [%s] late spawn: +%u object(s) composed into this playthrough", m.id.c_str(), created);
        created_total += created;
    }
    return created_total;
}

u32 RecomposeAndLateSpawn(CALifeSimulatorBase& sim)
{
    if (!XMS::Active())
        return 0;
    sim.spawns().xms_recompose();
    return LateSpawnCompose(sim);
}
} // namespace XmsGame

// ---- Lua natives -----------------------------------------------------------

namespace
{
const XMS::Module* find_enabled(pcstr id)
{
    const XMS::Module* m = XMS::FindModule(id);
    return m && !m->disabled ? m : nullptr;
}

luabind::object xms_native_modules(lua_State* L)
{
    luabind::object result = luabind::newtable(L);
    int index = 1;
    for (const XMS::Module& m : XMS::Modules())
    {
        luabind::object entry = luabind::newtable(L);
        entry["id"] = m.id.c_str();
        entry["name"] = m.name.c_str();
        entry["version"] = m.version.c_str();
        entry["ns"] = u32(m.ns);
        entry["layer"] = u32(m.layer);
        entry["enabled"] = !m.disabled;
        result[index++] = entry;
    }
    return result;
}

bool xms_native_is_loaded(pcstr id) { return !!find_enabled(id); }

u32 xms_native_ns(pcstr id)
{
    const XMS::Module* m = find_enabled(id);
    return m ? m->ns : 0;
}

// (ns << 16) | local: collision-free story ids across modules
u32 xms_native_story_id(pcstr id, u32 local_id)
{
    const XMS::Module* m = find_enabled(id);
    if (!m)
        return u32(-1);
    return (u32(m->ns) << 16) | (local_id & 0xFFFF);
}

bool valid_script_name(pcstr name)
{
    if (!name || !name[0] || strstr(name, ".."))
        return false;
    for (pcstr c = name; *c; ++c)
    {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '_' || *c == '-' || *c == '/' || *c == '.';
        if (!ok)
            return false;
    }
    return true;
}

luabind::object xms_native_read_script(lua_State* L, pcstr id, pcstr name)
{
    const XMS::Module* m = find_enabled(id);
    if (!m || !valid_script_name(name))
        return luabind::object();
    string_path path;
    xr_sprintf(path, "%sscripts" DELIMITER "%s.script", m->root.c_str(), name);
    for (char* c = path; *c; ++c)
        if (*c == '/')
            *c = _DELIMITER;

    FILE* f = fopen(path, "rb");
    if (!f)
        return luabind::object();
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    xr_vector<char> buffer(size_t(std::max(0l, len)));
    const size_t got = len > 0 ? fread(buffer.data(), 1, size_t(len), f) : 0;
    fclose(f);

    lua_pushlstring(L, buffer.data(), got);
    luabind::object result(luabind::from_stack(L, -1));
    lua_pop(L, 1);
    return result;
}

bool xms_native_save_data(lua_State* L, pcstr id, luabind::object data)
{
    const XMS::Module* m = find_enabled(id);
    if (!m)
        return false;
    if (luabind::type(data) == LUA_TNIL)
    {
        XmsGame::SetPendingBlob(m->ns, "", 0);
        return true;
    }
    if (luabind::type(data) != LUA_TSTRING)
        return false;
    data.push(L);
    size_t size = 0;
    pcstr bytes = lua_tolstring(L, -1, &size);
    XmsGame::SetPendingBlob(m->ns, bytes, size);
    lua_pop(L, 1);
    return true;
}

luabind::object xms_native_load_data(lua_State* L, pcstr id)
{
    const XMS::Module* m = find_enabled(id);
    if (!m)
        return luabind::object();
    const xr_vector<u8>* blob = XmsGame::GetLoadedBlob(m->ns);
    if (!blob || blob->empty())
        return luabind::object();
    lua_pushlstring(L, reinterpret_cast<pcstr>(blob->data()), blob->size());
    luabind::object result(luabind::from_stack(L, -1));
    lua_pop(L, 1);
    return result;
}

void xms_native_log(pcstr text) { Msg("[xms] %s", text ? text : ""); }

// ---- game modes -------------------------------------------------------------

void xms_native_set_modes(pcstr csv) { XMS::SetActiveModes(csv); }
pcstr xms_native_active_modes() { return XMS::ActiveModesCsv(); }
bool xms_native_mode_active(pcstr id) { return XMS::ModeActive(id); }

// every mode any installed module declares: {id, title, provider}
luabind::object xms_native_known_modes(lua_State* L)
{
    luabind::object result = luabind::newtable(L);
    int index = 1;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled)
            continue;
        for (const XMS::ProvidedMode& mode : m.provides_modes)
        {
            luabind::object entry = luabind::newtable(L);
            entry["id"] = mode.id.c_str();
            entry["title"] = mode.title.c_str();
            entry["provider"] = m.id.c_str();
            result[index++] = entry;
        }
    }
    return result;
}

// legacy-save repair: after Lua derives the playthrough's modes from the base
// game's own state and publishes them (xms.set_modes), this re-runs the spawn
// composer with the now-correct gate and instantiates what is missing
u32 xms_native_recompose_spawns()
{
    const CALifeSimulator* sim = ai().get_alife();
    if (!sim)
        return 0;
    return XmsGame::RecomposeAndLateSpawn(const_cast<CALifeSimulator&>(*sim));
}

// nearest composed game vertex on a level - lets mode/level scripts wire
// level changers without knowing generated ids in advance
u32 xms_native_graph_vertex(pcstr level_name, float x, float y, float z)
{
    if (!ai().get_game_graph())
        return u32(-1);
    const CGameGraph& graph = ai().game_graph();
    if (!graph.header().level_exist(level_name))
        return u32(-1);
    const GameGraph::_LEVEL_ID level_id = graph.header().level(level_name).id();
    const Fvector position = {x, y, z};
    u32 best = u32(-1);
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

// Lua bootstrap: hooks, module require, registries, persistence facade.
// Kept in C++ so the engine is self-contained; a loose gamedata\scripts\
// xms.script (if present) is loaded instead to allow iteration.
constexpr pcstr kXmsBootstrap = R"xms(
xms = xms or {}
xms.api = 1
xms.log = xms_native_log
xms.modules = xms_native_modules
xms.is_loaded = xms_native_is_loaded
xms.ns = xms_native_ns
xms.story_id = xms_native_story_id
xms.save_data = xms_native_save_data
xms.load_data = xms_native_load_data
xms.set_modes = xms_native_set_modes
xms.active_modes = xms_native_active_modes
xms.mode_active = xms_native_mode_active
xms.known_modes = xms_native_known_modes
xms.graph_vertex = xms_native_graph_vertex

-- ---- module script namespaces ----------------------------------------------
local script_cache = {}
function xms.require(mod_id, name)
    local key = mod_id .. "/" .. name
    local cached = script_cache[key]
    if cached ~= nil then return cached end
    local source = xms_native_read_script(mod_id, name)
    if not source or source == "" then
        xms.log("! xms.require: no script [" .. key .. "]")
        return nil
    end
    local chunk, err = loadstring(source, "@mods/" .. key .. ".script")
    if not chunk then
        xms.log("! xms.require: " .. tostring(err))
        return nil
    end
    local env = setmetatable({}, { __index = _G })
    setfenv(chunk, env)
    local ok, call_err = pcall(chunk)
    if not ok then
        xms.log("! xms.require: " .. tostring(call_err))
        return nil
    end
    script_cache[key] = env
    return env
end

-- ---- function hooks ---------------------------------------------------------
-- xms.hook("xr_logic.pstor_load", fn, {mode="post", priority=0})
-- modes: pre (fn(...)), post (fn(...)), around (fn(original, ...))
local hook_records = {}

local function hook_install(container, fname, path)
    local rec = { original = container[fname], pre = {}, post = {}, around = {} }
    hook_records[path] = rec
    container[fname] = function(...)
        for i = 1, #rec.pre do
            local ok, err = pcall(rec.pre[i].fn, ...)
            if not ok then xms.log("! xms.hook pre [" .. path .. "]: " .. tostring(err)) end
        end
        local call = rec.original
        for i = #rec.around, 1, -1 do
            local inner, wrapper = call, rec.around[i].fn
            call = function(...) return wrapper(inner, ...) end
        end
        local results = { call(...) }
        for i = 1, #rec.post do
            local ok, err = pcall(rec.post[i].fn, ...)
            if not ok then xms.log("! xms.hook post [" .. path .. "]: " .. tostring(err)) end
        end
        return unpack(results)
    end
    return rec
end

function xms.hook(path, fn, opts)
    opts = opts or {}
    local mode = opts.mode or "post"
    local priority = opts.priority or 0
    -- resolve "namespace.fn" or "namespace.table.fn" (autoload does the rest)
    local segments = {}
    for segment in string.gmatch(path, "[^%.]+") do segments[#segments + 1] = segment end
    if #segments < 2 then
        xms.log("! xms.hook: bad path [" .. tostring(path) .. "]")
        return nil
    end
    local container = _G[segments[1]]
    for i = 2, #segments - 1 do
        if not container then break end
        container = container[segments[i]]
    end
    local fname = segments[#segments]
    if not container or type(container[fname]) ~= "function" then
        xms.log("! xms.hook: cannot resolve [" .. path .. "]")
        return nil
    end
    local rec = hook_records[path]
    if not rec then rec = hook_install(container, fname, path) end
    local list = rec[mode]
    if not list then
        xms.log("! xms.hook: bad mode [" .. tostring(mode) .. "]")
        return nil
    end
    local entry = { fn = fn, priority = priority }
    local slot = #list + 1
    for i = 1, #list do
        if list[i].priority > priority then slot = i break end
    end
    table.insert(list, slot, entry)
    return {
        remove = function()
            for i = 1, #list do
                if list[i] == entry then table.remove(list, i) break end
            end
        end
    }
end

-- ---- named registries -------------------------------------------------------
local registries = {}
xms.registry = {}
function xms.registry.get(name)
    local reg = registries[name]
    if reg then return reg end
    local entries = {}
    reg = {
        add = function(_, key, value, opts)
            if entries[key] ~= nil and not (opts and opts.override) then
                xms.log("! xms.registry [" .. name .. "]: duplicate key [" .. tostring(key) .. "] ignored")
                return false
            end
            entries[key] = value
            return true
        end,
        get = function(_, key) return entries[key] end,
        remove = function(_, key) entries[key] = nil end,
        all = function(_) return entries end,
    }
    registries[name] = reg
    return reg
end

-- ---- module registrars ------------------------------------------------------
-- A module ships a startup script to run something before anything else asks
-- for it - adding its game mode to the new-game screen, registering a scheme,
-- whatever. Loaded here, in load order, once the whole api above exists:
-- without this a module's scripts sit on disk with nobody to require them.
-- Two reserved names, so a tool and a human never overwrite each other:
--   mode_register  generated (XFined Editor rewrites it on every export)
--   register       the author's own, never touched by anything
-- Generated first, so the author's script can override what it set up.
--
-- A registrar runs once per SCRIPT ENGINE, not once per process: the engine
-- rebuilds the Lua state for a new game, a save load and every level change,
-- and each of those states needs its own wrappers. Registrars must therefore
-- be idempotent and must not read the active mode set - on the load path the
-- state is rebuilt before the save's modes are restored.
local REGISTRARS = { "mode_register", "register" }
function xms.load_registrars()
    for _, m in ipairs(xms.modules() or {}) do
        if m.enabled then
            for i = 1, #REGISTRARS do
                local name = REGISTRARS[i]
                local source = xms_native_read_script(m.id, name)
                if source and source ~= "" then
                    xms.log("registrar: " .. m.id .. "/" .. name)
                    xms.require(m.id, name)
                end
            end
        end
    end
end
-- guarded: one module's broken registrar must not take the bootstrap with it
local ok_reg, reg_err = pcall(xms.load_registrars)
if not ok_reg then xms.log("! xms.load_registrars: " .. tostring(reg_err)) end

xms.log("bootstrap ready, api " .. tostring(xms.api))
)xms";

void run_bootstrap(lua_State* L)
{
    if (!XMS::Active())
        return;

    // Loose override for iteration: gamedata\scripts\xms.script wins - but only
    // a REAL loose file does. Module gamedata is mounted into the same virtual
    // namespace, so without this check one module shipping that path would
    // replace the whole xms api, and every other module's registrar with it.
    if (FS.exist("$game_scripts$", "xms.script"))
    {
        string_path loose;
        FS.update_path(loose, "$game_scripts$", "xms.script");
        const CLocatorAPI::file* desc = FS.GetFileDesc(loose);
        const bool from_module = desc && XMS::ResolvePhysical(desc->name);
        if (!from_module)
        {
            GEnv.ScriptEngine->process_file_if_exists("xms", false);
            return;
        }
        Msg("! XMS: a module ships scripts\\xms.script - ignored, the bootstrap is not overridable by modules");
    }

    if (0 != luaL_loadbuffer(L, kXmsBootstrap, xr_strlen(kXmsBootstrap), "@xms_bootstrap"))
    {
        Msg("! XMS: bootstrap load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    if (0 != lua_pcall(L, 0, 0, 0))
    {
        Msg("! XMS: bootstrap run error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}
} // namespace

void xms_registrator::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        def("xms_native_modules", &xms_native_modules),
        def("xms_native_is_loaded", &xms_native_is_loaded),
        def("xms_native_ns", &xms_native_ns),
        def("xms_native_story_id", &xms_native_story_id),
        def("xms_native_read_script", &xms_native_read_script),
        def("xms_native_save_data", &xms_native_save_data),
        def("xms_native_load_data", &xms_native_load_data),
        def("xms_native_log", &xms_native_log),
        def("xms_native_set_modes", &xms_native_set_modes),
        def("xms_native_active_modes", &xms_native_active_modes),
        def("xms_native_mode_active", &xms_native_mode_active),
        def("xms_native_known_modes", &xms_native_known_modes),
        def("xms_native_graph_vertex", &xms_native_graph_vertex),
        def("xms_native_recompose_spawns", &xms_native_recompose_spawns)
    ];

    // defer the bootstrap to the end of CScriptEngine::init - the base Lua
    // libraries are not open yet while export_all is running
    CScriptEngine::g_xms_on_init = &run_bootstrap;
}
