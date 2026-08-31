# Dead Air: Refined x64 dependencies

This file records the dependency state used by the Windows x64 runtime. A version is upgraded only when its public API, binary ABI, and Dead Air behavior remain compatible with the engine.

## Source submodules

| Component | Revision | Notes |
| --- | --- | --- |
| AMD AGS SDK | `v6.3.1` (`086c47ed8`) | Pinned public SDK source. |
| DirectXMath | `93e6399d6d` (`may2026-5-g93e6399`) | Header-only SIMD math library built from pinned source. |
| DirectXTex | `oct2025` (`232bbf2e39`) | Static texture-processing library built from pinned source. |
| Dear ImGui | docking `b334d19b6` | Pinned docking revision based on 1.92.9. |
| GameSpy | `5597a6c582` | Pinned OpenXRay fork plus local x64 warning fixes. |
| GLI | `3542f8830` | Pinned upstream revision. |
| LuaJIT | `ade7495df8` | Dead Air bytecode compatibility is applied by the canonical build wrapper. |
| Luabind | `ffb1e5adb8` | Pinned Dead Air integration revision. |
| mimalloc | `v3.4.3` (`152fbf2634`) | Static allocator built from pinned source for each configuration. |
| SDL2 | `2.32.8` (`98d1f3a45a`) | Shared runtime and headers built from pinned source. |
| SSE2NEON | `3b70b3727` | Pinned upstream revision. |
| SSE2RVV | `f1ab91659` | Pinned upstream revision. |
| xrLuaFix | `e0fadfd9d1` | Lua marshal compatibility is applied by the canonical build wrapper. |
| zlib | `v1.3.2` (`da607da73`) | Built with warnings enabled and treated as errors. |

## Vendored source and generated bindings

| Component | Version or revision | Notes |
| --- | --- | --- |
| parallel-hashmap | master `48f4c5fb0` | Swiss-table containers used by engine lookup structures. |
| Tracy | 0.13.1 | Client source only. |
| GLAD | 2.0.8 | Vendored under `sdk/include/glad`; compiled only by the OpenGL renderer target, which is removed from the build (sources retained, not built). |
| DirectXMesh | may2026 `8c6fdb1c` | Pinned `FlexibleVertexFormat.h` with the existing X-Ray integration retained. |
| NVAPI | `cd6918f6` | Pinned public SDK headers and x64 import library. |

## Built and packaged x64 libraries

| Component | Version | Notes |
| --- | --- | --- |
| OpenSSL | 4.0.1 | `libcrypto-4-x64.dll` replaces the OpenSSL 3 runtime. |
| libjpeg-turbo | 3.2.0 | Release and Debug static libraries include runtime SIMD dispatch. |
| OpenAL Soft | 1.25.2 package | The official package still reports 1.25.1 in its Windows version resource. |
| libogg | 1.3.6 (`06a5e026`) | Rebuilt with embedded debug information and the dynamic MSVC runtime. |
| libvorbis | 1.3.7 (`e3c9861f`) | Rebuilt with embedded debug information and the dynamic MSVC runtime. |
| libtheora | `28fd5ec7` | Pinned upstream revision, rebuilt with the dynamic MSVC runtime. |
| LZO | 2.10 | Packaged compatibility build using the dynamic MSVC runtime. |

## Compatibility-pinned components

These components cannot be replaced with an unrelated modern library without changing game behavior or removing supported features.

| Component | State | Reason |
| --- | --- | --- |
| ODE | X-Ray compatibility fork | Dead Air uses a heavily modified solver, collision layer, ABI, and OPCODE integration. ODE 0.16.6 is not a drop-in update. The fork is compiled with warnings treated as errors. |
| OPCODE | X-Ray compatibility fork | Coupled to the ODE and game collision ABI. It is compiled as part of the warning-clean solution. |
| NVIDIA Ansel | 1.6.490 | NVIDIA discontinued the standalone Ansel SDK. The last compatible SDK remains delay-loaded. |
| Discord Game SDK | packaged SDK snapshot | Discord discontinued public Game SDK distribution. The integration remains optional at runtime. |
| Diagnostic reports | Engine-native implementation | BugTrap was removed from the runtime. Dead Air: Refined now creates compact anonymous session and crash reports without an external crash-handler DLL. |
| DirectPlay and EAX headers | Windows legacy SDK interfaces | Compatibility declarations, not independently versioned runtime libraries. |

## Content system components

