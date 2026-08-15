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
