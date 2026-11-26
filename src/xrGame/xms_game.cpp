// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "pch_script.h"

#include "xms_game.h"
#include "ui/UIModsWnd.h"
#include "xms_world.h"
#include "xms_pda.h"
#include "xms_screens.h"
#include "save_extension_gameplay.h"
#include "save_extension_chunk_ids.h"
#include "PhraseDialog.h"
#include "xrCore/XMS/xms_core.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "alife_simulator_base.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "xrServer.h"
#include "xrServerEntities/xrServer_Objects_ALife.h"
#include "xrServerEntities/xrServer_Objects_ALife_Items.h"
#include "xrServerEntities/xrServer_Objects_ALife_Monsters.h"
#include "xrServerEntities/xrMessages.h"
#include "GameTaskDefs.h"
#include "xrCore/XMS/xms_catalog.h"
#include "xms_net.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include "xrCommon/xr_unordered_map.h"

#include <io.h>
#include <string_view>

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
    // Modules the loaded save was written with that are gone or gated off now, with the
    // spawn-id range the manifest kept for them: their composed objects are released after
    // the registry is up, before the level goes online. Without this a box the module placed
    // stays in the save with a visual that left with the module - and the load dies on it.
    struct OrphanModule
    {
        shared_str id;
        shared_str version;
        u16 ns{};
        u32 base{};
        u32 size{};
    };
    xr_vector<OrphanModule> orphans;
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
        u32 base = 0, size = 0;
        const bool has_range = XMS::SpawnRange(m.id.c_str(), m.budget_spawns, base) && XMS::SpawnRangeOf(m.id.c_str(), base, size);
        writer.w_u32(has_range ? base : 0);
        writer.w_u32(has_range ? size : 0);
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
            // the objects of an absent module are told by its range: only one that can be a module's
            const bool own_range = XMS::SpawnRangeValid(id.c_str(), base, size);
            bs().orphans.push_back({ id, version, ns, own_range ? base : 0, own_range ? size : 0 });
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
    state.orphans.clear();
    state.skipped_objects = 0;

    // The mode set is process-global and outlives "quit to menu, load another
    // save", so it is cleared HERE rather than only overwritten by a manifest
    // that may not be there: a save written before this module existed, one
    // whose .scov went stale, or one made with -no_xms carries no manifest at
    // all, and inheriting the previous session's modes would let mode-gated
    // content into a save that never asked for it.
    XMS::SetActiveModes("");
    if (!XMS::Active() && chunks.empty())
    {
        XmsWorld::OnLoaded(nullptr);
        XmsSaves::OnSnapshotLoaded(chunks);
        return;
    }

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
    // the world this save was started with (engine blob 0xFF02); none = no world rules
    const auto world = state.loaded.find(0xFF02);
    XmsWorld::OnLoaded(world != state.loaded.end() ? &world->second : nullptr);
    XmsSaves::OnSnapshotLoaded(chunks);
}

namespace
{
// The ids of the new-game screen's MODIFIERS - "hardcore" (one life), "easy" - whose keys still end with
// _mode (new_game_hardcore_mode): the checkboxes [modifiers] of dead_air_x64_mode_exclusive.ltx names.
// That file is the game's word about the installed screen; a mod that replaces the screen replaces it.
// Without it nothing is told apart, and every _mode key stays a mode as before.
xr_vector<xr_string> NewGameModifierIds()
{
    xr_vector<xr_string> out;
    IReader* reader = FS.exist("$game_config$", "dead_air_x64_mode_exclusive.ltx")
        ? FS.r_open("$game_config$", "dead_air_x64_mode_exclusive.ltx")
        : nullptr;
    if (!reader)
        return out;
    const xr_string text(static_cast<pcstr>(reader->pointer()), reader->length());
    FS.r_close(reader);
    bool inside = false;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        if (end == xr_string::npos)
            end = text.size();
        std::string_view line(text.data() + at, end - at);
        at = end + 1;
        if (const size_t comment = line.find(';'); comment != std::string_view::npos)
            line = line.substr(0, comment);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            inside = line == "[modifiers]";
            continue;
        }
        if (!inside)
            continue;
        // check_hardcore and check_easy_mode name new_game_hardcore_mode and new_game_easy_mode
        xr_string id(line.substr(0, line.find_first_of(" \t=")));
        if (id.size() > 6 && 0 == _strnicmp(id.c_str(), "check_", 6))
            id.erase(0, 6);
        if (id.size() > 5 && 0 == _stricmp(id.c_str() + id.size() - 5, "_mode"))
            id.erase(id.size() - 5);
        if (id.empty())
            continue;
        xr_strlwr(&id[0]);
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    }
    return out;
}
} // namespace

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
    state.orphans.clear();
    state.skipped_objects = 0;
    XmsSaves::Reset();
    if (!XMS::Active())
    {
        // process-global like the mode set: a world left over from the last session must not
        // reach a game that has no modules at all
        XmsWorld::ResolveForNewGame(nullptr);
        return;
    }

    // Always publish a set, even an empty one: XMS::SetActiveModes writes
    // process-global state that survives "quit to menu, start another game",
    // so skipping the call would carry the previous session's modes over.
    string_path options;
    FS.update_path(options, "$game_config$", "axr_options.ltx");
    if (!FS.exist(options))
    {
        XMS::SetActiveModes("");
        XmsWorld::ResolveForNewGame(nullptr);
        return;
    }

    CInifile ini(options, TRUE /*read only*/);
    constexpr pcstr section = "character_creation";
    if (!ini.section_exist(section))
    {
        XMS::SetActiveModes("");
        XmsWorld::ResolveForNewGame(nullptr);
        return;
    }

    // The screen writes one key per checkbox and leaves the unticked ones
    // empty. new_game_metro_mode -> "metro", matching the ids of
    // a module's [module] mode.
    //
    // Both halves of the name are required. That section holds plenty of other
    // new_game_* keys the same screen writes as booleans - good_wpn, good_loot,
    // unlocked_guide, loadout_code - and none of them is a mode. The value has
    // to read exactly "true" for the same reason: that is what the game's own
    // is_*_mode() helpers compare against, and a mode active in C++ but not in
    // Lua is worse than one that is off in both.
    constexpr pcstr suffix = "_mode";
    const size_t suffix_len = xr_strlen(suffix);
    // a modifier of the screen is no mode: a plain game with "hardcore" ticked is still the plain game
    const xr_vector<xr_string> modifiers = NewGameModifierIds();
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
        if (std::find(modifiers.begin(), modifiers.end(), id) != modifiers.end())
            continue;
        if (!csv.empty())
            csv += ",";
        csv += id;
    }

    XMS::SetActiveModes(csv.c_str());
    if (!csv.empty())
        Msg("* XMS: new game modes: %s", csv.c_str());
    // the world of the modules that apply to these modes, with the player's choices
    // from the same screen - before the composer, which already removes by it
    XmsWorld::ResolveForNewGame(&ini);
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
        u32 base = 0, size = 0;
        if (!XMS::SpawnRange(m.id.c_str(), m.budget_spawns, base) || !XMS::SpawnRangeOf(m.id.c_str(), base, size))
            continue;

        xr_vector<u16>& ledger = state.pending_ledgers[m.ns];
        u32 created = 0;
        // the range it holds, not the budget it asks for: a range that could not grow is followed by another module's
        const u32 top = std::min(base + size, 65535u);
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

namespace
{
struct ComposedGraph
{
    u32 base_vertex_count{u32(-1)};
    xr_vector<std::pair<u8, xr_string>> module_levels;
};

ComposedGraph& composed_graph()
{
    static ComposedGraph graph;
    return graph;
}
} // namespace

void NoteComposedGraph(u32 base_vertex_count, const xr_vector<std::pair<u8, xr_string>>& module_levels)
{
    ComposedGraph& graph = composed_graph();
    graph.base_vertex_count = base_vertex_count;
    graph.module_levels = module_levels;
}

u32 RecomposeAndLateSpawn(CALifeSimulatorBase& sim)
{
    if (!XMS::Active())
        return 0;
    sim.spawns().xms_recompose();
    return LateSpawnCompose(sim);
}
} // namespace XmsGame

// ---- what a module brings, kept out of the original save format -----------

