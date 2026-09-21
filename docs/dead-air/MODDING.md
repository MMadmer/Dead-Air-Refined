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
                     ; [vfs], [redirects] (see below); author/description/
                     ; website and [update] github for the Mods menu
                     ; (MOD_UPDATES.md)
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
- The main menu's Mods entry lists every module with its name, author and
  version, shows the description, opens the module's AP-PRO or ModDB page and
  updates a module from the GitHub releases its manifest names
  (`[update] github = owner/repo`). A release is always the complete module plus
  an index of its files; the game compares that index with the files it has and
  downloads, by HTTP range, only what differs - however many versions were
  skipped. The result is verified file by file while the game runs and swapped
  in by the next start, before modules mount. The manifest keys, the release
  layout and the client rules are one frozen contract: `MOD_UPDATES.md`. Console: `xms_update <id>|all`,
  `xms_update_status`, `xms_mods`.
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
  `xms.patrol_point(name, x, y, z)` registers a one-point runtime patrol path
  (`xms_rt_*`) at the navmesh cell nearest to a position, so the stock schemes
  walk to a place nobody drew a path for; it is level data, never saved.
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

## The engine is called XFined-Ray

The engine reports itself under that name, which changes a handful of names an addon or a tool
may have hard-coded. Nothing was removed: every old name still works, so a mod written against
the previous one keeps running.

| What | Was | Is | If you used the old one |
| --- | --- | --- | --- |
| Log banner and file | `OpenXRay ...`, `appdata\logs\openxray_<user>.log` | `XFined-Ray ...`, `appdata\logs\xfined-ray_<user>.log` | A tool that opens the log by name has to follow. The `_lua.log` and the profiler files carry the same prefix. |
| Engine settings file | `configs\openxray.ltx` | `configs\xfined-ray.ltx` | Still read: the new name wins when both exist, the old one is used when it is the only one there. |
| Engine string file | `configs\text\<lang>\openxray.xml` | `configs\text\<lang>\xfined-ray.xml` | Still recognised; either name counts as "the engine's own file" when the folder holds nothing else. |
| Lua accessor for it | `openxray_ini()` | `xfined_ray_ini()` | Still exported and returns the same thing. A public Lua name is never withdrawn. |
| Shader cache | `appdata\shaders_cache_oxr` | `appdata\shaders_cache_xfr` | The old folder is not read any more, so the first start after the update compiles shaders once. Nothing else uses it and it can be deleted by hand. |
| RenderDoc captures | `appdata\captures\openxray` | `appdata\captures\xfined-ray` | A diagnostic path only. |

Unchanged on purpose: the `xray` C++ namespace and the `xr*` module names, because those are
X-Ray rather than the fork; the upstream links in source comments; and the copyright headers of
the files that carry them. `docs/dead-air/UPSTREAM.md` says what the engine descends from.

## The quality preset owns the quality settings

There is one control for how the game looks at a given cost: the preset list on the basic video
page. The advanced page next to it carries taste - grass height, sun shaft strength, sharpen,
bloom, chromatic aberration, depth of field, technicolor, grain, lens dirt - and nothing that a
preset also writes. A row for a value the preset owns is a second control for one value,
and the player who moves it finds it moved back.

What follows from that, for anyone shipping a preset file or a menu page of their own:

- `rspec_minimum|low|default|high|extreme.ltx` is replayed in full every time the renderer comes
  up (`xrRender_apply_preset()`), not only when a preset is chosen. `user.ltx` executes before
  that and still carries preset-owned values from its own last save - below the `_preset` line
  that would have set them - so the file has to lose to the preset. The values it saves are
  corrected on the way out, so a `user.ltx` written by an older build heals itself after one run.
  A session override still works: the console command applies until the next start, as always.
- `xrRender_sync_preset_derived()` runs right after the file and has the last word on everything
  it names, so a preset file that carries a value from both worlds does not fight it. Five of
  those switches (shadow map size, AO technique, grass density and radius, visor droplets) used
  to be held back on renderer start because the menu also showed them; the menu does not any
  more, and they follow the preset like the rest.
- Antialiasing is one ladder: FXAA on the two cheapest presets, SMAA 1x from Default up, camera
  TAA on top of it on Extreme. `phase_combine` runs SMAA or FXAA and never both, so the FXAA
  checkbox that used to sit on the advanced page did nothing on three presets out of five; it is
  gone and `r2_fxaa` is a runtime command now, session-only like `r__smaa` beside it.
- `appdata\qa_autoexec.ltx`, the measurement hook, still executes last of all.
- Every node of the advanced page is still in `ui_mm_opt.xml` and `ui_mm_opt_16.xml`, including
  the ones the page no longer builds. An addon that ships its own `ui_mm_opt_video_adv.script`
  gets exactly the page that script asks for.
- `ui_mm_opt_main.script` belongs to the game, not to Refined, and it reaches into the advanced
  page for three controls by name (`texture_lod_track`, `tab_ao_opt`, `combo_ssao`). The page
  still builds those three - hidden children of the page rather than rows on the list - so the
  stock script and every addon copy of it keep working. They read and write their console values
  as before, which costs nothing: the options manager writes back only what the player changed.

## Colour grade and foliage saturation

The final combine applies a restrained, camera-like grade to the tonemapped value
(`da_grade()` in `shaders/r3/common_functions.h`, constants from `da_grade_params`):

```
r__grade_sat       0.9    ; saturation of everything
r__grade_green     0.8    ; extra factor for green-dominant colour (greens end at 0.72)
r__grade_olive     0.3    ; share of the green channel handed to red: foliage toward olive
r__grade_contrast  1.05   ; contrast around linear middle grey
r__foliage_vibrance 1.1   ; foliage albedo saturation (a plain lerp from luminance)
r__lod_sat         1.4    ; impostor saturation, kept in step with the crowns
```

The grade is part of the look, not a quality tier: `xrRender_sync_preset_derived` re-applies
these values on every start whatever the preset, and the console commands change them for the
session only (they are never written to `user.ltx`). A zero `da_grade_params` constant - a
shader that does not bind it - leaves the frame untouched, so archive shaders keep working.

Why these numbers. The foliage multiplier 1.6 came from the sibling engine, tuned for the old
dull textures; the HD set carries its own saturation and 1.6 on top read as plastic greens.
The grade values follow the current photoreal practice rather than taste: a grade applied at
20-60 % strength keeps highlight and texture detail, greens are what oversaturates first under
outdoor contrast, hue-preserving tonemappers (AgX, ACES) desaturate brights and are then
given a little contrast back to stay punchy.

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

[particle_wind]    ; particle effect name (exact, or the longest substring match) -> wind share 0..1
smoke = 1.0
spark = 0.35
flame = 0.0
```

Particle effects take the wind only when a `[particle_wind]` rule names them: the drift
replaces a particle's horizontal velocity, so a long-lived sprite that was never meant to
travel - a campfire flame, a muzzle flash, a ring on water - would leave its source and sail
off with the weather. An effect no key matches takes no wind at all; the longest matching key
decides (`anomaly` at 0 beats `smoke` at 1 inside an anomaly's smoke), and an exact effect
name wins over every substring. The share scales the wind the particles relax toward.

### Shader fire

A campfire's flame is not a sprite flipbook: the effect names listed in `[shader_fire]` of
`dead_air_x64_fire.ltx` are drawn by `CDaFireEffect` (`da_fire.ps`) as a flame volume marched
along the view ray on a camera-facing quad, with a smoke plume the engine simulates and
`da_smoke.ps` draws as eroded, lit billboards. The stand-in takes the effect's slot with the
effect's interface, so the campfire object, the group it plays and every script around it are
untouched; the sparks, the heat haze and the air drawn into the fire stay the sprites they were.

```ini
[shader_fire]        ; particle effect name -> preset section, or "off" to draw nothing
explosions\effects\campfire_flame = campfire
explosions\effects\campfire_glow  = off        ; the glow sprite the shader flame replaces

