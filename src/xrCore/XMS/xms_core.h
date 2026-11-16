// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS - eXtensible Module System core.
// Module discovery, deterministic ordering, VFS overlay mounting with a
// conflict ledger, and the persistent ns/id registry.
// Design doc: docs/dead-air/XMS_ARCHITECTURE.md

#include "xrCore/xrCore.h"
#include "xrCommon/xr_vector.h"
#include "xrCore/xrstring.h"

class CInifile;

namespace XMS
{
struct ProvidedMode
{
    xr_string id;    // [a-z0-9_.-]+
    xr_string title; // what the new-game screen lists it as: text or a string table id
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

// [world] of a manifest: what of the original game a mode keeps, where the player may
// go, and which transitions start closed. Applies only while the module applies
// (ModuleApplies) and only to NEW games - a save carries the world it was started with.
// Contract: docs/dead-air/MODDING.md, "World rules".
struct WorldRules
{
    enum Rule : u8
    {
        OriginalNpcs,   // story squads, garrisons, traders, leaders, base-spawn NPCs
        RandomStalkers, // stalker squads the simulation spawns at start and respawns
        Mutants,        // mutant squads the simulation spawns at start and respawns
        NpcQuests,      // jobs squad leaders hand out
        StoryQuests,    // tasks of named characters and the storylines
        Count,
    };
    bool declared{false};                 // the manifest has a [world] section
    bool keep[Count]{true, true, true, true, true};
    bool player_choice[Count]{};          // offered on the new-game screen
    xr_vector<xr_string> levels;          // where the rules apply; empty = everywhere
    xr_vector<xr_string> area;            // the only levels the player may reach; empty = anywhere
    xr_vector<xr_string> locked_levels;   // every transition ON these levels starts closed
    xr_vector<xr_string> locked;          // these transitions (level changer names) start closed