namespace XmsSaves
{
namespace
{
constexpr pcstr kPlaceholderSection = "space_restrictor";
constexpr pcstr kPlaceholderPrefix = "xms_ph_";
constexpr size_t kPlaceholderPrefixLength = 7;
constexpr u16 kChunkVersion = 1;
constexpr u32 kMaxRecords = 65535;
constexpr size_t kMaxChunkBytes = 16u * 1024 * 1024;
// the owner of an object during a capture: unknown, the game's, or kModule + the module's index
constexpr u16 kUnknown = 0;
constexpr u16 kGame = 1;
constexpr u16 kModule = 2;
constexpr u8 kDeclared = 1;
constexpr u16 kNoObject = 0xffff;

// what a record of the state chunk says of an object of the game
enum StateKind : u8
{
    AmmoType = 1,    // the weapon's loaded ammo type: an index the base ammo_class does not reach
    GrenadeType = 2, // the same for the grenade launcher's grenade_class
    Cartridges = 3,  // a shotgun's tube: the cartridges of such ammo types, by position
    Upgrade = 4,     // an upgrade a module made, installed on the item
    ActorVisual = 5, // the actor's model when it is a module's mesh (a module outfit worn)
    Task = 6,        // a quest task of the module in the actor's task registry
};

using Bytes = xr_vector<u8>;

struct Location
{
    GameGraph::_GRAPH_ID graph{0};
    u32 node{u32(-1)};
    Fvector position{};
};

// an id an absent module keeps: its placeholder stays in the world, the object in the .scov
struct Reserved
{
    u16 id{};
};

struct KeptModule
{
    shared_str id;
    shared_str version;
    u16 ns{};
    u32 spawn_base{};
    u32 spawn_size{};
    u32 objects{};
    xr_vector<Reserved> reserved;
};

// a quest task of a present module, read from its state chunk, waiting for the registries
struct KeptTask
{
    u16 key{};
    u16 place{};
    Bytes bytes;
};

struct SpawnRange
{
    u32 base{};
    u32 size{};
    u16 module{};
};

struct State
{
    // declared at run time (xms.own_object): object id -> module index
    xr_unordered_map<u16, u16> declared;
    xr_vector<KeptModule> kept;
    xr_vector<shared_str> forgotten;
    // a present module's chunks found under another ns: the next save erases them
    xr_vector<u32> stale_chunks;
    xr_vector<SpawnRange> ranges;
    bool ranges_ready{};
    // the .scop carried a lineage whose .scov was not found
    bool lineage_lost{};
    // the limit of a game in a module's mode or world has been said for this game
    bool mode_said{};
    // from the placeholders' replacement until the squads register their members: member -> squad
    bool loading{};
    xr_unordered_map<u16, u16> loading_groups;
    // the present modules' quest tasks of the save, put back once the registries are read
    xr_vector<KeptTask> tasks;
    // module ids handed to scripts that no module of this session spells
    xr_vector<shared_str> names;
    // configs read only to learn what the modules' patches make in them
    xr_vector<xr_string> read_files;
    // the capture of the save in progress
    bool capturing{};
    bool captured{};
    u64 save_id{};
    xr_vector<u16> owner;
    xr_vector<u8> written;
    xr_vector<Bytes> objects;
    xr_vector<u32> object_counts;
    xr_vector<Bytes> states;
    xr_vector<u32> state_counts;
    u32 placeholders{};
    CSE_Abstract* placeholder{};
};

State& st()
{
    static State state;
    return state;
}

template <typename T>
void put(Bytes& out, T value)
{
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void put_bytes(Bytes& out, const void* data, size_t size)
{
    const u8* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

void put_string(Bytes& out, pcstr text)
{
    text = text ? text : "";
    put_bytes(out, text, xr_strlen(text) + 1);
}

// a bounded reader: a chunk that passed its CRC can still say something that does not fit
struct Cursor
{
    const u8* at;
    const u8* end;
    bool ok{true};

    template <typename T>
    T get()
    {
        T value{};
        if (size_t(end - at) < sizeof(T))
        {
            ok = false;
            at = end;
            return value;
        }
        memcpy(&value, at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    pcstr string()
    {
        const void* zero = at < end ? memchr(at, 0, size_t(end - at)) : nullptr;
        if (!zero)
        {
            ok = false;
            at = end;
            return "";
        }
        const pcstr text = reinterpret_cast<pcstr>(at);
        at = static_cast<const u8*>(zero) + 1;
        return text;
    }

    const u8* bytes(size_t size)
    {
        if (size_t(end - at) < size)
        {
            ok = false;
            at = end;
            return nullptr;
        }
        const u8* start = at;
        at += size;
        return start;
    }
};

u16 index_of(const XMS::Module& m) { return u16(&m - XMS::Modules().data()); }

const XMS::Module* present(pcstr id)
{
    const XMS::Module* m = id && id[0] ? XMS::FindModule(id) : nullptr;
    return m && !m->disabled ? m : nullptr;
}

bool present_ns(u16 ns)
{
    for (const XMS::Module& m : XMS::Modules())
        if (!m.disabled && m.ns == ns)
            return true;
    return false;
}

KeptModule* kept_module(pcstr id)
{
    for (KeptModule& kept : st().kept)
        if (kept.id.c_str() && id && 0 == xr_strcmp(kept.id.c_str(), id))
            return &kept;
    return nullptr;
}

KeptModule& kept_candidate(pcstr id, pcstr version, u16 ns, u32 base, u32 size)
{
    if (KeptModule* kept = kept_module(id))
    {
        if (!kept->ns)
            kept->ns = ns;
        if (!kept->spawn_size && size)
        {
            kept->spawn_base = base;
            kept->spawn_size = size;
        }
        return *kept;
    }
    KeptModule& kept = st().kept.emplace_back();
    kept.id = id;
    kept.version = version;
    kept.ns = ns;
    kept.spawn_base = base;
    kept.spawn_size = size;
    return kept;
}

void refresh_ranges()
{
    State& s = st();
    s.ranges.clear();
    for (const XMS::Module& m : XMS::Modules())
    {
        u32 base = 0, size = 0;
        if (!m.disabled && XMS::SpawnRangeOf(m.id.c_str(), base, size) && size)
            s.ranges.push_back({base, size, index_of(m)});
    }
    s.ranges_ready = true;
}

bool base_vertex(GameGraph::_GRAPH_ID vertex)
{
    if (!ai().get_game_graph())
        return false;
    const u32 count = ai().game_graph().header().vertex_count();
    return vertex < std::min(count, XmsGame::composed_graph().base_vertex_count);
}

const XMS::Module* module_of_level(GameGraph::_GRAPH_ID vertex)
{
    const auto& levels = XmsGame::composed_graph().module_levels;
    if (levels.empty() || !ai().get_game_graph() || vertex >= ai().game_graph().header().vertex_count())
        return nullptr;
    const u8 level = u8(ai().game_graph().vertex(vertex)->level_id());
    for (const auto& [id, module] : levels)
        if (id == level)
            return present(module.c_str());
    return nullptr;
}

// The rules of an object of its own (SAVE_COMPATIBILITY.md, "What a module brings"), in their
// order: a section a module made, a module's spawn layer, a level a module added (for an object
// that stands in the world), a module's declaration.
u16 base_owner(const CSE_ALifeDynamicObject& object, bool root)
{
    State& s = st();
    if (const XMS::Module* m = XMS::ModuleOfSection(object.s_name))
        return kModule + index_of(*m);
    if (object.m_tSpawnID != ALife::_SPAWN_ID(-1))
    {
        if (!s.ranges_ready)
            refresh_ranges();
        for (const SpawnRange& range : s.ranges)
            if (object.m_tSpawnID >= range.base && object.m_tSpawnID < range.base + range.size)
                return kModule + range.module;
    }
    if (root)
        if (const XMS::Module* m = module_of_level(object.m_tGraphID))
            return kModule + index_of(*m);
    if (const auto found = s.declared.find(object.ID); found != s.declared.end())
        return kModule + found->second;
    return kGame;
}

// A squad and its members are one: the squad's own rule, else its first member's that has one.
u16 unit_owner(const CALifeObjectRegistry& registry, CSE_ALifeOnlineOfflineGroup& group)
{
    const u16 own = base_owner(group, true);
    if (own != kGame)
        return own;
    for (const auto& [id, member] : group.squad_members())
    {
        (void)member;
        if (CSE_ALifeDynamicObject* object = registry.object(id, true))
            if (const u16 owner = base_owner(*object, true); owner != kGame)
                return owner;
    }
    return kGame;
}

// Whose an object is: the actor never a module's; what is inside a module's object is that
// module's, whatever its own section; then the object's own rules, then its squad's.
u16 owner_of(const CALifeObjectRegistry& registry, CSE_ALifeDynamicObject& object, xr_vector<u16>* memo, u32 depth = 0)
{
    if (memo && (*memo)[object.ID] != kUnknown)
        return (*memo)[object.ID];
    u16 owner = kGame;
    if (depth > 64 || smart_cast<CSE_ALifeCreatureActor*>(&object))
        owner = kGame;
    else if (object.ID_Parent != kNoObject)
    {
        CSE_ALifeDynamicObject* parent = registry.object(object.ID_Parent, true);
        const u16 from_parent = parent ? owner_of(registry, *parent, memo, depth + 1) : kGame;
        owner = from_parent != kGame ? from_parent : base_owner(object, false);
    }
    else
    {
        owner = base_owner(object, true);
        if (owner == kGame)
        {
            if (CSE_ALifeOnlineOfflineGroup* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(&object))
                owner = unit_owner(registry, *group);
            else if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(&object))
            {
                // a member knows its squad once the squads register them; until then the rosters say
                u16 squad_id = monster->m_group_id;
                if (squad_id == kNoObject && st().loading)
                    if (const auto found = st().loading_groups.find(object.ID); found != st().loading_groups.end())
                        squad_id = found->second;
                if (squad_id != kNoObject)
                    if (CSE_ALifeDynamicObject* squad = registry.object(squad_id, true))
                        if (CSE_ALifeOnlineOfflineGroup* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(squad))
                            owner = unit_owner(registry, *group);
            }
        }
    }
    if (memo)
        (*memo)[object.ID] = owner;
    return owner;
}

CSE_ALifeDynamicObject* actor_of(const CALifeObjectRegistry& registry) { return registry.object(0, true); }

// Where a placeholder stands: where the object is when that is a level of the base game, else
// where the actor is. The node is the game vertex's own, which the base game always has.
Location location_of(const CALifeObjectRegistry& registry, CSE_ALifeDynamicObject& object)
{
    CSE_ALifeDynamicObject* root = &object;
    for (u32 depth = 0; root->ID_Parent != kNoObject && depth < 64; ++depth)
    {
        CSE_ALifeDynamicObject* parent = registry.object(root->ID_Parent, true);
        if (!parent)
            break;
        root = parent;
    }
    Location at;
    const CSE_ALifeDynamicObject* actor = actor_of(registry);
    if (base_vertex(root->m_tGraphID))
    {
        at.graph = root->m_tGraphID;
        at.position = root->o_Position;
    }
    else if (actor && base_vertex(actor->m_tGraphID))
    {
        at.graph = actor->m_tGraphID;
        at.position = actor->o_Position;
    }
    else
    {
        at.graph = 0;
        at.position = ai().game_graph().vertex(0)->level_point();
    }
    at.node = ai().game_graph().vertex(at.graph)->level_vertex_id();
    return at;
}

// exactly what the object registry writes of an object
void write_object(IWriter& out, CSE_Abstract& object, NET_Packet& packet)
{
    packet.r_pos = 0;
    object.Spawn_Write(packet, TRUE);
    out.w_u16(u16(packet.B.count));
    out.w(packet.B.data, packet.B.count);
    packet.w_begin(M_UPDATE);
    object.UPDATE_Write(packet);
    out.w_u16(u16(packet.B.count));
    out.w(packet.B.data, packet.B.count);
}

void write_object(Bytes& out, CSE_Abstract& object, NET_Packet& packet)
{
    packet.r_pos = 0;
    object.Spawn_Write(packet, TRUE);
    put<u16>(out, u16(packet.B.count));
    put_bytes(out, packet.B.data, packet.B.count);
    packet.w_begin(M_UPDATE);
    object.UPDATE_Write(packet);
    put<u16>(out, u16(packet.B.count));
    put_bytes(out, packet.B.data, packet.B.count);
}

// A space restrictor of the base game that does nothing: no shape, never online, nothing on the
// map or for the AI, kept by any game that saves it.
CSE_ALifeDynamicObject* make_placeholder()
{
    CSE_Abstract* entity = F_entity_Create(kPlaceholderSection, true);
    CSE_ALifeDynamicObject* placeholder = smart_cast<CSE_ALifeDynamicObject*>(entity);
    if (!placeholder || !smart_cast<CSE_ALifeSpaceRestrictor*>(entity))
    {
        if (entity)
            F_entity_Destroy(entity);
        return nullptr;
    }
    placeholder->m_flags.set(CSE_ALifeObject::flSwitchOnline, FALSE);
    placeholder->m_flags.set(CSE_ALifeObject::flVisibleForMap, FALSE);
    placeholder->m_flags.set(CSE_ALifeObject::flInteractive, FALSE);
    placeholder->m_flags.set(CSE_ALifeObject::flCanSave, TRUE);
    placeholder->m_story_id = INVALID_STORY_ID;
    placeholder->m_spawn_story_id = INVALID_SPAWN_STORY_ID;
    placeholder->client_data.clear();
    return placeholder;
}

void name_placeholder(CSE_Abstract& placeholder, u64 lineage)
{
    string128 name;
    xr_sprintf(name, "%s%016llx_%u", kPlaceholderPrefix, static_cast<unsigned long long>(lineage), u32(placeholder.ID));
    placeholder.set_name_replace(name);
}

// The placeholder under an object's id. Its name carries the lineage, its custom data the module
// (an anchor names none).
void place_placeholder(CSE_ALifeDynamicObject& placeholder, u16 id, const Location& at, pcstr module, u64 lineage)
{
    placeholder.ID = id;
    placeholder.ID_Parent = kNoObject;
    placeholder.ID_Phantom = kNoObject;
    placeholder.o_Position = at.position;
    placeholder.o_Angle.set(0.f, 0.f, 0.f);
    placeholder.m_tGraphID = at.graph;
    placeholder.m_tNodeID = at.node;
    placeholder.m_fDistance = 0.f;
    placeholder.m_tSpawnID = ALife::_SPAWN_ID(-1);
    name_placeholder(placeholder, lineage);
    string256 data;
    if (module)
        xr_sprintf(data, "[xms_placeholder]\nmodule = %s\n", module);
    else
        xr_strcpy(data, "[xms_placeholder]\nanchor = true\n");
    placeholder.m_ini_string = data;
}

void write_placeholder(IWriter& out, u16 id, const Location& at, pcstr module, NET_Packet& packet)
{
    State& s = st();
    CSE_ALifeDynamicObject* placeholder = smart_cast<CSE_ALifeDynamicObject*>(s.placeholder);
    place_placeholder(*placeholder, id, at, module, s.save_id);
    write_object(out, *placeholder, packet);
}

bool placeholder_lineage(pcstr name, u64& lineage)
{
    if (!name || 0 != strncmp(name, kPlaceholderPrefix, kPlaceholderPrefixLength))
        return false;
    pcstr hex = name + kPlaceholderPrefixLength;
    u64 value = 0;
    for (u32 i = 0; i < 16; ++i)
    {
        const char c = hex[i];
        u32 digit;
        if (c >= '0' && c <= '9')
            digit = u32(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = u32(c - 'a' + 10);
        else
            return false;
        value = (value << 4) | digit;
    }
    if (hex[16] != '_')
        return false;
    lineage = value;
    return true;
}

bool is_placeholder(const CSE_Abstract& object)
{
    static const shared_str section(kPlaceholderSection);
    u64 lineage = 0;
    return object.s_name == section && placeholder_lineage(object.name_replace(), lineage);
}

shared_str placeholder_module(const CSE_Abstract& object, bool& anchor)
{
    anchor = false;
    pcstr text = object.m_ini_string.c_str();
    if (!text)
        return {};
    if (strstr(text, "anchor = true"))
    {
        anchor = true;
        return {};
    }
    pcstr key = strstr(text, "module = ");
    if (!key)
        return {};
    key += 9;
    pcstr end = key;
    while (*end && *end != '\r' && *end != '\n')
        ++end;
    return shared_str(xr_string(key, size_t(end - key)).c_str());
}

void destroy_placeholder()
{
    State& s = st();
    if (s.placeholder)
        F_entity_Destroy(s.placeholder);
    s.placeholder = nullptr;
}

void end_capture()
{
    State& s = st();
    destroy_placeholder();
    s.capturing = false;
    s.captured = false;
    xr_vector<u16>().swap(s.owner);
    xr_vector<u8>().swap(s.written);
    s.objects.clear();
    s.object_counts.clear();
    s.states.clear();
    s.state_counts.clear();
    s.placeholders = 0;
}

void state_record(u16 module, u16 id, u32 section_crc, StateKind kind, const Bytes& payload)
{
    State& s = st();
    Bytes& out = s.states[module];
    put<u16>(out, id);
    put<u32>(out, section_crc);
    put<u8>(out, kind);
    put_bytes(out, payload.data(), payload.size());
    ++s.state_counts[module];
}

u32 crc_of(const xr_vector<u8>& values) { return crc32(values.data(), u32(values.size())); }

u32 crc_of(const xr_vector<shared_str>& names)
{
    xr_string joined;
    for (const shared_str& name : names)
        joined.append(name.c_str() ? name.c_str() : "").append(1, ',');
    return crc32(joined.data(), u32(joined.size()));
}

u32 section_crc_of(const CSE_Abstract& object) { return crc32(object.s_name.c_str(), u32(object.s_name.size())); }

// The quest runtime names a task "nq.<module>.<quest>.<task>"; a module id may hold dots itself.
const XMS::Module* task_module(const shared_str& id)
{
    pcstr text = id.c_str();
    if (!text || 0 != strncmp(text, "nq.", 3))
        return nullptr;
    const XMS::Module* best = nullptr;
    for (const XMS::Module& m : XMS::Modules())
        if (!m.disabled && (!best || m.id.size() > best->id.size()) && 0 == strncmp(text + 3, m.id.c_str(), m.id.size()) &&
            text[3 + m.id.size()] == '.')
            best = &m;
    return best;
}

// What .scop holds of a list a module adds to - the fingerprint a state record is checked by, the
// same whichever of several modules' records comes back first.
xr_vector<shared_str> game_upgrades(const xr_vector<shared_str>& upgrades)
{
    xr_vector<shared_str> own;
    for (const shared_str& upgrade : upgrades)
        if (!XMS::ModuleOfSection(upgrade))
            own.push_back(upgrade);
    return own;
}

xr_vector<u8> game_cartridges(const shared_str& section, const xr_vector<u8>& cartridges)
{
    xr_vector<u8> own = cartridges;
    for (u8& type : own)
        if (XMS::ListEntryModule(section, false, type))
            type = 0;
    return own;
}

// The module whose files give a model: a mesh only a module ships is not there for the base game.
const XMS::Module* mesh_module(pcstr visual)
{
    if (!visual || !visual[0])
        return nullptr;
    string_path name, path;
    strconcat(sizeof(name), name, visual, ".ogf");
    FS.update_path(path, "$game_meshes$", name);
    const CLocatorAPI::file* desc = FS.GetFileDesc(path);
    pcstr physical = desc ? XMS::ResolvePhysical(desc->name) : nullptr;
    return physical ? XMS::ModuleOfLayer(XMS::LayerOfPath(physical)) : nullptr;
}

// The actor wearing a module's outfit has its mesh: .scop gets the actor section's own model.
bool capture_actor(CSE_ALifeCreatureActor& actor, IWriter& out, u32& record_count, NET_Packet& packet)
{
    CSE_Visual* visual = actor.visual();
    const XMS::Module* module = visual ? mesh_module(visual->get_visual()) : nullptr;
    pcstr own = module && pSettings->line_exist(actor.s_name, "visual") ? pSettings->r_string(actor.s_name, "visual") : nullptr;
    if (!own || !own[0])
        return false;
    const shared_str worn = visual->get_visual();
    visual->set_visual(own, false);
    Bytes payload;
    put_string(payload, worn.c_str());
    put_string(payload, visual->get_visual());
    state_record(index_of(*module), actor.ID, section_crc_of(actor), ActorVisual, payload);
    write_object(out, actor, packet);
    ++record_count;
    visual->set_visual(worn.c_str(), false);
    return true;
}

// What an object of the game carries of a module's data is not written into .scop: the record
// there keeps what the base game understands - the first ammo type, no module upgrade - and the
// state chunk of the module the rest. True when it wrote the object.
bool capture_state(CSE_ALifeDynamicObject& object, IWriter& out, u32& record_count, NET_Packet& packet)
{
    if (CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(&object))
        return capture_actor(*actor, out, record_count, packet);
    CSE_ALifeItemWeapon* weapon = smart_cast<CSE_ALifeItemWeapon*>(&object);
    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(&object);
    if (!weapon && (!item || item->m_upgrades.empty()))
        return false;
    CSE_ALifeItemWeaponShotGun* shotgun = smart_cast<CSE_ALifeItemWeaponShotGun*>(&object);

    const XMS::Module* ammo = weapon ? XMS::ListEntryModule(object.s_name, false, weapon->ammo_type) : nullptr;
    const XMS::Module* grenade =
        weapon ? XMS::ListEntryModule(object.s_name, true, weapon->a_elapsed_grenades.grenades_type) : nullptr;
    xr_vector<const XMS::Module*> cartridge_modules;
    bool cartridges_changed = false;
    if (shotgun)
        for (const u8 type : shotgun->m_AmmoIDs)
        {
            const XMS::Module* m = XMS::ListEntryModule(object.s_name, false, type);
            cartridge_modules.push_back(m);
            cartridges_changed |= !!m;
        }
    xr_vector<std::pair<u32, const XMS::Module*>> upgrades;
    if (item)
        for (u32 i = 0; i < item->m_upgrades.size(); ++i)
            if (const XMS::Module* m = XMS::ModuleOfSection(item->m_upgrades[i]))
                upgrades.emplace_back(i, m);
    if (!ammo && !grenade && !cartridges_changed && upgrades.empty())
        return false;

    const u8 ammo_was = weapon ? weapon->ammo_type : 0;
    const u8 grenade_was = weapon ? u8(weapon->a_elapsed_grenades.grenades_type) : 0;
    const xr_vector<u8> cartridges_were = shotgun ? shotgun->m_AmmoIDs : xr_vector<u8>();
    const xr_vector<shared_str> upgrades_were = item ? item->m_upgrades : xr_vector<shared_str>();
    if (ammo)
        weapon->ammo_type = 0;
    if (grenade)
        weapon->a_elapsed_grenades.grenades_type = 0;
    if (cartridges_changed)
        for (size_t i = 0; i < shotgun->m_AmmoIDs.size(); ++i)
            if (cartridge_modules[i])
                shotgun->m_AmmoIDs[i] = 0;
    for (auto at = upgrades.rbegin(); at != upgrades.rend(); ++at)
        item->m_upgrades.erase(item->m_upgrades.begin() + at->first);

    const u32 section_crc = crc32(object.s_name.c_str(), u32(object.s_name.size()));
    if (ammo)
        state_record(index_of(*ammo), object.ID, section_crc, AmmoType, {ammo_was, 0});
    if (grenade)
        state_record(index_of(*grenade), object.ID, section_crc, GrenadeType, {grenade_was, 0});
    if (cartridges_changed)
    {
        const u32 written = crc_of(game_cartridges(object.s_name, shotgun->m_AmmoIDs));
        for (size_t i = 0; i < cartridge_modules.size(); ++i)
        {
            const XMS::Module* m = cartridge_modules[i];
            if (!m || std::find(cartridge_modules.begin(), cartridge_modules.begin() + i, m) != cartridge_modules.begin() + i)
                continue;
            // the cartridges of this module's ammo types, by their place in the tube
            Bytes payload;
            put<u32>(payload, written);
            u8 count = 0;
            for (const XMS::Module* other : cartridge_modules)
                count += other == m ? 1 : 0;
            put<u8>(payload, count);
            for (size_t k = 0; k < cartridge_modules.size(); ++k)
                if (cartridge_modules[k] == m)
                {
                    put<u8>(payload, u8(k));
                    put<u8>(payload, cartridges_were[k]);
                }
            state_record(index_of(*m), object.ID, section_crc, Cartridges, payload);
        }
    }
    if (!upgrades.empty())
    {
        const u32 written = crc_of(game_upgrades(item->m_upgrades));
        for (const auto& [position, m] : upgrades)
        {
            Bytes payload;
            put<u32>(payload, written);
            put<u8>(payload, u8(std::min<u32>(position, 255)));
            put_string(payload, upgrades_were[position].c_str());
            state_record(index_of(*m), object.ID, section_crc, Upgrade, payload);
        }
    }

    write_object(out, object, packet);
    ++record_count;

    if (weapon)
    {
        weapon->ammo_type = ammo_was;
        weapon->a_elapsed_grenades.grenades_type = grenade_was;
    }
    if (shotgun)
        shotgun->m_AmmoIDs = cartridges_were;
    if (item)
        item->m_upgrades = upgrades_were;
    return true;
}

// Whether the .scov of this save will hold module state that no placeholder carries the lineage of.
bool module_state_follows()
{
    State& s = st();
    const bool active = XMS::Active();
    auto& blobs = XmsGame::bs();
    if (active)
    {
        for (const auto& [ns, blob] : blobs.pending)
            if (!blob.empty())
                return true;
        for (const auto& [ns, ids] : blobs.pending_ledgers)
            if (!ids.empty())
                return true;
    }
    for (const u32 count : s.state_counts)
        if (count)
            return true;
    const SaveExtensionContainer::ChunkSnapshot base = SaveExtensionContainer::snapshot_loaded_chunks();
    for (const SaveExtensionContainer::Chunk& chunk : *base)
    {
        if (!XmsGame::IsModuleFamily(chunk.type) || chunk.payload.empty())
            continue;
        const u32 family = chunk.type & 0xFFFF0000;
        const u16 ns = u16(chunk.type & 0xFFFF);
        if (active && family == XmsGame::kModuleChunkBase && blobs.pending.contains(ns))
            continue;
        if (active && family == XmsGame::kLedgerChunkBase && blobs.pending_ledgers.contains(ns))
            continue;
        if ((family == XmsGame::kObjectChunkBase || family == XmsGame::kStateChunkBase) && present_ns(ns))
            continue;
        return true;
    }
    return false;
}

// ---- reading what a save keeps ----

struct ObjectRecord
{
    u16 id{};
    u16 parent{};
    u8 flags{};
    pcstr section{};
    u64 clsid{};
    pcstr parent_section{};
    const u8* state{};
    u32 size{};
};

struct ObjectFamily
{
    u32 type{};
    pcstr module{};
    pcstr version{};
    xr_vector<ObjectRecord> records;
};

bool parse_objects(const SaveExtensionContainer::Chunk& chunk, ObjectFamily& family)
{
    family.type = chunk.type;
    Cursor cursor{chunk.payload.data(), chunk.payload.data() + chunk.payload.size()};
    const u32 count = cursor.get<u32>();
    family.module = cursor.string();
    family.version = cursor.string();
    if (!cursor.ok || count > kMaxRecords)
        return false;
    family.records.reserve(count);
    for (u32 i = 0; i < count && cursor.ok; ++i)
    {
        ObjectRecord& record = family.records.emplace_back();
        record.id = cursor.get<u16>();
        record.parent = cursor.get<u16>();
        record.flags = cursor.get<u8>();
        record.section = cursor.string();
        record.clsid = cursor.get<u64>();
        record.parent_section = cursor.string();
        record.size = cursor.get<u32>();
        record.state = cursor.bytes(record.size);
    }
    return cursor.ok && cursor.at == cursor.end;
}

// the header of a module chunk of objects or state: its module
bool chunk_module(const SaveExtensionContainer::Chunk& chunk, pcstr& module, pcstr& version)
{
    Cursor cursor{chunk.payload.data(), chunk.payload.data() + chunk.payload.size()};
    cursor.get<u32>();
    module = cursor.string();
    version = cursor.string();
    return cursor.ok && module[0];
}

// A record is the two packets the game writes for an object, each behind its length and opened by
// its id, the first naming the section the record was checked for. The object reader trusts every
// byte it is handed, so a record that is anything else never reaches it.
bool record_frames(const ObjectRecord& record)
{
    u32 at = 0;
    for (u32 packet = 0; packet < 2; ++packet)
    {
        if (record.size - at < sizeof(u16))
            return false;
        u16 count;
        memcpy(&count, record.state + at, sizeof count);
        at += sizeof count;
        if (count < sizeof(u16) || count >= NET_PacketSizeLimit || count > record.size - at)
            return false;
        u16 id;
        memcpy(&id, record.state + at, sizeof id);
        if (id != (packet ? M_UPDATE : M_SPAWN))
            return false;
        if (!packet)
        {
            // the name ends inside the packet and is the section's
            const u8* name = record.state + at + sizeof id;
            const u32 room = count - u32(sizeof id);
            if (!memchr(name, 0, room) || xr_strcmp(reinterpret_cast<pcstr>(name), record.section))
                return false;
        }
        at += count;
    }
    return true;
}

pcstr restore_refusal(const CALifeObjectRegistry& registry, const ObjectRecord& record)
{
    if (!pSettings->section_exist(record.section) || !pSettings->line_exist(record.section, "class"))
        return "its section is gone from the module";
    if (pSettings->r_clsid(record.section, "class") != record.clsid)
        return "its section's class is not the one it was written for";
    if (record.parent != kNoObject)
    {
        CSE_ALifeDynamicObject* parent = registry.object(record.parent, true);
        if (!parent)
            return "its owner is gone";
        if (xr_strcmp(parent->s_name.c_str(), record.parent_section))
            return "its owner is no longer what it was";
    }
    if (!record_frames(record))
        return "its record is malformed";
    return nullptr;
}

void remove_loaded(CALifeObjectRegistry& registry, CSE_ALifeDynamicObject*& object)
{
    registry.remove(object->ID, true);
    CSE_Abstract* abstract = object;
    F_entity_Destroy(abstract);
    object = nullptr;
}

// An older save keeps an absent module's spawn-layer objects in .scop: they become kept records
// like any the module has, instead of standing in a world without it.
void keep_orphans(CALifeObjectRegistry& registry, NET_Packet& packet, SaveExtensionContainer::ChunkList& chunks,
    bool& chunks_changed)
{
    State& s = st();
    xr_unordered_map<u16, xr_vector<u16>> children;
    bool any = false;
    for (const KeptModule& kept : s.kept)
        any |= kept.spawn_size != 0;
    if (!any)
        return;
    for (const auto& [id, object] : registry.objects())
        if (object->ID_Parent != kNoObject)
            children[object->ID_Parent].push_back(id);

    for (KeptModule& kept : s.kept)
    {
        if (!kept.spawn_size)
            continue;
        xr_vector<u16> roots;
        for (const auto& [id, object] : registry.objects())
            if (object->ID_Parent == kNoObject && object->m_tSpawnID != ALife::_SPAWN_ID(-1) &&
                object->m_tSpawnID >= kept.spawn_base && object->m_tSpawnID < kept.spawn_base + kept.spawn_size &&
                !smart_cast<CSE_ALifeCreatureActor*>(object))
                roots.push_back(id);
        if (roots.empty())
            continue;

        Bytes records;
        u32 count = 0;
        xr_vector<std::pair<u16, Location>> taken;
        for (const u16 root : roots)
        {
            xr_vector<u16> pending{root};
            while (!pending.empty())
            {
                const u16 id = pending.back();
                pending.pop_back();
                CSE_ALifeDynamicObject* object = registry.object(id, true);
                if (!object)
                    continue;
                CSE_ALifeDynamicObject* parent = object->ID_Parent != kNoObject ? registry.object(object->ID_Parent, true) : nullptr;
                put<u16>(records, object->ID);
                put<u16>(records, object->ID_Parent);
                put<u8>(records, 0);
                put_string(records, object->s_name.c_str());
                put<u64>(records, object->m_tClassID);
                put_string(records, parent ? parent->s_name.c_str() : "");
                const size_t size_at = records.size();
                put<u32>(records, 0);
                write_object(records, *object, packet);
                const u32 size = u32(records.size() - size_at - sizeof(u32));
                memcpy(records.data() + size_at, &size, sizeof(size));
                ++count;
                taken.emplace_back(id, location_of(registry, *object));
                if (const auto found = children.find(id); found != children.end())
                    for (auto child = found->second.rbegin(); child != found->second.rend(); ++child)
                        pending.push_back(*child);
            }
        }

        // appended to what the save keeps of the module already
        const u32 type = XmsGame::kObjectChunkBase | kept.ns;
        Bytes payload;
        u32 total = count;
        Bytes earlier;
        auto existing = std::find_if(chunks.begin(), chunks.end(), [type](const auto& chunk) { return chunk.type == type; });
        if (existing != chunks.end())
        {
            ObjectFamily family;
            if (existing->version != kChunkVersion || !parse_objects(*existing, family))
                continue;
            total += u32(family.records.size());
            Cursor cursor{existing->payload.data(), existing->payload.data() + existing->payload.size()};
            cursor.get<u32>();
            cursor.string();
            cursor.string();
            earlier.assign(cursor.at, cursor.end);
        }
        if (!kept.ns || total > kMaxRecords)
            continue;

        // out of the world, a placeholder under each id
        for (const auto& [id, at] : taken)
        {
            if (CSE_ALifeDynamicObject* object = registry.object(id, true))
                remove_loaded(registry, object);
            if (CSE_ALifeDynamicObject* placeholder = make_placeholder())
            {
                place_placeholder(*placeholder, id, at, kept.id.c_str(), 0);
                registry.add(placeholder);
                kept.reserved.push_back({id});
            }
        }
        put<u32>(payload, total);
        put_string(payload, kept.id.c_str());
        put_string(payload, kept.version.c_str());
        put_bytes(payload, earlier.data(), earlier.size());
        put_bytes(payload, records.data(), records.size());
        if (existing != chunks.end())
            chunks.erase(existing);
        chunks.push_back({type, kChunkVersion, 0, std::move(payload)});
        chunks_changed = true;
        kept.objects += count;
        Msg("* XMS: module [%s] is absent - %u object(s) of its spawn layer this older save kept in .scop are kept "
            "for it now, out of the world", kept.id.c_str(), count);
    }
}

// What the game's own objects carry of the present modules' data comes back where the .scop
// record still holds what was written there for it.
void restore_states(const CALifeObjectRegistry& registry, const SaveExtensionContainer::ChunkList& chunks)
{
    State& s = st();
    for (const SaveExtensionContainer::Chunk& chunk : chunks)
    {
        if ((chunk.type & 0xFFFF0000) != XmsGame::kStateChunkBase || chunk.version != kChunkVersion)
            continue;
        Cursor cursor{chunk.payload.data(), chunk.payload.data() + chunk.payload.size()};
        const u32 count = cursor.get<u32>();
        pcstr module_id = cursor.string();
        cursor.string();
        const XMS::Module* module = present(module_id);
        if (!cursor.ok || !module)
            continue;
        if (module->ns != u16(chunk.type & 0xFFFF))
            s.stale_chunks.push_back(chunk.type);
        u32 restored = 0, dropped = 0;
        for (u32 i = 0; i < count && cursor.ok; ++i)
        {
            const u16 id = cursor.get<u16>();
            const u32 section_crc = cursor.get<u32>();
            const u8 kind = cursor.get<u8>();
            CSE_ALifeDynamicObject* object = registry.object(id, true);
            const bool same = object && crc32(object->s_name.c_str(), u32(object->s_name.size())) == section_crc;
            CSE_ALifeItemWeapon* weapon = same ? smart_cast<CSE_ALifeItemWeapon*>(object) : nullptr;
            CSE_ALifeItemWeaponShotGun* shotgun = same ? smart_cast<CSE_ALifeItemWeaponShotGun*>(object) : nullptr;
            CSE_ALifeInventoryItem* item = same ? smart_cast<CSE_ALifeInventoryItem*>(object) : nullptr;
            pcstr why = same ? nullptr : "its object is gone or is another one now";
            switch (kind)
            {
            case AmmoType:
            case GrenadeType:
            {
                const u8 value = cursor.get<u8>();
                const u8 written = cursor.get<u8>();
                if (why)
                    break;
                if (!weapon)
                    why = "it is no weapon";
                else if (kind == AmmoType && weapon->ammo_type != written)
                    why = "its ammo was changed in a game without the module";
                else if (kind == GrenadeType && weapon->a_elapsed_grenades.grenades_type != written)
                    why = "its grenades were changed in a game without the module";
                else if (kind == AmmoType)
                    weapon->ammo_type = value;
                else
                    weapon->a_elapsed_grenades.grenades_type = value & 3;
                break;
            }
            case Cartridges:
            {
                const u32 written = cursor.get<u32>();
                const u8 entries = cursor.get<u8>();
                xr_vector<std::pair<u8, u8>> places;
                for (u8 k = 0; k < entries && cursor.ok; ++k)
                {
                    const u8 place = cursor.get<u8>();
                    places.emplace_back(place, cursor.get<u8>());
                }
                if (why)
                    break;
                if (!shotgun)
                    why = "it is no shotgun";
                else if (crc_of(game_cartridges(object->s_name, shotgun->m_AmmoIDs)) != written)
                    why = "its cartridges were changed in a game without the module";
                else
                    for (const auto& [place, value] : places)
                        if (place < shotgun->m_AmmoIDs.size())
                            shotgun->m_AmmoIDs[place] = value;
                break;
            }
            case Upgrade:
            {
                const u32 written = cursor.get<u32>();
                const u8 place = cursor.get<u8>();
                const shared_str upgrade(cursor.string());
                if (why)
                    break;
                if (!item)
                    why = "it is no item";
                else if (!pSettings->section_exist(upgrade))
                    why = "the module has no such upgrade any more";
                else if (crc_of(game_upgrades(item->m_upgrades)) != written)
                    why = "its upgrades were changed in a game without the module";
                else if (std::find(item->m_upgrades.begin(), item->m_upgrades.end(), upgrade) == item->m_upgrades.end())
                    item->m_upgrades.insert(item->m_upgrades.begin() + std::min<size_t>(place, item->m_upgrades.size()), upgrade);
                break;
            }
            case ActorVisual:
            {
                const shared_str worn(cursor.string());
                const shared_str written(cursor.string());
                if (why)
                    break;
                CSE_Visual* visual = object->visual();
                if (!visual)
                    why = "it has no model";
                else if (xr_strcmp(visual->get_visual(), written.c_str()))
                    why = "its model was changed in a game without the module";
                else
                    visual->set_visual(worn.c_str(), false);
                break;
            }
            case Task:
            {
                KeptTask task;
                task.key = cursor.get<u16>();
                task.place = cursor.get<u16>();
                const u32 size = cursor.get<u32>();
                const u8* bytes = cursor.bytes(size);
                if (why || !bytes)
                    break;
                task.bytes.assign(bytes, bytes + size);
                s.tasks.emplace_back(std::move(task));
                break;
            }
            default:
                // a kind this build does not know ends the chunk: what follows cannot be found
                cursor.ok = false;
                why = "its kind is unknown";
                break;
            }
            if (why)
            {
                ++dropped;
                Msg("! XMS: [%s] state of object %u not restored: %s", module->id.c_str(), u32(id), why);
            }
            else
                ++restored;
        }
        if (restored)
            Msg("* XMS: [%s] what %u record(s) of the game's objects say of its data is back", module->id.c_str(),
                restored);
        (void)dropped;
    }
}
} // namespace

void BeginCapture(u64 save_id)
{
    State& s = st();
    end_capture();
    if (!XMS::Active() && s.kept.empty() && s.declared.empty())
        return;
    CSE_ALifeDynamicObject* placeholder = make_placeholder();
    if (!placeholder)
    {
        Msg("! XMS: [%s] cannot be made - this save writes the modules' objects into .scop as before",
            kPlaceholderSection);
        return;
    }
    s.placeholder = placeholder;
    s.save_id = save_id;
    s.owner.assign(65536, kUnknown);
    s.written.assign(65536, 0);
    const size_t modules = XMS::Modules().size();
    s.objects.assign(modules, {});
    s.object_counts.assign(modules, 0);
    s.states.assign(modules, {});
    s.state_counts.assign(modules, 0);
    refresh_ranges();
    s.capturing = true;
}

bool CaptureObject(const CALifeObjectRegistry& registry, CSE_ALifeDynamicObject& object, IWriter& out,
    u32& record_count, NET_Packet& packet)
{
    State& s = st();
    if (!s.capturing)
        return false;
    s.written[object.ID] = 1;
    if (is_placeholder(object))
    {
        // an absent module's placeholder carries the lineage of the .scov this save writes now
        name_placeholder(object, s.save_id);
        ++s.placeholders;
        return false;
    }
    const u16 owner = owner_of(registry, object, &s.owner);
    if (owner < kModule)
        return capture_state(object, out, record_count, packet);

    const u16 index = owner - kModule;
    Bytes& records = s.objects[index];
    CSE_ALifeDynamicObject* parent = object.ID_Parent != kNoObject ? registry.object(object.ID_Parent, true) : nullptr;
    put<u16>(records, object.ID);
    put<u16>(records, object.ID_Parent);
    put<u8>(records, s.declared.contains(object.ID) ? kDeclared : 0);
    put_string(records, object.s_name.c_str());
    put<u64>(records, object.m_tClassID);
    put_string(records, parent ? parent->s_name.c_str() : "");
    const size_t size_at = records.size();
    put<u32>(records, 0);
    write_object(records, object, packet);
    const u32 size = u32(records.size() - size_at - sizeof(u32));
    memcpy(records.data() + size_at, &size, sizeof(size));
    ++s.object_counts[index];

    write_placeholder(out, object.ID, location_of(registry, object), XMS::Modules()[index].id.c_str(), packet);
    ++record_count;
    ++s.placeholders;
    return true;
}

void FinishCapture(const CALifeObjectRegistry& registry, IWriter& out, u32& record_count, NET_Packet& packet)
{
    State& s = st();
    if (!s.capturing)
        return;
    CSE_ALifeDynamicObject* actor = actor_of(registry);
    if (!s.placeholders && module_state_follows())
    {
        // nothing else carries the lineage: one more inert object under a free id does
        for (u32 id = 1; id < kNoObject; ++id)
            if (!s.written[id] && !registry.object(u16(id), true))
            {
                Location at;
                if (actor && base_vertex(actor->m_tGraphID))
                {
                    at.graph = actor->m_tGraphID;
                    at.position = actor->o_Position;
                }
                else
                    at.position = ai().game_graph().vertex(0)->level_point();
                at.node = ai().game_graph().vertex(at.graph)->level_vertex_id();
                write_placeholder(out, u16(id), at, nullptr, packet);
                ++record_count;
                break;
            }
    }
    if (actor && !base_vertex(actor->m_tGraphID))
        Msg("! XMS: the player stands on a level a module added - this save loads only in a game that has it");
    if (!s.mode_said)
    {
        const XMS::Module* provider = nullptr;
        pcstr mode = nullptr;
        for (const XMS::Module& m : XMS::Modules())
            for (const XMS::ProvidedMode& p : m.provides_modes)
                if (!provider && !m.disabled && XMS::ModeActive(p.id.c_str()))
                {
                    provider = &m;
                    mode = p.id.c_str();
                }
        if (provider)
            Msg("! XMS: this game runs mode [%s] of module [%s] - a game of its own, which the base game never starts",
                mode, provider->id.c_str());
        else if (XmsWorld::Declared())
            Msg("! XMS: this game runs the world rules of its modules - a world of its own, which the base game "
                "never starts");
        s.mode_said = true;
    }
    s.capturing = false;
    s.captured = true;
}

bool CollectMutations(xr_vector<SaveExtensionGameplay::ChunkMutation>& mutations)
{
    State& s = st();
    if (!s.captured)
        return true;
    const auto& modules = XMS::Modules();
    bool fits = true;
    for (u16 i = 0; i < modules.size() && fits; ++i)
    {
        const XMS::Module& m = modules[i];
        if (m.disabled)
            continue;
        for (const bool objects : {true, false})
        {
            SaveExtensionGameplay::ChunkMutation mutation;
            mutation.chunk.type = (objects ? XmsGame::kObjectChunkBase : XmsGame::kStateChunkBase) | m.ns;
            mutation.chunk.version = kChunkVersion;
            mutation.newestSupportedVersion = kChunkVersion;
            const u32 count = objects ? s.object_counts[i] : s.state_counts[i];
            const Bytes& records = objects ? s.objects[i] : s.states[i];
            if (!count)
                mutation.erase = true;
            else if (count > kMaxRecords || records.size() + 256 > kMaxChunkBytes)
            {
                Msg("! XMS: module [%s] has %u %s, %u bytes - more than one chunk of a save holds; the save stops",
                    m.id.c_str(), count, objects ? "object(s)" : "state record(s)", u32(records.size()));
                fits = false;
                break;
            }
            else
            {
                Bytes& payload = mutation.chunk.payload;
                payload.reserve(records.size() + 64);
                put<u32>(payload, count);
                put_string(payload, m.id.c_str());
                put_string(payload, m.version.c_str());
                put_bytes(payload, records.data(), records.size());
            }
            mutations.emplace_back(std::move(mutation));
        }
    }
    for (const u32 type : s.stale_chunks)
    {
        if (std::any_of(mutations.begin(), mutations.end(), [type](const auto& mutation) { return mutation.chunk.type == type; }))
            continue;
        SaveExtensionGameplay::ChunkMutation mutation;
        mutation.chunk.type = type;
        mutation.chunk.version = kChunkVersion;
        mutation.newestSupportedVersion = kChunkVersion;
        mutation.erase = true;
        mutations.emplace_back(std::move(mutation));
    }

    SaveExtensionGameplay::ChunkMutation kept;
    kept.chunk.type = SaveExtensionChunkIds::XmsKeptModules;
    kept.chunk.version = kChunkVersion;
    kept.newestSupportedVersion = kChunkVersion;
    kept.erase = s.kept.empty();
    if (!kept.erase)
    {
        put<u32>(kept.chunk.payload, u32(s.kept.size()));
        for (const KeptModule& module : s.kept)
        {
            put_string(kept.chunk.payload, module.id.c_str());
            put_string(kept.chunk.payload, module.version.c_str());
            put<u16>(kept.chunk.payload, module.ns);
            put<u32>(kept.chunk.payload, module.spawn_base);
            put<u32>(kept.chunk.payload, module.spawn_size);
        }
    }
    mutations.emplace_back(std::move(kept));
    end_capture();
    return fits;
}

void ResolveLineage(std::span<const u8> scop_source, pcstr sidecar_path, std::span<const u8> own_sidecar,
    bool own_valid, u64 own_save_id, SaveExtensionContainer::ChunkList& chunks)
{
    State& s = st();
    s.lineage_lost = false;
    if (own_valid && std::any_of(chunks.begin(), chunks.end(), [](const auto& chunk) { return XmsGame::IsModuleFamily(chunk.type); }))
        return;

    // the lineage of the first placeholder the .scop holds: they all carry the same one
    u64 lineage = 0;
    {
        IReader reader(const_cast<u8*>(scop_source.data()), int(scop_source.size()));
        if (!reader.find_chunk(OBJECT_CHUNK_DATA) || reader.elapsed() < int(sizeof(u32)))
            return;
        const u32 count = reader.r_u32();
        const size_t section_length = xr_strlen(kPlaceholderSection) + 1;
        bool found = false;
        for (u32 i = 0; i < count && !found; ++i)
        {
            if (reader.elapsed() < int(sizeof(u16)))
                return;
            const u16 spawn = reader.r_u16();
            if (reader.elapsed() < int(spawn))
                return;
            const u8* packet = static_cast<const u8*>(reader.pointer());
            // u16 M_SPAWN, the section, the name
            if (spawn > sizeof(u16) + section_length && 0 == memcmp(packet + sizeof(u16), kPlaceholderSection, section_length))
            {
                const u8* name = packet + sizeof(u16) + section_length;
                const u8* end = packet + spawn;
                if (memchr(name, 0, size_t(end - name)))
                    found = placeholder_lineage(reinterpret_cast<pcstr>(name), lineage);
            }
            reader.advance(spawn);
            if (reader.elapsed() < int(sizeof(u16)))
                return;
            const u16 update = reader.r_u16();
            if (reader.elapsed() < int(update))
                return;
            reader.advance(update);
        }
        if (!found || (own_valid && lineage == own_save_id))
            return;
    }

    const auto take = [&](const SaveExtensionContainer::ChunkList& from, pcstr where)
    {
        u32 taken = 0;
        for (const SaveExtensionContainer::Chunk& chunk : from)
        {
            const bool module_state = XmsGame::IsModuleFamily(chunk.type) || chunk.type == XmsGame::kManifestChunkId ||
                chunk.type == SaveExtensionChunkIds::XmsKeptModules;
            if (!module_state || SaveExtensionContainer::find_chunk(chunks, chunk.type))
                continue;
            chunks.push_back(chunk);
            ++taken;
        }
        Msg("* XMS: the module content of this save was found in [%s] (lineage %016llx): %u chunk(s)", where,
            static_cast<unsigned long long>(lineage), taken);
    };

    SaveExtensionContainer::Container container;
    if (!own_sidecar.empty() && SaveExtensionContainer::load_detached(own_sidecar, container) ==
        SaveExtensionContainer::LoadResult::Valid && container.binding.saveId == lineage)
    {
        take(container.chunks, sidecar_path);
        return;
    }

    // another save of the folder: a game that saved this world under a name of its own; the
    // newest first, by their first 16 bytes alone
    string_path folder, mask;
    FS.update_path(folder, "$game_saves$", "");
    strconcat(sizeof(mask), mask, folder, "*", SaveExtensionContainer::extension);
    struct Candidate
    {
        xr_string path;
        __time64_t written;
    };
    xr_vector<Candidate> candidates;
    _finddata64i32_t entry;
    const intptr_t handle = _findfirst64i32(mask, &entry);
    if (handle != -1)
    {
        do
        {
            if (!(entry.attrib & _A_SUBDIR))
                candidates.push_back({xr_string(folder) + entry.name, entry.time_write});
        } while (_findnext64i32(handle, &entry) == 0);
        _findclose(handle);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.written > b.written; });
    for (const Candidate& candidate : candidates)
    {
        if (0 == _stricmp(candidate.path.c_str(), sidecar_path))
            continue;
        u64 save_id = 0;
        if (!SaveExtensionContainer::read_save_id(candidate.path.c_str(), save_id) || save_id != lineage)
            continue;
        FILE* file = fopen(candidate.path.c_str(), "rb");
        if (!file)
            continue;
        xr_vector<u8> contents;
        fseek(file, 0, SEEK_END);
        const long length = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (length > 0 && length <= 64l * 1024 * 1024)
        {
            contents.resize(size_t(length));
            contents.resize(fread(contents.data(), 1, contents.size(), file));
        }
        fclose(file);
        if (SaveExtensionContainer::load_detached({contents.data(), contents.size()}, container) ==
            SaveExtensionContainer::LoadResult::Valid)
        {
            take(container.chunks, candidate.path.c_str());
            return;
        }
    }
    s.lineage_lost = true;
    Msg("! XMS: the module content of this save could not be found - lineage %016llx was looked for in [%s] and in "
        "%u other save(s) of [%s]; its placeholders are removed", static_cast<unsigned long long>(lineage),
        sidecar_path, u32(candidates.size()), folder);
}

void OnSnapshotLoaded(const SaveExtensionContainer::ChunkList& chunks)
{
    State& s = st();
    end_capture();
    s.tasks.clear();
    s.loading = false;
    s.loading_groups.clear();
    s.declared.clear();
    s.kept.clear();
    s.forgotten.clear();
    s.stale_chunks.clear();
    s.ranges_ready = false;

    for (const SaveExtensionContainer::Chunk& chunk : chunks)
    {
        if (chunk.type != SaveExtensionChunkIds::XmsKeptModules || chunk.version != kChunkVersion)
            continue;
        Cursor cursor{chunk.payload.data(), chunk.payload.data() + chunk.payload.size()};
        const u32 count = cursor.get<u32>();
        for (u32 i = 0; i < count && cursor.ok; ++i)
        {
            pcstr id = cursor.string();
            pcstr version = cursor.string();
            const u16 ns = cursor.get<u16>();
            const u32 base = cursor.get<u32>();
            const u32 size = cursor.get<u32>();
            if (!cursor.ok || !id[0] || present(id))
                continue;
            // a range that cannot be a module's would take the game's own objects out with it
            const bool own_range = XMS::SpawnRangeValid(id, base, size);
            if (own_range)
                XMS::AdoptSpawnRange(id, base, size);
            kept_candidate(id, version, ns, own_range ? base : 0, own_range ? size : 0);
        }
    }
    for (const auto& orphan : XmsGame::bs().orphans)
        if (!present(orphan.id.c_str()))
            kept_candidate(orphan.id.c_str(), orphan.version.c_str(), orphan.ns, orphan.base, orphan.size);
    for (const SaveExtensionContainer::Chunk& chunk : chunks)
    {
        const u32 family = chunk.type & 0xFFFF0000;
        pcstr module = nullptr, version = nullptr;
        if ((family == XmsGame::kObjectChunkBase || family == XmsGame::kStateChunkBase) && chunk.version == kChunkVersion &&
            chunk_module(chunk, module, version) && !present(module))
            kept_candidate(module, version, u16(chunk.type & 0xFFFF), 0, 0);
    }
}

void OnObjectsLoaded(CALifeObjectRegistry& registry)
{
    State& s = st();
    const SaveExtensionContainer::ChunkSnapshot snapshot = SaveExtensionContainer::snapshot_loaded_chunks();
    refresh_ranges();

    // the placeholders of the .scop
    struct Found
    {
        CSE_ALifeDynamicObject* object{};
        shared_str module;
        bool anchor{};
    };
    xr_vector<Found> found;
    xr_unordered_map<u16, size_t> by_id;
    const shared_str section(kPlaceholderSection);
    for (const auto& [id, object] : registry.objects())
    {
        u64 lineage = 0;
        if (object->s_name != section || !placeholder_lineage(object->name_replace(), lineage))
            continue;
        Found& placeholder = found.emplace_back();
        placeholder.object = object;
        placeholder.module = placeholder_module(*object, placeholder.anchor);
        by_id[id] = found.size() - 1;
    }

    // a present module's objects come back in their places
    xr_vector<u16> newer;
    for (const SaveExtensionContainer::Chunk& chunk : *snapshot)
    {
        if ((chunk.type & 0xFFFF0000) != XmsGame::kObjectChunkBase)
            continue;
        if (chunk.version != kChunkVersion)
        {
            // written by a newer game: its placeholders keep their ids, the chunk stays as it is
            newer.push_back(u16(chunk.type & 0xFFFF));
            continue;
        }
        ObjectFamily family;
        if (!parse_objects(chunk, family))
        {
            Msg("! XMS: the object chunk %08x of the save does not read - ignored", chunk.type);
            continue;
        }
        const XMS::Module* module = present(family.module);
        if (!module)
        {
            if (KeptModule* kept = kept_module(family.module))
                kept->objects += u32(family.records.size());
            continue;
        }
        if (module->ns != u16(chunk.type & 0xFFFF))
            s.stale_chunks.push_back(chunk.type);
        u32 restored = 0, dropped = 0;
        for (const ObjectRecord& record : family.records)
        {
            const auto at = by_id.find(record.id);
            if (at == by_id.end() || !found[at->second].object)
            {
                ++dropped;
                Msg("! XMS: [%s] object %u [%s] dropped: its placeholder is not in the save any more",
                    module->id.c_str(), u32(record.id), record.section);
                continue;
            }
            Found& placeholder = found[at->second];
            pcstr why = restore_refusal(registry, record);
            remove_loaded(registry, placeholder.object);
            CSE_ALifeDynamicObject* object = nullptr;
            if (!why)
            {
                IReader reader(const_cast<u8*>(record.state), int(record.size));
                object = CALifeObjectRegistry::get_object(reader);
                if (!object || object->ID != record.id)
                    why = "its record does not read back";
            }
            if (why)
            {
                if (object)
                {
                    CSE_Abstract* abstract = object;
                    F_entity_Destroy(abstract);
                }
                ++dropped;
                Msg("! XMS: [%s] object %u [%s] dropped: %s", module->id.c_str(), u32(record.id), record.section, why);
                continue;
            }
            registry.add(object);
            if (record.flags & kDeclared)
                s.declared[record.id] = index_of(*module);
            ++restored;
        }
        Msg("* XMS: [%s] %u object(s) back from the save's .scov%s", module->id.c_str(), restored,
            dropped ? ", some dropped (above)" : "");
    }

    // what is left: anchors, an absent module's placeholders - they stay and hold its ids -, and
    // placeholders whose object is lost
    xr_unordered_map<shared_str, u32, std::hash<shared_str>> lost;
    u32 removed = 0;
    for (Found& placeholder : found)
    {
        if (!placeholder.object)
            continue;
        const XMS::Module* module = present(placeholder.module.c_str());
        const bool newer_chunk = module && std::find(newer.begin(), newer.end(), module->ns) != newer.end();
        if (placeholder.anchor || s.lineage_lost || !placeholder.module || (module && !newer_chunk))
        {
            if (!placeholder.anchor && !s.lineage_lost)
                ++lost[placeholder.module];
            ++removed;
            remove_loaded(registry, placeholder.object);
            continue;
        }
        kept_candidate(placeholder.module.c_str(), "", 0, 0, 0).reserved.push_back({placeholder.object->ID});
    }
    for (const auto& [module, count] : lost)
        Msg("! XMS: [%s] %u placeholder(s) without the object they stood for - removed", module.c_str(), count);
    if (s.lineage_lost && removed)
        Msg("! XMS: %u placeholder(s) of the lost module content removed", removed);

    // what an older save keeps in .scop of a module that is absent now
    SaveExtensionContainer::ChunkList chunks;
    bool chunks_changed = false;
    {
        NET_Packet packet;
        chunks = *snapshot;
        keep_orphans(registry, packet, chunks, chunks_changed);
    }
    if (chunks_changed)
        SaveExtensionContainer::set_loaded_chunks(chunks);

    // the kept set: an absent module this save holds anything of
    for (auto kept = s.kept.begin(); kept != s.kept.end();)
    {
        const bool has_chunks = std::any_of(chunks.begin(), chunks.end(), [&](const SaveExtensionContainer::Chunk& chunk)
        {
            pcstr module = nullptr, version = nullptr;
            const u32 family = chunk.type & 0xFFFF0000;
            if ((family == XmsGame::kObjectChunkBase || family == XmsGame::kStateChunkBase) && chunk_module(chunk, module, version))
                return 0 == xr_strcmp(module, kept->id.c_str());
            return kept->ns && XmsGame::IsModuleFamily(chunk.type) && u16(chunk.type & 0xFFFF) == kept->ns;
        });
        if (!has_chunks && kept->reserved.empty())
        {
            kept = s.kept.erase(kept);
            continue;
        }
        Msg("! XMS: module [%s] is absent - this game keeps %u object(s) of it out of the world, and its saved data, "
            "until it is back (xms_forget %s drops them)", kept->id.c_str(), kept->objects, kept->id.c_str());
        ++kept;
    }

    s.tasks.clear();
    restore_states(registry, chunks);

    // until the squads register their members (CSE_ALifeOnlineOfflineGroup::on_after_game_load),
    // a member's squad is what the rosters say - the scripts ask who owns what as objects register
    s.loading_groups.clear();
    for (const auto& [id, object] : registry.objects())
        if (CSE_ALifeOnlineOfflineGroup* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(object))
            for (const auto& [member, pointer] : group->squad_members())
            {
                (void)pointer;
                s.loading_groups[member] = id;
            }
    s.loading = true;
}

void OnRegistriesLoaded(CGameTaskRegistry& tasks)
{
    State& s = st();
    u32 back = 0;
    for (KeptTask& kept : s.tasks)
    {
        IReader reader(kept.bytes.data(), kept.bytes.size());
        SGameTaskKey task;
        task.load(reader);
        vGameTasks* list = tasks.object(kept.key, true);
        if (!list)
        {
            vGameTasks empty;
            tasks.add(kept.key, empty, true);
            list = tasks.object(kept.key, true);
        }
        const bool there = std::any_of(list->begin(), list->end(), [&](const SGameTaskKey& other) { return other.task_id == task.task_id; });
        if (there)
        {
            task.destroy();
            continue;
        }
        list->insert(list->begin() + std::min<size_t>(kept.place, list->size()), task);
        ++back;
    }
    if (back)
        Msg("* XMS: %u quest task(s) of the modules back in the actor's task registry", back);
    s.tasks.clear();
}

void OnLoadFinished()
{
    State& s = st();
    s.loading = false;
    s.loading_groups.clear();
}

void SaveTaskRegistry(CGameTaskRegistry& tasks, IWriter& out)
{
    State& s = st();
    struct Moved
    {
        u16 key;
        size_t place;
        SGameTaskKey task;
    };
    xr_vector<Moved> moved;
    const CALifeSimulator* sim = ai().get_alife();
    if (s.captured && !s.states.empty() && sim)
    {
        xr_vector<u16> keys;
        for (const auto& [key, list] : tasks.objects())
            keys.push_back(key);
        for (const u16 key : keys)
        {
            vGameTasks* list = tasks.object(key, true);
            CSE_ALifeDynamicObject* holder = sim->objects().object(key, true);
            if (!list || !holder)
                continue;
            for (size_t i = list->size(); i-- > 0;)
            {
                const XMS::Module* module = task_module((*list)[i].task_id);
                if (!module || !(*list)[i].game_task)
                    continue;
                CMemoryWriter bytes;
                (*list)[i].save(bytes);
                Bytes payload;
                put<u16>(payload, key);
                put<u16>(payload, u16(std::min<size_t>(i, 0xFFFF)));
                put<u32>(payload, u32(bytes.size()));
                put_bytes(payload, bytes.pointer(), bytes.size());
                state_record(index_of(*module), key, section_crc_of(*holder), Task, payload);
                moved.push_back({key, i, (*list)[i]});
                list->erase(list->begin() + i);
            }
        }
    }
    tasks.CGameTaskRegistry::save(out);
    std::sort(moved.begin(), moved.end(), [](const Moved& a, const Moved& b) { return a.key != b.key ? a.key < b.key : a.place < b.place; });
    for (const Moved& task : moved)
        if (vGameTasks* list = tasks.object(task.key, true))
            list->insert(list->begin() + std::min(task.place, list->size()), task.task);
}

void Reset()
{
    State& s = st();
    end_capture();
    s.declared.clear();
    s.kept.clear();
    s.tasks.clear();
    s.loading = false;
    s.loading_groups.clear();
    s.forgotten.clear();
    s.stale_chunks.clear();
    s.ranges_ready = false;
    s.lineage_lost = false;
    s.mode_said = false;
}

void ForgetObject(u16 id) { st().declared.erase(id); }

pcstr ObjectOwner(u16 id)
{
    const CALifeSimulator* sim = ai().get_alife();
    if (!sim || id == kNoObject)
        return nullptr;
    for (const KeptModule& kept : st().kept)
        for (const Reserved& reserved : kept.reserved)
            if (reserved.id == id)
                return kept.id.c_str();
    const CALifeObjectRegistry& registry = sim->objects();
    CSE_ALifeDynamicObject* object = registry.object(id, true);
    if (!object)
        return nullptr;
    if (is_placeholder(*object))
    {
        // read before the load replaced it, or kept for a module that is away: the id is the module's
        bool anchor = false;
        const shared_str module = placeholder_module(*object, anchor);
        if (!module)
            return nullptr;
        if (const XMS::Module* m = present(module.c_str()))
            return m->id.c_str();
        if (const KeptModule* kept = kept_module(module.c_str()))
            return kept->id.c_str();
        auto& names = st().names;
        if (std::find(names.begin(), names.end(), module) == names.end())
            names.push_back(module);
        return std::find(names.begin(), names.end(), module)->c_str();
    }
    const u16 owner = owner_of(registry, *object, nullptr);
    return owner >= kModule ? XMS::Modules()[owner - kModule].id.c_str() : nullptr;
}

pcstr SectionOwner(pcstr section, pcstr file)
{
    if (!section || !section[0])
        return nullptr;
    if (!file || !file[0] || 0 == _stricmp(file, "system.ltx"))
    {
        const XMS::Module* m = XMS::ModuleOfSection(shared_str(section));
        return m ? m->id.c_str() : nullptr;
    }
    const XMS::Module* m = XMS::ModuleOfFileSection(file, section);
    if (!m && XMS::Active())
    {
        // a config nobody has read yet in this session: reading it applies the modules' patches,
        // which note what they make there
        xr_string key(file);
        xr_strlwr(key);
        auto& read = st().read_files;
        if (std::find(read.begin(), read.end(), key) == read.end())
        {
            read.push_back(key);
            string_path path;
            FS.update_path(path, "$game_config$", file);
            if (FS.exist(path))
            {
                CInifile ini(path, TRUE);
            }
            m = XMS::ModuleOfFileSection(file, section);
        }
    }
    return m ? m->id.c_str() : nullptr;
}

bool OwnObject(u16 id, pcstr module_id)
{
    const XMS::Module* module = present(module_id);
    const CALifeSimulator* sim = ai().get_alife();
    if (!module || !sim || id == kNoObject)
        return false;
    CSE_ALifeDynamicObject* object = sim->objects().object(id, true);
    if (!object || smart_cast<CSE_ALifeCreatureActor*>(object) || is_placeholder(*object))
        return false;
    st().declared[id] = index_of(*module);
    return true;
}

bool Forget(pcstr module_id, xr_string& answer)
{
    State& s = st();
    if (present(module_id))
    {
        answer.assign("! XMS: module [").append(module_id).append("] is here - nothing of it is kept away");
        return false;
    }
    KeptModule* kept = kept_module(module_id);
    if (!kept)
    {
        answer.assign("! XMS: this game keeps nothing of [").append(module_id ? module_id : "").append("] (xms_list)");
        return false;
    }
    CALifeSimulator* sim = const_cast<CALifeSimulator*>(ai().get_alife());
    if (!sim)
    {
        answer = "! XMS: no game is loaded";
        return false;
    }
    const KeptModule module = *kept;
    s.kept.erase(s.kept.begin() + (kept - s.kept.data()));

    // its chunks leave the save's state, so the next save writes none of them
    const SaveExtensionContainer::ChunkSnapshot snapshot = SaveExtensionContainer::snapshot_loaded_chunks();
    SaveExtensionContainer::ChunkList chunks;
    u32 dropped = 0;
    for (const SaveExtensionContainer::Chunk& chunk : *snapshot)
    {
        pcstr owner = nullptr, version = nullptr;
        const u32 family = chunk.type & 0xFFFF0000;
        const bool named = (family == XmsGame::kObjectChunkBase || family == XmsGame::kStateChunkBase) &&
            chunk_module(chunk, owner, version) && 0 == xr_strcmp(owner, module.id.c_str());
        const bool by_ns = module.ns && XmsGame::IsModuleFamily(chunk.type) && u16(chunk.type & 0xFFFF) == module.ns;
        if (named || by_ns)
        {
            ++dropped;
            continue;
        }
        chunks.push_back(chunk);
    }
    SaveExtensionContainer::set_loaded_chunks(std::move(chunks));
    if (module.ns)
    {
        auto& blobs = XmsGame::bs();
        blobs.loaded.erase(module.ns);
        blobs.pending.erase(module.ns);
        blobs.loaded_ledgers.erase(module.ns);
        blobs.pending_ledgers.erase(module.ns);
    }
    // its placeholders leave the world and free the ids
    const CALifeObjectRegistry& registry = static_cast<const CALifeSimulator*>(sim)->objects();
    for (const Reserved& reserved : module.reserved)
        if (CSE_ALifeDynamicObject* placeholder = registry.object(reserved.id, true))
            if (is_placeholder(*placeholder))
                sim->release(placeholder, true);
    s.forgotten.push_back(module.id);

    // the scripts' part: what the quest runtime and the save scripts keep for the module
    if (GEnv.ScriptEngine)
    {
        try
        {
            luabind::functor<void> forget;
            if (GEnv.ScriptEngine->functor("xms_saves.forget", forget))
                forget(module.id.c_str());
            luabind::functor<void> dispatch;
            if (GEnv.ScriptEngine->functor("xms.dispatch_forget", dispatch))
                dispatch(module.id.c_str());
        }
        catch (...)
        {
            Msg("! XMS: xms_forget %s: a script failed to drop what it keeps for the module", module.id.c_str());
        }
    }

    string512 text;
    xr_sprintf(text, "* XMS: forgot module [%s]: %u object(s), %u id(s) and %u chunk(s) of its saved state dropped from "
        "this game - the next save keeps none of it", module.id.c_str(), module.objects, u32(module.reserved.size()), dropped);
    answer = text;
    return true;
}

void DescribeKept(xr_vector<xr_string>& lines)
{
    for (const KeptModule& kept : st().kept)
    {
        string512 line;
        xr_sprintf(line, "* [kept] %s %s - absent; this game keeps %u object(s) of it out of the world "
            "(xms_forget %s drops them)", kept.id.c_str(), kept.version.c_str(), kept.objects, kept.id.c_str());
        lines.emplace_back(line);
    }
}

void KeptModules(xr_vector<shared_str>& out)
{
    for (const KeptModule& kept : st().kept)
        out.push_back(kept.id);
}

const xr_vector<shared_str>& Forgotten() { return st().forgotten; }
} // namespace XmsSaves

// ---- Lua natives -----------------------------------------------------------

namespace
{
const XMS::Module* find_enabled(pcstr id)
{
    // every caller takes the id straight from Lua, where nil is a normal argument
    if (!id || !id[0])
        return nullptr;
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
        entry["author"] = m.author.c_str();
        entry["description"] = m.description.c_str();
        entry["website"] = m.website.c_str();
        entry["pda"] = m.pda_layout.c_str();
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

// ---- module files (NQ assets and other data a runtime reads itself) --------

// A path inside a module: relative, no '..' anywhere, no drive/UNC, no glob
// or control characters. Both '/' and '\' separate.
bool valid_rel_path(pcstr path)
{
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\' || strstr(path, ".."))
        return false;
    for (pcstr c = path; *c; ++c)
        if (u8(*c) < 0x20 || *c == ':' || *c == '<' || *c == '>' || *c == '"' || *c == '|' || *c == '?' || *c == '*')
            return false;
    return true;
}

// subdir "" = module root; a subdir that fails the guard yields nil, an
// absent one just an empty list
luabind::object xms_native_list_files(lua_State* L, pcstr id, pcstr subdir, pcstr mask, bool recursive)
{
    const XMS::Module* m = find_enabled(id);
    if (!m || !mask || !mask[0])
        return luabind::object();
    xr_string dir = m->root;
    xr_string prefix;
    if (subdir && subdir[0])
    {
        if (!valid_rel_path(subdir))
            return luabind::object();
        prefix = subdir;
        for (char& c : prefix)
            if (c == '/')
                c = _DELIMITER;
        if (prefix.back() != _DELIMITER)
            prefix += _DELIMITER;
        dir += prefix;
    }
    xr_vector<xr_string> files;
    XMS::ListFilesRelative(dir.c_str(), mask, recursive, files);

    luabind::object result = luabind::newtable(L);
    int index = 1;
    for (const xr_string& file : files)
        result[index++] = (prefix + file).c_str();
    return result;
}

luabind::object xms_native_read_file(lua_State* L, pcstr id, pcstr relpath)
{
    const XMS::Module* m = find_enabled(id);
    if (!m)
    {
        Msg("! XMS: read_file: no enabled module [%s]", id ? id : "");
        return luabind::object();
    }
    if (!valid_rel_path(relpath))
    {
        Msg("! XMS: read_file [%s]: refused path [%s]", id, relpath ? relpath : "");
        return luabind::object();
    }
    string_path path;
    xr_sprintf(path, "%s%s", m->root.c_str(), relpath);
    for (char* c = path; *c; ++c)
        if (*c == '/')
            *c = _DELIMITER;

    FILE* f = fopen(path, "rb");
    if (!f)
    {
        Msg("! XMS: read_file [%s]: cannot open [%s]", id, relpath);
        return luabind::object();
    }
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    constexpr long kMaxFileSize = 8 * 1024 * 1024;
    if (len < 0 || len > kMaxFileSize)
    {
        fclose(f);
        Msg("! XMS: read_file [%s]: [%s] is %ld bytes, limit is %ld", id, relpath, len, kMaxFileSize);
        return luabind::object();
    }
    xr_vector<char> buffer(static_cast<size_t>(len));
    const size_t got = len > 0 ? fread(buffer.data(), 1, size_t(len), f) : 0;
    fclose(f);

    lua_pushlstring(L, buffer.data(), got);
    luabind::object result(luabind::from_stack(L, -1));
    lua_pop(L, 1);
    return result;
}

bool xms_native_module_applies(pcstr id)
{
    const XMS::Module* m = id ? XMS::FindModule(id) : nullptr;
    return m && XMS::ModuleApplies(*m);
}

// ---- persistent blobs ------------------------------------------------------

// Engine-owned state rides the module blob channel under namespaces the
// registry never allocates. The "xms." id prefix is reserved for these and
// resolves before any module id.
struct CoreBlob
{
    pcstr id;
    u16 ns;
};
constexpr CoreBlob kCoreBlobs[] = {
    {"xms.nq", 0xFF01}, // NQ quest-graph runtime state, chunk 0x584DFF01
    {"xms.radio", 0xFF04}, // the radios' playlists and tuning (xms_radio.script), chunk 0x584DFF04
};
static_assert(kCoreBlobs[0].ns >= XMS::kReservedNsBase && kCoreBlobs[1].ns >= XMS::kReservedNsBase);
// 0xFF02 (the world) and 0xFF03 (PDA vars) are written by XmsWorld and XmsPda themselves
static_assert(kCoreBlobs[1].ns != 0xFF02 && kCoreBlobs[1].ns != 0xFF03);
static_assert((XmsGame::kModuleChunkBase | kCoreBlobs[0].ns) == SaveExtensionChunkIds::XmsNqState);

bool resolve_blob_ns(pcstr id, u16& ns)
{
    if (!id)
        return false;
    for (const CoreBlob& blob : kCoreBlobs)
        if (0 == xr_strcmp(id, blob.id))
        {
            ns = blob.ns;
            return true;
        }
    const XMS::Module* m = find_enabled(id);
    if (!m)
        return false;
    ns = m->ns;
    return true;
}

bool xms_native_save_data(lua_State* L, pcstr id, luabind::object data)
{
    u16 ns = 0;
    if (!resolve_blob_ns(id, ns))
        return false;
    if (luabind::type(data) == LUA_TNIL)
    {
        XmsGame::SetPendingBlob(ns, "", 0);
        return true;
    }
    if (luabind::type(data) != LUA_TSTRING)
        return false;
    data.push(L);
    size_t size = 0;
    pcstr bytes = lua_tolstring(L, -1, &size);
    XmsGame::SetPendingBlob(ns, bytes, size);
    lua_pop(L, 1);
    return true;
}

luabind::object xms_native_load_data(lua_State* L, pcstr id)
{
    u16 ns = 0;
    if (!resolve_blob_ns(id, ns))
        return luabind::object();
    const xr_vector<u8>* blob = XmsGame::GetLoadedBlob(ns);
    if (!blob || blob->empty())
        return luabind::object();
    lua_pushlstring(L, reinterpret_cast<pcstr>(blob->data()), blob->size());
    luabind::object result(luabind::from_stack(L, -1));
    lua_pop(L, 1);
    return result;
}

// ---- what a module brings and the save (XmsSaves) --------------------------

luabind::object xms_native_object_owner(lua_State* L, u32 id)
{
    pcstr owner = id < 0xffff ? XmsSaves::ObjectOwner(u16(id)) : nullptr;
    return owner ? luabind::object(L, owner) : luabind::object();
}

bool xms_native_own_object(u32 id, pcstr module_id) { return id < 0xffff && XmsSaves::OwnObject(u16(id), module_id); }
bool xms_native_module_present(pcstr id) { return !!find_enabled(id); }

luabind::object xms_native_section_owner(lua_State* L, pcstr section, pcstr file)
{
    pcstr owner = XmsSaves::SectionOwner(section, file && file[0] ? file : nullptr);
    return owner ? luabind::object(L, owner) : luabind::object();
}

luabind::object id_table(lua_State* L, const xr_vector<shared_str>& ids)
{
    luabind::object result = luabind::newtable(L);
    int index = 1;
    for (const shared_str& id : ids)
        result[index++] = id.c_str();
    return result;
}

luabind::object xms_native_kept_modules(lua_State* L)
{
    xr_vector<shared_str> ids;
    XmsSaves::KeptModules(ids);
    return id_table(L, ids);
}

luabind::object xms_native_forgotten(lua_State* L) { return id_table(L, XmsSaves::Forgotten()); }

// ---- the streamer mode and the radio (MODDING.md, "Streamer mode", "Screens and music") ----------------------

bool xms_native_streamer_mode() { return psSoundStreamerMode != 0; }
bool xms_native_sound_copyrighted(pcstr name) { return name && snd_copyrighted(name); }

luabind::object xms_native_sound_replacement(lua_State* L, pcstr name)
{
    const xr_string instead = name ? snd_replacement(name) : xr_string();
    return instead.empty() ? luabind::object() : luabind::object(L, instead.c_str());
}

// { only = <module id | nil>, tracks = { { module, id, sound, copyrighted, streamer, volume, weight }, ... },
//   playlists = { { module, id, title, order, tracks = { <sound name>, ... } }, ... } }
luabind::object xms_native_radio(lua_State* L)
{
    const XmsScreens::Radio radio = XmsScreens::RadioMusic();
    luabind::object result = luabind::newtable(L);
    if (radio.only)
        result["only"] = radio.only->id.c_str();
    luabind::object tracks = luabind::newtable(L);
    int index = 1;
    for (const XmsScreens::RadioTrack& t : radio.tracks)
    {
        luabind::object row = luabind::newtable(L);
        row["module"] = t.module->id.c_str();
        row["id"] = t.track->id.c_str();
        row["sound"] = t.sound.c_str();
        row["copyrighted"] = t.copyrighted;
        if (!t.streamer.empty())
            row["streamer"] = t.streamer.c_str();
        row["volume"] = t.track->volume;
        row["weight"] = t.track->weight;
        tracks[index++] = row;
    }
    result["tracks"] = tracks;
    luabind::object playlists = luabind::newtable(L);
    index = 1;
    for (const XmsScreens::RadioPlaylist& p : radio.playlists)
    {
        luabind::object row = luabind::newtable(L);
        row["module"] = p.module->id.c_str();
        row["id"] = p.playlist->id.c_str();
        row["title"] = p.playlist->title.c_str();
        row["order"] = p.playlist->shuffle ? "shuffle" : "list";
        luabind::object sounds = luabind::newtable(L);
        int k = 1;
        for (const xr_string& sound : p.sounds)
            sounds[k++] = sound.c_str();
        row["tracks"] = sounds;
        playlists[index++] = row;
    }
    result["playlists"] = playlists;
    return result;
}

// ---- virtual dialogs (NQ) --------------------------------------------------

bool xms_native_dialog_register(pcstr id, pcstr init_func) { return CPhraseDialog::RegisterVirtual(id, init_func); }
void xms_native_dialog_unregister(pcstr id) { CPhraseDialog::UnregisterVirtual(id); }
bool xms_native_dialog_invalidate(pcstr id) { return CPhraseDialog::InvalidateVirtual(id); }

void xms_native_log(pcstr text) { Msg("[xms] %s", text ? text : ""); }

// ---- game modes -------------------------------------------------------------

void xms_native_set_modes(pcstr csv) { XMS::SetActiveModes(csv); }
pcstr xms_native_active_modes() { return XMS::ActiveModesCsv(); }
bool xms_native_mode_active(pcstr id) { return XMS::ModeActive(id); }

// every mode any installed module declares: {id, title, caption, provider}, once per id - the first
// module in load order that brings it (XMS::ModeOffered; the later ones are reported at mount)
luabind::object xms_native_known_modes(lua_State* L)
{
    luabind::object result = luabind::newtable(L);
    int index = 1;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled)
            continue;
        for (size_t n = 0; n < m.provides_modes.size(); ++n)
        {
            if (!XMS::ModeOffered(m, n))
                continue;
            const XMS::ProvidedMode& mode = m.provides_modes[n];
            luabind::object entry = luabind::newtable(L);
            entry["id"] = mode.id.c_str();
            entry["title"] = mode.title.c_str();
            entry["caption"] = ModsDisplayText(mode.title.empty() ? mode.id : mode.title).c_str();
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

// A destination nobody drew: registers the one-point runtime patrol path
// `name` at the navmesh cell nearest to the position, or moves it there when it
// exists already. With it the STOCK schemes (walker, camper, ...) walk to any
// place - danger, cover and combat intact - instead of the scripts growing a
// movement system of their own. Runtime names live under one reserved prefix so
// a module can never shadow or move a path the level shipped.
bool xms_native_patrol_point(pcstr name, float x, float y, float z)
{
    static constexpr pcstr kPrefix = "xms_rt_";
    if (!name || 0 != strncmp(name, kPrefix, xr_strlen(kPrefix)) || xr_strlen(name) > 120)
        return false;
    CPatrolPathStorage* storage = ai().get_patrol_paths();
    if (!storage || !ai().get_level_graph() || !ai().get_cross_table() || !ai().get_game_graph())
        return false;
    const Fvector position = {x, y, z};
    if (!_valid(position))
        return false;
    // 15 m: far enough for an object lying off the mesh, close enough that a
    // place in the void is refused instead of walked to the map's edge
    return storage->set_runtime_point(name, position, 15.f, ai().get_level_graph(), ai().get_cross_table(),
        ai().get_game_graph());
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
xms.patrol_point = xms_native_patrol_point
-- NQ quest-graph support (feature-test with xms.nq_api)
xms.nq_api = 1
xms.list_files = xms_native_list_files
xms.read_file = xms_native_read_file
xms.module_applies = xms_native_module_applies
xms.dialog_register = xms_native_dialog_register
xms.dialog_unregister = xms_native_dialog_unregister
xms.dialog_invalidate = xms_native_dialog_invalidate

-- The network (docs/dead-air/MODDING.md, "The network"); feature-test with xms.http_api. A
-- module whose code names xms.http carries the network mark where players pick and manage
-- modules, and the game answers requests only while an enabled module carries it.
xms.http_api = 1
do
    local waiting = {}
    local function header_lines(headers)
        if type(headers) ~= "table" then return "" end
        local lines = {}
        for name, value in pairs(headers) do
            lines[#lines + 1] = tostring(name) .. ": " .. tostring(value) .. "\r\n"
        end
        table.sort(lines)
        return table.concat(lines)
    end
    xms.http = {
        -- ({ url, method, headers, body, timeout } | url, function(answer)) -> id | nil, why
        -- answer: { ok, status, body, error, headers }
        request = function(options, done)
            if type(options) == "string" then options = { url = options } end
            if type(options) ~= "table" then return nil, "the request is a table or an address" end
            local id, why = xms_native_http_request(tostring(options.url or ""), tostring(options.method or "GET"),
                header_lines(options.headers), options.body ~= nil and tostring(options.body) or "",
                tonumber(options.timeout) or 0)
            if not id then return nil, why end
            waiting[id] = done
            return id
        end,
        cancel  = function(id) waiting[id] = nil xms_native_http_cancel(id) end,
        allowed = xms_native_http_allowed, -- () -> true | false, why
    }
    -- the engine's: a request has ended
    xms._http_done = function(id, ok, status, body, why, headers)
        local done = waiting[id]
        waiting[id] = nil
        if type(done) == "function" then
            done({ ok = ok, status = status, body = body, error = why, headers = headers })
        end
    end
end

-- World rules of the mode (docs/dead-air/MODDING.md, "World rules"); feature-test with
-- xms.world_api. Rules are named by their manifest keys: original_npcs, random_stalkers,
-- mutants, npc_quests, story_quests.
xms.world_api = 1
xms.world = {
    keep            = xms_native_world_keep,             -- (rule, level) -> true when the original content stays
    set_rule        = xms_native_world_set_rule,         -- (rule, 1 keep | 0 remove | -1 clear), whole world
    in_area         = xms_native_world_in_area,          -- (level) -> true inside the playable area
    area_level      = xms_native_world_area_level,       -- (level, true|false) grows or shrinks the area
    transition_lock = xms_native_world_transition_lock,  -- (level changer name, locked)
    level_lock      = xms_native_world_level_lock,       -- (level, locked): every changer on it
    transition_open = xms_native_world_transition_open,  -- (level changer name) -> bool
    declared        = xms_native_world_declared,         -- () -> any rule, area or lock in force
    settle          = xms_native_world_settle,           -- () the faction start jump is not coming
    describe        = xms_native_world_describe,         -- () -> text
    choices         = xms_native_world_choices,          -- (mode id) -> { {key, keep, module}, ... }
}

-- The module's own PDA interface (docs/dead-air/MODDING.md, "PDA layouts"); feature-test with
-- xms.pda_api. Texts may be UTF-8 or cp1251; xms_pda.script converts what needs it.
xms.pda_api = 1
xms.pda = {
    active   = function() return xms_native_pda_active() end,             -- () -> layout id | nil
    set      = function(name, value) return xms_pda.set(name, value) end, -- {name} of every text; nil clears; saved
    get      = function(name) return xms_native_pda_get(name) end,
    page     = function(id) return xms_native_pda_page(id) end,           -- (page id) -> true when it exists
    current  = function() return xms_native_pda_current() end,
    show     = function(id, on) return xms_native_pda_show(id, on == false and 0 or 1) end,
    hide     = function(id) return xms_native_pda_show(id, 0) end,
    toggle   = function(id) return xms_native_pda_show(id, -1) end,
    text     = function(id, text) return xms_pda.text(id, text) end,      -- a text or a button's caption
    rows     = function(id, rows) return xms_pda.rows(id, rows) end,      -- a list's rows, { {field = value}, ... }
    click    = function(id, row) return xms_native_pda_click(id, row or 0) end, -- a click on the item (a list: its row)
    open     = function(page) return xms_native_pda_open(page) end,       -- opens the PDA, on a page when named
    close    = function() return xms_native_pda_close() end,
    describe = function() return xms_native_pda_describe() end,
}

-- What a module brings and the save (docs/dead-air/SAVE_COMPATIBILITY.md, "What a module brings");
-- feature-test with xms.saves_api.
xms.saves_api = 1
xms.object_owner = xms_native_object_owner     -- (id) -> the module an ALife object is of | nil
xms.own_object = xms_native_own_object         -- (id, module id) -> true: the object is the module's, saved with it
xms.module_present = xms_native_module_present -- (module id) -> enabled and loaded now
-- (section, config path under configs | nil for the system config) -> the module that made it | nil
function xms.section_owner(section, file) return xms_native_section_owner(section or "", file or "") end
xms.kept_modules = xms_native_kept_modules     -- () -> { ids of the absent modules this game keeps content of }
xms.forgotten = xms_native_forgotten           -- () -> { ids xms_forget dropped since the game was loaded }
local forget_handlers = {}
-- fn(module id) runs when xms_forget drops what the game keeps of an absent module
function xms.on_forget(fn) forget_handlers[#forget_handlers + 1] = fn end
function xms.dispatch_forget(id)
    for i = 1, #forget_handlers do
        local ok, err = pcall(forget_handlers[i], id)
        if not ok then xms.log("! xms.on_forget: " .. tostring(err)) end
    end
end

-- The streamer mode and the radio's music of the modules (docs/dead-air/MODDING.md, "Streamer mode",
-- "Screens and music"; docs/dead-air/RADIO.md); feature-test with xms.sound_api.
xms.sound_api = 1
xms.streamer_mode = xms_native_streamer_mode           -- () -> true while snd_streamer_mode is on
xms.sound_copyrighted = xms_native_sound_copyrighted   -- (sound name) -> true when it is copyrighted
xms.sound_replacement = xms_native_sound_replacement   -- (sound name) -> what plays in its place in the mode | nil
xms.radio = xms_native_radio                           -- () -> { only, tracks = {...}, playlists = {...} }

-- Console commands of the modules (docs/dead-air/MODDING.md, "Console commands"); feature-test
-- with xms.commands_api. mod.ltx [commands] name = <script>.<function>: the engine calls
-- function(args, module, command) - args the text after the name - and prints a returned string.
xms.commands_api = 1
-- 'a "b c" d' -> { "a", "b c", "d" }: words split at spaces, double quotes keep a phrase together
function xms.command_args(text)
    local out = {}
    text = tostring(text or "")
    local i, n = 1, #text
    while i <= n do
        local c = string.sub(text, i, i)
        if string.find(c, "%s") then
            i = i + 1
        elseif c == '"' then
            local close = string.find(text, '"', i + 1, true) or (n + 1)
            out[#out + 1] = string.sub(text, i + 1, close - 1)
            i = close + 1
        else
            local stop = string.find(text, '[%s"]', i) or (n + 1)
            out[#out + 1] = string.sub(text, i, stop - 1)
            i = stop
        end
    end
    return out
end

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

-- ---- the modes modules declare ----------------------------------------------
-- [provides_mode] in a manifest is all it takes for the new-game screen to offer a mode.
function xms.offer_modes()
    local known = xms.known_modes()
    if #known == 0 then return end
    local screen = dead_air_x64_mode_select
    if not (screen and screen.add_mode) then
        xms.log("! xms modes: the new-game screen has no list of module modes")
        return
    end
    for i = 1, #known do screen.add_mode(known[i].id, known[i].caption) end
    xms.log("module mode(s) offered on the new-game screen: " .. #known)
end
local ok_modes, modes_err = pcall(xms.offer_modes)
if not ok_modes then xms.log("! xms.offer_modes: " .. tostring(modes_err)) end

-- ---- module registrars ------------------------------------------------------
-- A module ships scripts\register.script to run something before anything else
-- asks for it - registering a scheme, wrapping a function, whatever. Loaded here,
-- in load order, once the whole api above exists: without this a module's scripts
-- sit on disk with nobody to require them.
--
-- A registrar runs once per SCRIPT ENGINE, not once per process: the engine
-- rebuilds the Lua state for a new game, a save load and every level change,
-- and each of those states needs its own wrappers. Registrars must therefore
-- be idempotent and must not read the active mode set - on the load path the
-- state is rebuilt before the save's modes are restored.
function xms.load_registrars()
    for _, m in ipairs(xms.modules() or {}) do
        if m.enabled then
            local source = xms_native_read_script(m.id, "register")
            if source and source ~= "" then
                xms.log("registrar: " .. m.id .. "/register")
                xms.require(m.id, "register")
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
    XmsNet::Register(luaState);
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
        def("xms_native_patrol_point", &xms_native_patrol_point),
        def("xms_native_recompose_spawns", &xms_native_recompose_spawns),
        def("xms_native_list_files", &xms_native_list_files),
        def("xms_native_read_file", &xms_native_read_file),
        def("xms_native_module_applies", &xms_native_module_applies),
        def("xms_native_dialog_register", &xms_native_dialog_register),
        def("xms_native_dialog_unregister", &xms_native_dialog_unregister),
        def("xms_native_dialog_invalidate", &xms_native_dialog_invalidate),
        def("xms_native_object_owner", &xms_native_object_owner),
        def("xms_native_own_object", &xms_native_own_object),
        def("xms_native_module_present", &xms_native_module_present),
        def("xms_native_section_owner", &xms_native_section_owner),
        def("xms_native_kept_modules", &xms_native_kept_modules),
        def("xms_native_forgotten", &xms_native_forgotten),
        def("xms_native_streamer_mode", &xms_native_streamer_mode),
        def("xms_native_sound_copyrighted", &xms_native_sound_copyrighted),
        def("xms_native_sound_replacement", &xms_native_sound_replacement),
        def("xms_native_radio", &xms_native_radio)
    ];
    XmsWorld::ScriptRegister(luaState);
    XmsPda::ScriptRegister(luaState);

    // defer the bootstrap to the end of CScriptEngine::init - the base Lua
    // libraries are not open yet while export_all is running
    CScriptEngine::g_xms_on_init = &run_bootstrap;
}

// ---- module console commands -------------------------------------------------

namespace XmsGame
{
namespace
{
// The console looks a name up as typed and ends it at the first space.
bool valid_command_name(const xr_string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

bool lua_identifier(std::string_view word)
{
    if (word.empty() || (word[0] >= '0' && word[0] <= '9'))
        return false;
    for (const char c : word)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

// <script>.<function>, exactly two names
bool valid_command_function(const xr_string& spec)
{
    const size_t dot = spec.find('.');
    if (dot == xr_string::npos || spec.size() > 128)
        return false;
    const std::string_view view(spec.data(), spec.size());
    return lua_identifier(view.substr(0, dot)) && lua_identifier(view.substr(dot + 1));
}

// The console draws Windows-1251. A module's text is UTF-8 unless it was written in the game's
// own code page, which is passed as it is - the Mods window's rule.
xr_string console_text(std::string_view text)
{
    const int length = static_cast<int>(text.size());
    const int wide_length = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0) : 0;
    if (wide_length <= 0)
        return xr_string(text.data(), text.size());
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), wide_length);
    const int ansi_length = WideCharToMultiByte(1251, 0, wide.data(), wide_length, nullptr, 0, "?", nullptr);
    if (ansi_length <= 0)
        return xr_string(text.data(), text.size());
    xr_string ansi(static_cast<size_t>(ansi_length), '\0');
    WideCharToMultiByte(1251, 0, wide.data(), wide_length, ansi.data(), ansi_length, "?", nullptr);
    return ansi;
}

// One log line per line of the text, each with `prefix` in front.
void log_lines(pcstr prefix, std::string_view text)
{
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        xr_string line(prefix);
        for (const char c : text.substr(start, end - start))
        {
            if (c == '\t')
                line += "    ";
            else if (c != '\r')
                line += c;
        }
        if (end < text.size() || line.size() > xr_strlen(prefix))
            Log(line.c_str());
        start = end + 1;
    }
}

// Dead Air's debug mode: _g.script sets DEV_DEBUG from -dbg, the debug menu switches it.
bool cheats_allowed(lua_State* L)
{
    lua_pushstring(L, "DEV_DEBUG");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool on = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return on;
}

// lua_pcall's message handler: the error with the Lua stack under it.
int command_traceback(lua_State* L)
{
    lua_pushstring(L, "debug");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1))
    {
        lua_pushstring(L, "traceback");
        lua_rawget(L, -2);
        if (lua_isfunction(L, -1))
        {
            lua_pushvalue(L, 1);
            lua_pushinteger(L, 2);
            lua_call(L, 2, 1);
            return 1;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 1;
}

// Runs under lua_pcall with [function, args, module, command] on the stack. The script table
// comes through the globals, whose autoload loads <script>.script on first use; whatever is
// not a function of that script is an error here, never a fatal in the engine's own lookup.
int command_call(lua_State* L)
{
    pcstr spec = lua_tostring(L, 1);
    pcstr dot = strchr(spec, '.');
    lua_pushlstring(L, spec, static_cast<size_t>(dot - spec));
    lua_pushvalue(L, -1);
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1))
        return luaL_error(L, "there is no script %s", lua_tostring(L, -2));
    // raw: a script's environment falls back to the globals, a typo must not call one of those
    lua_pushstring(L, dot + 1);
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "%s is not a function", spec);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 4);
    lua_call(L, 3, 1);
    return 1;
}

class CCC_ModuleCommand final : public IConsole_Command
{
public:
    CCC_ModuleCommand(const shared_str& name, const XMS::Module& owner, const XMS::ModuleCommand& command)
        : IConsole_Command(name.c_str()), m_name(name), m_module(owner.id), m_function(command.function),
          m_help(command.help), m_args(command.args), m_cheat(command.cheat)
    {
        bEmptyArgsHandled = true;
        bLowerCaseArgs = false;
    }

    const xr_string& Owner() const { return m_module; }

    void Execute(pcstr args) override
    {
        CScriptEngine* engine = GEnv.ScriptEngine;
        lua_State* L = engine ? engine->lua() : nullptr;
        if (!L)
        {
            Msg("! %s: the scripts are not loaded yet", Name());
            return;
        }
        if (m_cheat && !cheats_allowed(L))
        {
            Msg("! %s: a cheat of module [%s] - it runs only in debug mode (start the game with -dbg)", Name(),
                m_module.c_str());
            return;
        }
        // a command that runs itself through the console again stops here, not at the end of the C stack
        static u32 depth = 0;
        if (depth >= 8)
        {
            Msg("! %s: module commands nest too deep", Name());
            return;
        }
        ++depth;
        // what the module's script does is the module's, not the player's (MOD_CATALOG 9.2)
        XMS::Protect::OriginScope origin(XMS::Protect::Origin::Script);
        const int top = lua_gettop(L);
        try
        {
            lua_pushcfunction(L, command_traceback);
            lua_pushcfunction(L, command_call);
            lua_pushstring(L, m_function.c_str());
            lua_pushstring(L, args ? args : "");
            lua_pushstring(L, m_module.c_str());
            lua_pushstring(L, Name());
            if (0 != lua_pcall(L, 4, 1, top + 1))
            {
                pcstr error = lua_tostring(L, -1);
                const xr_string text = console_text(error ? error : "the script raised an error that is not text");
                Msg("! %s (module [%s], %s) failed:", Name(), m_module.c_str(), m_function.c_str());
                log_lines("!   ", text);
            }
            else if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)
            {
                size_t length = 0;
                pcstr result = lua_tolstring(L, -1, &length);
                log_lines("", console_text({result, length}));
            }
        }
        catch (...)
        {
            Msg("! %s (module [%s], %s) failed in the engine", Name(), m_module.c_str(), m_function.c_str());
        }
        lua_settop(L, top);
        --depth;
    }

    void Info(TInfo& I) override
    {
        xr_string text = m_help;
        // a word with no space may be a string table id, the way every caption of the game is
        if (!text.empty() && text.size() <= 128 && text.find_first_of(" \t\n") == xr_string::npos &&
            StringTable().has_translation(text.c_str()))
        {
            const shared_str translated = StringTable().translate(text.c_str());
            text = translated.c_str() ? translated.c_str() : "";
        }
        else
            text = console_text(text);
        if (!m_args.empty())
            text += xr_string(text.empty() ? "" : "; ") + Name() + " " + console_text(m_args);
        text += (text.empty() ? "(module " : " (module ") + m_module + (m_cheat ? ", cheat: -dbg only)" : ")");
        strncpy_s(I, sizeof(I), text.c_str(), _TRUNCATE);
    }

    void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        if (!m_args.empty())
            tips.push_back(shared_str(console_text(m_args).c_str()));
    }

private:
    shared_str m_name; // owns the text cName points at
    xr_string m_module;
    xr_string m_function;
    xr_string m_help;
    xr_string m_args;
    bool m_cheat;
};

xr_vector<CCC_ModuleCommand*>& module_commands()
{
    static xr_vector<CCC_ModuleCommand*> commands;
    return commands;
}
} // namespace

void RegisterModuleCommands()
{
    if (!Console)
        return;
    xr_vector<CCC_ModuleCommand*>& registered = module_commands();
    bool ledger = false;
    for (const XMS::Module& m : XMS::Modules())
    {
        if (m.disabled)
            continue;
        xr_string added;
        for (const XMS::ModuleCommand& c : m.commands)
        {
            pcstr why = nullptr;
            xr_string first;
            if (!valid_command_name(c.name))
                why = "a command name is a-z, 0-9 and _ only";
            else if (c.function.empty())
                why = "its [command.<name>] section has no line in [commands]";
            else if (!valid_command_function(c.function))
                why = "[commands] takes <script>.<function>";
            else if (IConsole_Command* taken = Console->GetCommand(c.name.c_str()))
            {
                for (const CCC_ModuleCommand* own : registered)
                    if (own == taken)
                        first = own->Owner();
                why = first.empty() ? "the game has a console command of that name" : "a module earlier in the load order has it";
                XMS::AddConflict(XMS::ConflictKind::Name, ("console command " + c.name).c_str(), m.id.c_str(),
                    first.empty() ? "game" : first.c_str());
                ledger = true;
            }
            if (why)
            {
                Msg("! XMS: module [%s] console command [%s] refused - %s%s%s", m.id.c_str(), c.name.c_str(), why,
                    first.empty() ? "" : ": ", first.c_str());
                continue;
            }
            CCC_ModuleCommand* command = xr_new<CCC_ModuleCommand>(shared_str(c.name.c_str()), m, c);
            Console->AddCommand(command);
            registered.push_back(command);
            added += (added.empty() ? "" : ", ") + c.name;
        }
        if (!added.empty())
            Msg("* XMS: module [%s] adds console command(s): %s", m.id.c_str(), added.c_str());
    }
    if (ledger)
        XMS::WriteReport();
}

void UnregisterModuleCommands()
{
    // the console may be gone already; a command takes itself out of a live one
    for (CCC_ModuleCommand*& command : module_commands())
        xr_delete(command);
    module_commands().clear();
}
} // namespace XmsGame
