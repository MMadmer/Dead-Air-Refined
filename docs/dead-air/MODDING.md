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
                     ; add-ops reach EXISTING playthroughs too: a loaded save
                     ; composes the registry the same way, and objects this
                     ; playthrough never saw are instantiated once, recorded
                     ; in a per-module ledger inside the .scov - a prop the
                     ; player destroyed stays destroyed. modify/remove of
                     ; base objects act on NEW games only: a save already
                     ; carries its own copy of every existing object.
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
cannot be redirected to — and that covers nearly everything the game ships, see
*Content bundles* below. Both sections refuse `..` and report bad or missing
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
  `xms.story_id(id, n)` for collision-free story ids,
  `xms.list_files(id, subdir, mask, recursive)` / `xms.read_file(id, relpath)`
  to read a module's own data files, `xms.module_applies(id)` for the engine's
  `mode=` gate, and `xms.dialog_register/unregister/invalidate(id, …)` for
  dialogs built by script instead of XML (`xms.nq_api` feature-tests the group).
- Saves: the module set is recorded in the save's `.scov` sidecar; removing a
  module does not brick the save — its objects are skipped on load and
  reported. `.scop` stays byte-compatible with original Dead Air 0.98b.
- Spawn ids for module objects come from persistent per-module ranges
  (`appdata/xms_registry.ltx`); base `all.spawn` ids never change.
- Kill switch: `-no_xms` command line. JSGME layers and `xtra_*.xdb0` keep
  working unchanged, with one exception: an archive whose name matches the
  reserved content bundle shape is mounted only when the installed content
  manifest declares it at that exact size — see *Content bundles* below for the
  grammar to stay out of. A folder without `mod.ltx` is not a module and is
  ignored in either root.
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
- LEGACY saves heal themselves. A save written before the engine recorded the
  mode set carries no manifest, so it loads with an empty set and every
  mode-gated module composes nothing. The modes never left the save, though:
  the base game keeps `enable_<x>_mode = true` in alife_storage_manager's
  state, and `dead_air_x64_mode_restore.script` derives the set from it on the
  first actor update (generically - any true `enable_<x>_mode` key), publishes
  it and re-composes (`xms_native_recompose_spawns`: the composer skips
  existing vertices, the late pass instantiates the missing objects once).
  The next save writes a proper manifest and the script never fires again.
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

### NQ quest graphs

A module can ship whole quests without a line of Lua. One quest is one
`*.nqasset` file — declarative Lua (`return { … }`, loaded in an empty
environment) describing nodes, dialog phrases, objectives, conditions and PDA
tasks. The game interprets it; nothing is compiled, generated or merged into
`gamedata`, so a runtime fix in a game update fixes every module's quests at
once.

- Put the files **anywhere inside the module** — the runtime scans the module
  root recursively for `*.nqasset`. Quests are gated by `mode=` like the rest of
  a module's content, and a quest reaches an EXISTING save as soon as the module
  is installed.
- New node kinds come from `gamedata/configs/nq/kinds/<mod>.ltx` plus
  implementations registered in `scripts/register.script` through
  `xms.registry.get("nq.kinds")`.
- Quest state is one blob in the save's `.scov` sidecar
  (`xms.save_data("xms.nq", …)`, chunk `0x584DFF01`); `.scop`/`.scoc` are not
  touched and removing the module does not brick the save.
- Console: `nq list`, `nq state <uid>`, `nq jump`/`nq fire`, `nq reload`,
  `nq validate`, `nq dump`, `nq debug 1`. Log lines are prefixed `[nq]`.
- Full contract — format, catalog, execution model, dialogs, persistence,
  validation codes: `NQ_RUNTIME.md`.

### Adding a new game mode

The engine publishes modes, it does not invent them: a mode exists for the
player only when a checkbox for it exists on the new-game screen, because that
screen is built in Lua by the game. A module that declares one in its manifest

```ini
[provides_mode]
id    = my_campaign
title = Моя кампания      ; what the player reads, not a string id
```

shows up on that screen through Refined's **module-mode dropdown**: one shared
combo in the band between the loadout section and the modes/options block
(`dead_air_x64_mode_select.script`; geometry in
`dead_air_x64_mode_exclusive.ltx` `[module_mode_bar]`; drawn only when at
least one module registered, so a module-less screen stays stock). The
registrar the editor generates joins it with
`dead_air_x64_mode_select.add_mode(<id>, "st_cap_check_<id>_mode")`, and that
script also writes `new_game_<id>_mode` on start; picking a module campaign
clears the checkbox campaigns and ticking a checkbox campaign resets the
dropdown, through the exclusivity script's `on_campaign_picked` hook. On a
plain DA install without the Refined layer the registrar falls back to the
module's own checkbox - which is why the export still

