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
}
