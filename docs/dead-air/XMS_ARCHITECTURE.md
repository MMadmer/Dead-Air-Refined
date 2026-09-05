# XMS — XFined Module System

Modular modding architecture for Dead Air: Refined (x64) and the XFined editor.

Status: **P0–P4 implemented in the engine** (2026-08-11), P5–P6 are design. All engine references follow the current `src/` tree.

## Implementation status

| Phase | State | Where in code |
|---|---|---|
| P0 registry/order/mounting/ledger | implemented | `src/xrCore/XMS/xms_core.{h,cpp}`, hooks in `LocatorAPI.cpp` (`xms_register`, redirect in `r_open_impl`, remount in `rescan_path`, initialization in `_initialize`) |
| P1 LTX merge + `.ltxp` | implemented | merge in `Xr_ini.cpp` (`insert_section`/`xms_merge_section`, layers in `Sect`), patches in `XMS/xms_ltx.cpp`, invoked from `x_ray.cpp::InitSettings` |
| P1 XML `.xmlp` | implemented | `XMS/xms_xml.cpp`, hook in `XMLDocument::Load`; the mutation API added to the tinyxml fork (`Xms*` methods) |
| P1 layered string table | implemented | `StringTable.cpp` (sort by layer + `CompositionHash` in the cache signature) |
| P2 multicast callbacks | implemented | `script_callback_ex.h` (subscribers with a 0–1 fast path), `game_object:add_callback/remove_callback` in `script_game_object_*` |
| P2 xms Lua core | implemented | `src/xrGame/xms_game.cpp`: `xms_native_*` natives + the embedded bootstrap (`xms.require`, `xms.hook` pre/post/around, `xms.registry`, `xms.save_data/load_data`, `xms.story_id`); loose override via `gamedata\scripts\xms.script` |
| P3 saves | implemented | the `XMS1` manifest + per-mod chunks `0x584D0000|ns` in `.scov` (`xms_game.cpp`, hooks in `save_extension_gameplay.cpp` and `alife_storage_manager.cpp`), unknown-section skip in `alife_object_registry.cpp::get_object`, dangling-children pruning in `alife_storage_manager.cpp::load` |
| P4 spawn composer | implemented | `src/xrGame/xms_spawn_composer.cpp` (`CALifeSpawnRegistry::xms_compose`, invoked before `build_story_spawns`), persistent ns/ranges in `xms_registry.ltx` |
| Console/report | implemented | `xms_list`, `xms_conflicts`, `xms_why <fragment>` (who supplied a file/section/key: the overlay map + the ledger) in `console_commands.cpp`; `$app_data_root$\xms_report.json` |
| Composed pSettings cache | deferred | an optimization, the format does not block it |
| Game modes | implemented | `Module::mode` gates the module's spawn/graph contribution, `mode=` on individual .xspawn ops; `[provides_mode]` declares a new mode; Lua `xms.set_modes/active_modes/mode_active/known_modes`; console `xms_modes`; active modes persist in the `.scov` manifest and are restored on load BEFORE composition. Config patches are not mode-gated (they are composed before the mode is chosen) — documented. `ModuleApplies` semantics: `mode=<id>` — the mode is active; **no `mode=` — the plain game only** (an empty active-mode set; a module with `[provides_mode]` is implicitly gated by its own mode); `mode=*` — everywhere |
| P5 composite game.graph | implemented (runtime) | `src/xrGame/xms_graph_composer.cpp`: the blob is rebuilt in memory on spawn load; module levels (`gamedata/levels/<name>/level.ai`) get an auto fragment (vertex sampling on a ~32 m grid, k=4 edges, a dense cross-table), a persistent u8 id (128+), `graph_links.ltx` stitching by coordinates; byte offsets of base vertices are recomputed, base indices and the GUID stay unchanged — saves survive |
| P6 collision | implemented | the module's `overlay.xcform` is merged into the static CDB in `CObjectSpace::Load` BEFORE the tree is built: overlay triangles go to the end; materials by name through the GMLib resolver; the cache is keyed by `crc ^ hash(overlays)`; the embedded OPCODE tree is ignored when overlays are present |
| P6 cutting | implemented | XCF1 **v2** carries a cut-box list after the triangles: a base triangle fully inside a box is dropped BEFORE `Create()`. Indices after it shift — that is safe, because a triangle index never survives level load (verified across all ~60 consumers of `rq_result.element`); the only serialization is the OPCODE tree in `objspace.bin` and the tail of `level.cform`, both already invalidated via `crc ^ hash` and `cacheStream = nullptr`. Vertices are NOT removed (that would require remapping `TRI::verts` for every consumer). Known consequences, documented: `level.ai` nodes are coordinate-based and do not disappear with the geometry; `detect_sector` casts a ray into collision, so a cut floor breaks sector detection |
| P6 AI map | implemented (experimental) | `overlay.aimap` appends NodeCompressed13 nodes to a heap copy of the array; the columnar index honestly falls back to a full scan with a warning when sorting is violated; the cross-table clamps on OOB nodes. Limitation: packed positions must land on the level's base grid |
| P6 visuals | implemented | `<module>\levels\<level>\overlay_visuals.ltx` lists world `.ogf` files (editor export); `CRender::LoadOverlayVisuals` (`r2_loader.cpp`) loads them physically (bypassing the VFS — otherwise last-writer-wins would kill every module's contribution but the last), detects the sector by the visual's center and push_backs into the root's `children`; the root's box grows **only** when the overlay actually exceeds it (otherwise the sector's base geometry loses fcvInside); `UnloadOverlayVisuals` detaches and frees on `level_Unload`. Section keys: `file` (required), `mode` (an extra mode gate), `sector` (a forced id). Skeletons/dynamics are rejected. For small things the alternative is a regular spawn (P4) |

Runtime implementation notes:
- Modules live in `<game root>\modules\<id>\` — their OWN folder, not `mods\`. `mods\` is the JSGME folder (the same as `MODS`, case-insensitively): JSGME shows every subfolder there as a mod of its own and "activation" copies it over the game — exactly the merge the module exists to avoid, plus the content applies twice. `mods\` keeps being read (what is already installed must not break), but every module found there logs a warning, and a `mod.ltx` in the game root (the trace of an already performed JSGME install) is reported separately.
- Enabling/disabling a module is its own mechanism, not a mod manager's: `xms_disable <id>` / `xms_enable <id>` write the id into `modules\disabled.ltx` (one per line, hand-editable) and copy NOTHING anywhere. Takes effect on the next launch (modules mount during FS initialization).
- The kill switch: `-no_xms` on the command line.
- Module layout: `mod.ltx` + `gamedata\` (the classic 1:1 overlay) + `patch\*.ltxp|*.xmlp` + `spawn\*.xspawn` + `scripts\` (namespaced, via `xms.require`).
- New config sections arrive through `gamedata\configs\xms\*.ltx` (parsed into pSettings on top of the base with merge semantics).
- `.xspawn` coordinates are level-local; `game_vertex = auto` takes the level's nearest game vertex (the editor will stamp exact ids on export in the future).
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
| **Export** | building a module into a flat `gamedata\` | for those playing on vanilla DA. Works when the mod uses no XFined-only runtime |

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

Recognized sections: `[module]` (`id` `[a-z0-9_.-]`, `name`, `version`, `mode`),
`[provides_mode]` (`id`/`title`), `[requires]`, `[conflicts]`,
`[order]` (`after`/`before`), `[budget]` (`spawns`, default 256), `[vfs]`,
`[redirects]`. Unknown keys are ignored by the engine but survive the editor's
round-trip — format extensions are added as new keys only.

```ini
[module]
id            = madmer.better_cordon     ; [a-z0-9_.-], reverse-dns
name          = Better Cordon
version       = 1.2.0

[requires]
xfined.core   = >=1.0

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
`mods/order.ltx` (drag-n-drop in the editor/launcher), then by `id`. Fully deterministic.
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
```

The injection point is after `InitSettings` (`src/xrEngine/x_ray.cpp:187`), before the first reader.
`pSettings` stays `const` for the game; the patch pass works at composition time.

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

**Lighting.** The lightmap is bound to the level's shader table (`r2_loader.cpp:49`), therefore:
* the cheap path — an overlay on a self-contained OGF shader (the `OGF_TEXTURE` branch,
  `FBasicVisual.cpp:58`): dynamic light only, but it works immediately;
* the honest path — the patch compiler in XFined Editor bakes its own lightmap and **appends**
  entries to the shader table at runtime (`getShader` is index-based, append is safe). Which is
  exactly what the editor is being built for.

**AI map.** `level.ai` is mmap'ed and welded by GUIDs (`level_graph.cpp:28`). An overlay needs a
composite node array in memory — the path already exists: `CLevelGraphManager` can own a heap array
during version conversion (`level_graph_manager.h:15`). Plus appending cross-table cells and
rebuilding the columnar index (`level_graph.cpp:48`, degrades softly when sorting is violated).

**What degrades, documented:** HOM (new geometry can be occluded but does not occlude), SOM (sound
passes through), grass in slots outside the compiled grid (`DetailManager_CACHE.cpp:256` returns an
empty slot without crashing); wallmarks on overlays work (they go through the static CDB — and ours
is composite).

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
* **XFined Editor**: the module list with drag ordering, the conflict matrix, a composition dry-run
  without launching the game, a legacy-mod "lift" into a module (by diffing against the base:
  LTX/XML convert to patches automatically, binaries stay opaque, scripts are flagged for manual
  review), flat `gamedata\` export for vanilla DA with a "vanilla-compatible: yes/no" badge and the
  list of reasons.
* All of this is already reachable through XFinedMCP — composition can be debugged with API calls.

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
| **P6** | XL | geometry overlays: the composite CDB, visual injection, the composite `level.ai` (append-only), the patch compiler in the editor, the composition-keyed cform cache | "a house in Cordon" from two mods at once |
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
* **We do not guarantee vanilla export for XFined-only features** (the composite graph, overlays, the
  additive spawn). The editor says so openly, not quietly.
* **We do not resolve semantic mod conflicts.** No runtime arbitration, no load blocking, no
  negotiation between mods. A deterministic order, a winner, a report line. Mod A killed an NPC that
  mod B was waiting for — that is for the mod authors and the player to sort out, not the engine.
* **We do not re-sort `level.ai` nodes.** Re-sorting would give one perfect columnar index, but it
  would renumber all base `level_vertex_id`s and kill save compatibility. The price of the append
  variant is one extra binary search over a small index and only on levels with an overlay (§10.1);
  that is cheap, and renumbering buys nothing of what it costs.
