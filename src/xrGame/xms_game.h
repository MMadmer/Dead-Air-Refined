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

// ---- per-module persistent blobs (Lua xms.save_data / xms.load_data) -------
void SetPendingBlob(u16 ns, const void* data, size_t size);
const xr_vector<u8>* GetLoadedBlob(u16 ns);

// Appends the manifest chunk + pending module blobs to the capture mutations.
void CollectSaveMutations(xr_vector<SaveExtensionGameplay::ChunkMutation>& mutations);

// Reads the manifest + module blobs from a freshly loaded .scov snapshot;
// clears state when the snapshot is empty. Reports module set differences.
void OnSnapshotLoaded(const SaveExtensionContainer::ChunkList& chunks);

// Objects skipped by the save loader because their module/section is gone.
u32 SkippedObjectCount();
void ResetSkippedObjects();
void NoteSkippedObject(pcstr section);

// P5: composes the game graph blob with module levels appended. Returns the
// original reader when nothing to add, else a fresh reader over the composed
// blob (the original is closed). Called on the all.spawn chunk 4.
IReader* ComposeGameGraph(IReader* base_chunk);
}