    // manifest key of a rule ("original_npcs", ...); null for an unknown index
    static pcstr Key(u32 rule)
    {
        constexpr pcstr keys[Count] = {"original_npcs", "random_stalkers", "mutants", "npc_quests", "story_quests"};
        return rule < Count ? keys[rule] : nullptr;
    }
    // rule index of a manifest key; Count when unknown
    static u32 FromKey(pcstr key)
    {
        for (u32 i = 0; key && i < Count; ++i)
            if (0 == _stricmp(key, Key(i)))
                return i;
        return Count;
    }
};

// [screens], [screen.<id>], [track.<id>], [playlist.<id>] and [radio] of a manifest: the module's loading and menu
// screens, each a stack of layers, their music and the radio's. Contract: docs/dead-air/MODDING.md, "Screens and
// music".
enum ScreenSurface : u8
{
    ScreenLoading,
    ScreenMenu,  // the main menu
    ScreenPause, // the same menu over a loaded level
    ScreenSurfaceCount,
};

// what of the loading screen's own interface a module's screen leaves out
enum ScreenHide : u32
{
    ScreenHideBar = 1 << 0,
    ScreenHidePercent = 1 << 1,
    ScreenHideStage = 1 << 2,
    ScreenHideTip = 1 << 3,
};

struct ScreenLayer
{
    enum Kind : u8
    {
        Empty,   // a layer nothing was put on yet: not drawn
        Texture, // a picture, a GIF, a video, a sequence
        Shader,  // a shader of the module's, a .s with its .ps
        Page,    // an HTML page, drawn once into a picture
    };
    enum Fit : u8
    {
        Cover,   // fills its place, the picture cut to its proportions
        Contain, // whole, inside its place
        Stretch, // fills its place, whatever its proportions
    };
    Kind kind{Empty};
    Fit fit{Cover};
    bool sound{true}; // a video's own sound, where the screen asks for it
    float opacity{1.f};
    float x{0.f}, y{0.f}, w{100.f}, h{100.f}; // the place on the screen, per cent of its width and height
    xr_string file; // inside the module, '\' separators
    xr_string name; // Texture: the texture name it is mounted under; Shader: the shader's name
    xr_string base; // Shader: the texture name it draws over; empty for none
    xr_string base_file; // what `base` is mounted from, inside the module
};

struct ScreenDecl
{
    enum Music : u8
    {
        MusicAny,    // whatever the surface's music is
        MusicNone,   // silence
        MusicVideo,  // a video layer's own sound (the loading screen only)
        MusicTracks, // one of the tracks named in `tracks`
    };
    xr_string id;
    u32 show{(1u << ScreenLoading) | (1u << ScreenMenu) | (1u << ScreenPause)};
    xr_vector<xr_string> levels; // where it may show; empty = everywhere
    xr_vector<xr_string> modes;  // the game modes it may show in; empty = every game
    float weight{1.f};
    Music music{MusicAny};
    xr_vector<xr_string> tracks;
    u32 hide{}; // ScreenHide
    xr_vector<ScreenLayer> layers; // bottom first
};

// A track's music is a file of the module (`file`), mounted under a sound name of its own, or a sound of the game
// (`sound`), played under its own name. Whether it is copyrighted is said by that sound's description alone.
struct TrackDecl
{
    xr_string id;
    xr_string file;  // inside the module, '\' separators; empty for a `sound` track
    xr_string sound; // a sound name, under sounds without the extension; wins over `file`
    xr_string name;  // the sound name the menus and the loading screen play: `sound`, or the mount of `file`
    u32 play{};      // the surfaces whose shuffle it joins; none: only where a screen names it
    bool radio{false}; // `play` says radio: the radios of the world shuffle it among their music
    float weight{1.f};
    float volume{1.f};
    xr_string radio_name; // the sound name the radio plays; empty when no radio plays it
    bool stray_rights{false}; // the manifest wrote copyrighted / streamer here, where they mean nothing
};

// [playlist.<id>] of a manifest: a station of the radio, the module's tracks in an order.
struct PlaylistDecl
{
    xr_string id;
    xr_string title;              // text or a string table id, as [module] name
    xr_vector<xr_string> tracks;  // track ids of the module
    bool shuffle{true};           // `order = shuffle`; false: `order = list`
};

struct ScreensRules
{
    // indexed by Loading = 0, Menu = 1: the pause menu is the menu
    bool only_screens[2]{};
    bool only_music[2]{};
    bool next_track[2]{}; // a track that ends gives way to another instead of starting again
    bool only_radio{false}; // [radio] music = only: this module's radio tracks are the radio's whole music
    xr_vector<ScreenDecl> screens;
    xr_vector<TrackDecl> tracks;
    xr_vector<PlaylistDecl> playlists;
    static u32 Side(ScreenSurface s) { return s == ScreenLoading ? 0 : 1; }
};

// One console command of a module: a [commands] line `name = <script>.<function>` and its
// [command.<name>] section. The game registers it once every stock command exists and calls
// the function with the text typed after the name. Contract: MODDING.md, "Console commands".
struct ModuleCommand
{
    xr_string name;     // as the manifest spells it; the game takes [a-z0-9_] only
    xr_string function; // <script>.<function>; empty when only [command.<name>] names it
    xr_string help;     // the console's help line; text or a string table id
    xr_string args;     // what the command takes, e.g. "<night 1-7>"
    bool cheat{false};  // runs only in Dead Air's debug mode (DEV_DEBUG, -dbg)
};

struct Module
{
    xr_string id;      // [a-z0-9_.-]+, unique
    xr_string name;    // human readable
    xr_string version; // 1-4 dot-separated numbers when the module takes updates, else free-form
    xr_string root;    // absolute physical path, trailing delimiter
    xr_string mode;    // gate: "" = ordinary game only (unless provides_modes), "*" = everywhere, else that mode
    // Mods menu metadata (docs/dead-air/MOD_UPDATES.md). Text is kept as the manifest
    // spells it - UTF-8, or Windows-1251 in a hand-written file - and may be a string table id.
    xr_string author;
    xr_string description; // '\n' separates lines
    xr_string website;       // empty unless it passed ValidWebsite
    xr_string update_github; // "owner/repo", empty unless it passed ValidGithubRepo
    // MOD_CATALOG 6: a DDS picture inside the module ('\' separators, never climbing out) and
    // the tags of the fixed vocabulary, as the manifest spells them
    xr_string thumbnail;
    xr_string tags;
    bool disable_game_updates{false}; // [module]: a last resort - the game neither updates itself nor takes bug reports
    bool legacy_root{false}; // lives in the "mods" folder JSGME manages, not in "modules"
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
    WorldRules world;
    ScreensRules screens;
    // [pda] layout: the module's PDA interface, a .pdaasset path relative to the module root
    // ('\' separators, never climbing out). Empty = the module leaves the PDA alone. Only the
    // last applying module that declares one is used (XmsPda); contract: MODDING.md "PDA layouts".
    xr_string pda_layout;
    // [commands]: the console commands the module adds, in manifest order
    xr_vector<ModuleCommand> commands;
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
// Whether the module is on for the next start: what SetModuleEnabled last wrote, not what this
// start mounted. Reads disabled.ltx - not for every frame.
XRCORE_API bool ModuleEnabledNextStart(pcstr id);

// ---- Mods menu metadata and staged updates ----------------------------------
// Contract: docs/dead-air/MOD_UPDATES.md.

// https:// on ap-pro.ru or moddb.com and nothing else. The allow-list is code on purpose:
// a manifest must not be able to send the player anywhere it likes.
XRCORE_API bool ValidWebsite(pcstr url);

// "owner/repo" in GitHub's own character set.
XRCORE_API bool ValidGithubRepo(pcstr repo);

// <game>\modules\.staged\ with a trailing delimiter; empty until the file system is up. A
// downloaded update is unpacked to <id>\payload\ under it and swapped in by the next start.
XRCORE_API xr_string StagedRoot();

// Arms the payload already unpacked under StagedRoot()\<id>\payload\: writes ready.ltx, the
// one file the next start looks for. Written last, so an interrupted download is never applied.
XRCORE_API bool MarkStagedReady(pcstr module_id, pcstr version, xr_string& err);

// The Mod Browser (MOD_CATALOG 14): a new module unpacked under StagedRoot()\<id>\payload\ is moved to
// modules\<id> by the next start; a removal takes the module folder away at the next start.
// StagedAction says which of update / install / remove waits for a module, empty for none.
XRCORE_API bool MarkStagedInstall(pcstr module_id, pcstr version, xr_string& err);

// Publishes a file that belongs to no module at a path under $game_data$ ("textures\\a\\b.dds"),
// so the engine opens it like game data - the Mod Browser's thumbnails. Main thread only. A path
// published again points at the new file.
XRCORE_API bool MountFile(pcstr relative_path, pcstr physical_path);
XRCORE_API bool StageRemoval(pcstr module_id, xr_string& err);
XRCORE_API xr_string StagedAction(pcstr module_id);

// Version of the update waiting for the next start; false when none is armed.
XRCORE_API bool StagedVersion(pcstr module_id, xr_string& version);

// Deletes StagedRoot()\<id>\, armed or not. Staging is only ever deleted through these two:
// the walk never follows a link, which a generic recursive delete does not promise.
XRCORE_API void DiscardStaged(pcstr module_id);

// Deletes every staging folder that was never armed - what an interrupted download leaves.
XRCORE_API void DiscardUnarmedStaged();

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

// Namespaces from here up are never handed to a module: the engine keeps its
// own .scov blobs there (see kCoreBlobs in xms_game.cpp) on the same
// 0x584D0000|ns channel the modules use.
inline constexpr u16 kReservedNsBase = 0xFF00;

// Grow-only spawn-id range of a module ([base, base+size)). Allocated on
// first request, persisted in $app_data_root$\xms_registry.ltx. A module that
// raises its budget grows in place while no other range follows it. Returns
// false when the id space is exhausted.
XRCORE_API bool SpawnRange(pcstr module_id, u32 size, u32& base);

// The range a module really holds - smaller than its budget when it could not
// grow. False when it has none yet.
XRCORE_API bool SpawnRangeOf(pcstr module_id, u32& base, u32& size);

// Grow-only local key allocation inside the module's spawn range. Returns
// false when the range is full.
XRCORE_API bool SpawnLocalId(pcstr module_id, pcstr key, u32& id);

// Adopt a range recorded in a savegame manifest (self-healing when the local
// registry file was deleted). Never shrinks an existing allocation, and takes
// only a range SpawnRangeValid passes.
XRCORE_API void AdoptSpawnRange(pcstr module_id, u32 base, u32 size);

// Whether a range a save names for a module can be that module's: inside the
// ids modules are given, and crossing no other module's range. A save is a
// file: what it says of ids is checked before objects are told apart by it.
XRCORE_API bool SpawnRangeValid(pcstr module_id, u32 base, u32 size);

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
// True when the module's level work applies to the CURRENT game:
//   mode = <id>  -> that mode is active
//   mode = *     -> always
//   no mode      -> the module provides a mode? that mode is active
//                   : the ordinary game (no active modes at all)
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

// The order a module's files are taken in, wherever the order decides something (two items that
// change one section, two patches of one key): the paths compared byte by byte with their ASCII
// letters lower-cased and '/' between folders - so `a\k` comes before `a2\k`, and `B` after `a`.
XRCORE_API bool PathOrderLess(const xr_string& left, const xr_string& right);

// Files under `dir` (trailing delimiter) whose NAME matches `mask` ('*' and
// '?', case-insensitive), optionally descending into subfolders. Results are
// paths relative to `dir` with '\' separators, in the order of PathOrderLess.
XRCORE_API void ListFilesRelative(pcstr dir, pcstr mask, bool recursive, xr_vector<xr_string>& out);

// ---- ltx parse context -----------------------------------------------------

// Track the physical file currently parsed by CInifile so the merge policy
// can attribute sections to layers. Push/pop discipline, cheap.
XRCORE_API void PushLtxFile(pcstr physical_path);
XRCORE_API void PopLtxFile();
XRCORE_API u16 CurrentLtxLayer();
// The file PushLtxFile named last, for a log line; "" when none. Valid while it is pushed.
XRCORE_API pcstr CurrentLtxFile();
// The enabled module at that layer; null for the base game.
XRCORE_API const Module* ModuleOfLayer(u16 layer);
// Who made a section of the system config: the config reader notes every section it creates
// there with the layer of the file or module doing it. A module that only changed or merged
// into a section the game has does not make it its own. Null for the game's sections.
XRCORE_API void NoteSectionCreated(const CInifile& ini, const shared_str& section, u16 layer);
XRCORE_API const Module* ModuleOfSection(const shared_str& section);
// The same for a config read by name (a path under configs, e.g. "misc\\task_manager.ltx"): the
// module whose `#file` patch made the section there. Known once an ini of that file was read.
XRCORE_API void NoteFileSectionCreated(pcstr file, pcstr section, u16 layer);
XRCORE_API const Module* ModuleOfFileSection(pcstr file, pcstr section);
// A section of the game whose class a module changed counts as made by that module.
// The module that made the ammo_class (grenades: grenade_class) list of a section of the game
// longer than `index`; null when the index is within the list the game itself gives it, or the
// section is no section of the game. Kept from the config stage (xms_ltx.cpp).
XRCORE_API const Module* ListEntryModule(const shared_str& section, bool grenades, u32 index);
XRCORE_API bool LtxMergeEnabled();
XRCORE_API void SetLtxMergeEnabled(bool enabled);
}
