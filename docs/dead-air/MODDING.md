# Modding capabilities specific to Dead Air: Refined

## XMS modules (XFined Module System)

Additive mod packages that compose instead of overwriting each other. Full
design: `XMS_ARCHITECTURE.md`. Quick authoring reference:

```
<game root>/mods/<mod.id>/
  mod.ltx            ; manifest: [module] id/name/version, [requires], [order]
                     ; after/before, [conflicts], [budget] spawns=N
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

- Load order is deterministic: `[requires]`/`[order]` topology, then
  `mods/order.ltx` (one id per line), then id. `xms_list` shows it.
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
  working unchanged; folders under `MODS/` without `mod.ltx` are ignored.

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