[shader_fire_campfire]
base_height = 0.45   ; metres above the effect origin where the flame starts (inside the barrel)
radius      = 0.32   ; fuel bed radius, m
height      = 0.8    ; calm mean flame height, m
smoke_rate  = 14     ; puffs per second; smoke = 0 for none
heat_kw     = 100    ; convective heat release, drives the plume rise
```

The flame is a handful of tongues rooted across the fuel bed, each a tapering column whose
surface the advected noise pushes in and out (more the higher it climbs, so a tongue is a
clean cone at its root and a ragged, breaking tip), with its own height and width breathing
with the puffing wave at `1.5/√D` Hz, wandering more the higher it climbs and leaning in
toward the axis; inside the surface the colour runs from the red edge to the white core by
depth, and the fuel bed below is the same thing lying flat. The axis leans by a saturating
law (18° at 2 m/s, 30° at 4, 45° at 8): the pool-fire correlations of the literature (AGA,
Thomas) lay a fire this size flat in a breeze and were tried and dropped. The smoke is a bent-over buoyant plume (Briggs): a parcel's lift decays as it cools,
horizontally it is the air of its own height (the service's log profile makes smoke aloft run
ahead of the flame) plus an Ornstein-Uhlenbeck wander, its radius grows by entrainment and
dispersion, and its opacity by dilution. The noise volumes are the cloud deck's.

Both shaders read the G-buffer depth themselves, so the flame fades into the barrel rim and
the smoke into walls; both are sorted with the other transparent effects and cut at
`r__particle_dist` like any sprite.

#### The fluid campfire

Close up, on the two top presets, the nearest such fire is not marched on a quad at all: it is
simulated. `CDaFireEffect` builds a 64x96x64 grid (3.5 cm cells, a 2.24 x 3.36 x 2.24 m box)
over the fire on the engine's 3D fluid subsystem, and the field the grid carries is
temperature, fuel, burn and soot rather than the stock single density. Fuel is not a blob in
mid-air: the level around the fire is voxelised once into the grid, and only a cell that rests
on a solid one - inside the fire's disc and near its bed - is given fuel and a pilot flame. So
the flame's base follows whatever the fire actually stands on, cell by cell, whether that is
the logs, the ground between them or a rooftop, and gas leaves each burning surface along that
surface's own normal, which is what rolls the flame up off the flank of a log instead of
standing it over the pile.

Each step advects the field (MacCormack), burns what is hot enough and has fuel left, adds the
buoyancy of the heat, the jet off the burning surface and the wind (a parcel takes the wind up
over a fraction of a second, and the closer to the bed the more the fuel shelters it), applies
vorticity confinement, and projects the velocity through a Jacobi pressure solve whose right
hand side carries the volume the reaction makes out of the wood. The simulation runs at a fixed
60 Hz whatever the frame rate, and at most two steps are spent catching up.

The volume is drawn by its own ray-cast: hot soot radiates as the fourth power of its
temperature through a four-band ramp (dull red tips, orange body, yellow, a white-yellow core),
only the soot that has cooled absorbs - so a flame is never hidden behind its own smoke - and
where a ray ends on the wood the gas a step back from it lights the embers there. Beyond
`r__fire_fluid_dist` the fire falls back to the marched flame, and the two cross over in half a
second rather than swapping in a frame.

```ini
[shader_fire_campfire]
fluid_base      = 0.12   ; the fuel disc's centre above the effect origin, m
fluid_radius    = 0.42   ; its radius, m (0 = radius * 1.15)
fluid_bed       = 0.35   ; how far above and below it a surface still counts as fuel, m
fluid_cooling   = 3.2    ; 1/s - this is the knob that sets how tall the flame stands
fluid_burn      = 4.5    ; burn per degree over the ignition point, 1/s
fluid_buoyancy  = 18     ; m/s2 on gas at the core temperature
fluid_emission  = 165    ; how hard the hot soot radiates
fluid_ember     = 1.1    ; the glow the hot gas leaves on the wood under it
```

#### The fluid blast

The same grid runs explosions. `[shader_blast]` maps a particle effect to a `[shader_blast_<name>]`
preset, and instead of a fuel bed that burns steadily, a sphere of fuel and heat is thrown into
the grid over the first tenth of a second and the whole thing lives a couple of seconds. The
ground under it is read as a grid of downward rays rather than a triangle sweep: a blast has no
time to pay for the latter, and it is the surface it goes off on that shapes it anyway.

What the charge gets at t = 0 is an outward push that grows with radius (a ball expanding
uniformly moves fastest at its rim) plus a turbulent velocity of its own, because a clean sphere
otherwise expands into a clean sphere and a real fireball breaks into lobes from the instability
of its own surface. Most of the expansion is not that push, though: it is a divergence the
reaction makes out of nothing, on its own clock - hard at the moment it goes off, decaying, then
slightly negative as the air rushes back in behind it. That last part is what folds a fireball in
on itself instead of leaving a ball that simply stops growing. Vorticity confinement is scheduled
the same way: nothing while the charge is still expanding cleanly, hard through the moments the
surface is breaking up, then down to a level that keeps the smoke churning without shredding it.

A charge that goes off on the ground cannot expand downward, so what it would have spent going
down it spends going outward along the surface, dragging the dust of that surface with it: that
is the ring that runs away from the base. It outlives the charge by about half a second.

Rendering differs from a flame in one thing that matters. A flame must not be hidden behind its
own smoke, so only cooled soot absorbs there; a fireball is optically thick while it burns and
what you see is its surface, so `fluid_absorb_hot` lets its glowing soot absorb as well and the
ball gets a lit face and a dark limb out of it. `fluid_emission_pow` sets how steeply emission
climbs with temperature - soot in the visible band goes as a very high power of it, so a blast
uses a steeper curve than a campfire and its cooler skin falls away instead of glowing evenly.
The brightness ceiling is on luminance rather than on each channel: clamping channels separately
drags a hot amber core to white and throws its hue away.

An explosion is hooked as a group, not as one of the sprites inside it, and the difference is
not academic: the same sprite gets played on its own elsewhere in the world - an anomaly in
Escape plays two of the barrel's - so hooking one sets off a fireball where nothing exploded,
and that fireball takes the grid away from whatever campfire the player is standing at. The
group gets a child of ours added to it instead, and the sprites that child makes redundant are
silenced inside that group alone.

```ini
[shader_blast_group]
explosions\explosion_barrel = barrel
explosions\expl_mushroom_01 = barrel   ; the helicopter
explosions\expl_vehichels   = barrel   ; and the cars

[shader_blast_mute]                    ; silenced inside those groups only
explosions\effects\expl_benzin_05              = 1
explosions\effects\expl_mushroom_glow00_barrel = 1

[shader_blast_barrel]
blast_radius     = 1.45   ; the sphere the charge is injected into, m
blast_lift       = 0.85   ; its centre above the effect origin, m
blast_duration   = 2.6    ; how long it lives, s
blast_inject     = 0.15   ; how long the charge keeps going in, s
blast_speed      = 10     ; outward speed of that gas, m/s
blast_divergence = 34     ; the expansion the reaction makes, 1/s at the moment it goes off
blast_ring       = 2.2    ; how hard it runs outward along the ground
blast_dust       = 1.6    ; and how much dust that tears up
```

The flash, the shock distortion and the flying debris are not in the mute list, so they still
play. A blast outranks a campfire for the one grid the frame can afford, but by a factor rather
than absolutely: something going off across the camp has no business taking the grid from the
fire the player is standing at. It costs about the same as a campfire while it is on screen.

The march itself runs at roughly half resolution and is filtered back up, which is what the
stock volume does too. Filtering a bright volume across a silhouette drags it half a texel over
whatever stands in front, and that is where a lit rim around everything inside the volume's own
outline comes from - a gun the smoke never reached, a blade of grass painted over instead of
covering the flame behind it. The stock edge detector does not catch it.

So the composite does not trust the filtered value. The full-resolution ray data knows exactly
how far this pixel's ray travels, and the four low-resolution texels are weighted by how nearly
their ray agrees with it: a texel that marched past the edge disagrees and drops out. Where none
of them agrees the ray is simply marched again here. Marching every pixel at full resolution
fixes it too and costs a third of the frame rate.

`r__fire_fluid` (preset ladder `0 0 0 1 1`) turns it on and `r__fire_fluid_dist` sets the range;
both are session overrides like the other render controls. Only the nearest eligible fire is
simulated - one grid is what the frame can afford, and it costs about 3 frames out of 60 with
the fire filling the screen. The log line the fire prints when it builds its grid gives the
number of cells the flame can take hold on and how far the fuel bed reaches above the effect's
origin, which is what `fluid_radius` and `fluid_bed` should be sized against.

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
march keeps (it fades out on its own while the view turns). Tree and grass shadows follow the sway in every
shadow pass, the sun cascades and the local lights alike; the sway costs nothing measurable there.

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
report the bottom's material, and the ring is placed on the surface above them. One ring per spot per quarter second, so a body sliding in makes one. A ring the field saw
born is the field's from its first step (see The ripple field); the analytic packet that covers
the rest runs at the field's own speed - or at ~0.9 m/s, a blast's bore at 4.5, with no field -
with the longest crest (~30 cm) leading, shorter ones trailing and dying out behind it, nothing
running ahead of the front, and it thins as about 1/r until it is too faint to see: it has no
rim and no lifetime. The knobs are the `ring_*` lines
of `[water_impact]` in `dead_air_x64_water.ltx`. The cost is the
same loop the puddles already ran: a quiet world walks nothing, a busy one at most eight
rows per water pixel - no preset gate is warranted. Scripts can drive the actor's input
for a probe: `level.press_action(id)`, `level.hold_action(id)` (every frame, like a held
key), `level.release_action(id)`, `level.action_id("fwd")` (the binding names of the `bind` command);
`qa_water_goto` (console) puts the actor on the nearest shore of the level facing the water,
looking down at it, and `wind_dbg 1` logs every ring (`[water] ring`) and every body impact
near the camera (`[water] body contact`, with whether the spot counted as water).

The stock hit set lays flat sprites on the water too - `hit_water_hit` / `_big`, a white ring
texture growing to metres, and the `hit_water_hit_distort` discs - and each of those is an
expanding ring with a lifetime, which is exactly what the ripple field replaced. They are listed
in `suppress_effects` of `[water_impact]` and skipped wherever they would play: as a child of a
material pair's group, as a group child's own child, or played by name from the game. The
mechanism is the engine's (`da_particle_suppress.h`, consulted by `ParticleGroup.cpp` and by the
game's own splash calls); the names are data, and the sprays around them are not in the list.

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

### Wind motors and tall vegetation

The motors (`da_wind_motors.h`: a press under a walking actor or creature, the wake of a shot, the
ring of a blast) drive two different rules.

Grass (the detail shaders, `da_wind_motors_bend`) keeps the original one: displacement = strength
times the vertex height above the tuft root, measured at the root, so a tuft moves as a whole; the
press footprint is boot-sized (sigma 0.3 m), the shot wake a hand's width (45 cm cut, 16 cm sigma)
measured to the vertex height.

Wood is not a plant. Every multiple-use model compiles to the tree vertex format, and
`flora\trunk_wave` dresses stumps, logs and snags as readily as the trunk under a crown; the
`tc.z` "flexibility" the shaders read is the compiler's height fraction (0 at the bottom, ~1 at
the top of any model), not an authored property. So the wind, the flutter and the motors are gated
per root: a root that carries at least one alpha-tested `leaf_wave` visual is a plant and bends
(trunk included); a `trunk_wave` visual with no foliage sibling is wood and stands still
(`FTreeVisual::rigid`, `c_tree.w`; `-wvdbg` lists every level kind with its foliage count).
`def_shaders\def_objects_lod` (vehicles, most props) never waved: it compiles to the static
`tree_s` vertex program.

Tree-shader geometry (`flora\leaf_wave`, `flora\trunk_wave` - crowns, trunks and Dead Air's
bushes, which are tree models) takes `da_tree_motors_bend`. The grass rule did not carry over: a
boot flattens a blade of grass but a shoulder does not fold a branch, and a crown card is metres
wide where a tuft is a hand - a footprint narrower than the card tore it into spikes, and a lever of
the card's height threw it out by metres (the "lens" a trunk showed when the actor stood against
it). Two regimes, blended on the model height (`c_tree.x`, the tallest part sharing the root: one
under 3 m, the other over 4.5 m):

- A *bush* is one plant. A press and a blast are measured at the root with the canopy radius
  (0.4 x height) taken off the distance: a body inside the bush leans the whole bush away from
  itself (0.7 x height x flexibility, the usual 0.5 H cap) and every card moves together. A shot
  through the canopy at card height shivers the whole bush (up to 0.8 x min(height, 1.2 m) x the
  wake strength), softly in the vertical - the vertical width is 0.35 x the plant's height - so a
  card a metre tall does not tear.
- A *tree* has a stiff trunk, and a body reaches no higher than it stands. A press moves only the
  flexible foliage - authored flexibility (`tc.z`) above 0.3, so trunks and thick branches stay
  put - within 2.2 m above the presser's feet, fading out by 3.2 m, with a body-wide footprint
  (full within 0.35 m of the body axis, sigma 0.45 m beyond) and a bounded travel of
  0.22 x min(height, 2.2 m) x strength. A shot gives the low foliage a gentle wide shiver (sigma
  0.45 m, 0.25 x min(height, 2.2 m), nothing above six metres). A blast bends the whole tree from
  its root through the authored flexibility, one distance and one phase for every card, the way
  the wind does.

Bullets and bushes are a separate matter and unchanged: Dead Air's `materials\bush` has a shoot
factor of zero and no density, so a bullet passes through untouched (no speed or damage loss) while
the material pair still plays its hit sound and particles.

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

First-person rigs - the hands, the attached weapon and the scene items of the animation
module - are exempt from `min_time`: their cycles are authored with their own timing (a shot
cycle snaps in at once, as in the original), and the floor turned every shot into a crossfade
shorter than itself, with the bolt barely moving. The curve still applies to them.

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

The animation module (FDDA item scenes, skinning, pickup, backpack, wear, parkour) is
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
  target_ms[, start_ms])` returns the length in ms (0 = nothing to play); `game.stop_hud_motion()`
  ends it; `game.only_allow_movekeys(true/false)` gates the input;
  `level.set_cam_custom_position_direction` owns the camera until
  `level.remove_cam_custom_position_direction()`. `start_ms` begins the cycle part way in, which
  is how a reversed scene picks up at the pose the other one reached instead of snapping back.
  `game.hold_hud_motion()` clears the scene clock so the scene stands until you stop it - for a
  scene that lasts as long as a window, not a fixed length - and `game.scene_active()` says
  whether one is standing, which is the only way a fresh session can tell that a held scene
  outlived the script state that put it there.
