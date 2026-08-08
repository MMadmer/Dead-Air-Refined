# Modding capabilities specific to Dead Air: Refined

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
