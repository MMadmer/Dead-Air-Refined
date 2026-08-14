# Modding capabilities specific to Dead Air: Refined

## XMS modules (XFined Module System)

Additive mod packages that compose instead of overwriting each other. Full
design: `XMS_ARCHITECTURE.md`. Quick authoring reference:

A module is a self-contained folder the game reads IN PLACE: nothing is ever
copied, unpacked or merged into `gamedata` or any other game folder, and the
engine refuses writes into module folders by code. The only thing the game
writes because of modules is its own bookkeeping under `_appdata_`
(`xms_registry.ltx`, `xms_report.json`, caches).

```
<game root>/modules/<mod.id>/
  mod.ltx            ; manifest: [module] id/name/version, [requires], [order]
                     ; after/before, [conflicts], [budget] spawns=N,
                     ; [vfs], [redirects] (see below)
  gamedata/          ; classic overlay, mirrors game layout 1:1 (assets,
                     ; full-file overrides); later modules win per file
  gamedata/configs/xms/*.ltx   ; NEW config sections, merged into system.ltx
  patch/*.ltxp       ; config directives: key = v, +list_key = a, -list_key = b,
                     ; !key (delete), [!section] (delete), [new] : parent
  patch/*.xmlp       ; XML DOM patches: <xms-patch target="ui\file.xml">
                     ; <append into="a:b"/>, <set-attr/>, <replace/>, <remove/>
  spawn/*.xspawn     ; additive spawn ops:
                     ;   [obj:my_guard]  op=add section=... level=... position=x,y,z
                     ;   [obj:base:<level>/<name>]  op=modify|remove ...
  scripts/*.script   ; namespaced, loaded via xms.require("mod.id","file")
  levels/<level>/    ; level overlays, exported by XFined Editor:
    overlay.xcform   ;   collision triangles appended to the static CDB
    overlay.aimap    ;   AI nodes appended to level.ai
    visuals/*.ogf    ;   world-space geometry
    overlay_visuals.ltx ; registry for the .ogf above
```

The `gamedata/` mirror is a CONVENTION, not a requirement. A module may keep
any folder layout it likes and publish it through its manifest:

```ini
[vfs]
; <virtual game path> = <path inside the module>  (file or folder)
configs\xms\balance.ltx = tuning\balance.ltx     ; single file
textures\wpn            = art\weapon_textures    ; whole folder, recursive

[redirects]
; <retired virtual path> = <current virtual path> - the UE redirector idea:
; renaming an asset inside the module never breaks references from saves,
; other modules or base configs that still use the old name
meshes\dynamics\old_crate.ogf = meshes\dynamics\props\crate_a.ogf
```

`[vfs]` entries mount after the module's own `gamedata/` mirror (an explicit
mapping wins over the mirror); between modules the usual load order applies.
`[redirects]` are resolved after EVERY module has mounted, so a redirect may
point at any module's content or at a loose base file; archive-backed targets
cannot be redirected to. Both sections refuse `..` and report bad or missing
entries in the log instead of silently dropping them.

Visual overlay registry — one section per entry, section name is free-form. A
section either **adds** a visual (`file`) or **hides** base ones (`hide`):

```ini
[v_0]
file    = visuals/my_shed.ogf   ; add: world-space .ogf, relative to levels/<level>/
mode    = hardcore              ; optional, extra game-mode gate
sector  = 12                    ; optional, skips sector auto-detection

[cut_0]
hide    = 12.5,3,-40, 6,4,6     ; hide: centre x,y,z + HALF-extents x,y,z
overlap = true                  ; optional: also take visuals that merely touch
                                ; the box - needed to drop one big terrain sheet
```

All hides run before all adds, so a module can never delete geometry another
module just added: hides only ever hit the base level.

**Subtractive edits** ("dig a pit", "delete this fence") are the pair
`hide` + a cut box in `overlay.xcform` — the render side detaches base visuals,
the collision side drops base triangles that lie wholly inside the box. Both are
written in one action by the editor's *Export Level Cut*. Two caveats worth
knowing: AI nodes are coordinate-based and do **not** disappear with the
geometry (patch them with `overlay.aimap` if NPCs must not walk there), and
render sectors are detected by raycast against collision, so cutting away the
floor of a sector breaks sector detection for whatever stands on it.

