# XMS — eXtensible Module System

Modular modding architecture for Dead Air: Refined (x64).

Status: **P0–P4 implemented in the engine** (2026-08-11), P5–P6 are design. All engine references follow the current `src/` tree.

## Implementation status

| Phase | State | Where in code |
|---|---|---|
| P0 registry/order/mounting/ledger | implemented | `src/xrCore/XMS/xms_core.{h,cpp}`, hooks in `LocatorAPI.cpp` (`xms_register`, redirect in `r_open_impl`, remount in `rescan_path`, initialization in `_initialize`) |
| P1 LTX merge + `.ltxp` | implemented | merge in `Xr_ini.cpp` (`insert_section`/`xms_merge_section`, layers in `Sect`), patches in `XMS/xms_ltx.cpp` (read by `XMS/xms_ltxp.cpp` into directives), invoked from `x_ray.cpp::InitSettings` after the game's own overlays. A module's change of a key reaches the sections that inherit it (`Sect::XmsParents` / `XmsOwn`, the rule in `XMS/xms_inherit.h`, the child index made at the first change); `#file <path>` patches are kept by file and applied by every read-only `CInifile` read from files under `$game_config$` that read that file (itself or an `#include`), once it is whole (`CInifile::xms_apply_file_patches`; `game.ltx` and the engine settings after the stage; a script's `ini_file` made writable is read as written). The ability `config-file-patches`. QA: `tools/qa/ltx/Run-LtxQa.ps1` (the reader and the rule), `tools/qa/items/Run-ItemsQa.ps1` (in game). Contract: MODDING.md, "Patches of any config" |
| Items | implemented | `*.itemasset` anywhere in an enabled module, read by `XMS/xms_items.cpp` in the config stage after the module's characters and before its overlays: new sections (`[section] : base`), changes of sections that exist, their strings (published with the characters' `PublishGenerated`, swept by `FinishCharacters`), icons, hud and other sections through the `CInifile` XMS calls at the module's layer; recipes into `[xms_craft]`; loot, stock, scenes and lists as `#file` patches. The ability `item-assets`. QA: as above. Contract: MODDING.md, "A module's items" |
| P1 XML `.xmlp` | implemented | `XMS/xms_xml.cpp`, hook in `XMLDocument::Load`; the mutation API added to the tinyxml fork (`Xms*` methods) |
| P1 layered string table | implemented | `StringTable.cpp` (sort by layer + `CompositionHash` in the cache signature) |
| P2 multicast callbacks | implemented | `script_callback_ex.h` (subscribers with a 0–1 fast path), `game_object:add_callback/remove_callback` in `script_game_object_*` |
| P2 xms Lua core | implemented | `src/xrGame/xms_game.cpp`: `xms_native_*` natives + the embedded bootstrap (`xms.require`, `xms.hook` pre/post/around, `xms.registry`, `xms.save_data/load_data`, `xms.story_id`); loose override via `gamedata\scripts\xms.script` |
| P3 saves | implemented | the `XMS1` manifest + per-mod chunks `0x584D0000|ns` in `.scov` (`xms_game.cpp`, hooks in `save_extension_gameplay.cpp` and `alife_storage_manager.cpp`), unknown-section skip in `alife_object_registry.cpp::get_object`, dangling-children pruning in `alife_storage_manager.cpp::load` |
| P4 spawn composer | implemented | `src/xrGame/xms_spawn_composer.cpp` (`CALifeSpawnRegistry::xms_compose`, invoked before `build_story_spawns`), persistent ns/ranges in `xms_registry.ltx` |
| Console/report | implemented | `xms_list`, `xms_conflicts`, `xms_why <fragment>` (who supplied a file/section/key: the overlay map + the ledger) in `console_commands.cpp`; `$app_data_root$\xms_report.json` |
| Module console commands | implemented | `[commands]` `name = <script>.<function>` + `[command.<name>]` help/args/cheat, parsed into `Module::commands` (`xms_core.cpp`); `XmsGame::RegisterModuleCommands` (`xms_game.cpp`, called last in `xrGameModule::initialize`) adds a `CCC_ModuleCommand` per command - a name the game or an earlier module has is refused with a log line - which calls the function under `lua_pcall` with a traceback handler as `(args, module, command)`, prints a returned string (UTF-8 converted to cp1251), refuses a `cheat` outside `DEV_DEBUG`, runs it under the Script origin of protected mode and stops self-nesting at 8; `xms.command_args` splits the text. QA: `Run-NqQa.ps1 -Scenario Commands`. Contract: MODDING.md, "Console commands" |
| Composed pSettings cache | deferred | an optimization, the format does not block it |
| Game modes | implemented | `Module::mode` gates the module's spawn/graph contribution, `mode=` on individual .xspawn ops; `[provides_mode]` declares a new mode; Lua `xms.set_modes/active_modes/mode_active/known_modes`; console `xms_modes`; active modes persist in the `.scov` manifest and are restored on load BEFORE composition. Config patches are not mode-gated (they are composed before the mode is chosen) — documented. `ModuleApplies` semantics: `mode=<id>` — the mode is active; **no `mode=` — the plain game only** (an empty active-mode set; a module with `[provides_mode]` is implicitly gated by its own mode); `mode=*` — everywhere |
| P5 composite game.graph | implemented (runtime) | `src/xrGame/xms_graph_composer.cpp`: the blob is rebuilt in memory on spawn load; module levels (`gamedata/levels/<name>/level.ai`) get an auto fragment (vertex sampling on a ~32 m grid, k=4 edges, a dense cross-table), a persistent u8 id (128+), `graph_links.ltx` stitching by coordinates; byte offsets of base vertices are recomputed, base indices and the GUID stay unchanged — saves survive |
| P6 collision | implemented | the module's `overlay.xcform` is merged into the static CDB in `CObjectSpace::Load` BEFORE the tree is built: overlay triangles go to the end; materials by name through the GMLib resolver; the cache is keyed by `crc ^ hash(overlays)`; the embedded OPCODE tree is ignored when overlays are present |
| P6 cutting | implemented | XCF1 **v2** carries a cut-box list after the triangles: a base triangle fully inside a box is dropped BEFORE `Create()`. Indices after it shift — that is safe, because a triangle index never survives level load (verified across all ~60 consumers of `rq_result.element`); the only serialization is the OPCODE tree in `objspace.bin` and the tail of `level.cform`, both already invalidated via `crc ^ hash` and `cacheStream = nullptr`. Vertices are NOT removed (that would require remapping `TRI::verts` for every consumer). Known consequences, documented: `level.ai` nodes are coordinate-based and do not disappear with the geometry; `detect_sector` casts a ray into collision, so a cut floor breaks sector detection |
| P6 exact cuts and AI edits | implemented | section 8.4. `levels\<level>\overlay.cut` (`XCU1`): regions that clip the base level's static collision in `CObjectSpace::Load` before the tree is built (new vertices appended after the overlays', the cache keyed by `crc ^ hash(overlays) ^ hash(cuts)`) and the level's own normal and progressive visuals in `CRender::ApplyLevelCuts` (`r2_loader.cpp`, R4, R5 and R6: buffers of their own, read again from `level.geom`); `levels\<level>\overlay.aiedit` (`XAE1`): nodes moved and closed in place in `CLevelGraph::xms_apply_ai_edits`. One reader and one clip in `src/xrCore/XMS/xms_level_edits.*` and `xms_cut_clip.*`. The abilities `level-cuts` and `ai-edits`. QA of the clip: `tools/qa/cuts/Run-CutClipQa.ps1` |
| P6 AI map | implemented (experimental) | `overlay.aimap` appends NodeCompressed13 nodes to a heap copy of the array; the columnar index honestly falls back to a full scan with a warning when sorting is violated; the cross-table clamps on OOB nodes. Limitation: packed positions must land on the level's base grid |
| P6 level pieces | implemented | `<module>\levels\<level>\pieces\<name>\` (section 8.3): a compiled, lightmapped part of the level's picture with its own shader table, buffers and lightmap pages, hung on the level's sectors and streamed by distance; `CRender::LoadPieces` / `PiecesFrame` / `UnloadPieces` (`r2_loader.cpp`), the same in R4, R5 and R6. The ability `level-pieces` (`ProductVersion.h`). QA: `tools/qa/pieces/Run-PiecesQa.ps1` |
| P6 visuals | implemented | `<module>\levels\<level>\overlay_visuals.ltx` lists world `.ogf` files (editor export); `CRender::LoadOverlayVisuals` (`r2_loader.cpp`) loads them physically (bypassing the VFS — otherwise last-writer-wins would kill every module's contribution but the last), detects the sector by the visual's center and push_backs into the root's `children`; the root's box grows **only** when the overlay actually exceeds it (otherwise the sector's base geometry loses fcvInside); `UnloadOverlayVisuals` detaches and frees on `level_Unload`. Section keys: `file` (required), `mode` (an extra mode gate), `sector` (a forced id). Skeletons/dynamics are rejected. For small things the alternative is a regular spawn (P4) |
| Asset descriptions | implemented | `gamedata\textures\**\*.texasset`, `gamedata\sounds\**\*.sndasset`, `gamedata\meshes\**\*.meshasset` of every enabled module, plain Lua data read by `src/xrCore/XMS/xms_luadata.*` (the characters' reader too) into the registry of `xms_assets.*` when the modules are mounted (`* [assets] ...`). Applied by `CTextureDescrMngr::ApplyDescriptions` over the `.thm` files, `CSoundRender_Source::LoadWave` over the `.ogg` comment, and the model's first load: surfaces in `dxRender_Visual::Load`, bones and user data in `CKinematics::Load`, the model's own motions in `CKinematicsAnimated::describe_motions`. The version rule of every asset reader (`UpgradeLuaData`; the PDA's `upgrade` in `xms_pda_layout.script`). The ability `asset-descriptions`. Contract: MODDING.md, "A module's models, textures and sounds". QA of the reader: `tools/qa/luadata/Run-LuaDataQa.ps1` |

Runtime implementation notes:
- Modules live in `<game root>\modules\<id>\` — their OWN folder, not `mods\`. `mods\` is the JSGME folder (the same as `MODS`, case-insensitively): JSGME shows every subfolder there as a mod of its own and "activation" copies it over the game — exactly the merge the module exists to avoid, plus the content applies twice. `mods\` keeps being read (what is already installed must not break), but every module found there logs a warning, and a `mod.ltx` in the game root (the trace of an already performed JSGME install) is reported separately.
- Enabling/disabling a module is its own mechanism, not a mod manager's: `xms_disable <id>` / `xms_enable <id>` write the id into `modules\disabled.ltx` (one per line, hand-editable) and copy NOTHING anywhere. Takes effect on the next launch (modules mount during FS initialization).
- The kill switch: `-no_xms` on the command line.
- Module layout: `mod.ltx` + `gamedata\` (the classic 1:1 overlay) + `patch\*.ltxp|*.xmlp` + `spawn\*.xspawn` + `scripts\` (namespaced, via `xms.require`).
- New config sections arrive through `gamedata\configs\xms\*.ltx` (parsed into pSettings on top of the base with merge semantics).
- `.xspawn` coordinates are level-local; `game_vertex = auto` takes the level's nearest game vertex (the authoring tool will stamp exact ids on export in the future).
- Module spawns materialize immediately on a new game; on an existing save — from the next spawn-logic cycle (emission), or the module itself calls `alife():create` in an `on_install` hook.

---

## 0. The problem

1. The old DA modding pipelines (JSGME layers in `gamedata\`, `xtra_*.xdb0` in `database\`) keep
   working **as is**, without a single change on the modder's side. One name shape in `database\`
   is reserved since content bundles shipped: an archive matching
   `xtra_dead_air_x64_content_<group>_<NN>_<16 hex>.xdb0` mounts only when the installed content
   manifest declares it at that exact size (`src/xrCore/Content/ContentPin.cpp`). Every other
   archive name is untouched; the grammar and its consequences are in `MODDING.md`.
2. On top of them — a new mod format that can do what the classics cannot: two mods edit
   one config section, one script, one level, one spawn — and **do not clobber each other**.
3. No "monoliths that must be rewritten whole" (`all.spawn`, `game.graph`, `level.cform`,
   `xr_logic.script`). Everything a mod changes is **an operation on a named entity**, not a file.
4. Features must be meaningful: every phase delivers a concrete visible result, not abstract
   "extensibility".

### Priorities (descending; disputed decisions resolve down this list)

1. **Runtime performance.** No XMS feature may cost a single cycle on the hot path.
   All work happens at composition time and the result is cached (§10).
2. **Save compatibility with the original DA** given the same mod set and the same mode (§11).
3. **Mod compatibility** — but only logical composition (§2.6). Two mods that contradict each
   other in meaning are the player's problem, not the engine's.

### Three compatibility modes

| Mode | What it is | Behavior |
|---|---|---|
| **Legacy** | a mod as a `gamedata\` folder or `xtra_*.xdb0` | exactly as today: the last layer wins per file (the one reserved bundle name shape aside, §0). Plus — now it is visible who overrode whom |
| **Module** | an XMS package (`mods/<id>/`) | additive composition, conflicts are declarative |
| **Export** | building a module into a flat `gamedata\` | for those playing on vanilla DA. Works when the mod uses no Grizzly-only runtime |

Scripts detect the engine via `if xms then ... end` — a mod can degrade itself.

---

## 1. What blocks us today (facts from the code)

Not guesses — the exact points where things break:

| Subsystem | What happens | Anchor |
|---|---|---|
| VFS | a second `Register()` of the same path silently overwrites the entry, no diagnostics | `src/xrCore/LocatorAPI.cpp:344-353` |
| VFS | archive mount order is lexicographic by file name, no priorities; the only precondition is the content gate, which refuses a bundle-shaped name the manifest does not declare | `src/xrCore/LocatorAPI.cpp:1050` (sort), `:753` (gate) |
| LTX | **a second `[section]` with the same name = Fatal**, the process dies | `src/xrCore/xr_ini.cpp:421` |
| LTX | key-over-key inside a section is silent last-wins, no log | `src/xrCore/xr_ini.cpp:392` |
| XML | one document = one file, no node merging, `#include "*.xml"` does not exist | `src/xrCore/XML/XMLDocument.cpp:69` |
| Lua | module name = file name = global table, the file resolves to a single VFS winner | `src/xrScriptEngine/script_engine.cpp:937` |
| Lua | `CScriptCallbackEx` holds **one** functor: a second `set_callback` silently drops the first | `src/xrScriptEngine/script_callback_ex.h:70` |
| Spawn | `all.spawn` is one file, positional chunks, spawn_id are absolute indices duplicated inside object packets | `src/xrGame/alife_spawn_registry.cpp:126` |
| Spawn | `header().guid()` is tied to the save: rebuild the spawn → "DELETE SAVED GAME" | `src/xrGame/alife_spawn_registry.cpp:132` |
| Graph | `game.graph` is a flat blob with byte offsets; +1 vertex = re-emit the whole file | `src/xrAICore/Navigation/game_graph_inline.h:301` |
| Graph | a level missing from the graph → `R_ASSERT` with no bypass | `src/xrAICore/AISpaceBase.cpp:42` |
| AI map | `level.ai` is mmap'ed and welded by GUIDs to the graph and the cross-table (3 asserts, no bypass) | `src/xrAICore/AISpaceBase.cpp:48-52` |
| Geometry | everything loads by fixed names under `$level$`, no overlays | `src/xrEngine/IGame_Level.cpp:103` |
| Collision | exactly one `CDB::MODEL Static`; `rq_result.element` is a raw index into it, ~57 places in the code | `src/xrCDB/xr_area.h:39`, `src/xrCDB/xr_collide_defs.h:104` |
| Save | `REGISTRY_CHUNK` is a positional concatenation without framing or ids | `src/xrGame/alife_registry_container.cpp:52` |
| Save | an unknown object section → `R_ASSERT2 "Can't create entity."` | `src/xrGame/alife_object_registry.cpp:374` |
| Everything | a mod registry, manifest, order, priority, provenance and a conflict report **do not exist at all** | — |