ships generated files - XFined Editor writes them on export, and they are
the whole contract if you write them by hand:

| file | what it does |
| --- | --- |
| `scripts/mode_register.script` | registers the mode(s) with the dropdown (`dead_air_x64_mode_select.add_mode`); on an install without that layer, falls back to creating the module's own checkbox and writing `[character_creation] new_game_<id>_mode` on start |
| `gamedata/configs/text/rus/<module id>_modes.xml` | defines `st_cap_check_<id>_mode`, cp1251 like every other string table |
| `gamedata/configs/ui/<module id>_modes.xml` (+`_16`) | the FALLBACK checkbox layout - the module's own file, measured against the screen the linked game ships; the screen's xml itself is never patched, so nothing fights over its frame |

The registrar's file name is fixed, so renaming a mode rewrites it instead of
leaving a stale checkbox behind. The string table and the layout land in the
game's shared namespace, so they carry the module id - two modules must not
collide on them.

Two things bite anyone writing the fallback by hand. `faction_ui` is a luabind
class, so its methods are replaced by plain assignment (`cls.InitControls = ...`)
- `xms.hook` resolves through `_G` and cannot reach them. And the screen keeps
its `CScriptXmlInit` local, so the wrapper parses the module's own layout
itself (`xml:ParseFile("<id>_modes.xml")`, the engine picks the aspect
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

**A module's campaign is exclusive with the others for free.** The stock screen
enforces nothing - it registers no handler for any mode checkbox, so all of
them could be ticked at once - and Refined's
`gamedata/scripts/dead_air_x64_mode_exclusive.script` adds the rule. The script
is mechanism only and knows no checkbox by name; the group is assembled when
the screen opens, from three sources:

- `gamedata/configs/dead_air_x64_mode_exclusive.ltx`, section
  `[exclusive_campaigns]` - one registered control name per line. This is DATA
  about the installed screen (the screen itself ships in a third-party mod, so
  knowledge about it lives in a config other mods can patch, never in code).
  Controls the installed screen does not have are skipped silently.
- XMS module modes, DISCOVERED rather than declared: the generated registrar
  keeps its controls in `self.xms_checks[<mode id>]` and registers each under
  `main_dialog:check_<id>_mode` - those two names are the whole contract, keep
  them and a hand-written registrar joins the group as well.
- the public calls, for anything else:

```lua
dead_air_x64_mode_exclusive.register("check_my_mode")
dead_air_x64_mode_exclusive.unregister("check_rev_mode")   -- let it stack instead
```

The log line `* dead_air_x64: N campaign checkbox(es) made exclusive` on
opening the screen is how you check what joined. The options column (easy,
hardcore, rerum, good weapons, good loot) is deliberately not listed - those
are modifiers and stay multi-select; note their keys carry `_mode` too, the
suffix proves nothing.

## Content bundles

Refined's own assets ship as versioned archives in `database\` instead of riding
inside the update payload: the installer fetches them, the game verifies them on
every launch and refuses to start a level while any of them is missing or wrong.
The full contract — manifest format, resolver, repair, the play gate — is in
`CONTENT_BUNDLES.md`. What a mod has to know is here.

### The reserved name shape

A file in `database\` is treated as a content bundle when its name matches this
grammar exactly:

```
xtra_dead_air_x64_content_<group>_<NN>_<hash16>.xdb0
```

| Part | Grammar |
| --- | --- |
| `<group>` | one or more of `a`–`z`, `0`–`9`, `_`; never empty |
| `<NN>` | exactly two decimal digits |
| `<hash16>` | exactly 16 lowercase hex digits (`0`–`9`, `a`–`f`), the first 16 of the packed file's SHA-256 |

A name that fails any of those rules is not a bundle and mounts as an ordinary
archive: `xtra_dead_air_x64_content_pack.xdb0` and
`xtra_dead_air_x64_content_textures_1_deadbeef.xdb0` both load normally. It is
the shape that is reserved, not the prefix.

The match is strict because this grammar is a delete authority. The uninstaller
sweeps `database\` by it, and a content commit moves every file matching it that
the installed manifest does not name out of `database\` into the content cache.
An archive that lands in the shape by accident is therefore not merely refused —
it is moved out from under the game and removed with Refined.

**So do not use the `xtra_dead_air_x64_content_` prefix for a mod archive at
all.** Every other `xtra_*.xdb0` name is yours and behaves exactly as it always
has.

A refused archive is named in the log as `! [content] skipped <name> (<reason>)`
and listed by the `dar_content_state` console command. A refused archive the
manifest does not declare is reported as a notice, not a problem: it does not
mark the installation incomplete and does not stop the game from starting.

### Where bundles sit in load order

Archives mount in byte order of file name (`xr_strcmp`, ascending) and the later
archive wins per virtual path. So bundles mount after `xtra_dead_air_x64.xdb0`
— `.` (0x2E) sorts before `_` (0x5F) — and after `configs`, `meshes`,
`sounds`, `textures`, `levels` and `xtra.xdb0`, which is what makes content
override the base game.

Against a third-party `xtra_*.xdb0` the answer is whatever the byte order says,
and it cuts both ways. `xtra_zzz.xdb0` mounts after the bundles and overrides
them; `xtra_dar2.xdb0` mounts BEFORE them, because at the seventh character `e`
(0x65) sorts before `r` (0x72), so a bundle overrides it. Neither is a special
case worth designing around — work out where your archive lands and name it for
the outcome you want. A mod overriding content is a mod doing its job; a mod
that expected to override content and did not is a mod that got outsorted.

Loose files beat all of it. `$arch_dir$` (`database\`) is listed before
`$game_data$` (`gamedata\`) in `fsgame.ltx` and a second registration of the same
virtual path replaces the entry, so loose `gamedata\`, JSGME layers and XMS
overlays (a module's `gamedata/` mirror and its `[vfs]` mappings) override
content bundles exactly the way they override any other archive. Content changes
nothing about that order.

### Verification never looks at your mod

The startup check, `dar_content_verify` and the repair path look at bundle FILES
in `database\` by name — stat, then SHA-256 — straight through the filesystem,
never at a resolved virtual path. No override can make an installation report
incomplete, and verification can never break an override. The two systems do not
meet.

### Redirects and archive-backed content

`[redirects]` cannot point at anything an archive supplies, and that limit now
covers nearly everything the game ships: the base game's `database\*.xdb*`, the
Refined compatibility archive and the content bundles are all archives. A
redirect target has to be a loose file or a file some module supplies itself.

### Never write into `database\` or the content cache

Dropping a plain `xtra_*.xdb0` into `database\` is still the classic pipeline and
still works. Everything else in there belongs to the content system:

- Never add, edit, rename, move or delete a content bundle. Verification hashes
  every one of them, and a mismatch marks the installation incomplete and blocks
  play until it is repaired — which re-downloads the bundle and discards whatever
  was done to it.
- Never write a file with the reserved shape. It is sidelined on the next commit
  and removed on uninstall, as above.
- Never write into `<game>\.dead-air-x64\content-cache\`. It is download and
  retirement staging: its garbage collector deletes entries by hash name, and the
  uninstaller deletes the whole directory.

Nothing in the engine enforces this — `database\` has no write guard. The
consequence of breaking it is a player whose installation reports itself broken
for a reason that points at the game rather than at the mod.

## Loose particle overrides

Individual particle effects and groups can be replaced or added through loose
files without rebuilding `particles.xr`:

- Location: `gamedata/particles/**` (also works from XDB archives and JSGME
  layers — the engine enumerates the virtual namespace with normal VFS
  precedence, loose files win over archives exactly like other gamedata).
  Content bundles are archives in that same namespace and lose to loose files
  like every other archive.
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

## Weapon faults: fouling, deformation, breakage

The faults of the player's weapon (the `condition_avail` / `st_condition_type_N` list of the
original) come from use and neglect, not from a per-shot lottery. The original rolled three
independent chances on every shot, with no memory: the expected time to the first dirty fault
of an AK-74 was ~195 rounds, but 14 % of magazines produced one within the first 30 rounds and
a fault could land on a brand-new weapon. The rework keeps the original's expected values and
replaces the distribution with three accumulators per weapon:

- **Fouling** - rounds since the last cleaning. The first dirty fault (mainspring, return
  spring, barrel, sear, firing pin or bolt - ids 3, 5, 11, 16, 19, 22) lands at a threshold
  drawn once per cleaning cycle with +-15 % play; later ones at `fault_fouling_repeat` of the
  interval. The tooltip announces the fouling from half of the interval on. A kit or the
  mechanic clearing any dirty bit starts a fresh cycle.
- **Deformation** - durability only. Above `misfire_condition_ceiling` (0.75) nothing bends.
  Below it the progress advances at the original's `d2` rate, scaled by how far below the
  ceiling the weapon is and by its fouling (a filthy gun wears up to twice as fast). The pool:
  worn receiver and mainspring, the deformed parts, split grip, broken stock, and the
  selector/sight-rail/muzzle-thread/handguard faults on a weapon that has more than one fire
  mode / an attachable scope / silencer / launcher - the same gates the loot roll in
  `items_condition.get_break` uses. The separate selector roll on every fire-mode switch
  (44 % per switch at half condition) is gone.
- **Breakage** - a deformed part that keeps being fired. Every shot with n breakable
  deformations present advances the count by n (the original's per-part rate); the break
  lands on the counterpart of one of them (9->10, 12->13, 14->15, 17->18, 20->21, 1->2), never
  on a healthy part, at the original's expected `5 x d3` rounds.

Keys, read as engine default -> `[inventory]` -> the weapon section (invalid or non-positive
values fall back):

```
[inventory]
fault_fouling_rounds = 50     ; x condition_coeff / ((condition_shot_dec + 0.0001) * 1000) rounds to the first dirty fault
fault_fouling_repeat = 0.5    ; later dirty faults at this fraction of the interval
fault_deform_rounds  = 100    ; the original d2 numerator (bias 20 is built in)
fault_break_rounds   = 250    ; the original 5 x d3 numerator (bias 5 is built in)
```

`condition_coeff` keeps its original meaning - higher is more reliable - and the stock
`[default_weapon_params]` sets it to 3 for every weapon; the reliability upgrades that lower
`condition_shot_dec` stretch every interval as before. With the defaults: AK-74 195 rounds to
the first dirty fault (then every ~97), PM 188, SPAS-12 100, SVD 136; a deformation at 50 %
condition after ~640 / 620 / 360 / 470 rounds and never above 75 %; a breakage ~490 / 470 /
255 / 345 rounds after a deformation at 50 %. NPC weapons are untouched: the loot roll of
`items_condition.script` stays the only source of their faults.

Repair data is not part of this: the kits, their masks and condition floors, and the
mechanic's price are the stock ones. Two stock data gaps remain as they are and are only
noted here - `wpn_ak74u` and `wpn_wincheaster1300` carry no `condition_avail` and therefore
never fault, in the engine or in loot (an addon closes it with the AK-74 or shotgun mask), and
the cracked gas tube (id 24) is repaired by the mechanic only, so the rework never produces it
in play (loot still can).

The accumulators live in the `WFL1` chunk of the `.scov` sidecar (`SAVE_COMPATIBILITY.md`);
the original 0.98b ignores them and keeps the fault mask, an original save starts them at
zero, with a weapon that already carries dirty faults treated as one cleaning cycle in.
Read-only from Lua: `obj:get_weapon_fouling()` (rounds), `obj:get_weapon_fouling_ratio()`
(0..1 of the interval), `obj:get_weapon_stress()`, `obj:get_weapon_wear_progress()`. No
setters: a script cannot desync the accumulators from the mask; clearing bits through
`set_weapon_condition_type` resets the matching accumulators. `wpn_fault_dbg 1` (session
only, never saved) logs every shot's accumulators as `[wfault]`.

The misfire weights of the fault bits in `CWeapon::CheckForMisfire` are the Dead Air 1.0
values imported with the 1.0 mechanics (0.01 / 0.02 / 0.03 / 0.07 / 0.10 and 0.99 for the
broken firing parts); the x86 0.98b build used 0.03 / 0.05 / 0.15 for the lesser faults. That
difference is a 1.0 decision, not a porting defect, and durability never fed the misfire
chance in either build: misfires come from the fault bits and the ammo's `misfire_chance`.

## Screen-space contact shadows (r__sss)

A short depth-buffer ray march toward the sun in the near sun pass, giving contact shadows
to detail smaller than a sun-map texel (grass roots, small props). The strength follows the
quality preset - 0.6 on High, 0.7 on Maximum, off below - like the other preset-driven
switches; `r__sss 0..1` overrides it for the session, and the preset re-applies on the next
start. Tuning knobs: `r__sss_len` (ray length in metres, default 0.35), `r__sss_thick`
(assumed occluder thickness, 0.5), `r__sss_steps` (8). Limitations inherent to the
technique: only visible geometry casts, and the shadow fades at the screen edge. Ported
from the sibling open-source engine.

## Wind service and cloud deck

One wind for the whole world. The engine's environment keeps a single wind state - heading,
speed, a minute-scale trend, discrete gust events and a travelling gust field - ticked at a
fixed 60 Hz on its own clock, and every consumer reads that one state at its own position:
grass and tree crowns (each tree is a damped oscillator that follows the gust field with
its own inertia - it lags a tongue and overshoots a little after it), impostors, the wind sound, rain slant, particle systems, the
bullet drift, the actor and NPC movement, light physics bodies (a can or a box a gale can
out-pull from the ground), open water (wind waves stretched along the heading, calm air
leaves a mirror) and the cloud deck, which drifts with the wind aloft - the same log profile
that says a treetop feels more wind than the grass, evaluated at the deck's altitude.

The shader-side maths and the C++ maths are one header (`shaders/r3/da_wind_core.h`), so a
blade of grass, a particle and a cloud shadow agree on what the wind is doing at a spot.

Configuration lives in `configs/dead_air_x64_wind.ltx` (the compatibility archive; a mod
overrides it like any other config):

```ini
[wind_service]
z0 = 0.12          ; surface roughness in metres: 0.03 open steppe, 0.3-0.5 forest/village

[clouds]
altitude  = 1300   ; deck base above the level's ground, metres
thickness = 1200   ; deck thickness, metres (tiers 2-3 march through it)

[wind_profiles]    ; weather cycle name (exact, or the longest substring match) -> base 0..1
storm = 0.95
clear = 0.14
```

The cloud deck is a world object: one coverage field, rendered once per frame into a 16 km
map around the camera, is what the sky shows, what the sun passes shade the ground with and
what the sun shafts read. Coverage comes from the weather's `clouds_color` alpha - mapped,
because that alpha was the opacity of the old cloud texture and not a sky fraction: a "clear"
cycle gets scattered fair-weather cumulus, a storm a closed deck. The deck is drawn as a
full-screen pass over the sky pixels; the stock cloud dome (whose cap never reached the
zenith) is no longer used.

Quality follows the preset like the other preset-driven switches:

| Preset | Tier | The deck |
| --- | --- | --- |
| Minimum | 0 | flat: the map's column at the base plane, one fetch |
| Low | 1 | flat, plus a per-pixel rim and a one-tap sun probe |
| Medium | 1 | the same |
| High | 2 | volumetric: 14 steps through the slab at half resolution, temporal blend |
| Maximum | 3 | volumetric: 24 steps (more toward the horizon) |

The weather map's alpha carries the transmittance of the vertical column through the
volume, so the shadow the deck casts on the ground (and the break in the sun shafts) is the
shadow of the cloud that shows, and the lens flare and the sun sprite fade by the column
between the camera and the sun (read back from the map one frame late).

Lightning lights the deck locally: the discharge is a bent channel inside the slab - a line
a few kilometres long from where the line of sight through the bolt enters the deck (just
inside the base for a visible bolt, so the glow sits on the channel's top; in the body of
the slab for sheet lightning), its heading, length and bend rolled per discharge - lit like
a capsule, falling off over a few hundred metres from the line and scattered by the cloud
around it. The flash's share of the fog and sun colours is taken back out of the deck's own
light, and the deck keeps the sun's real direction while the engine lends sun_dir to the
bolt - so the clouds along the channel go white and the far deck stays as it was, instead of
every cloud brightening with the fog. The sky dome behind the deck gets the same treatment:
the compatibility archive carries `sky2.vs`/`sky2.ps`, which take the flash's uniform share
of the sky colour (thunderbolt.ltx `sky_color`) back out and light the dome in a glow
around the bolt, sixteen degrees wide, with a sixth of the stock share left everywhere else.
The bolt's own direction is read before the effect inverts it to stand in for the sun -
reading the inverted one had put the glow twelve kilometres away on the far side of the sky,
a broad far brightening instead of a burst around the channel. The effect applies nothing
on the frame it goes idle: the stock code still added the last flash colour after switching
state, so for one frame the sun colour carried a full-strength flash that the deck had no
record of - the whole deck lit by a noon sun, smeared over the next ten frames by the
temporal blend. The sun shafts (`sunshaftsgeneration.ps` in the compatibility archive) take
the sun's own direction, not the bolt's, so a discharge no longer sweeps the frame with rays
from its screen position. The glow's falloff is a Lorentzian core with an exponential skirt
(1.5 km), so a flash lights a few kilometres of deck around the channel and nothing beyond.
Strikes cluster around a storm-cell heading that wanders from bolt to bolt (the stock effect
kept every bolt in one narrow sector opposite the sun); two in five discharges stay inside
the cloud with no channel drawn - sheet lightning, anywhere in the sky, living 0.6-1.8 s
with two to five pulses of flicker, the flash of the scene following the same envelope. The
deck's sun light also follows the weather's authored sun strength now: under a thunder
cycle's near-black sun the clouds are lit by the sky alone.

The volumetric tiers march real volumes: a Perlin-Worley base shape carved by Worley
octaves, a height profile from stratus to cumulus chosen per cell, a Worley detail volume
eroding the edges (wispy at the base, billowy at the top), a light march toward the sun with
Beer-Lambert and three multiple-scattering octaves, a dual-lobe phase (silver lining, back
glow) and a powder term. The noise volumes are `textures\da\da_cloud_shape.dds` (128^3) and
`da_cloud_detail.dds` (32^3), generated by `tools\graphics\gen_cloud_noise.py` (numpy;
pass the output directory) and shipped in the compatibility archive. The march is jittered per frame and blended with the previous frame's
result reprojected by direction, so the cost stays at a few milliseconds on Maximum.

`r__clouds_quality -1..3` overrides the tier for the session (`-1` follows the preset),
`r__clouds_cover -1..1` pins the coverage, `r__cloud_map_dump` writes the weather map and the
march buffer (colour and transmittance) as PNGs next to the screenshots, `r__clouds_debug 1`
shows the deck's transmittance in place of the sky, `2` its raw colour, `3` the temporal
reprojection's offset; `r__clouds_temporal 0..0.95` is the share of the previous frame the
march keeps (it fades out on its own while the view turns). Tree shadows follow the sway on every preset but Minimum
(`r__tree_shadow_sway`); the sway itself costs nothing extra in the shadow pass.

Trees bend as one body. The sway is the crown's: height times the waveform (a static
downwind lean with harmonic oscillation around it, one phase per tree) times the tree's
response to the gust field, the gust lean on top. Who moves how far is the larger, softly,
of two weights: the trunk's cantilever profile ((h/H)^1.5 from the root, three quarters of
the crown's travel at the top - the tree's height rides in `c_tree` / instance row 9) and
the authored per-vertex flexibility (`tc.z`), so a branch card's stiff base rides the trunk
it grows from while its tip keeps the travel the model was made for; the total is softly
capped at half the height (~30 degrees). The leaf flutter rides the flexibility. Shot
wakes, blasts and presses are measured at the vertex, so a bullet through a bush shakes the
branches at the trace.

One wind state per tree. A tree in a level is several visuals on one root (the trunk, the
crown, sometimes more); the oscillator and the height are kept in one record per root that
every visual of the model shares (refcounted, integrated once per frame by the first visual
drawn). A state per visual gave the crown its own natural frequency and its own height
(its box starts at the lowest branch), and the crown visibly swung against its trunk. The
state stands IN for the gust field's instantaneous value in the shader - it never multiplies
it (that squared the lull-to-tongue contrast) - with a damping ratio of 0.3 (foliage damps
a crown hard; 0.06 rang for cycles and read as rocking), the natural frequency is
1.0/sqrt(H) Hz from the tree's real height, and the state starts at the field's value on
the first frame after a load rather than swinging down to it.

Water impact rings on all water. `Environment::water_hit` keeps eight impact spots (rings
and, for puddles, drains) that the puddle shader and the open-water shader read through one
header (`da_water_rings.h`): a bullet, a blast, a foot or a body makes the same ring on a
lake as on a rain puddle. Emitters: bullets (`Level_bullet_manager_firetrace.cpp`),
explosions (`Explosive.cpp` - the surface below or above the epicentre, since a grenade
sinks before it goes off; open water takes the splash and the ring and never dries),
footsteps of the actor, stalkers and monsters (`step_manager.cpp`) and physics bodies
(`physics_game.cpp` - the ring grows with the impact; characters are left to their
footsteps). Whether a spot is water, and where the ring goes, is one function
(`da_water_surface`): a liquid material, the rain puddle mask, or a liquid surface straight
above the spot - the water mesh is passable, so a foot on the lake floor and a body that sank
report the bottom's material, and the ring is placed on the surface above them. One ring per spot per quarter second, so a body sliding in makes one. A ring is a wave
packet: the front runs at ~0.9 m/s (a blast's bore at 4.5), the longest crest (~30 cm) leads,
shorter ones trail and die out behind it, nothing runs ahead of the front, and the crest thins
as the circle grows, fading out before it reaches its rim. The knobs are the `ring_*` lines
of `[water_impact]` in `dead_air_x64_water.ltx`. The cost is the
same loop the puddles already ran: a quiet world walks nothing, a busy one at most eight
rows per water pixel - no preset gate is warranted. Scripts can drive the actor's input
for a probe: `level.press_action(id)`, `level.hold_action(id)` (every frame, like a held
key), `level.release_action(id)`, `level.action_id("fwd")` (the binding names of the `bind` command);
`qa_water_goto` (console) puts the actor on the nearest shore of the level facing the water,
looking down at it, and `wind_dbg 1` logs every ring (`[water] ring`) and every body impact
near the camera (`[water] body contact`, with whether the spot counted as water).

Scripts read and drive the service through the environment object:

```lua
local env = level.environment() -- CEnvironment
env:wind_speed()                      -- m/s at 10 m, gusts included
env:wind_direction()                  -- heading, radians
env:wind_gust()                       -- 0..1 gustiness envelope
env:wind_at(pos)                      -- vector, m/s, 1.5 m above the ground at pos
env:wind_at_height(pos, h)            -- the same at a height of your choice
env:wind_exposure(pos)                -- 0 under a roof, 0.35 in the lee of a wall, 1 in the open
env:wind_blast(pos, radius, strength) -- a burst (an explosion): grass, particles and bodies feel it
env:wind_press(pos, radius, strength) -- a sustained push (rotor wash)
env:wind_freeze(true)                 -- pause the service clock (cutscenes)
```

Diagnostics: `weather_list` logs every weather cycle and effect the environment loaded (the
names `set_weather` accepts live in packed configs), `wind_dbg 1` logs the state once a second (with one watched tree's oscillator),
`wind_seed N` pins the random stream so a scene replays identically, `wind_force 0..1` pins
the base strength (`-1` releases it), `wind_freeze 1` stops the clock. `r__gpu_stats` dumps
the GPU timers (frame, scene, shadows, sun, lights, clouds, combine) once, `r__gpu_log N`
logs them every N frames, `r__screenshot_every N` photographs every N-th frame - the
headless QA rig uses the last two to grade a run without a window.

## First-person self-shadow

`r__hud_shadow` (on for the Maximum preset) gives the hands and the held item a shadow of
their own. From the sun it is a dedicated 1024-texel map around the eye (`hud_shadow.ps`).
Under local lights the first-person pixels are at least lit from the right side now: the
depth buffer is copied once after the g-buffer, a first-person pixel is told apart by its
depth slice and its position is rebuilt with the HUD field of view (the deferred
decompression put it about twice as far off-axis). The self-shadow of the hands is the sun's
alone; a screen-space march toward each local light was tried and removed - along a
silhouette it read as a dotted contact band. Off under MSAA, where the depth copy does not
exist.

## Actor movement tuning

The speed penalty a held weapon applies, and the overweight slowdown curve, used to be
literals in the engine. Both are now optional keys of the actor section
(`configs/creatures/actor.ltx`). Every key defaults to the value it replaced, so a config
that does not mention them behaves exactly as before.

```ini
[actor]
; sprint penalty, legacy branch: penalty = clamp(active item weight / divisor, 0, max)
sprint_weight_divisor    = 10.0   ; smaller value = heavier penalty per kilogram
sprint_weight_penalty_max = 0.5   ; the cap that makes everything above ~5 kg equal today
; sprint penalty, gear branch (active when sprint_weapon_koef/sprint_outfit_koef are set)
sprint_gear_penalty_max  = 1.5
; floor both branches clamp the sprint factor to
sprint_koef_min          = 0.3
; overweight = clamp((TotalWeight - max_walk_weight) * max_walk_weight * rate, 0, max)
overweight_slowdown_rate = 0.0015
overweight_slowdown_max  = 1.0
; walking, running and sprinting are all lerped towards this factor by the overweight value
overweight_speed_min     = 0.3
```

- The legacy branch is what a stock Dead Air config uses: `sprint_koef` is a multiplier
  over running speed, and the penalty comes from the weight of whatever is *in hands* -
  including a knife or a detector, and including the ammo loaded in the magazine.
  `sprint_weight_penalty_max` is why an RPD, a PKM and an SVD all slow the actor down by
  the same amount today; raise the divisor or the cap to make heavy weapons separate.
- The gear branch replaces that with an absolute sprint factor and only counts
  `CWeaponMagazined` weight plus outfit weight. It turns on when both `sprint_weapon_koef`
  and `sprint_outfit_koef` exist in the section (they must be defined together).
- Overweight is a separate effect and applies to walking, running and sprinting, so a
  heavy weapon slows the actor down twice: once through the inventory weight and once
  through the sprint penalty.
- Values are validated: a non-finite, negative, or (for the divisor) zero value falls back
  to the engine default instead of breaking the game. The section is re-read whenever the
  outfit changes, so scripted section swaps pick the new tuning up.

## Animation blend tuning

Animation transition timing can be added to `system.ltx` by an XMS config layer. Every key
is optional. Existing XMS layers and mod configs need no migration: an absent section or
key uses the engine default shown below.

```ini
[animation_blend]
min_time                    = 0.2
curve                       = smooth
fall_at_end_time            = 0.5
default_motion_accrue_time  = 0.5
default_motion_falloff_time = 0.5
movement_blend_fraction     = 0.2
```

| Key | Default | Meaning | Zero |
|---|---:|---|---|
| `min_time` | `0.2` | Minimum wall-clock accrue and falloff duration for normal cycles. Slower authored rates stay slower. | Disables the floor and restores authored timing. |
| `curve` | `smooth` | `linear` keeps the stored weight; `smooth` reads it through `3a^2 - 2a^3` before the existing normalization. | Not applicable. Use `linear` for legacy shape. |
| `fall_at_end_time` | `0.5` | Automatic falloff duration for stop-at-end hit channels, kept separate from the normal-cycle floor. | Invalid because the engine stores its reciprocal. |
| `default_motion_accrue_time` | `0.5` | Constructor accrue duration for a motion without an authored value. | Invalid because the engine stores its reciprocal. |
| `default_motion_falloff_time` | `0.5` | Constructor falloff duration for a motion without an authored value. | Invalid because the engine stores its reciprocal. |
| `movement_blend_fraction` | `0.2` | Fraction (`0..1`) of a root-motion clip used to interpolate its starting pose. | Disables that starting-pose interval. |

The curve changes only the pose weight at read time. The stored linear amount remains the
source for blend state, callbacks, eviction, and hit-channel logic.

For a stop-at-end cycle shorter than `min_time`, the engine relaxes both the incoming and
outgoing floor to the playable part of that clip (length minus its final sample), using the
speed known when the cycle starts. Authored rates that are already slower are still kept.
This prevents a missing/fast blend from outliving a short animation without changing a
deliberately slow authored transition. Later dynamic speed changes do not retime a blend
that is already running.

`animation_blend_min_time` and `animation_blend_curve` are persistent console commands.
The minimum applies when the next cycle starts or begins fading; the curve changes current
pose reads immediately. Both are saved in `user.ltx`, so a saved user value overrides the
XMS startup value. The remaining keys are startup config.

For a timing-and-shape parity check, use `animation_blend_min_time 0` together with
`animation_blend_curve linear`. The zero-rate underflow fix and deliberately corrected
same-skeleton transitions remain active.

Invalid, non-finite, negative, out-of-range, or zero reciprocal-time values fall back to
the defaults above and produce one startup log line per invalid key.

## Opting the installation out of online services

A mod that changes the build owns the installation: our automatic update would overwrite
its files with the payload of a version it never targeted, and a bug report from it would
describe someone else's game. A mod can therefore declare itself, by name, and the engine
will:

- never start the update check;
- drop the `Отправить bug report` entry from the main menu;
- print a red line at the bottom centre of the menu:
  `Автообновление отключено модами: <names>`.

The names come from the mods themselves and are shown in declaration order, deduplicated,
so a player (and we, in a screenshot) can always tell who took over the installation.

**From a config**, for a plain addon — `configs/dead_air_x64_mod_opt_out.ltx`:

```ini
[auto_update_opt_out]
weapon_pack     = "Оружейный Пак 2.0"   ; quotes required when the name has spaces
hardcore_tweaks = Хардкор-Твики
```

The key only keeps declarations apart; the value is the displayed name. A key with no
value declares itself by key. The same `[auto_update_opt_out]` section is also read from
`system.ltx`, which is what an XMS module should patch through `.ltxp` - that way two
modules can each add their own line without fighting over one file.

**From Lua**, available in the menu context as well:

```lua
main_menu.disable_auto_update("Оружейный Пак 2.0")
if main_menu.auto_update_disabled() then
    -- ...
end
```

Notes:

- Declarations are additive and deduplicated; declaring twice is a no-op.
- An empty or whitespace-only name is refused and logged - the list must stay meaningful.
- Config declarations are read before the menu appears, so the notice and the missing
  button are correct from the first frame. A Lua declaration takes effect from the moment
  it runs.
- Nothing else changes: the crash report prompt, saves, and every other menu entry keep
  working exactly as before.

## First-person animation scenes

The animation module (FDDA item scenes, skinning, body search, backpack, wear, parkour) is
described in [`ANIMATIONS.md`](ANIMATIONS.md). What a mod can plug into:

- **A new item scene**: an `item_ea_<name>_hud` section (`hands_position`, `item_visual`,
  `anm_ea_show = hands_cycle, item_cycle[, speed]`) plus an entry in `anims_list.ltx`
  (`anm`, `snd`, `cam`, `tm`) or an `ea_addon_*.ltx` file - the settings file includes them by
  mask. Hud sections go through the `dead_air_x64_animations.ltx` overlay path or any config
  merged into `system.ltx`.
- **A bag for a new backpack**: `[da_backpack_<section>_hud]:da_backpack_hud` with its own
  `item_visual`; tune the seat in game with `hud_scene_item_pos/rot/scale` and paste what
  `hud_scene_item_dump` prints.
- **A helmet or outfit equip scene**: `[da_wear_<section>_hud]` with `anm_ea_show`, `cam`,
  `snd`; `da_wear_anims.covers()` decides by the presence of that section.
- **Hand cycles for a new hands model**: add the model stem to `[da_hud_animations]`, or the
  scenes will stand still in that suit.
- **Scripted scenes of your own**: `game.play_hud_motion(hand, section, "anm_xxx", mix, speed,
  target_ms)` returns the length in ms (0 = nothing to play); `game.stop_hud_motion()` ends it;
  `game.only_allow_movekeys(true/false)` gates the input; `level.set_cam_custom_position_direction`
  owns the camera until `level.remove_cam_custom_position_direction()`.
- **Intent hooks**: `_G.da_register_before_item_use(function(npc, item, flags) ... end)` sees
  every use before it happens (`flags.ret_value = false` cancels it); `_G.da_before_inventory`,
  `_G.da_before_body_search`, `_G.da_before_wear` are single functions - wrap the existing one
  if you replace it.

Do not add spawnable sections for the sake of a scene: a section the original game does not
know ends up in saves. Every scene here is a hud section and an existing item.