The `.ogf` must be baked in **world space** (the editor's OGF export does that)
and static — skeletons and dynamics are rejected with a log line. Every module
contributes its own registry; these files live outside `gamedata/` on purpose,
so overlays add up instead of overwriting each other.


Runtime facts:

- Where modules live, and why not next to the JSGME ones. A module is read
  from `<game>/modules/<id>/`. `<game>/mods/` is the folder JSGME manages (same
  directory as `MODS/` — Windows ignores the case), and JSGME lists every
  subfolder there as one of its own: "activating" a module in it copies the
  module over the game, which is the exact merge a module exists to avoid, and
  the content then applies twice — once as an overlay, once as loose gamedata.
  `mods/` is still read so nothing installed before this stops working; every
  module found there gets a log line saying so, and a `mod.ltx` in the game
  root (the fingerprint of a module JSGME has already installed) is reported
  loudly at startup.
- Switching a module off is XMS's own job, not a mod manager's:
  `xms_disable <id>` / `xms_enable <id>` write the id to
  `modules/disabled.ltx` (one per line, editable by hand) and copy nothing
  anywhere. Modules mount while the file system comes up, so it takes effect
  on the next start. JSGME keeps working normally for JSGME mods, including a
  module exported as a flat `gamedata_<id>` overlay — that IS a JSGME mod.
- Load order is deterministic: `[requires]`/`[order]` topology, then
  `modules/order.ltx` (one id per line), then id. `xms_list` shows it.
- Conflicts never block loading: the later layer wins and the resolution is
  recorded — `xms_conflicts` in console, full ledger in
  `appdata/xms_report.json`.
- `xms_why <fragment>` answers "who gave me this?" for a file path, config
  section, key or xml node — matching overlay files plus the ledger rows.
- Same-path files in two modules collide at the VFS level (last layer wins,
  ledger row); prefer `patch/` directives over shipping whole config files.
- Lua API: `xms.modules()`, `xms.require`, `xms.hook(path, fn, {mode, priority})`
  (pre/post/around), `game_object:add_callback(type, id, fn[, priority])` /
  `remove_callback(type, id)`, `xms.registry.get(name)`,
  `xms.save_data(id, str)` / `xms.load_data(id)` (persisted in `.scov`),
  `xms.story_id(id, n)` for collision-free story ids.
- Saves: the module set is recorded in the save's `.scov` sidecar; removing a
  module does not brick the save — its objects are skipped on load and
  reported. `.scop` stays byte-compatible with original Dead Air 0.98b.
- Spawn ids for module objects come from persistent per-module ranges
  (`appdata/xms_registry.ltx`); base `all.spawn` ids never change.
- Kill switch: `-no_xms` command line. JSGME layers and `xtra_*.xdb0` keep
  working unchanged; a folder without `mod.ltx` is not a module and is ignored
  in either root.
- What `mode=` gates, exactly. Everything that edits a LEVEL is gated - the
  spawn composer, `overlay.xcform` (including its cut boxes), `overlay.aimap`,
  `overlay_visuals.ltx` (both the added `.ogf` and the `hide` boxes) and the
  composite `game.graph` all ask `XMS::ModuleApplies` before contributing, so a
  map edit made for one campaign cannot appear in another.
- The gate's three states. `mode = <id>` applies when that mode is active.
  **No `mode=` at all means the ORDINARY game only**: a stock new game runs
  with no active modes, and a campaign like Revolution II must not inherit
  props that were never made for it. (A module that declares
  `[provides_mode]` is implicitly gated to its own mode instead.)
  `mode = *` opts out of gating - the module's level work lands everywhere.
- What it does NOT gate: the VFS mount, the config stage (`.ltxp`, new LTX
  sections), XML patches and module scripts. Those run at engine start, before
  the player has chosen anything, so they apply in every game. Keep
  mode-specific CONTENT in spawn/levels, and gate mode-specific BEHAVIOUR in
  Lua with `xms.mode_active(id)` rather than in plain file overrides.
- The active mode set comes from the new-game screen: `[character_creation]`
  keys named exactly `new_game_<x>_mode` and set to `true` are published as XMS
  modes when a new game starts (`new_game_metro_mode` -> `metro`), and a loaded
  save restores the set recorded in its `.scov`. Both halves of the name are
  required and the value has to read `true`, so the other `new_game_*` booleans
  the same screen writes (`good_wpn`, `good_loot`, `unlocked_guide`, ...) stay
  what they are - options, not modes - and a mode is never active in the engine
  while the game's own `is_*_mode()` helpers say it is off. A save that carries
  no XMS manifest starts from an EMPTY set, so an old save can never inherit
  the modes of the session before it. A module adding its OWN mode has to put
  its checkbox on that screen itself - see below.
- Two module scripts run on their own, in load order, right after the `xms` API
  exists (`xms.load_registrars`): `scripts/mode_register.script` (generated by
  XFined Editor, rewritten on every export) and then `scripts/register.script`
  (the author's, never touched by any tool). Every other module script waits
  for an `xms.require`.
- A registrar runs once per SCRIPT ENGINE, not once per process. The engine
  rebuilds the Lua state for a new game, a save load and every level change,
  and each of those states needs its own wrappers - so a registrar must be
  idempotent, must keep its side effects inside Lua, and must NOT read the
  active mode set: on the load path the state is rebuilt before the save's
  modes are restored. Ask `xms.mode_active(id)` when the thing actually
  happens, not while registering it.

### Adding a new game mode

The engine publishes modes, it does not invent them: a mode exists for the
player only when a checkbox for it exists on the new-game screen, because that
screen is built in Lua by the game. A module that declares one in its manifest

```ini
[provides_mode]
id    = my_campaign
title = Моя кампания      ; what the player reads, not a string id
```

ships three generated files - XFined Editor writes them on export, and they are
the whole contract if you write them by hand:

| file | what it does |
| --- | --- |
| `patch/xms_modes.xmlp` | appends `check_<id>_mode` + `cap_check_<id>_mode` to `main_dialog`, grows the frame around the mode column and pushes whatever sat below it down |
| `gamedata/configs/text/rus/<module id>_modes.xml` | defines `st_cap_check_<id>_mode`, cp1251 like every other string table |
| `scripts/mode_register.script` | wraps `faction_ui:InitControls` to create the controls and `faction_ui:OnStartGame` to write `[character_creation] new_game_<id>_mode` |

The two file names that stay inside the module are fixed, so renaming a mode
rewrites them instead of leaving a stale checkbox behind. The string table is
the one that lands in the shared game namespace, so it carries the module id -
two modules must not collide on it.

Two things bite anyone writing that script by hand. `faction_ui` is a luabind
class, so its methods are replaced by plain assignment (`cls.InitControls = ...`)
- `xms.hook` resolves through `_G` and cannot reach them. And the screen keeps
its `CScriptXmlInit` local, so the wrapper parses the layout again itself
(`xml:ParseFile("ui_mm_faction_select.xml")`, the engine picks the aspect
variant); there is no `self.xml` to reuse.

The naming is not decoration: the engine turns `new_game_<id>_mode` back into
the mode id `<id>`, which is what `mode=` in the manifest and `xms.mode_active`
compare against. The screen layout is measured, never assumed - the editor reads
the layout the install actually ships (a global mod like Revolution II replaces
it wholesale) and places the row under the last one of the campaign column.

Such a module is a NEW GAME, not an addon: the campaign it adds only exists in
an install that has it. Old saves and stock campaigns keep working because
nothing base is overwritten - the checkbox is appended, and every map edit it
carries is gated behind its own mode.

## Loose particle overrides

Individual particle effects and groups can be replaced or added through loose
files without rebuilding `particles.xr`:

- Location: `gamedata/particles/**` (also works from XDB archives and JSGME
  layers — the engine enumerates the virtual namespace with normal VFS
  precedence, loose files win over archives exactly like other gamedata).
- Format: the engine's ini-style single-particle formats — `.pe` for an effect,
  `.pg` for a group (the same files the SDK particle editor reads and writes).
- Naming: the effect name is the file path without extension relative to the
  particles directory. `gamedata/particles/anomaly2/effects/x.pe` defines
  effect `anomaly2\effects\x`; the on-disk layout must mirror archive names to
  replace them.
- Semantics: an existing name is replaced, a new name is added; removing the
  file fully restores archive behavior. With no loose files present the
  registry is byte-identical to the archive.
- Failure handling: a malformed file is reported in the log with its path and
  skipped; the archive definition stays intact. The load summary line reports
  replaced/added/failed counts.
- Limitations: only the runtime parser's subset of effect data is honored;
  editor-only action data that the game parser does not read is ignored. The
  base game ships no loose overrides — the capability exists for addons.

## Addon script audit

`tools/compat/lua_call_audit.py` statically checks a script tree against the
engine's exported Lua bindings before an addon silently loses behavior:

```bash
python tools/compat/lua_call_audit.py --bindings lua_help.script --scripts <addon-scripts-dir>
```

- `--bindings` takes a `lua_help.script` dump produced by the engine's bindings
  exporter (or a JSON inventory from `lua_help_inventory.py`).
- Conservative by design: it audits `db.actor:method(...)` calls and
  `namespace.func(...)` calls for namespaces the engine really exports;
  dynamic constructs are ignored, not guessed.
- `--json`, `--baseline`, and `--allowlist` support CI usage; the exit code is
  non-zero only for new, unreviewed findings. `--selftest` runs the built-in
  parser fixtures.

## Scope-driven HUD bones

Weapons can hide or show first-person model bones depending on whether a scope is
mounted. Three optional keys, all inert when absent:

| Key | Section | Effect |
| --- | --- | --- |
| `scopes_hide_bone` | weapon | single bone, hidden while a scope is attached |
| `scope_hide_bones` | scope | list, hidden while that scope is the mounted one |
| `scope_show_bones` | scope | list, visible only while that scope is the mounted one |

```ini
[wpn_ak74]
scopes_sect = wpn_addon_scope, wpn_addon_scope_susat
scopes_hide_bone = iron_sight_rear

[wpn_addon_scope_susat]
scope_hide_bones = gas_tube_cover, carry_handle
scope_show_bones = riser_block
```

Rules and limits:

- Lists are comma separated. Spaces are accepted and normalised to commas, so the
  canonical on-disk form stays comma separated and XMS `+key` / `-key` list
  composition keeps working.
- Bone names are matched case-insensitively; they are lowercased on both sides.
- A name absent from the current visual is skipped silently. Nothing is logged, so
  a typo looks exactly like the key doing nothing.
- Only the first 64 bones of a visual can be addressed. The engine's visibility mask
  is 64 bits wide; bones past that index are skipped instead of corrupting a
  neighbour.
- The keys apply only while `scope_status` is attachable. A weapon with a permanent
  scope, or one whose scope was promoted to permanent by an upgrade, ignores them.
- Visibility affects the first-person model only. The world model is untouched, which
  matches the reference implementation.
- Bones are returned to the model default when the weapon leaves the player's hands,
  so a weapon that shares its `hud_section` with another one never inherits its state.

## Weapon misfire ceiling

`misfire_condition_ceiling` is the condition above which a weapon never begins to
misfire, whatever curve its own `misfire_start_condition` describes. It is read from the
weapon section first and falls back to a global default in `[inventory]`, so an addon can
retune the whole game in one line and still special-case individual weapons.

```ini
[inventory]
misfire_condition_ceiling = 0.75   ; applies to every weapon that does not override it

[wpn_custom_prototype]
misfire_condition_ceiling = 0.95   ; this one is meant to be unreliable even in good shape
```

- The engine default is `0.75`. Set `1.0`, globally or per weapon, to disable the cap and
  get the stock curve back.
- The cap only ever removes malfunctions relative to the section's own curve. A weapon whose
  curve already starts below its ceiling keeps its own, lower threshold untouched.
- The HUD condition warning uses the capped threshold too, so the indicator and the actual
  behaviour agree.
- The legacy `misfire_probability` formula keeps its own built-in 0.95 floor even when the
  ceiling is disabled.
