# Dead Air x64 port status

## Baseline

- Upstream: OpenXRay `xray-16`
- Pinned commit: `29030f81b137f6ea5365b3d71f2b588490832f5b`
- Port branch: `dead-air-x64`
- Target: Windows x64, `Mixed` configuration
- Live Dead Air installation: untouched

## Current milestone

The first milestone is a clean upstream x64 engine build and an isolated launch
against the existing Dead Air filesystem layout. Dead Air-specific engine and
Lua API differences are ported only after that baseline is reproducible.

## Work completed

- Created a standalone repository with all upstream submodules.
- Verified an x64 MSVC compiler and Windows SDK are installed.
- Built the unmodified upstream x64 runtime successfully.
- Produced x64 `xrEngine.exe`, `xrCore.dll`, `xrGame.dll`, LuaJIT, DX11, and
  OpenGL renderer binaries under `bin\x64\Mixed`.
- Restored the pinned SDL2, DirectXMath, and DirectXTex build dependencies.
- Added a reproducible x64 build entry point at `tools/build/build_x64.ps1`.
- Recorded the addon and save compatibility contract.
- Preserved the existing `fsgame.ltx`, XDB, loose `gamedata`, and JSGME model as
  release requirements.
- Mounted all 34 existing Dead Air archives and 62,527 files from an isolated
  test root.
- Loaded all 98 localization files, existing user settings, audio, the DX11
  renderer, Dead Air shaders, 258 script-export nodes, and core Dead Air scripts.
- Reached engine startup and the Dead Air main-menu resource/script path without
  a fatal or Lua runtime error.
- Ported the first three data-compatibility differences:
  - x64 shared-string headers no longer reduce the x86 payload limit;
  - malformed but x86-compatible overlapping texture THM chunks are accepted;
  - intentionally silent Dead Air ambient sections are accepted.
- Generated a machine-readable inventory of 330 x86 Dead Air Lua classes, 4,483
  class functions, 718 properties, and 136 namespace functions.

## In progress

- Compare the Dead Air Lua/native API surface with upstream OpenXRay.
- Make main-menu shutdown clean and deterministic in the isolated harness.
- Restore Dead Air-specific console commands and renderer behavior.
- Reach new-game and save-load runtime gates.

## Next gates

1. Finish the Lua/API difference report.
2. Port missing Dead Air/DAR2 native and Lua exports in small, testable groups.
3. Start a new game in x64.
4. Load the latest x86 save and produce a reloadable x64 save.
5. Validate representative addons.
6. Run level-transition and long-session soak tests.
7. Build a non-destructive install package.

No public release is authorized or planned at this stage.
