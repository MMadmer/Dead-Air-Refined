// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS game-side integration: Lua natives + bootstrap, per-module .scov
// persistence, savegame module manifest, spawn composer support.

#include "xrScriptEngine/ScriptExporter.hpp"
#include "save_extension_container.h"

class xms_registrator
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

namespace SaveExtensionGameplay
{
struct ChunkMutation;
}

namespace XmsGame
{
// FourCC "XMS1" - module set manifest chunk in .scov
inline constexpr u32 kManifestChunkId = 0x31534D58;
// per-module data chunks: "XM" prefix + module ns in the low word
inline constexpr u32 kModuleChunkBase = 0x584D0000;
// per-module applied-spawn ledgers: "XL" prefix + module ns in the low word.
// Separate from the data chunk so a mod's own xms.save_data never collides
// with the engine's bookkeeping.
inline constexpr u32 kLedgerChunkBase = 0x584C0000;
// the ALife objects of a module, kept out of .scop: "XO" prefix + module ns
// (SAVE_COMPATIBILITY.md, "What a module brings")
inline constexpr u32 kObjectChunkBase = 0x584F0000;
// what the game's own objects carry of a module's data - a loaded ammo type, an
// installed upgrade: "XE" prefix + module ns
inline constexpr u32 kStateChunkBase = 0x58450000;
// the module chunk families, by their high word
inline constexpr bool IsModuleFamily(u32 type)
{
    const u32 family = type & 0xFFFF0000;
    return family == kModuleChunkBase || family == kLedgerChunkBase || family == kObjectChunkBase ||
        family == kStateChunkBase;
}

// ---- per-module persistent blobs (Lua xms.save_data / xms.load_data) -------
void SetPendingBlob(u16 ns, const void* data, size_t size);
const xr_vector<u8>* GetLoadedBlob(u16 ns);

// Appends the manifest chunk + pending module blobs to the capture mutations.
void CollectSaveMutations(xr_vector<SaveExtensionGameplay::ChunkMutation>& mutations);

// Reads the manifest + module blobs from a freshly loaded .scov snapshot;
// clears state when the snapshot is empty. Reports module set differences.
void OnSnapshotLoaded(const SaveExtensionContainer::ChunkList& chunks);

// Publishes the game modes the player ticked on the new-game screen as the
// active XMS mode set, so a module built for one of them applies exactly
// there. Dead Air keeps that choice in [character_creation] of
// axr_options.ltx (new_game_<x>_mode = true) and nothing translated it, which
// left every mode-gated module inert. New games only: a loaded save restores
// its own set from the .scov manifest. Must run before the spawn composer,
// and unconditionally - the active set is process-global and would otherwise
// be inherited from the previous session.
void SyncModesFromNewGameOptions();

// Objects skipped by the save loader because their module/section is gone.
u32 SkippedObjectCount();
void ResetSkippedObjects();
void NoteSkippedObject(pcstr section);

// P5: composes the game graph blob with module levels appended. Returns the
// original reader when nothing to add, else a fresh reader over the composed
// blob (the original is closed). Called on the all.spawn chunk 4.
IReader* ComposeGameGraph(IReader* base_chunk);

// ---- late spawn composition -------------------------------------------------
// The composer already registers module spawn vertices on BOTH paths (a save
// load re-reads all.spawn through the same loader), so a module updated since
// the save was written has its new vertices in the registry - what a load
// does not do is create the objects. This walks every applying module's spawn
// id range, instantiates the vertices this playthrough never applied, and
// records them in a per-module ledger chunk in the .scov - so a prop the
// player destroyed stays destroyed instead of respawning on every load.
// Runs inside the can_register_objects(false) window of BOTH paths: on a new
// game everything is freshly spawned, so it only seeds the ledger.
// Returns how many objects were created.
u32 LateSpawnCompose(class CALifeSimulatorBase& sim);

// Re-runs the spawn composer and then the late pass. For the Lua-side mode
// restore on LEGACY saves: the manifestless save's modes become known only
// once the game world is up, which is long after spawns().load composed with
// an empty set. Exposed to Lua as xms_native_recompose_spawns().
u32 RecomposeAndLateSpawn(class CALifeSimulatorBase& sim);

// ---- module console commands ------------------------------------------------
// The [commands] of every enabled module become console commands that call the
// module's Lua function (MODDING.md, "Console commands"). Registered once every
// stock command exists, so a name already taken - by the game or by a module
// earlier in the load order - is refused with a log line instead of shadowing.
void RegisterModuleCommands();
void UnregisterModuleCommands();

// ---- the composed game graph ----------------------------------------------------
// Set by ComposeGameGraph on every spawn load: how many vertices the base graph has and
// which module added each level appended after them.
void NoteComposedGraph(u32 base_vertex_count, const xr_vector<std::pair<u8, xr_string>>& module_levels);
}