- **Intent hooks**: `_G.da_register_before_item_use(function(npc, item, flags) ... end)` sees
  every use before it happens (`flags.ret_value = false` cancels it); `_G.da_before_inventory`
  and `_G.da_before_wear` are single functions - wrap the existing one if you replace it. The
  body search and the containers have no hook: they open the stock windows at once.

Do not add spawnable sections for the sake of a scene: a section the original game does not
know ends up in saves. Every scene here is a hud section and an existing item.


## Open water

Water is solved, not authored. The engine measures the level's water bodies once at load, works
out what sea the current wind has raised over the one the player is standing at, and hands the
shader physical quantities; nothing about the look is a number somebody eyeballed against a
screenshot, and there is no per-weather water colour to get wrong.

### What the engine solves

At level load `measure_water_body()` (`Level_load.cpp`) sweeps the static collision for the liquid
game material, bins the triangles into a 32x32 grid, flood-fills that into connected sheets, and
keeps the eight largest with their own footprint and their own measured depth (one downward ray
per occupied cell, from a real liquid centroid rather than a cell centre, so dry bank does not
drag the mean to nothing). One box around every puddle on the map would hand a two-metre pool the
fetch and the depth of a lake, and fetch is most of what decides how big the waves are.

Every frame, next to the wind service, `CEnvironment::water_tick()` picks the body nearest the
camera and solves the fetch-limited relations:

```
U    = 3 s low-pass of the 10 m wind      // a sea has inertia; a gust does not resize it
C_D  = 0.001 * (1.1 + 0.035 * U)
u*   = U * sqrt(C_D)
X    = the body's footprint projected on the wind heading, clamped to [5, 500] m
Hs   = 0.0413 * u* * sqrt(X / 9.81)       // significant wave height
lam  = 0.0898 * (X * u*)^(2/3) / 9.81^(1/3)
mss  = 0.003 + 5.12e-3 * U                // Cox & Munk 1954, mean square slope
```

`qa_water_state` prints all of it - the bodies, the sea, every wave row and the optical profile.

### Waves

Eight rows of `(heading, wavenumber, amplitude, phase)`, wavelengths geometric down from the peak
with ratio 0.75, re-seeded only when the wind has genuinely moved (20 % in speed or 15 degrees in
heading) with the phase carried across so the surface does not jump.

The amplitudes come from the **slope** budget, not from the wave height: the surface is drawn as a
normal and never as geometry, and `mss` is the slope variance of the whole real surface. The
explicit waves take half of it, the detail normal map takes `WATER_DETAIL_SHARE`, and what is left
is the width of the specular lobe. Bands shorter than 10 cm are not generated at all - no pixel
of this surface can resolve them at any distance, so all they can add is aliasing, and dropping
them hands their share of the budget to the bands that survive.

Nothing displaces geometry. At Zone fetch and Zone wind every wave is 0.5-6 cm high and 7-60 cm
long, which is smaller than a vertex and smaller than a texel. For the same reason there are no
whitecaps: fetch-limited steepness never folds the Jacobian, and Monahan's coverage is 0.09 % at
5 m/s over the open ocean.

### Optics

`dead_air_x64_water.ltx` carries the profiles: `sigma_t` is the per-channel extinction in 1/m and
`body_r` is what the water column glows, both linear sRGB; `scum` is the floating film; `fetch_max`
caps the fetch the wind gets over that water - the solver measures it off the body's footprint,
and a marsh's footprint is a lattice of reed islands, half a kilometre by the box and tens of
metres by the water, while the fetch is most of what sets the wave length - and `wave_damp` scales
the slope variance of everything the wind raises, waves, detail ripple and glitter alike. Six ship -
`clear`, `pond`, `swamp`, `bog`, `muddy`, `algae` - and `[water_levels]` maps each level to a pair
of them, because a level may carry two water materials at once. Which of the two a surface reads
is fixed in its own `.s` script (`water_soft` = the first, `water_green` = the second): the lua
shader API has no way to hand a material its own constant.

A level not listed there takes the `default` line, and a missing file or a missing key each falls
back one step without failing the load.

To retune a water body, edit its profile. Numbers to start from: red is half gone at 2 m and dead
at 8 in anything, blue survives tens of metres in clear water and dies in 20 cm in a peat bog, and
past about `a_CDOM(440) = 0.5` the blue coefficient exceeds the red and the water goes brown
rather than blue.

Below half a metre a second of wind the surface is a mirror. Cox-Munk's 0.003 intercept is the
ocean's residual swell, which a pond does not have, so both the CPU budget (`water_mss`) and the
shader (`da_w_mss`) take the threshold off the wind and fade the law in over the next metre a
second - with the gust field applied before the threshold, which is what draws cat's paws in a
light air: the lulls go glassy, the tongues ripple.

### The water field

Every water feature past the surface itself needs the same answer - is there water at this world
XZ, at what height, over what bed, and how far is the nearest bank - and water surfaces are static
level geometry, so the answer is baked once at load rather than solved per frame.

`measure_water_body()` in `Level_load.cpp` rasterises the liquid collision triangles into a
1024x1024 grid over the level's bounds, takes the bed from `CDetailManager`'s 2 m slot grid, and
runs a chamfer distance transform for the shore distance. It is uploaded as `$user$water_field`,
an immutable RGBA16F texture: **R** water surface world Y, **G** coverage, **B** bed world Y,
**A** metres to the nearest bank, clamped at 32.

Shaders read it through `da_water_field.h` (`da_wf_depth`, `da_wf_shore`, `da_wf_here`), mapped by
the `da_water_map` constant. Gameplay reads the same numbers off the CPU copy through
`CEnvironment::water_at` / `water_surface_at` / `water_shore_dist` - O(1) table lookups, safe every
frame per actor. `qa_water_state` prints what got baked.

The same sweep bakes the **puddle fill map** (`$user$puddle_fill`, R16F): a Planchon-Darboux fill
over the same terrain heights, so a puddle lands where water would actually pool instead of where
a noise function happened to cross a threshold. The noise still shapes the edge; the fill decides
the place. `r__puddle_fill` is its ladder column, and with it off the placement is exactly what it
was before.

### The ripple field