### What already works in our favor

This matters more than the problem list — half the foundation is already there:

* **Directory scan = a ready merge primitive.** `file_list` merges archives and loose files into one
  sorted set (`src/xrCore/LocatorAPI.cpp:1379`). DA's string table, weathers,
  `textures_descr`, UI styles and loose particles are already built on it.
* **`#include "dir\*.ltx"`** — the single additive config primitive, already present
  (`src/xrCore/xr_ini.cpp:525`).
* **The string table already merges N files into one map and logs overrides** (`StringTable.cpp:129-137`) —
  a ready model to generalize.
* **`.scov` (SaveExtensionContainer)** — an already additive, chunked, id-tagged,
  versioned save sidecar with per-chunk CRC that **skips** broken chunks and **preserves**
  chunks newer than it understands (`src/xrGame/save_extension_container.cpp`). A ready channel for
  per-mod state.
* **Runtime visual injection into a sector is already done** — `Load3DFluid`/`Unload3DFluid`
  (`src/Layers/xrRender_R2/r2_loader.cpp:498,542`) literally push_back a new visual into the children
  of the sector root.
* **`CObjectSpace::Create` builds the CDB from arrays (verts, tris, hdr)** and can read from an
  arbitrary `IReader` (`src/xrCDB/xr_area.cpp:183,217`) — a composite cform is mechanically
  possible without a new API.
