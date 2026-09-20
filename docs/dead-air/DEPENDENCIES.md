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
| LuaJIT | `8b7a19e030` (upstream v2.1 `c6ffc141`, 2026-09-08) | Dead Air fork of the upstream rolling release, see [LuaJIT](#luajit). The same changes are applied to a pristine upstream checkout by the canonical build wrapper. |
| Luabind | `ffb1e5adb8` | Pinned Dead Air integration revision. |
| mimalloc | `v3.4.3` (`152fbf2634`) | Static allocator built from pinned source for each configuration. |
| SDL2 | `2.32.8` (`98d1f3a45a`) | Shared runtime and headers built from pinned source. |
| SSE2NEON | `3b70b3727` | Pinned upstream revision. |
| SSE2RVV | `f1ab91659` | Pinned upstream revision. |
| xrLuaFix | `e0fadfd9d1` | Lua marshal compatibility is applied by the canonical build wrapper. |
| zlib | `v1.3.2` (`da607da73`) | Built with warnings enabled and treated as errors. |

## LuaJIT

The engine runs upstream LuaJIT v2.1 with a short list of changes on top, each of them a separate
commit on the `dependencies/luajit` branch. `patches/luajit-dead-air.patch` is the same list as one
diff against the upstream revision named above, for a checkout that comes from upstream directly.

LuaJIT is built without `LJ_GC64` (`LUAJIT_DISABLE_GC64`), and that is a compatibility decision, not
an oversight. A save carries Lua closures as dumped bytecode, written either by the original x86
game (LuaJIT 2.0) or by an earlier Refined build, and both use the one-slot frame layout. The
two-slot layout of `LJ_GC64` numbers the registers of every call differently, so those closures
could be neither run nor translated, and the saves holding them would stop loading.

| Change | Why the engine needs it |
| --- | --- |
| `math.mod`, `string.gfind` | Lua 5.0 names the game scripts still use. |
| C-style comments, `lj_allow_escape_sequences()` | The lexer accepts what the original script engine accepted. |
| `coroutine.cstacksize` | Scripts written for LuaJIT 1.1 call it. |
| `lua_gc(LUA_GCTIMEOUT)` | Time-boxed collection behind `lua_gc_method 2`. |
| LuaJIT 2.0 bytecode in `lj_bcread.c` | Closures inside saves of the original game. |
| String IDs derived from content | Upstream hashes a table key by an ID handed out when its string is interned, so `pairs()` order depends on the history of the session. Scripts save state with one `pairs()` walk and load it with another, in another session. The ID is the unseeded LuaJIT 2.0 hash again, which also keeps the order existing saves were written in. |
| Casts at four sites | The LuaJIT target builds without a single warning at `/W3`. |
| Low-address arena | See below. |

Without `LJ_GC64` every GC object has to live below 2 GB, and stock LuaJIT asks the kernel for
that memory piecemeal, whenever a state grows. The engine maps its archives, more than 16 GB of
views, before the first state exists. Where Windows puts a view is a matter of ASLR policy: with
bottom-up randomization, the default, the views land far above 4 GB; with it switched off (a
common "gaming tweak", and the default of some stripped-down Windows builds) they are packed from
the lowest address up and the range below 2 GB is gone before LuaJIT gets any of it. The game then
died during device creation with dozens of `not enough memory` lines, or with `Cannot initialize
script virtual machine`, on a machine with plenty of free RAM.

LuaJIT therefore reserves its memory when the DLL is loaded, which the loader does before the
engine runs a single instruction: the largest free blocks below 2 GB, up to 1.5 GB, as address
space without commit charge. All states are served from that arena in 64 KB pages, the kernel is
the fallback behind it, and `luaJIT_lowmem()` reports the numbers. The engine prints them once at
startup and next to every Lua allocation failure:

```
* LuaJIT low memory: 1536 MB reserved in 1 region(s), 0 MB in use, peak 0 MB, 0 MB outside the arena
```

A line starting with `!` instead means that less than 512 MB could be reserved or that an
allocation failed; the numbers tell a starved process (`reserved` small) from scripts that really
used their memory up (`in use` close to `reserved`).

`tools\qa\luajit-lowmem\Run-LuaJitLowMemQa.ps1` takes the address space below 2 GB away from a
test process and runs a workload, an exhaustion and a two-thread case on top. With `-BaselineDll`
it also shows that a DLL without the arena fails under the same conditions, and that both DLLs
walk the tables of `table_order_fingerprint.lua` in the same order, which is the check to repeat
after every LuaJIT update. `tools\qa\luajit-lowmem\Run-LowMemEngineBoot.ps1 -Rig <clone>` boots a
QA clone with bottom-up ASLR switched off for the engine process alone and waits for a loaded
level; `qa_luajit_probe.script` adds a save for a second process to load.

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