A 32 m window centred on the camera, on every preset - the preset buys texels into the window,
not the window itself: 256 on Minimum and Low (12.5 cm texels), 512 on Default and High (6 cm),
1024 on Ultra (3 cm) - stepped by `phase_water_ripple` at a fixed 1/30 s with an accumulator.
The state is one RG32F texture, `$user$water_ripple0`: R the height in metres, G its vertical
velocity.

The solver is spectral - Tessendorf's eWave. `da_water_ripple.ps` does what has to happen in
real space (the window shift, the losses at the rim and on dry ground, the sources) and packs
height and velocity as one complex texture; two FFT passes (`da_ripple_fft.h`, a radix-2
Stockham in compute, one thread group per line) take it to Fourier space; `da_ripple_propagate.cs`
advances every wavenumber by the exact solution of its own harmonic oscillator,
`omega^2 = (g k + sigma k^3 / rho) tanh(k D)` - gravity, surface tension and the body's mean
depth, the full dispersion relation of water - with a loss `exp(-(gamma0 + nu k^2) dt)` that
takes the short waves first; two more passes bring it back, and the result is copied into the
state under the one name every reader binds. There is no Courant number and no wavelength the
grid runs at the wrong speed: every wave the grid can hold moves at exactly its own speed, so a
bullet's ring is what it is on a pond - a train whose long waves run out ahead of its short
ones, spreading as it goes, three crisp crests a hand apart behind a faint swell.

That is the fourth solver this field has had, and the first three are why it is spectral. A
leapfrog stencil has one speed for everything it carries: pinned at a pretty Courant number it
ran rings at 3.75 m/s and they crossed the window in four seconds; tied to the texel it made
Ultra's rings the slowest; split into three octave bands it drew three crests moving as blocks,
and the energy shared between them by amplitude instead of by octave went nine tenths into the
gentle long waves. The references that settled it were a pistol and a rifle fired into a pond.

An impact is a cavity: four and a half centimetres of radius per metre of the slot's reach (6 to
7 cm for a bullet or a boot, growing faster past the two metres only a blast reaches), dug one and
a half times as deep as it is wide between 5 and 35 cm, as a ring whose radius runs out at 1.5 m/s
while it is fed - the collapsing cavity throwing its rim out, which is why a real splash's ring
starts fast and only then settles to the speed of its waves. The cavity's size is all that decides
which wavelengths the ring is made of; the solver does the rest. The front of a ring past the
window's edge is continued by the analytic layer at `water_ripple_speed`, the group speed of the
waves that lead a pistol's train.

A wading foot is a travelling pressure footprint, not a source of height. `CStepManager` feeds
each foot's own velocity through the water (finite-differenced on its bone, smoothed over a tenth
of a second: the planted foot pushes nothing, the swinging one does) into one wake slot per foot,
keyed by the source's id so a walker's two feet never fight over a slot, with a strength that is
the leg's immersion measured from the SOLE - a decimetre under the foot bone on these skeletons.
That last detail is not a nicety: in shin-deep water the foot bone rides above the surface for
most of a stride, and a feed gated on the bone touched the water only in the instants a bone
dipped under, at stance speed, which the rig showed as a 3 mm dent behind a walker doing 2.7 m/s.
`qa_water_state` prints the slots and the feet as the engine sees them. The sim relaxes the
surface inside the leg's radius (`wake_radius` in `[water_actor]`, 10 cm) onto the trough Bernoulli
sets for that speed - the mean pressure coefficient round a cylinder is -1, so the water over the
leg stands `v^2 / 2g` below the pond, 5 cm at a metre a second, capped there - and its vertical
velocity onto the trough's own rate of change as it travels, a kinematic condition on both state
fields the way an obstruction is in eWave (relaxing the height alone does not hold a trough on an
oscillator: the solver answers every pull with velocity, and the rig showed a 3 mm dent carrying a
fifth of a metre a second). Everything else comes out of the solver: the bow wave, the V of the wake
behind, and the ring a foot leaves when it stops and the trough springs back. On a grid coarser
than the leg the trough is spread over the texel and made shallower by the same ratio, so it moves
the same water on every preset. The version before pushed a half-metre bump up and down at a
made-up 1.7 Hz; a bump that wide holds nothing at the wavelength that frequency radiates, so it
breathed and made no wave, which is what "no rings under my feet" was.

The wave rows the surface sums are bands times headings, not one sinusoid per band. The bands
that fit between the peak and the 10 cm floor are few at a short fetch - one or two over a marsh -
and one sinusoid per band is corduroy: parallel crests marching in step. The surplus rows re-draw
the same band at other headings across the wind, which is what a young sea's wide directional
spread is, so the eight rows are eight crossing trains whatever the fetch.

Four things about it are not preferences, and each is a way this feature is usually built wrong:

* the step is **fixed**, or the ripple speed tracks the frame rate;
* the window snaps to **whole texels** as the camera moves, or every frame resamples the field
  into mush;
* dry ground is in the field too, with a puddle's losses: a wave that runs up a bank dies there
  in a second instead of reflecting, which is what a beach does;
* the damping ramps over the outer 4 m of the window (`water_ripple_edge`, the same metres on
  every grid), or every wave echoes off the invisible rim - and the spectral domain is periodic,
  so without it a ring leaving one side would come back in from the other.

Sources are injected inside the same pass from constants: the eight impact slots
(`Environment::water_hit` - bullets, blasts, footsteps, bodies), the wake slots
(`Environment::water_wake`, one per moving foot, refreshed every frame), and rain at a rate
derived from mm/h on the top tier. An impact is a **depth in metres spread over a number of
steps**, not a per-step amount: feeding a one-shot impulse once per step drives the whole field
into its clamp inside a tenth of a second. A wake is a **target the surface is relaxed onto**,
over a fixed settling time, which is what a pressure source is. The slot's radius says how far a
ring is meant to run, not how big the splash was: the cavity is derived from it.

The field runs over the whole window, dry ground included: a rain puddle is water too, and its
rings live in the same field as the lake's (`da_puddles.h` reads it through `da_wf_ripple_slope`,
bound in `uber_deffer.cpp` and `da_puddle_refl.s`). Rain is seeded only onto the water body; a
puddle's drops are already rings of their own, at a scale the grid cannot carry.

The eight analytic rings in `da_water_rings.h` cover only what the field cannot, and decide that
per ring. A slot records whether the field saw the ring born (`SWaterHit::crater`, the par row's
`w`): a ring the field carries is drawn analytically only past the window's edge, over the same
4 m the sim absorbs it in and at the same speed, so the front stays one front; a ring born outside
the window has no wave in the field at all and is drawn everywhere, window included - without that
a ring watched from the bank vanished the moment the window walked over it. The analytic envelope
has no lifetime either: it thins as about 1/r and the slot is freed when it is too faint to see,
tens of metres out.

### Underwater

`da_water_under.h` carries the medium; `combine_2_naa.ps` **and** `combine_2_aa.ps` apply it, in
both files deliberately - only the non-AA one carries the post stack, and that asymmetry has
already cost this project one silently dead feature. The tier ladder is `r__water_underwater`:
1 tint, 2 adds the distance fog, 3 adds the warp, 4 adds the vignette and the edge separation.
It ramps over the first 20 cm of submersion, and `water.ps` crosses over on the same ramp, so the
surface and the medium change sides together.

Seen from below the surface uses the **exact** Fresnel with total internal reflection past
48.607 degrees, which is what makes Snell's window: outside it the surface mirrors the bed, inside
it the sky refracts through. That needs the water pass two-sided, which is `dx10CullMode` in the
three `.s` scripts.

Caustics are not a pattern of their own. The bed is lit through the thin-lens relation
`1 / |1 + (1 - 1/n) d lap(h)|` with the Laplacian of the very surface `water.ps` draws - the wave
rows, the detail normal map at the surface's own amplitude law, and the ripple field
(`da_water_caustic.h`) - so the net moves at the speed of the waves the eye sees, its cells are
the detail layer's ten to thirty centimetres, a calm has no net, and a ring casts an arc.
Turbidity is not a blur by wavelength but a contrast, `exp(-b d)` with the scattering share of the
profile's extinction.

Where it is applied is the part that was learned the hard way. The obvious home is the sun
accumulator - modulate the sun's term and the net vanishes in shadow for free - and two versions
lived there without ever lighting a bed: the level compiler bakes the terrain's sun occlusion
with the water surface as an occluder, so a bed under water has no dynamic sun in the deferred
lighting to modulate, only the plants standing in it have. `water.ps` therefore applies the lens
to its own refracted background, weighted by the direct sun's share of the light on that patch
(hemi plus sun by the surface's baked access - a pond under a canopy throws no net), and the
medium pass in `combine_2_*.ps` does the same to the frame while the eye is under the surface.
`r__water_caustics`, from Default.

**There is deliberately no swimming and no drowning.** Deep water in this game is level-design
geometry - a barrier - and making it swimmable lets the player cross what a map means as a wall.
Wading is implemented in full.

### Rain

Everything is parameterised on one number, the rain rate `R` in mm/h, solved from the weather's
`rain_density` (`CEnvironment::RainRateMmh`). Drop count, streak length, splash rate, puddle fill
and the extinction folded into the fog all come off it, so they cannot disagree with each other.
`rain_density` itself is untouched as the authored input and as what `level.get_rain_volume()`
means to the dozen scripts that read it.

`r__rain_quality` is the ladder: oriented splashes from 2, streak lighting from 3, rain into the
ripple field at 4.

### The visor

The water standing on the actor's mask is a field with a memory, exactly as the ripple field is:
one RGBA16F target at the screen's own aspect (`$user$visor_drops0`, 256 to 512 across on the
preset ladder, `r__visor_drops`), stepped at a fixed 1/360 s by `phase_visor_drops` and read by the
combine. R is the water's thickness in millimetres, G the film a trail or a wipe left behind, BA
its velocity in millimetres a second. The effect rides `r2_lenswater` - the options checkbox and
the ladder that already existed - and costs nothing when it is off, target included.