class CALifeSimulatorBase;
class CALifeObjectRegistry;
class CSE_ALifeDynamicObject;
class NET_Packet;
struct CGameTaskRegistry;

// What a module brings never enters the original game's save format: a module's ALife objects
// are written to the module's chunk of the .scov, an inert placeholder holds each id in .scop, and
// the placeholders carry the lineage that finds the .scov again after the save went through a game
// that does not know it. Contract: docs/dead-air/SAVE_COMPATIBILITY.md, "What a module brings".
namespace XmsSaves
{
// ---- the save -------------------------------------------------------------------
// Starts the object capture of a save that will carry this transaction id. Nothing is done for a
// game without modules and without content kept for an absent one: the registry then writes
// exactly what it always did.
void BeginCapture(u64 save_id);
// Called by the registry for every object it is about to write. A module's object goes to the
// module's chunk and a placeholder with its id to `out`: true. The game's own: false, and the
// registry writes it.
bool CaptureObject(const CALifeObjectRegistry& registry, CSE_ALifeDynamicObject& object, IWriter& out,
    u32& record_count, NET_Packet& packet);
// When nothing else carries the lineage, its anchor.
void FinishCapture(const CALifeObjectRegistry& registry, IWriter& out, u32& record_count, NET_Packet& packet);
// The .scov side: the object chunks, the kept set. False stops the save (a chunk over its limit).
bool CollectMutations(xr_vector<SaveExtensionGameplay::ChunkMutation>& mutations);

// ---- the load -------------------------------------------------------------------
// After the save's own .scov was read: when it is missing or stale, or holds nothing of a module,
// and the .scop carries a lineage, the module chunks of the .scov of that lineage are added to
// `chunks` - from the stale sidecar of the same name, or from another save of the folder.
void ResolveLineage(std::span<const u8> scop_source, pcstr sidecar_path, std::span<const u8> own_sidecar,
    bool own_valid, u64 own_save_id, SaveExtensionContainer::ChunkList& chunks);
// The manifests of the loaded .scov (XMS1, XMK1): which absent modules this game keeps.
void OnSnapshotLoaded(const SaveExtensionContainer::ChunkList& chunks);
// After the .scop objects are read and before they are registered: every placeholder is replaced
// by the object it stands for, stays for an absent module (it holds the id), or is removed.
void OnObjectsLoaded(CALifeObjectRegistry& registry);
// After the registries are read: the quest tasks of the present modules go back into the actor's
// task registry (they were kept out of .scop's).
void OnRegistriesLoaded(CGameTaskRegistry& tasks);
// The squads have registered their members: an NPC's squad is its own field again.
void OnLoadFinished();
// The actor's task registry as .scop gets it: without the present modules' quest tasks, which go
// to the modules' state chunks.
void SaveTaskRegistry(CGameTaskRegistry& tasks, IWriter& out);
// A new game keeps nothing.
void Reset();
// An object released for good forgets what it was declared.
void ForgetObject(u16 id);

// ---- scripts and the console ----------------------------------------------------
// The module an ALife object belongs to; null for the game's own. An id an absent module keeps
// answers that module.
pcstr ObjectOwner(u16 id);
bool OwnObject(u16 id, pcstr module_id);
// The module that made a section of a config: the system config without `file`, else a config read
// by name (a path under configs). Null for the game's own sections and for one a module only changed.
pcstr SectionOwner(pcstr section, pcstr file);
// xms_forget: drops what the game keeps of an absent module.
bool Forget(pcstr module_id, xr_string& answer);
void DescribeKept(xr_vector<xr_string>& lines);
void KeptModules(xr_vector<shared_str>& out);
const xr_vector<shared_str>& Forgotten();
}
