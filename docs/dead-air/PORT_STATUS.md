# Dead Air x64 port status

## Baseline

- Upstream: OpenXRay `xray-16`
- Pinned commit: `29030f81b137f6ea5365b3d71f2b588490832f5b`
- Port branch: `dead-air-x64`
- Target: Windows x64, `Release`
- Live Dead Air x86 installation: untouched

## Current milestone

The engine now loads and runs the existing Dead Air 0.98b and Dead Air
Revolution II content directly. It retains the original root layout, XDB and
loose `gamedata` precedence, Lua addon API, JSGME workflow, and x86 save format.
The compatibility port, release hardening, isolated installation test, and
rollback verification are complete.

## Work completed

- Built a complete AMD64 runtime: all 42 shipped EXE/DLL files are x64.
- Added a reproducible `Release` build entry point at
  `tools/build/build_x64.ps1`, including required external-library
  compatibility patches.
- Mounted all existing Dead Air archives and loose overrides without
  repacking or converting content.
- Restored the complete Dead Air native/Lua API surface. The automated binding
  comparison reports no missing classes, methods, properties, or namespaces.
- Restored Dead Air renderer behavior, console variables, actor body, 3D HUD,
  screen-space sun shafts, and FXAA.
- Added compatibility for legacy Dead Air animation, texture, particle,
  registry, task, ALife, LuaJIT bytecode, Lua marshal, and save formats.
- Started a clean Dead Air 0.98b game and created a new x64 save.
- Reloaded the new x64 save successfully.
- Loaded the latest existing x86 DAR2 save with 22,958 spawn points and 27,198
  objects.
- Loaded representative packed and loose addons through the unchanged
  `database` and `gamedata` paths:
  - DA Inventory Sort from XDB;
  - DAR2 Oxygen HUD from loose `gamedata`.
- Performed an actual level transition and produced reloadable underground and
  Agroprom saves.
- Rebuilt the whole runtime from a clean build tree with zero errors.
- Launched the freshly rebuilt runtime from an isolated root against the
  unchanged live content.
- Verified runtime private memory above 2 GiB, demonstrating that the x86
  address-space ceiling is removed.
- Hardened stack traces so diagnostics use the system `dbghelp.dll` safely.
- Audited all runtime imports: every non-system dependency is present in the
  package.
- Completed a 15.29-minute Agroprom soak at 3.04 GiB private memory without a
  fatal error, assertion, packet overflow, hang, or memory growth.
- Built the final `1.0.0` archive from the clean output and verified all 42
  packaged hashes.
- Installed the final archive over an isolated x86 root and loaded the latest
  existing x86 save with 22,958 spawn points and 27,198 objects.
- Uninstalled it and restored all 28 original x86 runtime files by SHA-256,
  with no added x64 files or control directory left behind.

## Release constraints

- Do not replace, edit, or repack `database`, `gamedata`, saves, or JSGME state.
- Keep `fsgame.ltx` and normal addon precedence compatible with the x86 game.
- Keep the original x86 runtime recoverable.
- Do not publish a public release without explicit authorization.