What the solver does is what a drop on a pane does, and each of the four is a thing the lattice
this replaces could not express:

* **it stays where it lands.** Contact angle hysteresis holds it: the advancing edge wants a
  steeper angle than the receding one and the difference is a force along the contact line, so
  the criterion for sliding is a SIZE, not a wait. The Bond number crosses one at a radius of the
  capillary length times the root of the critical Bond number - `sqrt(gamma / rho g)` is 2.728 mm
  and Bo_c is 0.15 to 0.35 on glass, giving 1.1 to 1.6 mm, which is exactly where pipetted drops
  begin to run on a tilted slide. A cap that wide is about that tall, so the threshold is read
  here as a thickness (`DA_VD_PIN`), divided by the share of gravity the plate feels;
* **it grows and swallows its neighbours.** Rain keeps arriving and two drops that touch are one
  drop. In a field that is addition, which is the whole reason this is a field;
* **it beads.** Water on a plate does not spread, it DEWETS: thin water drains into thick, which
  is why a window in the rain is a field of round beads and a few narrow channels rather than an
  even sheet, and why every bead has a rim. Without it the field only transports and diffuses, so
  drops land as beads and immediately relax into wide flat smears - which is exactly what "they
  look like they have already been wiped" was. It is two terms, and it took both. The DEWETTING
  is a target velocity up the thickness gradient, the speed the water on a flank relaxes to the
  way it relaxes to its running speed under gravity, carried by the same conservative flux, read
  over a baseline of half a millimetre each way so that it is blind to anything grid-sized, and
  braked as the water it climbs into reaches the height a bead stands at - above which the
  surplus spreads back to its neighbours. The CAPILLARITY is the flow the surface's own Laplace
  pressure drives, from where it is convex to where it is concave: a fourth-order term whose
  equilibrium is the spherical cap, and the only thing in the field that knows what ROUND is.
  Its mobility is the water's and goes to zero on dry glass, so it rounds and smooths a bead but
  cannot spread it. Three wrong versions preceded these. Mass moved directly between neighbours
  is anti-diffusion, and the mode it grows fastest is the checkerboard - every drop filled with
  crawling speckle inside a second. A pull ADDED to the velocity every step is an acceleration
  balanced only by drag, at slope times 1500 mm/s, saturating the Courant limit for any slope and
  overshooting the brake by its own momentum: every bead a crater, its flank standing taller than
  its middle. And a dewetting pull without capillarity grows beads as staircases along the grid's
  axes, because a scalar climb knows nothing of shape. The capillary step has a hard ceiling too:
  the five-point Laplacian's eigenvalues reach -8, the biharmonic's their square, and an explicit
  step is stable only under a THIRTY-SECOND per step - at a twenty-fifth the checkerboard grew,
  went negative, was clamped, and the clamp minted water until half the glass was a millimetre
  deep;
* **once it runs it leaves a track, and the next drop follows it.** A sliding drop cannot take all
  its water: it leaves microns behind, that film is already wet, and wet glass pins worse - so the
  next drop veers into it. Rain on a window runs in a few channels for this reason and it is two
  lines here, a threshold lowered by the film and a direction biased toward the wetter side;
* **it moves in jerks.** Nothing implements stick-slip. A drop that runs thins, falls back under
  the threshold, stops, is fed until it is over it again, and goes.

**How the water moves is the whole feature, and it took three attempts.** The natural thing to
write in a pixel shader is a backward trace - what is standing here now is what was one step
upstream - which is what the ripple field does and is stable and cheap. It is also not
conservative, and on a field of discrete drops that fails in two stages. First, dry glass ahead of
a drop holds no velocity, so it traces back to itself, finds nothing and stays dry: no drop can
move onto glass that is not already wet, and every drop sits where it landed however heavy it
gets. Then, when the velocity is made to reach past the water so the glass ahead knows what is
coming, the leading edge gains without the trailing edge losing - a semi-Lagrangian trace
preserves the value it samples, not the mass - and where the velocity has divergence, which here
is everywhere, water is quietly created. The rig showed it as the glass filling to half covered
while the pattern never shifted by a single texel.

So the transport is a **flux**: each cell gives a share of what it holds to the neighbour its own
velocity points at and takes the shares its neighbours send it, momentum riding along with the
mass so that a fast drop swallowing a slow one comes out at the weighted speed of the two. What
one gives is exactly what another gets. The price is the Courant condition - water may not cross
more than one cell per step - and that is why the step is 1/360 s and why the grid stops at 512
across: a finer grid needs a proportionally shorter step to carry the same drop at the same speed.
The Courant number itself is 0.95 rather than something comfortable, because an upwind flux's
numerical diffusion goes as `CFL (1 - CFL)` and vanishes at both ends; at 0.8 a drop spread to
twice its width in a second of running and arrived at the bottom of the glass as a streak with no
head at all.

Two things pull the water and only using one of them is why the tracks used to run dead vertically
whatever the weather. **Gravity**, the world's, resolved into the plane of the glass: the visor
turns with the head so the drops are still in screen space, but which way is down on it is not -
look up at the sky, the plate goes horizontal, the in-plane component goes to zero and the drops
stop running and just sit. And **the air**, because a drop standing on the glass is in the
airflow and the drag on it is not small: a two-millimetre drop in five metres a second of relative
wind feels about as much push as it does weight. Only the component ALONG the glass counts - air
arriving square on presses the drop against the plate and moves it nowhere - so walking straight
ahead with the head level changes nothing, a crosswind lays the tracks over, and looking down while
running drives them up the visor. The relative wind is the weather's minus the eye's own travel,
differenced from the camera in the phase rather than asked of the game, so a sprint through still
air slants them too. Drag goes as the square of it, and the two are capped together: a gale may lay
the tracks flat but it must not drive them at twice the speed gravity does, because past that the
water is asked to cross more cells than the flux can send.

Arrivals are a lottery per 4 mm cell per step at a rate from `r2_lenswater_val`, read as a RATE
and not as a level. That one word is most of the old effect's problem: the driver walks that value
by 0.03 once a second, and a level that steps is a picture that steps, while a rate that steps is
a field that does not. A drop hits the visor at terminal velocity with a Weber number over a
thousand, an order past the splashing threshold, so it does not settle into a neat cap - it throws
three satellites.

`da_visor.h` is the optics. A drop two centimetres from the eye is a lens, and that decides
everything: it **inverts** (a cap reaches 45 degrees of tilt at its rim and a ray bent that far
crosses the axis - every photograph of rain on a window has the world upside down inside the
drops, and no amount of pinching the UV toward the drop centre can produce it, because a magnifier
does not cross the axis); its **rim goes dark**, because past the critical angle of 48.61 degrees
nothing leaves the water at all - the transmission near it is `saturate(7.68 * (|N.z| - 0.6612))`,
two constants for the ring every real drop carries; it is **brighter than what it covers**, since
it gathers about 165 degrees of the world - the sky included - into the solid angle it hides; and
it is **badly out of focus**, because the blur circle of something at the visor subtends
`D_pupil / distance` while the drop subtends `d_drop / distance`, the distance cancels, and a drop
is resolvable only when it is wider than the pupil. Ordinary drops are not, which is why a rainy
visor photographs as soft bright blobs with structure only in the big merged runners. The physical
blur is about a seventh of the screen height; a third of that is taken, because the honest figure
erases the effect.

A **wipe is a sweep, not a switch**. `visor_wipe delay duration` starts it, and both numbers come
from `dead_air_x64_visor.script` because both belong to an animation and an animation is data. The
trigger is the moment the cleaning item becomes the ACTIVE item and not the moment it is created:
the game has to put away whatever was in the hands first, and firing on creation wiped the glass
while the player was still watching a rifle being holstered. The delay is what is left of the draw
animation before the hand actually reaches the glass. The sweep runs one way and only one way -
it follows an animation, and that animation goes the same way every time; alternating it, which is
the obvious thing to do so that two wipes do not smear alike, had the glass being wiped against
the arm the player could see half the time. Over the better part of a second the hand crosses the
glass: the water just ahead of the edge is shoved along and piles into a bead
that rides in front of it and runs off the far side, what the edge has passed keeps six per cent
of its water as a smeared film streaked along the way the hand went, and what it has not reached
is untouched. It never clears to zero - a wiped visor is not clean glass, and the couple of per
cent left is exactly what makes it read as wiped.

**The look is set by four numbers and they trade against each other**, all in
`da_visor_drops.ps`. `DA_VD_FLUX` is how much rain arrives. `DA_VD_PIN` is how readily a bead lets
go - and it, not the rain, is what decides how WET the glass gets, because water leaves a visor by
running off far faster than it leaves by drying, so the equilibrium is set by how easily it runs;
it has to sit UNDER `DA_VD_BEAD_H`, or nothing short of a wide merged pancake ever runs and the
glass has no tracks. `DA_VD_BEAD_H` is the height the water pulls to, and for a given amount of
water it decides how much of the glass is covered: the same water in beads half as tall wets
twice the area. `DA_VD_RAD_MAX` is the biggest arriving drop, and drops that arrive near the
critical radius run almost at once and take their water off the bottom with them. Measured on
the rig in a downpour at the end of this: a quarter of the glass wet and holding, round beads
across all of it, and about one per cent of the water running in narrow channels at 90 mm/s.