* **Object records in the save are individually length-prefixed** (u16 spawn + u16 update) —
  skipping an unknown object is mechanically possible (`alife_object_registry.cpp:352`).
* **`spawn_item` — a runtime spawn by section name, non-asserting, already exposed to Lua**
  (`src/xrGame/alife_simulator_base.cpp:92`).
* **`append_path`** can already add an alias and scan a folder at runtime
  (`src/xrCore/LocatorAPI.cpp:2062`) — `fsgame.ltx` will not need editing.
* **`xrAI is absent from the repository.** We have no external spawn/graph compiler anyway —
  so composition logically belongs **in the engine at load time**, not in an offline tool. Not a
  crutch but a deliberate choice without an alternative.

---

## 2. Five principles

1. **Identity, not a file.** A mod describes changes to named entities: a section, a key,
   a string, an XML node, a spawn object, an AI-map node. The file is just transport.
2. **Folder scan instead of editing a shared list.** No mod ever edits a shared file
   (`system.ltx`, `all.spawn`, `[story_ids]`, `xr_logic.script`). Registration = drop your file
   into a scanned directory.
3. **Composition at load + cache.** The final world is assembled from the base and the layers in
   memory; the result is cached by the hash of the module set and their versions.
4. **Conflicts resolve deterministically, silently-but-into-the-report.** The later layer wins,
   the layer order is fully deterministic. No runtime arbitration, negotiation or load blocking.
   A mod does not have to "declare" anything.
5. **The base is untouchable.** The base game's `all.spawn`, `game.graph`, `level.*` are never
   rewritten. Layers on top only.
6. **Composition is logical only.** XMS must correctly combine non-contradictory changes
   (mod A changed an NPC's look, mod B put an item into its inventory — they combine). XMS is **not**
   obliged to resolve a semantic contradiction (mod A removed the NPC, mod B expects it alive): order
   wins, the fact goes into the report, from there it is the player's problem.

---

## 3. The module package

The actual layout and the author's reference is `MODDING.md`; here is the principle. A module
is a self-contained folder the game reads IN PLACE (the engine refuses writes into it
by code):

```
mods/<mod_id>/
  mod.ltx                     ; manifest
  gamedata/                   ; classic mirror overlay (a convention,
                              ;   not a requirement - see [vfs] below)
  gamedata/configs/xms/*.ltx  ; new LTX sections (merged into system.ltx)
  patch/*.ltxp                ; config directive patches            <- the primary path
  patch/**/*.xmlp             ; XML patches
  spawn/*.xspawn              ; additive spawn operations
  scripts/*.script            ; the module's namespace (xms.require)
  levels/<level>/             ; level overlays (xcform/aimap/visuals + registry)
  graph_links.ltx             ; composite game.graph links
```

Arbitrary structure: the manifest's `[vfs]` section publishes any module file or
folder under any virtual game path (on top of its own `gamedata/` mirror),
and `[redirects]` keeps old virtual names alive after renames inside the
module — the analog of UE redirectors. Syntax and resolution rules are in
`MODDING.md`.

### The `mod.ltx` manifest

Recognized sections: `[module]` (`id` `[a-z0-9_.-]`, `name`, `version`, `mode`,
and the Mods menu metadata `author`, `description`, `website`),
`[provides_mode]` (`id`/`title`), `[requires]`, `[conflicts]`,
`[order]` (`after`/`before`), `[budget]` (`spawns`, default 256), `[vfs]`,
`[redirects]`, `[update]` (`github`). The metadata keys, their text rules and
the update source are specified in `MOD_UPDATES.md`. Unknown keys are ignored by the engine but survive the authoring tool's
round-trip — format extensions are added as new keys only.

```ini
[module]
id            = madmer.better_cordon     ; [a-z0-9_.-], reverse-dns
name          = Better Cordon
version       = 1.2.0

[requires]
grizzly.core   = >=1.0

[order]
after         = someones.overhaul
before        = cosmetic.pack

[conflicts]
old.cordon_rework = *

[budget]
spawns        = 500

[vfs]
textures\wpn  = art\weapon_textures      ; arbitrary module structure

[redirects]
meshes\dynamics\old.ogf = meshes\dynamics\props\new.ogf
```

### Load order

Topological sort by `requires` + `after`/`before`; the tie-break is the user order from
`mods/order.ltx` (drag-n-drop in the authoring tool/launcher), then by `id`. Fully deterministic.
A cycle or a missing dependency → loading does not start, a report is shown.

### Namespace and the stable `ns`

Every module receives `ns : u16` from the persistent registry
`_appdata_/xms_registry.ltx` (assigned once, never reused within a profile;
the registry lives in `_appdata_`, not in `mods/`, because mod folders are
read-only to the game). `ns` ends up in the save — so it must be stable, not a
name hash that can collide.

Derived deterministically from `ns`:
* the `_SPAWN_ID` and `_OBJECT_ID` ranges (see §9);
* `story_id` / `spawn_story_id`: `(ns << 16) | local` — story id collisions vanish as a class;
* `.scov` chunk ids;
* prefixes of patrol path names and newly added sections.

---

## 4. Layer 1 — VFS: provenance and priority

**Changes:**

* Modules mount through the existing `append_path` (`LocatorAPI.cpp:2062`), not lines in
  `fsgame.ltx` — a duplicate alias there is fatal (`LocatorAPI.cpp:1190`), and it does not need touching.
* The `file` desc gains provenance: `owner_ns` + `priority` (the layer's position in load order).
* `CLocatorAPI::Register` (`LocatorAPI.cpp:320`): overwriting is allowed only by a layer with a
  higher priority; every overwrite goes into the **ledger** (path → layer list, winner).
* New API `FS.r_open_all(alias, path, out)` — return **all** layers of a path. Needed by every
  merge subsystem (configs, XML, scripts).
* Legacy layers (`gamedata\`, `xtra_*.xdb0`) get priorities by the current rules and participate in
  the ledger equally — so already at this phase the player sees which JSGME mod clobbered what. An
  archive the content gate refuses never mounts, so it never reaches the ledger either; it is
  reported through the content service instead (`dar_content_state`).
* Still to be done here: `unload_archive` (`LocatorAPI.cpp:821`) `break`s after the first file and
  leaves dangling entries pointing at a closed archive. It is **not** fixed. Content repair is
  written around that — a repaired bundle cannot be mounted into the running session, so repair
  always ends in a mandatory relaunch (`src/xrGame/ui/UIContentWnd.cpp`). Fixing the loop does not
  by itself license remounting content in place: the file index was built without those archives.

**What this gives immediately:** breaking nothing, the engine starts answering "why does my mod not
work" — the `xms_conflicts` console command and `xms_report.json`.

---

## 5. Layer 2 — Configs, XML, strings

### 5.1 LTX: merge instead of Fatal

`CInifile::insert_section` (`xr_ini.cpp:409`) receives a policy instead of the unconditional `Fatal`:

* **Same layer** → as before, Fatal (a genuine typo in the data; hiding it is wrong).
* **Different layers** → merge by keys: the section is extended, conflicting keys resolve by
  layer priority, every resolution is a ledger line. Loading is never blocked by this.

This is where "logical composition" lives: mod A edits `visual`, mod B edits `community`, both
changes arrive. If both edit `visual` — the later one wins, which is exactly the behavior the
player would get from JSGME anyway, only now with a report entry.

* **A section an item made** (`Sect::xms_item`) that the same module's overlay defines again is
  merged as if it came from another layer, the file's keys winning, with one log line; a second
  file of the module defining it again is the stop it always was.
* **The merge is a change like any other**: the keys it brings reach the sections that inherit
  them (5.2), and they become the section's own.

### 5.2 Directive patches `.ltxp`

Scanned from `<module>/patch/*.ltxp` of every enabled module (`xms_ltx.cpp`, flat, no recursion) after `pSettings` is composed. A mod edits one
field instead of dragging a copy of the section — so two mods changing different fields of one
weapon do not intersect at all.

```ini
[wpn_ak74]                       ; patch an existing section
cost          = 3000             ; set
+ammo_class   = ammo_5.45x39_ap  ; append to a list
-ammo_class   = ammo_5.45x39_fmj ; remove from a list
!immunities                      ; delete a key

[!wpn_obsolete]                  ; delete a section
[madmer.wpn_gold] : wpn_ak74     ; a new section with inheritance

#file misc\death_generic.ltx     ; what follows patches that config, wherever it is read
[item_count]
madmer_wpn_gold = 0, 1
#file                            ; back to the system config
```

The injection point is after `InitSettings` (`src/xrEngine/x_ray.cpp`), once the game's own overlays
(`dead_air_x64_ai.ltx`, `dead_air_x64_pda3d_items.ltx`, `dead_air_x64_animations.ltx`) are in, before
the first reader. `pSettings` stays `const` for the game; the patch pass works at composition time.
Per module, in the load order: its characters, its items (`*.itemasset`), its `gamedata\configs\xms\*.ltx`,
its `patch\*.ltxp`.

**A change reaches what inherits.** A section copies its parents' keys when it is read; on top of the copy
it remembers its parents (`Sect::XmsParents`) and the keys it says itself (`Sect::XmsOwn`: its own lines, a
merge into it, a directive on it, a key a directive took away), both only while modules are active and only
for a section with parents. Every XMS change of a key (`xms_set_line`, `xms_remove_line`, the list ops, a
merge, a section made with parents) recomputes that key in the sections naming the changed one as a parent,
through a child index (parent -> children) made at the first change: a child that does not say the key takes
it from its LAST parent that has it, or loses it when none has; a child whose value did not change stops the
walk (`XMS::PropagateKey`, `xms_inherit.h`). No ledger row is written for a child.

**`#file`.** The reader (`xms_ltxp.cpp`) tags every directive with the config it patches (its path under
`configs`, lower case, `\`). A directive of the system config - no `#file`, or a file the system config is
read from - is applied at once; any other is kept in a registry by file, in module order, with a ledger row
naming the file. Every `CInifile` read from files notes the files under `$game_config$` it reads (itself and
every `#include`, masks included) while modules may patch them; a read-only one applies the registry's
directives for those files once it is whole (in one pass, modules in load order, at each module's layer, with
the inheritance above). Never an ini opened for writing, never a string-made one; a script's `ini_file` keeps
a parse with the patches and one without in its cache and takes the latter when Lua makes it writable. With
no file patch registered nothing is noted and nothing is applied.

### 5.3 XML

`.xmlp` patches with node operations (a "path + attribute predicate" selector, not full XPath):
`append`, `set-attr`, `replace`, `remove`. The assembled document is fed through the already existing
`XMLDocument::Set` (parse from memory, `XMLDocument.cpp:183`); the assembly point is `ParseFile`
(`XMLDocument.cpp:69`), where a textual pre-pass with `#include` already happens.

### 5.4 String table

Already merges N files and logs overrides (`StringTable.cpp:129-137`). All that is needed: ordering by
layer priority instead of the alphabet + provenance in the ledger. An evening of work.

---

## 6. Layer 3 — Scripts

### 6.1 Module namespaces

The flat `$game_scripts$` stays for legacy 1:1. Module scripts live in `mods/<id>/scripts/` and
resolve separately: `xms.require("madmer.better_cordon", "logic")`, or through an extension of
`process_file_if_exists` (`script_engine.cpp:937`) with the `<mod>@<name>` syntax. The module name no
longer equals the global table name → two mods can each have their own `logic.script`.

### 6.2 Hooks instead of monkey-patching

```lua
xms.hook("xr_logic.pstor_load", my_fn, { mode = "post", priority = 100 })
xms.hook("bind_stalker.actor_binder.update", my_fn, { mode = "around" })
```

A wrapper goes on top of a function in a foreign namespace; `pre`/`post`/`around`, priority, owner.
The implementation is pure Lua over the registry; C++ is needed only for the loader. An undeclared
direct monkey-patch (reassigning a global function) is detected and lands in the ledger.

### 6.3 Multicast callbacks

`CScriptCallbackEx` (`script_callback_ex.h:70`) holds one functor — the place where a second mod
silently kills the first. A subscriber list `(mod id, priority, fn)` is added:

* `set_callback(type, fn)` — the legacy slot, semantics unchanged;
* `add_callback(type, fn, {id, priority})` / `remove_callback(type, id)` — new;
* the call order is deterministic, an exception in one subscriber does not take down the rest
  (today a crash inside a callback `clear()`s the slot — `script_callback_ex.h:118`).

### 6.4 Registries instead of shared tables

Logic schemes, effects, dialog conditions, the task manager, trade: instead of editing shared
scripts — an autoscan of `scripts/register/*.script` in every module and explicit registration:

```lua
xms.registry.effects:add("my_effect", fn)
xms.registry.schemes:add("mad_sleep", { on_activate = ..., on_update = ... })
```

Server entity classes already register additively (`object_factory:register`,
`object_factory_script.cpp:17`) — only name namespacing and duplicate checks are missing (today a
duplicate CLASS_ID is caught only in DEBUG, `object_factory_inline.h:118`).

> ⚠️ A separate landmine: the Lua `clsid.*` constants are **positional indices** into a sorted vector
> (`object_factory_script.cpp:78`). Any mod registering a new class shifts the values of all classes
> whose CLASS_ID sorts after it. Therefore: numeric clsids must be neither hardcoded nor saved. XMS
> must hand out clsids by name only, and the old numeric API is flagged as deprecated in the report.

---

## 7. Layer 4 — Spawn: additive layers

The main feature. The base `all.spawn` is **never touched**; its GUID remains the base GUID.

### 7.1 The `spawn/*.xspawn` operation format

```ini
[obj:madmer.guard_01]                       ; stable key = <mod_id>.<local>
op          = add
section     = stalker
level       = l01_escape
position    = 12.3, 0.5, -44.1
game_vertex = auto                          ; resolved by the composer from the position
custom_data = madmer.guard_01.ltx

[obj:base:l01_escape/esc_wolf]              ; a reference to a base object
op                = modify
character_profile = madmer_wolf

[obj:base:l01_escape/esc_trash_box_0021]
op = remove
```

**The stable key of a base object** = `<level>/<editor object name>` (`name_replace`),
not an index. The composer builds the name → `spawn_id` map once after the base loads.

### 7.2 The composer

Runs in memory on a **new game**, between `spawns().load()` and `build_root_spawns()`:

1. the base loads as today (`alife_spawn_registry.cpp:126`);
2. layers apply in module order:
   * `add` → `m_spawns.add_vertex` with an id from the module's range;
   * `modify` → an edit of the unpacked CSE;
   * `remove` → **physical vertex removal** plus dropping incoming/outgoing edges;
3. `build_root_spawns()` / `build_story_spawns()` recompute the derived indices themselves —
   appended vertices are visible automatically (`alife_spawn_registry.cpp:192,226`).

**Why `remove` is actual removal, not a "mark".** The spawn graph is an `AssociativeVector` whose key
IS the `_SPAWN_ID` (`graph_abstract_inline.h:228`), not an array position. Removing a vertex does not
shift anyone's ids — so tombstones are needed neither for integrity nor for saves. Nor would they
make sense: a marked-but-alive vertex permanently eats one of the 65535 spawn ids, sits in memory and
participates in the `build_root_spawns`/`fill_new_spawns` walks on every new game and every
emission. A disabled entity must disappear, not play dead.

Patrol paths already merge by name with a log (`patrol_path_storage.cpp:66`) — only namespacing is
missing. Artefact positions (chunk 2) are addressed by offsets inside anomaly packets, so the rule is
hard: **append only**, base anomaly offsets stay untouched.

### 7.3 Why this honestly works

Because the whole "spawn ↔ graph" linkage stays valid: we do not renumber base spawn_ids,
do not change game.graph, do not touch the GUIDs. New vertices are simply appended at the end.

---

### 7.4 The mode's world, the Player Start and module paths

Three more things ride the spawn layer and the manifest, all applied in memory
while a new game's spawn is composed (`XmsWorld`, `xms_world.cpp`):

- `mod.ltx [world]` - what the mode keeps of the original game. The composer
  removes base objects the rules do not keep BEFORE module ops run (so a
  module's own objects are never touched), and `xms_world.script` filters what
  the game's scripts spawn. Resolved once per new game into the engine blob
  `0x584DFF02` of the `.scov`: saves keep the world they began with.
- `op = start` - the module's Player Start: the actor spawns there, the faction
  jump of the new-game screen is dropped once; the first module's start wins,
  a second is a conflict.
- `[path:<name>]` - module patrol paths, registered with
  `CPatrolPathStorage::set_runtime_path` when the level loads; the level's own
  names are refused.

Level changers read the world on spawn and on load (`XmsWorld::Apply`), and
`CALifeUpdateManager::change_level` refuses a destination outside the playable
area. The contract is MODDING.md "World rules".

### 7.5 PDA layouts

`mod.ltx [pda] layout` names a module's own PDA interface, a `.pdaasset`
(`XMS::Module::pda_layout`, a path inside the module). `XmsPda::Owner()` picks the last
applying module that declares one (the others are conflicts). `CUIPdaWnd::Init` asks
`xms_pda.build` for the layout - `xms_nq_load.parse` reads it in the sandbox,
`xms_pda_layout.normalize` (pure Lua, shared with the authoring tool) turns it into a flat item
list - and `CUIPdaLayout` builds a window per item under `UIMainPdaFrame`, so the 3D
PDA's screen UV and the stock sections keep working unchanged. The map slot is a
second `CUIMapWnd` (`InitFromXml` from a generated description); map windows are kept
in a list, so a map location that goes away leaves the hint of each
(`MapLocationRelcaseAll`). Run-time vars ride the save as the engine blob `0x584DFF03`.
Nothing is composed or written: without a layout the PDA is the stock one. The contract
is MODDING.md "PDA layouts".

## 8. Layer 5 — World: new levels and geometry overlays

### 8.1 The composite `game.graph`

The on-disk format does not change — what changes is that `CGameGraph::Initialize`
(`game_graph_inline.h:11`) receives a **buffer synthesized in memory**, not the file's bytes.

A module with a new level ships `levels/<lvl>/level.graph_fragment`: the vertices, edges and
cross-table of one level. The composer:

* assigns a `level_id` (u8) from the persistent registry — it ends up in the save, so it is stable;
* shifts game vertex ids, fixes the **byte offsets** of edges and death points;
* concatenates the per-level cross-tables — they are already self-delimited by a u32 size prefix and
  are the only part of the format that concatenates naturally (`game_graph_inline.h:301`);
* recomputes the header and the composition GUID.

Cross-level transitions are declared **at home**, not in a shared file:

```ini
; mods/madmer.new_level/graph_links.ltx
connect = l01_escape:xms_gate_a <-> madmer_swamp:entry_north
```

The points are named game vertices in the fragment. Stitching = appending edges in the composer.
The three GUID asserts in `AISpaceBase::Load` (`AISpaceBase.cpp:48-52`) verify the fragment GUID in
composite mode, not the monolith's.

Level folder registration is already additive (`IGame_Persistent::Level_Scan`,
`IGame_Persistent.cpp:113`) — the graph was the only blocker.

### 8.2 The geometry overlay ("place a house in Cordon without touching Cordon")

Four independent pieces:
**Rendering** — done, `CRender::LoadOverlayVisuals/UnloadOverlayVisuals` (`r2_loader.cpp`), by the
`Load3DFluid` precedent. The `m_overlay_visuals` registry holds {root, visual} pairs: `level_Unload`
detaches the child before freeing, so the visual graph's destruction order does not matter. Overlays
do **not** enter `Visuals` — there is one owner, and it is the registry.

**Arbitrating the sector's `vis.box`**: child culling goes by the root's box
(`r__dsgraph_build.cpp:608`), and the box grows only when the overlay actually sticks out of it
(`contains` → `merge` + sphere recompute). Growing unconditionally is wrong: an inflated root box
robs the sector's **entire** base geometry of `fcvInside`, turning one test into a per-child walk.

The registry is read **physically**, from `<module>\levels\<level>\overlay_visuals.ltx`, not through
the VFS: mounting is last-writer-wins, and a shared virtual path would mean only the last module's
contribution is seen. Overlays must add up.

**Collision.** A second `CDB::MODEL` — **no**: `rq_result.element` is a raw index into
`GetStaticTris()`, and there are ~57 such places (`xr_collide_defs.h:104`). Instead — **rebuilding the
single static model on the fly** in `CObjectSpace::Create` (`xr_area.cpp:217`), which already builds
the CDB from arrays: base triangles keep their indices, overlay triangles are appended at the end.
Nothing "moves". Materials remap through the existing mechanism (`Level_load.cpp:233`), the `TRI`
sector comes from the host sector.

As a bonus this gives **physics for free**: the ODE tri-list does not own triangles, it walks
`ObjectSpace().GetStaticTris()` during collision (`PHWorld.cpp:66`, `dSortTriPrimitive.h:90`).

The cform cache (`xr_area.cpp:238`) is keyed by CRC — the key becomes `crc(base) ^ hash(overlays)`,
otherwise it goes stale; `prune_inactive_level_caches` learns to respect composite keys.

**Lighting.** The lightmap is bound to the level's shader table, therefore:
* the cheap path — an overlay on a self-contained OGF shader (the `OGF_TEXTURE` branch of
  `dxRender_Visual::Load`): dynamic light only, but it works immediately;
* the lit path — a **level piece** (8.3): the module's compiler bakes the piece with lightmap
  pages of its own, and the piece brings its own shader table naming them. The level's table is
  never appended to: a piece's visuals answer from the piece's tables alone.

**AI map.** `level.ai` is mmap'ed and welded by GUIDs (`level_graph.cpp:28`). An overlay needs a
composite node array in memory — the path already exists: `CLevelGraphManager` can own a heap array
during version conversion (`level_graph_manager.h:15`). Plus appending cross-table cells and
rebuilding the columnar index (`level_graph.cpp:48`, degrades softly when sorting is violated).

**Taking away.** The `[cut_N]` boxes of `overlay_visuals.ltx` and the XCF1 v2 box list drop whole
visuals and whole triangles; the exact way to take a part of the level away - the ground under a new
terrain - is `overlay.cut`, and the way to change the AI map's nodes under it is `overlay.aiedit`
(8.4).

**What degrades, documented:** HOM (new geometry can be occluded but does not occlude), SOM (sound
passes through), grass in slots outside the compiled grid (`DetailManager_CACHE.cpp:256` returns an
empty slot without crashing); wallmarks on overlays work (they go through the static CDB — and ours
is composite).

### 8.3 Level pieces

A piece is a compiled part of a level's static picture that the game loads beside the level: its
own visuals, vertex and index buffers, shader table and lightmap pages. It is made by a level's own
author (the level cut into streaming cells) or by a module for a level it does not own (a lit
building in Cordon). **Only the picture is a piece**: collision, the AI map, the game graph and the
spawn stay whole in the level's own files or in a module's `overlay.xcform`, `overlay.aimap` and
`.xspawn`, composed once at the load - nothing a save refers to streams. A piece brings no sectors or
portals: the engine's sector graph is per render context and static after the load, and `level.ltx`
keys on sector ids, so each root of a piece becomes a child of a sector root of the level, as an
overlay does.

The files, in `<module>\levels\<level>\pieces\<name>\`, read physically per module like
`overlay_visuals.ltx`, under the module's gate (`ModuleApplies`) and the piece's own `mode`:

```text
piece.ltx      [piece] version = 1
                       box = minx, miny, minz, maxx, maxy, maxz   streaming: its centre and radius
                       sector = auto | <sector id>                one sector for every root, or by each root
                       stream = true | false                      false: loaded with the level and kept
                       mode = <id>                                optional extra mode gate
piece          chunks as the level file has them: 1 header (hdrLEVEL, the compiler's version 14),
               2 shader table, 3 visuals; and 32 roots: u32 count, then per root u32 visual index,
               u16 sector (0xFFFF: the sector at the visual's centre), u16 reserved
piece.geom     chunks 9 vertex buffers, 10 index buffers, 11 sliding windows, as level.geom
piece.geomx    the fast (position only) buffers, as level.geomX; optional
<name>.dds     a texture the shader table names that lies in the folder is the piece's own
```

**Its own tables.** While a piece's visuals load, `getShader`, `getVisual`, `getVB_Format`, `getVB`,
`getIB` and `getSWI` answer from the piece's tables (`CRender::s_piece_loading`, a `thread_local`
set on the thread that loads them, for that time only): a dynamic model loading on another thread
meanwhile resolves against the level, and a piece's visual can never reach a level table, nor the
reverse. Before anything of a piece is made, every id its visuals name - the shader, the vertex and
index buffers and the ranges in them, the fast geometry, the sliding window, the children (an
earlier visual of the piece) - is checked against what the piece has, and so is everything a
visual's loader reads without looking: a visual that is drawn takes its geometry from the piece's
buffers and nowhere else, a tree has its whole definition, a LOD its eight faces, a hierarchy and a
LOD their children, and every sliding window - a progressive visual's own, its fast geometry's, a
tree's item of the piece's table - is one at least and lies inside that visual's indices and
vertices; the table of windows is no longer than its chunk can hold. A piece that fails is refused
with a `! [pieces]` line and the level goes on without it: a module is content from anywhere, and no
file of it may stop the game. Its visuals never enter `Visuals`, its shaders never the level's table.

**Its own textures.** A texture of the shader table whose file lies in the piece's folder is
registered under a name that keeps its first characters - the deferred blender tells a lightmap by
the third texture's name starting `lmap` - and adds `@piece<n>` (a counter: no other piece, and no
later load of the same piece, meets it). The texture loader resolves that name to the folder's file
before the season's folders, `$level$` and `$game_textures$` (`texture_resolve_name`, `texture_load`),
on whichever thread loads it. The textures go with the piece's shaders; they are never left to the
level's unload snapshot. A piece's own texture is described - its bump, its detail texture, its
material - as the texture before the `@piece` is (`TextureDescrManager`): a terrain a piece lights
keeps what the terrain's texture has.

**Attach and detach** happen at the frame's safe point, the top of `CRender::Calculate`, before any
view walks the sector roots: the root joins its sector root's children, and the root's box grows only
when the piece sticks out of it (as `AttachOverlayVisual`). The systems that keep the visual tree are
told the way `da_veg_apply_switch` tells them: the GPU-driven trees' registry and the ray tracing
scene let go before the change and gather the tree again after; the lamps' cached static shadow
slices are dropped (they hold what they drew, and a piece's trees by pointer); when the camera can see
the piece, the temporal history starts over. The clusters keep only the level's own
visuals and draw what the views list, so they need nothing. The vegetation's census (the procedural
shrubs and trees, the reeds, the trees' bake) is taken before any piece attaches: a piece's bushes and
trees draw as models.

**Streaming.** A manager looks at the distances every 8 frames: a piece is wanted while the camera is
nearer than the weather's far plane plus its radius plus 50 m, and leaves at 1.25 times that, after
5 seconds attached at least. The files are read, checked and the device buffers made on a task, one
piece at a time; the shaders, textures and visuals are made at the safe point, and the pieces that
were read are made together - when no other is waiting to be read, when eight wait, or after thirty
frames - because every change of the tree makes the trees' registry, the ray tracing scene and the
lamps' caches again: a far plane that moved is a few such changes, not one a frame. `stream = false` and the
console's `r__pieces_stream 0` load the pieces with the level (inside `level_Load`, after the
vegetation's census, before the GPU registry and the clusters) and keep them; the streamed ones the
camera can reach when the level starts are loaded in its first frame, all at once, so the level never
starts with a hole. `level_Unload` waits for a task in flight and frees every piece before the level's
own tables. A level without pieces pays one pointer test a frame.

The log: `* [pieces] <level>: N piece(s) found, M streamed`; `* [pieces] <module>/<piece>: attached,
V visual(s), R root(s), B buffer(s), T texture(s), task X ms, safe point Y ms`; `* [pieces]
<module>/<piece>: released`; `! [pieces] <module>/<piece>: <why>` for a piece refused.

What a piece does not have: sectors and portals of its own (its interior draws correctly without
them and only loses portal culling inside itself), lights, glows, HOM occluders, sound geometry,
details. The engine states the ability as `level-pieces` in its version resources.

### 8.4 Exact cuts and AI edits

A module that replaces a part of a level it does not own - new ground under a yard, a pit - takes
away exactly what it replaces, in two files of `<module>\levels\<level>\`. Both are read physically
from every applying module (`ModuleApplies`), in module order, like `overlay.xcform`; an older game
never looks for either name and loads the level as it is. One reader serves the collision, the
picture and the AI map (`src/xrCore/XMS/xms_level_edits.*`); a file it refuses - a magic or a
version it does not know (a newer one: "made for a newer game"), a count over its cap (4096
regions, a grid side of 4096, 2^20 edits, 64 MB a file), a value that is not a number, a region
without size, a grid under two by two, a normal that does not point up, bytes missing - is skipped
whole with one `! [cuts] <file>: <why>, refused` or `! [ai edits] <file>: <why>, refused` line. The
sizes are checked before anything is read.

```text
overlay.cut     "XCU1"  u32 version (1)  u32 region count
                region: u8 kind (0 everything, 1 ground)  u8[3] zero
                        f32 centre x, y, z   f32 half x, y, z   f32 yaw (radians about +Y)
                        kind 1: f32 tolerance (m)  u32 columns, rows
                                f32[columns * rows] the old ground's heights, row by row from local -z,
                                each row from local -x: sample (i, j) at
                                x' = -half.x + i * 2 half.x / (columns - 1), z' likewise with rows
overlay.aiedit  "XAE1"  u32 version (1)  u32 count
                edit:   f32 x, z   f32 old y   f32 new y   f32 normal x, y, z   u32 flags (1: closed)
```

**The clip** (`xms_cut_clip.cpp`, `XMS::Cut::ClipTriangle`) is normative; the authoring tool's
preview implements the same steps, in single precision, and `tools/qa/cuts/Run-CutClipQa.ps1`
holds four cases both sides share:

1. A vertex's local coordinates: with `d = p - centre`, `x' = d.x cos(yaw) - d.z sin(yaw)`,
   `z' = d.x sin(yaw) + d.z cos(yaw)` (the point turned by `-yaw` about +Y); its height stays the
   world `y`.
2. A triangle whose local XZ bounding box does not overlap `[-half.x, half.x] x [-half.z, half.z]`
   is kept whole.
3. The triangle, as a convex polygon P, is split by the lines `x' = -half.x`, `x' = +half.x`,
   `z' = -half.z`, `z' = +half.z`, in that order: walking P's edges, a vertex goes to the part on the
   rectangle's side when its distance to the line on that side is at least -1e-5 m (a point on the
   line counts as inside), else to the far part; an edge whose ends go to different parts adds the
   point at `t = da / (da - db)` (clamped to 0..1) to both. The far part is appended to the list
   `kept`; P becomes the near part. After the four lines P is what lies inside the rectangle.
4. If P has less than 1e-8 m² of area (in 3D), the triangle is kept whole and unchanged.
5. The test of P: kind 0 - every vertex of P has `centre.y - half.y <= y <= centre.y + half.y`;
   kind 1 - every vertex has `|y - H(x', z')| <= tolerance`, H bilinear over the grid (the sample
   coordinates clamped to the grid). If the test fails, the triangle is kept whole and unchanged.
6. Otherwise the triangle is replaced by the fan (from each polygon's first vertex) of every
   polygon of `kept` that has 1e-8 m² of area or more; with nothing kept it is gone. The winding is
   the triangle's. A new vertex is a linear combination of the triangle's three vertices - on an
   edge the interpolation of that edge's two, at a corner of the rectangle inside the triangle the
   combination of all three - and every attribute of it is the same combination.

The regions apply in order, each to what the earlier ones left, and a file's regions after the
earlier module's.

**The collision.** In `CObjectSpace::Load`, after the XCF1 v2 boxes and before the overlay
triangles are appended, every base triangle goes through the clip; the parts that stay keep the
material, the sector and the flags of their triangle, and their new vertices go to the end of the
vertex array, after the overlays' (whose triangles index it by offset). The overlay triangles are
never cut. As with the boxes, triangle indices after the first cut one shift, which is safe (P6
cutting); the cache key becomes `crc(base) ^ hash(overlays) ^ hash(cuts)`, the embedded tree is not
used, and a level without cuts keeps the key it had.

**The picture.** In `level_Load`, after the sectors and before the overlays and the pieces attach,
`CRender::ApplyLevelCuts` takes every visual of the level itself whose box a region touches: the
normal and the progressive ones that draw from the level's own buffers. Their vertices and indices
are read again from `level.geom` (the device keeps no copy), the triangles of the most detailed form
are clipped, and the visual gets a vertex and an index buffer of its own with what stays: a
progressive one keeps that form alone (one sliding window), neither keeps its fast geometry (the
shadows draw the full one). Every vertex element is interpolated by its declared type - floats,
half floats, bytes, shorts, unsigned shorts; normals, tangents and binormals renormalised; the
packed base texture coordinate of the level's formats (a short `TEXCOORD0` whose fractions are the
alpha of the tangent and the binormal) interpolated as the one number it is and packed again; the
10-bit packings take the nearest vertex's value. A visual left with no triangle is unlinked from its
parents as a hidden one is. A visual that would need more than 65535 vertices (or a progressive one
more than 65535 triangles) stays whole, counted in one `! [cuts]` line. Trees, LOD billboards,
hierarchies, a piece's and an overlay's visuals are never cut: a tree or a bush on the cut ground
stays, as its own object. The cluster hierarchy leaves a cut visual to the stock draw (it is no
longer in the level's buffers); the ray tracing scene reads its new buffers. The buffers are freed
after the visuals at `level_Unload`. R4, R5 and R6 share the loader.

**The AI map.** After the nodes are in memory - the level's and the appended `overlay.aimap` ones,
in a heap copy (the file's array may be mapped read-only; a converted older version is one already)
- every edit finds the node whose cell holds `(x, z)` (the node's packed xz) and whose height is
nearest `old y`, within 2 m (ties: the lowest id); it takes the new height (packed as the nodes are,
clamped into the level's box) and the plane of `normal` (`pvCompress`). With `closed` it is unlinked
from its four neighbours both ways (the link becomes the invalid one). An edit that finds no node is
skipped. No node is added, removed or reordered: the vertex ids saves keep, the cross table and the
game graph are untouched, and the xz order the nearest-vertex index needs holds.

**The details (grass)** need no code of their own: what grows where and how dense is the level's
`level.details`, but each item's height is found when its slot is decompressed, by a ray down the
slot's height range into the static collision (`CDetailManager::cache_Decompress`, the highest
triangle that is not passable). On cut ground the ray meets what the module put there
(`overlay.xcform`) - the grass follows the new ground - or, where the new ground leaves the slot's
height range or nothing is under the item, the item is not placed. Sectors are still detected by
rays into the collision (P6 cutting), HOM and SOM are the level's.

The log: `* [cuts] <level>: <n> region(s), <c> collision triangle(s) cut, <v> visual(s) cut` (the
renderer writes it, with what the collision counted), `* [ai edits] <level>: <n> node(s) moved, <c>
closed, <s> skipped` (a closed node counts as moved too). The engine states the abilities as
`level-cuts` and `ai-edits` in its version resources.

---

## 9. Layer 6 — Saves: install and remove mods without "delete your save"

1. **The mod-set manifest is a separate `.scov` chunk** listing
   `[ns, id, version, spawn_range, content_hash]`. **`.scop` is not touched at all**: the project
   runs a frozen contract (`SAVE_COMPATIBILITY.md`) under which `.scop` fields and order are
   immutable and all new state lives exclusively in `.scov`. Technically an extra top-level chunk in
   `.scop` would be safe (every `find_chunk` implementation jumps over foreign chunks via
   `advance(size)` — `FS_impl.h:65,102,186,257`), but the contract outweighs the technical
   possibility: the single extension channel is `.scov`.
2. **The spawn GUID check is rewritten.** Today `header().guid()` is compared with the stored one and
   any spawn change = "DELETE SAVED GAME" (`alife_spawn_registry.cpp:132`). It becomes: verify the
   **base** spawn's GUID (immutable by definition) and hand the module set to migration.
3. **Skipping unknown objects.** A record knows both lengths (u16/u16), so a skip is mechanically
   possible today — `get_object` (`alife_object_registry.cpp:352`) gets the path
   "section unknown → skip the record, write to the report", including skipping the child subtree.
4. **Per-mod state goes to `.scov`.** The container already does exactly what is needed: id tags,
   versions, per-chunk CRC, skipping the broken, preserving the not-understood-but-newer
   (`save_extension_container.cpp`). The single change is runtime chunk-id registration instead of
   the compile-time array (`save_extension_chunk_ids.h`); the id derives from `ns`.
5. **Installing a mod onto an existing save.** The new module's spawn layer injects through a "late
   pass" via `spawn_item` (`alife_simulator_base.cpp:92`) — the non-asserting runtime spawn by
   section name already exposed to Lua. A module may declare `on_install` / `on_uninstall` hooks.
6. **`REGISTRY_CHUNK` stays untouched.** Positional concatenation without framing
   (`alife_registry_container.cpp:52`) — any edit breaks everything; module state lives in `.scov`.
7. **Removing a mod from an existing save.** The manifest names every module the save was written
   with, its `ns` and its spawn-id range. A module that is gone or gated off at load is reported
   (`! XMS: save was made with module [...] which is no longer installed`) and then
   `XmsGame::ReleaseOrphanedModuleSpawns` releases what it had composed: every registered object
   whose spawn id lies in the module's range or in its applied-spawn ledger, children with their
   parents, server-side and offline, right after `on_register` and before the level goes online.
   The module's ledger goes with the objects, so a module that comes back places its spawns
   again. Objects the module's scripts created by hand carry no spawn id and stay: they are made
   of stock sections and visuals. Skipping unknown sections (item 3) covers the other half - a
   record whose section left with the module. Without this pass a box the module had placed
   stayed in the save with a visual that left with the module, and the load died on
   `Can't find model file`.

---

## 10. Optimization: all work at composition, zero at runtime

The main XMS invariant: **after composition the in-memory data structures have exactly the same shape
as vanilla.** No "layers" to walk at read time, no indirection, no mod tags in structures living in
hot code.

### 10.1 Hot paths — what happens to them

| Hot path | What changes | Cost |
|---|---|---|
| `FS.r_open` / `check_for_file` | nothing: the winner is fixed in `m_files` at mount time | 0 |
| `pSettings->r_*` | nothing: the same `Sect` (sorted vector + hash index) | 0 |
| dsgraph traversal, culling, portals | nothing: an overlay is an ordinary child of the sector root | same as base geometry |
| a level piece (8.3) | an ordinary child of a sector root; streaming looks at the distances every 8 frames | one pointer test a frame on a level without pieces |
| ray/box against statics | nothing: still **one** `CDB::MODEL`, `element` is the same raw index | 0 |
| static physics | nothing: the ODE tri-list already walks `ObjectSpace()` during collision | 0 |
| ALife update, registries | nothing: the same spawn graph, the same indices | 0 |
| Lua calls | a hook is one indirection, but only on functions someone actually hooked | 0 where there are no hooks |
| `CScriptCallbackEx` | the 0–1 subscriber fast path is preserved byte-for-byte; the vector appears from the 2nd | 0 in the vast majority of slots |
| AI-map node lookup | the base columnar index is untouched; appended nodes get a second small index | +1 binary search, only on levels with an overlay |

Provenance is **not stored per file**: `m_files` is a six-digit record count, and inflating the
`file` desc for a field needed once in a lifetime is not acceptable. The layer priority is known
globally at mount time; the owner is recorded only for genuinely conflicting paths, in a separate
ledger.

### 10.2 The composition cache — why it gets faster, not slower

Every composition artifact is cached to disk; the key is a hash of (module set + versions + input
mtimes):

| Artifact | Cache | Precedent |
|---|---|---|
| the composed `pSettings` | a binary snapshot instead of parsing ~196 ltx | new, the fattest win |
| the string table | already exists | `StringTable.cpp:24` |
| the composed XML documents | a binary snapshot | new |
| the composite `game.graph` | mmap of a ready blob — the same format as vanilla | the format does not change |
| the composite `level.ai` | same | the format does not change |
| the composite cform + OPCODE tree | the mechanism exists, only the key changes | `xr_area.cpp:238` |

A cold start (the first after a mod-set change) pays for composition once. Every subsequent start
mmaps the ready artifact — that is, **exactly the vanilla load path**. A side effect: vanilla DA
re-parses the whole `system.ltx` chain on every launch, XMS does it once per mod set.

### 10.3 What the design forbids

Otherwise the optimization silently drifts away:

* tombstones and "removed" marks instead of actual removal;
* any layers that must be walked **at read time** instead of composition time;
* per-object mod tags in structures living at runtime;
* "which mod put this here" checks in hot code;
* growing a sector's `vis.box` without dire need — it pessimizes culling of **all** base geometry of
  that sector. If an overlay does not fit the host's box, the patch compiler must pick another host
  sector or split the geometry. (On open levels the outdoor sector's box already covers the map, the
  delta is zero; indoors the rule is strict.)

---

## 11. Save compatibility with the original DA

The requirement: a save made in Refined with mod set X in mode Y loads in the original DA with the
same set and mode — and vice versa. It rests on one invariant: **XMS renumbers nothing.**

1. **Zero modules = byte identity.** With no XMS modules the composition is the identity operation:
   the same artifacts, the same GUIDs, the same `.scop`. This is regression test number one: a
   vanilla save → load in Refined → re-save → byte comparison.
2. **Base ids are immutable.** Base spawn ids do not move (they are keys, not indices); base game
   vertices keep their numbers (fragments are appended at the end); base `level.ai` nodes keep their
   indices (overlay nodes are append-only, which is why the append variant was chosen over
   re-sorting);
   module `story_id`s go into the `(ns << 16)` range; module object ids sit above the base's
   high-water mark.
3. **The base GUID never changes.** This is exactly why `all.spawn` is never rebuilt: the
   `header().guid()` check in the original DA compares the stored GUID with its own base one — and
   they match.
4. **Everything modular is in `.scov`, and only there.** A direct requirement of the frozen contract
   (`SAVE_COMPATIBILITY.md`): `.scop` and `.scoc` stay byte-for-byte original, `.scov` is the single
   extension channel. Original 0.98b simply does not see it; on a signature mismatch the sidecar is
   treated as stale and ignored with a log entry instead of failing the load. The mod-set manifest
   and module state are new `.scov` chunk ids registered from `ns`.
5. **An honest limitation.** Objects a mod added will load in the original DA only if it has the same
   config sections — that is, if the mod is installed there too. That is what "the same mod set"
   means. If the section is absent, with P3 the object is silently skipped with a report entry
   instead of `R_ASSERT2 "Can't create entity."`.

### 11.1 How this is verified

The rig and the method already exist, nothing to invent — XMS simply adds rows to the existing matrix
(`TEST_MATRIX.md`), which already runs "Original 0.98b pair without `.scov`" and
"Refined save in original 0.98b".

**The reference runtime:** `D:\Games\OriginDeadAir\Dead Air` — a clean original 0.98b, x86, 2018
binaries; the IDA databases `xrGame.dll.i64` / `xrEngine.exe.i64` sit next to it. The verification
order is the same as always in this project: **IDA on x86 → CoC ↔ DA 1.0 diff → x64 change**.

Three verification levels, cheapest to most expensive:

1. **Static, by code.** Read the real implementation instead of remembering it. An example from this
   very task: all four variants of `IReaderBase::find_chunk` (`FS_impl.h:65,102,186,257`) scan
   `{u32 type, u32 size}` headers and jump over a foreign chunk via `advance(size)` — so an unknown
   top-level chunk is technically safe. Yet `SAVE_COMPATIBILITY.md` still forbids touching `.scop`,
   and that settles the question without a single game launch.
2. **Comparison with the original binary through IDA** — when there is no source for the behavior
   (original 0.98b is not built from our tree) or when our x64 already diverged there. Decompile the
   specific function in `xrGame.dll.i64` and compare with ours.
3. **A live run on the reference.** XMS test cases:
   * **identity**: zero modules → new game → save in Refined → load in the original →
     re-save → compare the `.scop`/`.scoc` hashes (must be unchanged, no sidecar must appear);
   * **the reverse trip**: an original save → load in Refined → re-save → load in the original;
   * **the same mod set**: a mod exported to flat `gamedata` is installed into both runtimes;
     a Refined save loads in the original, the mod's objects are in place;
   * **the mod absent in the original**: the same save without the mod installed — the original must
     skip the foreign objects (that is the point of P3's skip-on-unknown-section), not die on
     `R_ASSERT2 "Can't create entity."`.

For the run the proven `_qa` scheme fits: a hidden desktop, save autoload, a Lua probe, a window
capture. Comparison — by SHA-256 of the save group files and by the object/spawn counters from the
log (the matrix already carries them: 22,958 spawns / 27,625 objects).

---

## 12. Hard format ceilings (budget for these)

They cannot be designed around — only counted and warned about upfront instead of crashing at
runtime:

| Quantity | Type | Ceiling | Anchor |
|---|---|---|---|
| spawn vertex id | `u16` | 65535 total | `alife_space.h:44` |
| live ALife object | `u16` | 65535 total | `alife_space.h:40` |
| game graph vertex | `u16` | 65535 **for the whole game** | `game_graph_space.h:17` |
| level id | `u8` | 255 levels | `game_graph_space.h:18` |
| level vertex from a game vertex | 24 bits | 16.7M | `game_graph_space.h:54` |
| neighbors of a game vertex | `u8` | 255 | `game_graph_space.h:54` |
| sector in `CDB::TRI` | 16 bits | 65536 | `xrCDB.h:57` |
| material in `CDB::TRI` | 14 bits | 16384 | `xrCDB.h:57` |
| object spawn packet | `u16` | 16 KB | `net_utils.h:19` |
| chunks in `.scov` | — | 1024, 16 MB each | `save_extension_container.h` |
| class name (CLASS_ID) | 8 characters | — | `TEXT2CLSID` |

Hence `[budget]` in the manifest: the loader hands out ranges deterministically and **refuses to
start with a clear message** when the requested sum does not fit — instead of a cryptic crash two
hours into the game.

The game-vertex ceiling (65535 for the whole game, a typical level is 1–3k) is the real bound on
"many new locations": roughly 20–30 levels fit on top of the base. Lifting it is P7.

---

## 13. Reporting and tooling

* `xms_report.json` on every start: the load order, the conflict ledger (files, keys, sections,
  strings, scripts, callbacks), spent budgets, skipped save objects, missing dependencies.
* Console: `xms_list`, `xms_conflicts`, `xms_why <path|section>` — who won and why.

---

## 14. Phases

Every phase is self-contained, breaks nothing in vanilla and delivers a concrete result.

| Phase | Size | What we do | What the player/modder gets |
|---|---|---|---|
| **P0** | S | module registry, manifest, order, mounting, ledger + the **identity test** (zero modules = byte-for-byte vanilla, saves included) | it is visible who clobbers whom; JSGME mods work as before |
| **P1** | M | LTX merge + `.ltxp`, XML patches, string table priorities, **the composed `pSettings` cache** | two mods edit one weapon/one section and do not fight; the game starts faster than vanilla |
| **P2** | M | script namespaces, hooks, multicast callbacks, registries | two mods change one logic; the end of the "who copied xr_logic last" era |
| **P3** | M | the mod-set manifest and module state in `.scov` (runtime chunk ids), unknown-object skip, tests on the 0.98b reference | install and remove mods **on an existing save** |
| **P4** | L | the additive spawn layer, stable keys, budgets, story_id namespacing | add NPCs/items/quests to any level without rebuilding `all.spawn` |
| **P5** | L | the composite `game.graph`, level fragments, transition stitching, the composition cache | new locations as ordinary mods, together, without rebuilding the world |
| **P6** | XL | geometry overlays: the composite CDB, visual injection, the composite `level.ai` (append-only), the patch compiler in the authoring tool, the composition-keyed cform cache | "a house in Cordon" from two mods at once |
| **P7** | XL, opt. | widening `_SPAWN_ID`/`_GRAPH_ID` to u32 | the ceilings go away; the price — the format stops being vanilla |

The first four phases close, by experience, the vast majority of real conflicts between mods.
P5–P6 are what a dedicated editor exists for in the first place.

---

## 15. Boundaries (what we deliberately do not do)

* **We do not merge binary assets.** `.ogf`, `.dds`, `.ogg` — last-wins + a ledger entry. Merging
  models is meaningless.
* **We do not touch `.scop` and `.scoc`.** The frozen contract (`SAVE_COMPATIBILITY.md`): fields and
  order are immutable, the single extension channel is `.scov`. This includes the mod-set manifest.
* **We do not touch `REGISTRY_CHUNK`** — there is no framing, any edit breaks every save. Module
  state lives in `.scov`.
* **We do not build a second static CDB** — `rq_result.element` has no spare field for a model id,
  and that is ~57 places in the code.
* **We do not hot-reload mods at runtime.** Composition happens at start and on save load.
* **We do not guarantee vanilla export for Grizzly-only features** (the composite graph, overlays, the
  additive spawn). The authoring tool says so openly, not quietly.
* **We do not resolve semantic mod conflicts.** No runtime arbitration, no load blocking, no
  negotiation between mods. A deterministic order, a winner, a report line. Mod A killed an NPC that
  mod B was waiting for — that is for the mod authors and the player to sort out, not the engine.
* **We do not re-sort `level.ai` nodes.** Re-sorting would give one perfect columnar index, but it
  would renumber all base `level_vertex_id`s and kill save compatibility. The price of the append
  variant is one extra binary search over a small index and only on levels with an overlay (§10.1);
  that is cheap, and renumbering buys nothing of what it costs.
