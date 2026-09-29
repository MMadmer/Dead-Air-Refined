# Microsoft DirectStorage 1.3.0 (vendored subset)

Texture (and level geometry) loading through DirectStorage queues for the DirectX 12 renderer
(`xrRender_R5.dll`). Plain vendor drop of the NuGet package, not a submodule; nothing here is
modified. `xrRender_R4.dll` never loads these DLLs.

| | |
| --- | --- |
| Version | 1.3.0 (NuGet `Microsoft.Direct3D.DirectStorage`), DLL file version 1.3.2506.2501 |
| Source | https://www.nuget.org/packages/Microsoft.Direct3D.DirectStorage/1.3.0 |
| Package | 2,816,824 bytes, SHA-256 `8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177` (checked after download) |
| Licences | Binaries (`bin/`): Microsoft Software License Terms, DirectStorage SDK (`LICENSE.txt`); headers (`include/`): MIT (`LICENSE-CODE.txt`); third-party notices of the runtime: `NOTICES.txt` |

## What was taken

- `include/`: `dstorage.h`, `dstorageerr.h`, unchanged.
- `lib/x64/dstorage.lib`: import library (2,538 bytes), for tools that link the runtime (the QA probe
  `tools/qa/dstorage/`); the engine loads the DLL itself.
- `bin/x64/`: the two distributables named in `distributable_files.txt`. Both carry a valid
  Authenticode signature of Microsoft Corporation (checked).

| File | Bytes | File version | SHA-256 |
| --- | ---: | --- | --- |
| `dstorage.dll` | 206,904 | 1.3.2506.2501 | `cae65787f61514bec6f012f44546e68b19e39d8854a9ab52af813370d44b272a` |
| `dstoragecore.dll` | 1,482,784 | 1.3.2506.2501 | `cf85b19a92a8276f8baca0db9bbcd60735bad50768deb303d310e84d5f4fdd11` |

Together 1,689,688 bytes (1.6 MiB) added to the game package.

- Licence files: `LICENSE.txt`, `LICENSE-CODE.txt`, `NOTICES.txt`, `distributable_files.txt`.

Left out on purpose: the x86 and ARM64 builds, the WinMD, the MSBuild targets, 1.4 (preview only:
Zstd, GACL; not used).

## Build and runtime use

CMake (`cmake/XRay.Dependencies.cmake`):

- `Microsoft::DirectStorageHeaders`: include directory only. The renderer loads `dstorage.dll` from
  the executable's directory by its full path and resolves `DStorageGetFactory` /
  `DStorageSetConfiguration1` itself, so a machine without the DLLs still runs (DirectStorage off).
- `XRAY_DSTORAGE_RUNTIME_FILES` lists both DLLs; `xray_stage_windows_runtime` copies them next to
  `xrEngine.exe`, and `packaging/dead-air-x64/installer/runtime-files.txt` lists them.
- Telemetry is switched off through `DStorageSetConfiguration1` before the factory is created.

## Obligations (Microsoft Software License Terms, `LICENSE.txt`)

The DLLs are shipped unmodified, as part of an application that adds significant primary
functionality; distributors and end users are bound to terms at least as protective of Microsoft
(`docs/dead-air/DEPENDENCIES.md`); no Microsoft trademark is used to suggest endorsement;
`NOTICES.txt` travels with the binaries.