**The flux carries the corners.** A cell sends fx (1 - fy) along x, fy (1 - fx) along y and
fx fy to the corner between them - corner transport upwind - and not fx along x and fy along y.
Sent along the axes only, a drop running diagonally leaks a share sideways at every cell it
crosses and the leak compounds: a hundred cells on, the drop is a triangle ten cells wide with its
own water spread over it microns deep. A storm lays every track over, so in a storm every track
ran diagonally, every one became a cone that widened as it fell and thinned past seeing, and the
glass held nothing visible however hard it rained. With the corner share a diagonal run is carried
exactly at the Courant limit. And everything that moves water AFTER the flux - the surplus spread,
the capillary flow - is scaled by what its sender still holds, the sender's own keep, which both
sides of a face can work out from the sender's velocity: without that a cell that had just sent
nine tenths of itself down the glass shed a surplus it no longer had, went under, was clamped, and
the clamp minted the difference. On the rig in a storm that flooded the visor to half a
millimetre everywhere, more water than had fallen on it, with a quarter of the glass running; the
same storm now holds thirty per cent wet at a twentieth of a millimetre, tracks laid over by
thirty degrees.

**A wipe leaves a smear, and the smear is the film.** The water the hand leaves as water - a
tenth of every bead - is regrouped by surface tension into beads too small to draw within a
second, so what the player sees after the hand is the film alone, and it has to show: the haze
ramps from three hundredths to fifteen, which keeps a running drop's trail at a whisper and puts
a hand's smear at a quarter. Under that, the residue vanished a second after the hand and the
wipe read as a delete again.

**Nothing on the glass dries while it rains.** The air over a plate in the rain is saturated;
water leaves a visor by running off, by being wiped, or not at all, which is why a window in
steady rain is COVERED. Dried at one rate whatever the weather, a small bead lasted twenty seconds
and the glass lost its water everywhere, all the time, and the hand had no point. The film keeps
a slow fade in the rain, standing in for the fresh drops that bury a smear.

**The wetting rate is `level.get_rain_exposure()`, not `level.get_rain_volume()`.** The volume
is a sound level with the old lighting hemi folded in, on purpose: shipped scripts turn it into
radiation and campfire dousing and were tuned against a number that dies at night. Read as the
rate the visor is wetted at, it dried the visor in every night storm - the drops came back for
the length of a lightning flash, which lit the hemi, and went again, and the player watched the
glass go dry while it poured. The exposure is the rain density times the real sky cover at the
head and nothing else, kept every frame the rain runs rather than only while the ambient bed has
feedback. On the rig in the same overcast rain at ten in the morning, on open marsh: volume
0.53 against exposure 0.82 - the hemi already takes a third off in daylight, and
by the engine's own comment on `rain_volume` it falls away in the dark.

**The sky cover in that exposure is a question about what is overhead.** The shelter test casts
five static rays from the eye, the axis leaned with the wind and four round it. It used to lean
the axis as far as the drops themselves lean, forty-five degrees, spread the ring thirty-five
degrees round that and reach thirty metres: the lowest ray left the eye ten degrees above the
horizon, and on open marsh in a storm every one of the five found a fence, a tree or a rise in
the ground within range. The cover went to zero in ten seconds - and with it the rain's own
sound, the radiation the shipped scripts take from the rain, and the water on the visor, which
dried while it poured and came back for a moment whenever the smoothing let a ray through. The
axis now leans no more than thirty degrees, the ring sits twenty-five round it, and the rays
reach twenty metres: a roof, a porch or an awning is within that above the head - a hangar roof
too - and a fence across the yard is not shelter. `qa_visor_state` prints the exposure as density times
cover, so the next time the glass is dry in the rain the answer is one line. On the rig in the
same storm the exposure holds at 1.0 where it had gone to nothing. `visor_live.lua`
is the probe: the visor driven the way the game drives it, with nothing pinned, reported every two
seconds for two minutes, and `live_timeline.py` lays the log out as a table.

**A tree crown is not a roof.** A player standing next to a poplar in a storm watched the visor
dry for twenty seconds at a time and fill again for three, in step with the wind. The sky probe
leans up to thirty degrees toward where the rain comes from and the ring twenty-five more, and
whenever the wind's heading put the crown inside that cone all five rays met it sixteen to twenty
metres up - `materialsush`, the game material every stock crown carries - and the cover went
to zero. The rain in the air went with it: a drop's birth ray met the same crown and the drop
died on it, which is why the player's video showed no rain under the tree either. The rain's ray
(`CEffect_Rain::RayPickThrough`) now carries on through every PASSABLE material but water -
foliage, occluders, kill volumes, invisible walls: what a bullet does not stop, a drop does not
stop at - to the first surface the water lands on, and the cover probe, the drop's birth and the
campfire's own roof test (`CZoneCampfire::SkyAbove`) all ask it that way. No material is named:
the flag is the game's, so a mod's foliage answers the same as the stock poplar. `visor_scene.lua`
is the probe: a player's own save copied to the rig, nothing pinned, reported every second with
every ray's hit (`qa_visor_state` prints the lean, the heading and what each ray met), and
`-PreCommands "wind_seed N"` replays the exact wind of a report - the seed is in the log as
`[wind] seed`, and a random one may never show the failure, since it depends on the heading.
Seed 1833.7 on that save: before, cover 0 for 27 of 45 seconds and five rays into a bush at 16-20
m; after, 1.0 throughout and 17-20 % of the glass wet.

**The wetting rate has its own channel.** `visor_rate` is what `dead_air_x64_visor.script`
pushes; `r2_lenswater_val`, the console float every older mask driver writes, is read only until
the mod's own driver has spoken. Not both and not the max of both: the base driver re-asserts its
own ramp once a second, and in anything but heavy rain that ramp is zero, so sharing the float put
the rain on and off at one hertz - which the player saw as the drops stopping for a while and
starting again.

**What the screen spans is 12 cm of visor**, not the visor's width: the eye is a few centimetres
behind the glass and the field of view cuts a window out of it, and the window is what the screen
shows edge to edge. That is one constant, `visor_width_m` in `r4_rendertarget.h`, and it sets the
cell size, from it the step, and the scale of every optical term: at 12 cm a millimetre of glass
is sixteen pixels of a 1080p screen, where a two-millimetre bead stops being a dot and starts
having an inside. At 22 cm it was nine, and every drop was a dot with a dark ring.

**The optics were wrong in the sign that matters.** The normal `refract` wants is the one on the
water side, facing back at the eye, and for a surface that bulges away from the eye that is
(dh/dx, dh/dy, -1). With its xy flipped, the bottom of every drop looked at the ground: a
magnifier, not a lens, and nothing in a screenshot of a uniform sky could show it. `qa_visor_blob
<mm>` holds a spherical cap of that radius at the centre of the glass, still, so the inversion can
be read off one screenshot at the horizon. The surface the eye is given is not the field's own
cells - a bead a few cells wide has edges a cell wide, and read raw every bead was rim, past the
critical angle everywhere but its middle pixel - but the field over thirteen taps on two rings
about a bead across, its slope capped at the contact angle. The rim is the edge of the wet mask,
a line and not a band, and not a Fresnel term at all; from inside, what the surface reflects is the
inside of the mask, so the reflected share is dark, and there is no highlight to mirror - the
bright arc a photograph from inside shows is the refracted sky, so the sun is looked for along the
refracted line and not in a specular lobe.

`r__visor_drops_stats` prints what the field holds - how much of the glass is wet, the mean and
deepest thickness, the film, the fastest water and **what share of it is actually running** - and
dumps three pictures under `appdata`: the thickness, the film and the speed. That last one is not
a nicety. Everything this feature is about is motion, a screenshot of a rainy visor looks much the
same whether the water is running or nailed down, and the rig cannot film the screen either: a
JPEG per frame costs half a megabyte and drops it to about one frame a second, which is slower
than the drops. Dumping the field costs a few hundred microseconds, so `visor_motion.lua` takes
five a second across a wipe and the strip can be stepped through and measured. Every wrong version
of the transport above was found that way and none of them was visible in a still.

**The one trap.** The old block lived in `combine_2_naa.ps` alone, so it died the moment MSAA was
on - the same asymmetry that had already cost this project once, noted in `combine_2_aa.ps` and
then repeated one effect later. The new call is in both, and the blender binds the field into all
four elements. Whoever adds the next post effect puts it in BOTH.

### Quality

`r__water_waves` is the wave-row budget, on the preset ladder at 2 / 4 / 6 / 8 / 8, session-only
from the console. Everything else about the model is unconditional: the depth fix, the optics, the
Fresnel and the sea state cost nothing and there is no tier where being wrong is cheaper.

`settings_da_water.h` holds what is genuinely a look and has no physical value to derive it from -
the detail layer's share, the rain and impact ripple amplitudes, the foam colour, the debris
window, the refraction strength, and the SSR hand-over distances. It is compile-time on purpose:
wiping `appdata/shaders_cache_xfr` applies a change without an engine rebuild.

## Rain puddles, wet ground and the far fades

Puddles are part of every quality tier now; what changes with the tier is how far they are drawn
and what they reflect. The ladders live in `xrRender_sync_preset_derived()` (`xrRender_console.cpp`):

| Preset | Puddles | Reflection (`r__puddles_refl`) | Puddle distance | Wet ground radius / rain map |
| --- | --- | --- | --- | --- |
| Minimum | mask in the G-buffer | none (0) | 15 m | 20 m / 256 |
| Low | mask | none (0) | 20 m | 20 m / 256 |
| Default | mask | sky only, no depth march (1) | 30 m | 25 m / 512 |
| High | mask | world ray-march, sky on a miss (2) | 45 m | 35 m / 512 |
| Extreme | mask | world ray-march, sky on a miss (2) | 60 m | 50 m / 1024 |

