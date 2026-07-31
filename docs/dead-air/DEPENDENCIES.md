# Dead Air: Refined x64 dependencies

This file records the dependency state used by the Windows x64 runtime. A version is upgraded only when its public API, binary ABI, and Dead Air behavior remain compatible with the engine.

## Source submodules

| Component | Revision | Notes |
| --- | --- | --- |
| AMD AGS SDK | `v6.3.1` (`086c47ed8`) | Current public release. |
| Dear ImGui | docking `b334d19b6` | Current docking branch, based on 1.92.9. |
| GameSpy | `3e434803d` | Current OpenXRay fork plus local x64 warning fixes. |
| GLI | `3542f8830` | Current upstream revision. |
| LuaJIT | upstream `5a5cd82e4` | One local Dead Air bytecode-compatibility commit and local x64 warning fixes are applied on top. |
| Luabind | upstream `8da131b83` | One local Swiss-table integration commit is applied on top. |
| SSE2NEON | `3b70b3727` | Current upstream revision. |
| SSE2RVV | `f1ab91659` | Current upstream revision. |
| xrLuaFix | upstream `0e89050f3` | One local x64 compatibility commit is applied on top. |
| zlib | `v1.3.2` (`da607da73`) | Built with warnings enabled and treated as errors. |

## NuGet dependencies

| Component | Version |
| --- | --- |
| SDL2 | 2.32.8 |
| DirectXMath | 2026.6.12.1 |
| DirectXTex Desktop | 2025.10.28.1 |

## Vendored source and generated bindings

| Component | Version or revision | Notes |
| --- | --- | --- |
| parallel-hashmap | master `48f4c5fb0` | Swiss-table containers used by engine lookup structures. |
| Tracy | 0.13.1 | Client source only. |
| GLAD | 2.0.8 | Regenerated for the existing OpenGL/GLES API surface. |
| DirectXMesh | may2026 `8c6fdb1c` | Current `FlexibleVertexFormat.h` with the existing X-Ray integration retained. |
| NVAPI | `cd6918f6` | Current public SDK headers and x64 import library. |

## Built and packaged x64 libraries

| Component | Version | Notes |
| --- | --- | --- |
| mimalloc | 3.4.3 | Release and Debug static libraries use the dynamic MSVC runtime. |
| OpenSSL | 4.0.1 | `libcrypto-4-x64.dll` replaces the OpenSSL 3 runtime. |
| libjpeg-turbo | 3.2.0 | Release and Debug static libraries include runtime SIMD dispatch. |
| OpenAL Soft | 1.25.2 package | The official package still reports 1.25.1 in its Windows version resource. |
| libogg | 1.3.6 (`06a5e026`) | Rebuilt with embedded debug information and the dynamic MSVC runtime. |
| libvorbis | 1.3.7 (`e3c9861f`) | Rebuilt with embedded debug information and the dynamic MSVC runtime. |
| libtheora | `28fd5ec7` | Current upstream revision, rebuilt with the dynamic MSVC runtime. |
| LZO | 2.10 | Latest upstream release, rebuilt with the dynamic MSVC runtime. |

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

## Build policy

- Windows x64 Release solution builds use warnings-as-errors.
- Third-party libraries are rebuilt with the dynamic MSVC runtime where the engine owns their allocation boundary.
- Local compatibility changes are kept as explicit commits or small project-level patches.
- Downloaded source trees and build tools live under `_work` and are not part of the repository or release package.
