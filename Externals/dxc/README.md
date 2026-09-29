# Microsoft DirectX Shader Compiler 1.9.2607 (vendored subset)

The shader compiler of the DirectX 12 renderer (`xrRender_R5.dll`): engine shaders as DXIL / Shader
Model 6, native passes (ray tracing, mesh shaders). Plain vendor drop, not a submodule; nothing here
is modified. `xrRender_R4.dll` keeps FXC (`d3dcompiler_47.dll`) and never loads these DLLs.

| | |
| --- | --- |
| Version | 1.9.2607 ("DX Compiler Release for July 2026"), file version 1.9.0.5402 |
| Source | https://github.com/microsoft/DirectXShaderCompiler/releases (asset `dxc_2026_07_29.zip`) |
| Archive | SHA-256 `a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241` (checked after download) |
| Licences | `dxcompiler.dll`: University of Illinois/NCSA (`LICENSE-LLVM.txt`) and MIT (`LICENCE-MIT.txt`); `dxil.dll` and the distributables: Microsoft Software License Terms (`LICENSE-MS.txt`) |

## What was taken

- `bin/x64/`: the two runtime DLLs. Both carry a valid Authenticode signature of Microsoft
  Corporation (checked).

| File | Bytes | File version | SHA-256 |
| --- | ---: | --- | --- |
| `dxcompiler.dll` | 28,079,968 | 1.9.0.5402 | `9a5100511e127c6a2fc78edf984f95074a76d35b90c90c4d342430a5ae160e9b` |
| `dxil.dll` | 3,831,600 | 1.9.0.5402 | `feb57253eff0a622561e29b44cedbe86b89fc9a5bc8dc00fa2f98fafd712c2d8` |

Together 31,911,568 bytes (30.4 MiB) added to the game package.

- Licence files: `LICENSE-MS.txt`, `LICENSE-LLVM.txt`, `LICENCE-MIT.txt`, unchanged.

Left out on purpose: headers (`dxcapi.h` and `d3d12shader.h` come from the Windows SDK 10.0.26100,
whose COM interfaces match this release), import libraries (the DLLs are loaded by absolute path),
`dxc.exe`, `dxv.exe`, the x86 and arm64 builds.

## Build and runtime use

CMake (`cmake/XRay.Dependencies.cmake`): `XRAY_DXC_RUNTIME_FILES` lists both DLLs;
`xray_stage_windows_runtime` copies them next to `xrEngine.exe`, and
`packaging/dead-air-x64/installer/runtime-files.txt` lists them. The layer loads `dxcompiler.dll`
from the executable's directory with `LoadLibraryExW` and the full path; `dxil.dll` must sit next to
it, because it signs the DXIL and the D3D12 runtime refuses unsigned shaders.

## Obligations (Microsoft Software License Terms, `LICENSE-MS.txt`)

The DLLs are shipped unmodified, as part of an application that adds significant primary
functionality; distributors and end users are bound to terms at least as protective of Microsoft
(`docs/dead-air/DEPENDENCIES.md`); no Microsoft trademark is used to suggest endorsement.