The look constants are the same on every tier and are re-applied on every start, so a `user.ltx`
from an earlier build cannot pin them: `r__puddles_size 0.6` (share of the ground under water
at full wetness), `r__puddles_dark 0.65` (soil under water is darker than dry soil),
`r__puddles_refl_power 1.0`, `r__puddles_facing 0.03` (Schlick F0; water is 0.02) and
`r__puddles_sky 1.0` (a ray that finds no geometry reflects the sky in full) and `r__puddles_gbuf 0`
(the water keeps the ground's normal in the G-buffer; the flat normal of the old default lit a
whole puddle as one sun highlight at noon, the sun is a tight glint in the reflection pass now).
The fresnel exponent is the physical 5 (`da_puddle_refl.ps`). Every `r__puddles_*` command is a session override and
is never written to `user.ltx`. The rain occlusion map covers exactly the wet radius
(`r3_dynamic_wet_surfaces_far`), so ground under a roof stays dry out to the fade edge.

The wet gloss belongs to continuous surfaces. The rain passes (`rain_patch_normal.ps`,
`rain_apply_normal_gloss.ps`) darken every wet pixel and add gloss to it; the gloss feeds the
hemisphere specular (`hmodel`: sky reflection x material Fresnel x gloss), whose Fresnel peaks
where the surface turns edge-on to the eye. On a road that is the sheen reaching to the horizon.
On foliage - leaf cards with spherised normals, grass blades, crown silhouettes against the sky -
every card edge is such a place, and a dark rain drew a white outline around every bush and salted
the grass with the splash-pop glints. The patch pass now measures the surface continuity from the
depth Laplacian of the G-buffer two pixels out (a plane has a zero second difference at any grazing
angle; a card edge or a silhouette does not), scales the ripple, run-off and pop normals by it and
carries it to the apply pass in the length of the patched normal; the gloss boost is multiplied by
it there. The darkening is untouched, so wet foliage still reads wet; a wet road, a wall, a trunk
or a block of concrete keep their full sheen and their rings.

The puddle reflection lands on the ground only. The reflection pass (`da_puddle_refl.ps`) used
to recompute the puddle mask for every pixel from the reconstructed position alone, and from a
few metres away a grass field or the underside of a crown reads as level as a floor - a blade or a
leaf card whose XZ fell on a puddle got the sky reflection painted over it (white grass at a
distance, a screen-door sky on a birch seen from below, a bush "covered by the puddle's water").
The ground shader (`deffer_impl_flat.ps`) now raises a flag in the G-buffer for the pixels its
puddle mask covers, and the reflection pass exits on every pixel without it. The flag rides in the
packed hemi/material word (`gbuf_pack_hemi_mtl`): hemi keeps seven bits (steps of 1/127, an
occlusion term never showed the eighth) and bit 13, the low FP16 mantissa bit, is the flag; in
the unpacked G-buffer layout (`r3_gbuffer_opt off`) the sign of the material float carries it.
`gbuffer_data.ground` exposes it to any pass; `common_iostructs.h` is now shipped by the mod for
that one field.

Ground wetness starts over with a level load. The accumulator (the `rain_params` binder in
`r2.cpp`, buildup and drying in minutes) carried a storm's puddles and dark ground into a save
made in clear weather. `CRender::level_Load` now asks the binder to resolve itself on the first
frame after the load, when the restored weather is current: raining - the ground has been under
it for a while (wetness 1), dry - dry (0). Nothing is saved; `.scop`/`.scoc`/`.scov` are unchanged.

Two fades that used to be cuts: a tree impostor thins out through its alpha test over the last
4x span of its size measure before `r__veg_discard` and over the last 12 % of the weather's
`far_plane` (the sets keep the fog saturating only for what stands below the horizon, so a crown
against the sky used to vanish in one frame), and detail objects keep a fade band of at least
twelve metres whatever `r__grass_fade_start` says (`DetailManager.cpp`), so a bush or a sapling at
the edge of the detail radius shrinks away instead of going in three visible steps.

## Fall damage and sliding

The character's collision damage (`fMinCrashSpeed` / `fMaxCrashSpeed`, `ph_collision_damage_factor`,
the material's `bounce_damage_factor`) is computed from the velocity INTO the surface. The stock
engine also counted the speed along the surface times the material friction, so a fall that
brushed a wall was charged as a hit at the fall speed on every step of the slide, more than the
landing itself; that term is gone (`PHSimpleCharacterInline.h`). A body contact with static
geometry while airborne keeps half the material friction (`PHSimpleCharacter.cpp`, `InitContact`),
so the surface brakes a slide by its normal load - barely on a wall, more on a slope. Passable
materials (bushes) keep the stock full-speed formula. Both are deliberate departures from the CoC
and DA sources, requested for the mod.


## Landing roll

Landing with the jump key held turns a survivable fall into a forward roll that takes part of the
impact - the Mirror's Edge landing, without animations. The engine decides at touchdown
(`CActor::g_Physics`): the stock fall damage must be non-zero and, through the outfit and the
immunities, less than the current health (`CActorCondition::PredictHealthLoss`); otherwise there
is no roll and the stock damage or death stands.

The damage with a roll has two knobs, one per effect. `fall_roll_threshold_raise` (0.375) is
the share by which the no-damage landing speed grows: `ph_crash_speed_min x (1 + raise)`, Dead
Air's 13 m/s becoming 17.9 m/s - a fall of some sixteen metres instead of nine. Above that the
stock slope continues, cut by `fall_roll_damage_reduction` (0.43): the roll costs `(1 - r)` of
what a stiff landing costs per extra metre per second, measured from the raised threshold. The
lethal speed (`ph_crash_speed_max`, 30 m/s) is not moved, and a fall that would kill a stiff
landing never starts a roll. The 0.43 is the force-plate result: the roll cut the peak vertical
landing force by 43 % against a stiff two-foot landing from 0.75 m (Puddle and Maulder, J Sports
Sci Med 2013; 90 % CI 34-51 %); trained landings cut it by 40-49 % at 0.44-0.88 m (Standing and
Maulder, J Sports Sci Med 2015), and a kinematic study from 0.9-2.7 m found the roll's advantage
in early deceleration narrowing with height (about 29 % at 1.8 m, parity at 2.7 m) while it still
stretched the landing to 320-364 ms and turned the fall into 2.6 m/s of forward speed. The first
build scaled the landing speed itself by `1 - r`, which raised the threshold to 22.8 m/s and made a
jump from a roof free; the raise is now half that and separate from the reduction. In numbers, for
the stock 13/30: at 20 m/s (a 20 m drop) a stiff landing loses 41 % of the crash range, the roll 7 %;
at 25 m/s (32 m) 71 % against 24 %.