The content asset system adds one first-party static library, two Windows system libraries and two build-time tools. The library is written against the standard library alone, because the same translation units have to run in three places: inside `xrCore` before the engine exists, inside `xrGame`, and inside the standalone updater, which is compiled straight from the sources rather than linked against a CMake artefact. One implementation, three linkers, and no second parser to drift away from the first.

| Component | Kind | Notes |
| --- | --- | --- |
| `xrContentSync` | In-repo static library | `src/xrContentSync`. Manifest format, SHA-256, the state files, and the resolve/fetch/commit pipeline. No dependency on `xrCore` and no engine types. `xrCore`, `xrGame` and `xr_3da` link the CMake target; `build_dead_air_x64_installer.ps1` compiles the same `.cpp` files into `DeadAirUpdater.exe`, which is byte-copied to `DeadAirContent.exe` so the installer's fetcher and the updater can never drift apart. |
| `bcrypt` | Windows system library | SHA-256 provider for `ContentHash.cpp`. `xrCore` already linked it for the diagnostic reports, so the CMake entry predates the content system; the updater link line names `bcrypt.lib` explicitly, and `ContentHash.cpp` carries its own `#pragma comment(lib, "bcrypt.lib")` for consumers CMake does not name. |
| `winhttp` | Windows system library | HTTP transport for `ContentDownload.cpp`. It appears in no `CMakeLists.txt`: the engine picks it up through `#pragma comment(lib, "winhttp.lib")` in that file, and the updater link line names `winhttp.lib` directly. Only consumers that actually reference the downloader pull it in, which is why `xrCore` does not need it. |
| `converter.exe` (AXRToolset) | External build-time tool | Packs and unpacks `.xdb0`. Used by `dead_air_x64_content_bundles.ps1` and by the compatibility archive script; `-ConverterPath` overrides the default `D:\Games\Dead Air\tools\AXRToolset\bin\converter.exe`. Not redistributed and not needed to build or run the engine, only to build archives. |
| `DarDelta.exe` | Build-time tool from repo source | Builds `.darpatch` v1 binary deltas between two revisions of a bundle. An ordinary CMake target rather than a packaging-script helper: `src/utils/DarDelta/DarDelta.cpp`, linked against `bcrypt` and nothing else, produced by the normal engine build (`tools\build\build_x64.ps1`) into `bin\x64\<configuration>\DarDelta.exe`. `dead_air_x64_content_bundles.ps1` runs it only when it is given `-PreviousManifest`, and finds it through `-DeltaTool`, which defaults to `bin\x64\Release\DarDelta.exe` and refuses the run outright when nothing is there. It is a build-side tool only: applying a delta is `xrContentSync`'s job on the client. |

Deltas are an optimisation on top of the bundle format, never a substitute for it. The resolver falls back to the full bundle whenever a delta is ineligible, and an applied delta is accepted only when the output hash matches both the `.darpatch` header and the manifest, so a build that cannot run `DarDelta.exe` still produces a correct, publishable release.

## Build policy

- CMake presets and Ninja Multi-Config are the only supported build pipeline.
- Debug, Mixed, Release, and ReleaseMasterGold builds use the same source graph.
- Engine sources use warnings-as-errors.
- Third-party libraries are rebuilt with the dynamic MSVC runtime where the engine owns their allocation boundary.
- Local compatibility changes are kept as explicit commits or small project-level patches.
- Temporary downloads remain outside tracked source. Bootstrapped local tools
  live under ignored `tools/third_party`; build and package output lives under
  ignored `build` and `artifacts`.
- The content bundle cache is not build output and the rule above does not apply
  to it. A published bundle can never be replaced, and a repack is not guaranteed
  to reproduce the published bytes, so losing the cache while a release is still
  reachable by players is unrecoverable. `dead_air_x64_content_bundles.ps1`
  therefore makes `-BundleCache` mandatory with no default, so nothing can quietly
  place it under `build`. It belongs in the assets repository working copy, beside
  the built `content-manifest.txt`, whose path is handed to
  `build_dead_air_x64_installer.ps1` explicitly through its mandatory
  `-ContentManifest` - that script has no default for it either - and it is backed
  up with the authoring content tree. Only the scratch
  `-WorkRoot` defaults into `build\content`, and that directory is disposable.
- The authoring content tree that `-SourceRoot` points at has no version-controlled
  backup either, and nothing in this repository can regenerate it. It carries the
  same retention obligation as the bundle cache.
