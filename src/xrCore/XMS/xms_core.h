#pragma once

// XMS - XFined Module System core.
// Module discovery, deterministic ordering, VFS overlay mounting with a
// conflict ledger, and the persistent ns/id registry.
// Design doc: docs/dead-air/XMS_ARCHITECTURE.md

#include "xrCore/xrCore.h"
#include "xrCommon/xr_vector.h"
#include "xrCore/xrstring.h"

namespace XMS
{
struct ProvidedMode
{
    xr_string id;    // [a-z0-9_.-]+
    xr_string title; // string-table id for the new-game checkbox
};

// One [vfs] / [redirects] manifest line. For [vfs] `from` is the VIRTUAL
// game path and `to` is the path inside the module (file or folder) that
// backs it. For [redirects] both sides are virtual: `from` is the retired
// name, `to` is where its content lives now.
struct PathMap
{
    xr_string from;
    xr_string to;
};

struct Module
{
    xr_string id;      // [a-z0-9_.-]+, unique
    xr_string name;    // human readable
    xr_string version; // free-form, informational
    xr_string root;    // absolute physical path, trailing delimiter
    xr_string mode;    // gate: content applies only when this mode is active ("" = always)
    xr_vector<ProvidedMode> provides_modes;
    xr_vector<xr_string> requires_ids;
    xr_vector<xr_string> after_ids;
    xr_vector<xr_string> before_ids;
    xr_vector<xr_string> conflict_ids;
    // [vfs]: free-form module layout - mounts a file or folder of the module
    // at an arbitrary virtual game path, on top of the gamedata/ mirror
    xr_vector<PathMap> vfs_maps;
    // [redirects]: retired virtual names resolving to the current ones, so a
    // rename inside the module never breaks outside references (saves, other
    // modules, base configs) - the UE redirector idea in manifest form
    xr_vector<PathMap> redirect_maps;
    u32 budget_spawns{256};
    u16 ns{0};        // stable namespace id from the persistent registry
    u16 layer{0};     // 1-based position in the final load order (0 = base game)
    bool disabled{false};
    xr_string disable_reason;
};

enum class ConflictKind : u8
{
    File,       // VFS path overridden by a later layer
    LtxSection, // section merged across layers
    LtxKey,     // key overridden inside a merged section
    XmlPatch,   // xml node touched by more than one module
    Script,     // script/module level collision
    Other,
};

struct ConflictRow
{
    ConflictKind kind{ConflictKind::Other};
    xr_string subject; // path / section / key / node
    xr_string loser;   // previous owner description
    xr_string winner;  // new owner description
};

// ---- lifecycle -------------------------------------------------------------

// Discover, order and mount every module. Called once from
// CLocatorAPI::_initialize after the fsgame paths are ready. Safe to call when
// no mods folder exists (becomes a no-op).
XRCORE_API void InitializeAndMount();

// True when at least one enabled module is mounted.
XRCORE_API bool Active();

// Modules in final load order (enabled and disabled; check Module::disabled).
XRCORE_API const xr_vector<Module>& Modules();

// Module lookup by id; null when unknown.
XRCORE_API const Module* FindModule(pcstr id);

// Stable hash of the enabled module set (ids + versions, order-sensitive).
// Folded into cache signatures that depend on the composition.
XRCORE_API u32 CompositionHash();

// ---- VFS overlay -----------------------------------------------------------

// Physical location for a registered overlay entry; null when the entry is a
// plain file. Key is the interned name pointer of CLocatorAPI::file::name.
XRCORE_API pcstr ResolvePhysical(pcstr registered_name);

// Re-registers overlays whose virtual path starts with the given prefix.
// Called by CLocatorAPI::rescan_path after it re-scanned loose files.
XRCORE_API void OnRescanPath(pcstr full_path);

// Layer of a physical path: module layer when the path is inside a module
// root, 0 otherwise.
XRCORE_API u16 LayerOfPath(pcstr physical_path);

// Switch a module off (or back on) for the next start. This is XMS's own
// answer to "activate / deactivate a mod": it writes the id to disabled.ltx
// next to the modules and nothing else - no file is copied anywhere, which is
// exactly what a mod manager doing the same job would get wrong. Modules mount
// while the file system comes up, so the change lands on the next launch.
// false + reason when there is no such module or the list cannot be written.
XRCORE_API bool SetModuleEnabled(pcstr id, bool enabled, xr_string& err);

// ---- ledger / report -------------------------------------------------------

XRCORE_API void AddConflict(ConflictKind kind, pcstr subject, pcstr loser, pcstr winner);
XRCORE_API const xr_vector<ConflictRow>& Conflicts();

struct OwnedFile
{
    xr_string vpath;     // virtual path as registered
    xr_string physical;  // where it actually comes from
    xr_string module_id; // owning module
};

// Overlay files whose virtual path contains `needle` (case-insensitive), in
// mount order - the last match for a path is the layer that currently wins.
// Backs the `xms_why` console diagnostic.
XRCORE_API void FindOwnedFiles(pcstr needle, xr_vector<OwnedFile>& out, size_t limit);

// Rewrites $app_data_root$\xms_report.json from the current state.
XRCORE_API void WriteReport();

// ---- persistent registry ---------------------------------------------------

// Grow-only spawn-id range of a module ([base, base+size)). Allocated on
// first request, persisted in $app_data_root$\xms_registry.ltx. Returns false
// when the id space is exhausted.
XRCORE_API bool SpawnRange(pcstr module_id, u32 size, u32& base);

// Grow-only local key allocation inside the module's spawn range. Returns
// false when the range is full.
XRCORE_API bool SpawnLocalId(pcstr module_id, pcstr key, u32& id);

// Adopt a range recorded in a savegame manifest (self-healing when the local
// registry file was deleted). Never shrinks an existing allocation.
XRCORE_API void AdoptSpawnRange(pcstr module_id, u32 base, u32 size);

// Persistent u8 level id for a module-added level (allocated from 128 up,
// stable across runs, recorded in savegame manifests). False when exhausted.
XRCORE_API bool LevelId(pcstr level_name, u8& id);
XRCORE_API void AdoptLevelId(pcstr level_name, u8 id);

// ---- game modes -------------------------------------------------------------

// Active mode set for the CURRENT game session. Set by the new-game UI (Lua)
// before starting, restored from the savegame manifest on load.
XRCORE_API void SetActiveModes(pcstr csv);
XRCORE_API pcstr ActiveModesCsv();
XRCORE_API bool ModeActive(pcstr mode_id);
// True when the module's content applies now (no mode gate, or gate active).
XRCORE_API bool ModuleApplies(const Module& m);

// ---- material resolution (set by the game once GMLib is alive) --------------

// name -> game material persistent id; returns u16(-1) when unknown.
using MaterialResolver = u16 (*)(pcstr material_name);
XRCORE_API void SetMaterialResolver(MaterialResolver resolver);
XRCORE_API u16 ResolveMaterial(pcstr material_name);

// ---- shared plumbing for other XMS consumers -------------------------------

struct IniRecord
{
    xr_string section;
    xr_string key;
    xr_string value;
};

// Plain physical-file ini reader (no CInifile, no VFS): sections + key=value,
// ';' comments, records in file order.
XRCORE_API bool ReadIniRecords(pcstr physical_path, xr_vector<IniRecord>& out);

// Non-recursive physical listing of dir+mask, name-sorted for determinism.
XRCORE_API void ListFiles(pcstr dir, pcstr mask, xr_vector<xr_string>& out);

// ---- ltx parse context -----------------------------------------------------

// Track the physical file currently parsed by CInifile so the merge policy
// can attribute sections to layers. Push/pop discipline, cheap.
XRCORE_API void PushLtxFile(pcstr physical_path);
XRCORE_API void PopLtxFile();
XRCORE_API u16 CurrentLtxLayer();
XRCORE_API bool LtxMergeEnabled();
XRCORE_API void SetLtxMergeEnabled(bool enabled);
}