During the roll (`fall_roll_time`, 1.2 s - the Mirror's Edge roll from touchdown to standing) the
view turns once forward about the camera's right axis (`CEffectorFallRoll`) with the give of a
neck and spine: the turn runs about nine degrees past the full circle by 78 % of the duration and
eases back onto 360 degrees over the rest, both halves start-soft and stop-soft. Pitch input is
ignored and yaw runs at `fall_roll_yaw_sensitivity` (0.25), the body crouches through the same
box switch and camera lerp a crouch key uses and is carried forward from `fall_roll_speed_start`
(2.6 m/s, the forward speed a measured roll leaves the ground with) down to `fall_roll_speed_end`
(1.0 m/s) - about two metres, the length of a real roll - in the camera's yaw direction. The speed
is the roll's own: it is applied in `g_Physics` past the hit slow-down, the stamina checks and the
crouch factor, so neither the speed the player arrived with nor low health changes it, the first-person legs are not drawn while the world shadow caster keeps going, and
every command except quit, console, screenshot, quick save/load and pause waits. The active item
and the detector go to the ruck at once through the inventory's own events and return to their
slots when the roll ends, the item into the hands two updates later. The roll plays its own
sound once as it begins (`fall_roll_snd`, default `actor\fall_roll`, shipped in the content
bundles as `sounds/actor/fall_roll.ogg`, mono, X-Ray ogg comment v3 with 1/10 m and full volume;
2D like the heavy breath; the sound object is created on the first roll, not at actor load).
Keys in `[actor]`: `fall_roll_damage_reduction`, `fall_roll_threshold_raise`, `fall_roll_time`, `fall_roll_speed_start`,
`fall_roll_speed_end`, `fall_roll_yaw_sensitivity`, `fall_roll_snd`. The console command
`fall_roll_test` starts the roll on the spot without a landing and logs one line about the sound
(handle, bytes, length, whether an emitter plays) - the way to check the tumble on a rig that
cannot jump, with `time_factor 0.2` and `r__screenshot_every 1` to catch its phases. Nothing about
the roll is saved.


## An artefact in a container answers for itself

Handing in an artefact meant unpacking it first, and almost every artefact is radioactive: the
player had to stand in the radiation of their own quest item for as long as the dialogue took,
every time. The check said no because of a naming detail, not because of a rule anybody wrote.

**A container does not hold an artefact - it BECOMES it.** Putting one in destroys both objects
and creates a single new object whose section is the two names joined: `af_medusa` in a `lead_box`
is one item of section `af_medusa_lead_box`, of the artefact class, with the container's `antirad`
folded into its radiation so it shields (`itms_manager.container_add`, and `bind_artefact.script`
for the shielding). Nothing is nested, so there was nothing to look inside: the object was in the
rucksack all along and its section was simply a different string, which every possession test in
the game compared against and rejected.

So the fix is a NAME RULE, and it goes where names are resolved:

* **The engine** answers `db.actor:object("af_medusa")` with the container standing in for it
  (`da_artefact_container.cpp`, from `CScriptGameObject::GetObjectByName`). That one seam covers
  every `:object(section)` test there is - `xr_conditions.actor_has_item`, `dialogs.actor_has_item`,
  the eighteen hand-written `zat_b29` checks, the Jupiter tolls, the DoctorX random fetch, and any
  mod we have never seen - without touching one of them. It is the LOOKUP that changed, nothing
  else: `iterate_inventory` is untouched, so the rucksack radiation sweep still sees exactly what
  it saw before and a containerised artefact still does not irradiate anybody.
* **The game side** (`dead_air_x64_af_container.script`) covers the two things a lookup cannot.
  The counters - `actor_has_item_count`, DAR2's `dar2_have_item`, our own `count_items` - walk the
  inventory and compare sections themselves, so they are wrapped. And the taking: a quest that
  takes the artefact must not walk off with the container, so the removal funnels
  (`dialogs.relocate_item_section_from_actor`, `xr_effects.remove_item`, our `remove_items`) empty
  it instead - the artefact goes to whoever asked for it, the container comes back empty. That
  mirrors `itms_manager.container_remove` exactly, stat for stat, minus its animation, because
  this runs inside a dialogue rather than from the player's own menu click.

**Loose first, always.** Every path counts the bare artefacts before it reaches into a bag: a
player carrying one of each hands over the one in their hand. The loose ones also go through the
original function untouched, which is what keeps its every side effect - the news line, the trade
events - exactly as it was; only the shortfall is taken out of a container.

**The rucksack only.** A container on the belt is being worn and its artefact is working there;
the engine scans `m_ruck` and the script asks `is_on_belt`.

**No container's name is written in either half.** `dead_air_x64_af_container.ltx` says where the
installed container mod keeps its own registry - file and section, both data - and both halves
read it. A mod that adds a fifth container type has to register it there to work at all, so it
arrives free; `[artefact_containers_extra]` and the script's `register()` are for one that keeps
its list somewhere unreadable. `quest_visible = false` turns the whole rule off.

The one thing this cannot reach is code that destroys an item straight from a lookup -
`alife():release(alife_object(db.actor:object(sec):id()), true)` - which would eat the container
rather than empty it. Every section used that way in the shipped trees was checked against the
container tables: DAR2 uses the idiom for craft parts and armour plates (`af_surge`, `af_kevlar`,
`af_camelbak`), none of which has a container variant, and the engine only ever stands in when
`<artefact>_<container>` is a real section - so the case does not arise today. A mod that hands in
artefacts that way would lose an empty container, not a quest.

`tools\qa\water\af_container.lua` is the acceptance probe: it spawns a filled container on the
rig, asks the real check, runs the real removal, and measures the inventory against what the save
started with - seventeen assertions covering the stand-in, the counters, the emptying, the
hand-over and the loose-first order.


## Hands that never come back

Sometimes, after using an item, the player could no longer draw anything at all - no weapon, no
bolt, and sometimes not even the PDA - for the rest of the session. Sometimes the item's use
animation played twice, the new one and then the old one right after it. The two are the same
bug, and it took a player's exact account to find it: *"took epinephrine, no animation played,
pressed 2 for the rifle, and instead of the rifle the epinephrine animation played - and then
nothing could be drawn."*

**There are two independent gates, and the two symptoms are one each.** A slot is blocked by a
REFCOUNT (`CInventory::m_blocked_slots`), raised through `hide_weapon()` and lowered through
`restore_weapon()`; the 3D PDA carries `ignore_slots_blocked`, so when only this one is stuck the
PDA still opens and nothing else does. The other is `g_da_block_all_except_movement`, the scene
input gate: it swallows every key at or above `kCAM_1`, which includes the PDA's, so when that
one is stuck even the PDA is gone. An extra `restore_weapon()` is silently clamped at zero; an
extra `hide_weapon()` is permanent, and the count lives on the actor, so only a fresh actor clears
it.

**The root cause is that this feature is installed twice.** DAR2 ships its own item-use
animations (`dar2_animations_enhanced`), and this project ported a second, better set. In the
addon's copy the two halves are the wrong way round - its scene ENDS with

```lua
db.actor:hide_weapon() --Возможность менять оружие
```

(the comment reads "the ability to change weapon", so the author had hide and restore swapped),
while the `restore_weapon` it owns sits at the START, where the count is already zero and the call
is thrown away. One frame later that scene asks for its slot back, the engine refuses because the
slot it just blocked is blocked, and the player is standing there with empty hands.

It reached the player through OUR scene rather than instead of it. The module consumes the item
mid-scene through the ordinary use, so that `actor_on_item_use` fires - which is exactly what the
addon listens on. A second scene started inside the first, span while our block was up, and ran to
its own end, and its end blocks. That is also the double animation the player saw. **One feature,
one owner:** while the module has a scene for an item, the addon's copy stays out of it; when the
module is off in Options, or has no scene for that item, the addon works as it always did.

Three more holes on the same path, each able to strand the hands on its own:

* **A hard Lua error in the waiter.** Between `anim_prepare()` and the scene actually starting,
  the module holds the hands and the input while nothing is yet scheduled to give them back -
  every release lives in a time event that only exists once the scene has STARTED. The waiter
  built its animation name by concatenating a section that can be nil (an entry with a sound and
  no `anm`; or a scene whose section was cleared under it, since a level change does not restart
  the Lua machine but does clear the scene). A hard error in one handler ends the whole callback
  list for that frame, so every other module's update stops too. Guarded, and the waiter is now
  unregistered on the first frame of a new level.
* **A watchdog.** The release no longer depends on the scene reaching its end: once a frame, if
  the module is holding anything and no scene of its own is running, it gives it back. It can only
  release what it took, so a ladder's block and a bloodsucker's are none of its business.
* **`unblock_all_slots()` was the wrong instrument.** It sets every counter to zero, including
  other owners' - a ladder, a car, the death effector. The scene now uses `restore_weapon()`,
  which takes back exactly the one it raised, through the same event queue that raised it.

And two engine bugs found along the way, both independent of the module:

* **`CActorDeathEffector` blocked every slot in its constructor and never unblocked.** `Stop()`
  restored the input and the indicators and not the slots. A death that is stopped rather than
  completed - which is what every revive is - left the player alive and unable to draw anything.
* **The scene input gate had no reset anywhere.** It is a plain global with the lifetime of the
  PROCESS: nothing in the engine cleared it on load, on a level change or on actor spawn, so a
  scene cut short before its own release left the player able to walk and look and nothing else,
  and reloading the save did not help because the save never held it. It is cleared on actor
  spawn now.

**`qa_hands_state`** prints every gate between a key press and an item in the hands, in the order
the game consults them: the per-slot block counts, the active/next/previous slot, the hud state
and pending flag of the items in them, the input globals, and the 3D PDA's own two flags. When the
hands stay empty the one thing nobody could see was WHICH gate was holding; now it is one line.
`tools\qa\water\hands.lua` is the acceptance probe - it claims the hands the way a scene does,
never starts one, and asserts that everything comes back.


## A motion id belongs to a rig

A player's crash report from 1.4.0 (access violation in `xrGame.dll`, `l01_escape`) ends on one
line of log and then the stack:

```
* [pda3d] hands swap in: [actor_hud_cs1] -> [pda3d_actor_hud]
! MODEL: LL_GetMotionDef rejected motion id [slot 64, idx 2]: visual has 44 slot(s)
```

**A MotionID is an index into ONE model.** It is a motion-slot number and an index inside that
slot, resolved by name once and kept; the model it was resolved against is not part of it. So the
moment the hands model is replaced - an outfit with its own `player_hud_section`, the 3D PDA with
its own rig - every id anything is still holding means something else, or nothing at all. The
ordinary rig here carries 80 motion slots (its own plus the addon OMFs `fill_player_hud_extra_omf`
appends), the PDA's carries 44, and slot 64 is past the end of it.

The guard on `LL_GetMotionDef` names the caller and refuses the read, which is the log line above.
What it returns is null, and **`player_hud::motion_length` read `md->flags` under a `VERIFY`** -
nothing at all in a release build. That read of address 0xC is the faulting instruction the report
carries (`xrGame.dll +0x36bb45`, confirmed against the shipped binary: `mov ... [rax+0Ch]` at
+0x45 of the function). It returns zero now, which is what every caller already reads as "no
animation".

**Two caches hold ids across a rig change, and each needs the same treatment.** The scene motion
cache (`m_scene_motions`, what `level.get_motion_length` and the hud scenes read) is cleared on a
hands reload - that fix went in with the 1.4.1 report batch and is what this player's crash needed.
The other is the **item pool**: `player_hud::create_hud_item` keeps one `attachable_hud_item` per
hud section for the life of the hud, and a non-monolithic item's cycles live in the HANDS model, so
its `m_hand_motions` are ids into whatever rig was loaded when the pool first met it. Nothing
cleared those. They are rebound now, against the model that is actually loaded, every time the rig
changes; cycles the new rig does not carry simply go, and come back with the old rig. An item asked
to play one it no longer has says so and plays nothing, where it used to assert.

**Diagnostics.** `qa_hands_state` now ends with the rig: its section, how many motion slots it has,
and for each attached item how many cycles it currently has bound - zero there is an item bound to
a rig that is gone. `qa_hands_rig <section>` loads another rig under whatever is in the hands, which
is the whole mechanism without a PDA, an outfit or a save; `qa_hud_motion <section> <alias>` asks
the pooled item for a cycle length on the rig loaded now. `tools\qa\water\hud_rig.lua` is the
acceptance probe: a weapon drawn, the rig swapped to the PDA's and back three times over, with
draws and holsters on the far side of each swap.
