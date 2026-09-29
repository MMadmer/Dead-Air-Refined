# NVIDIA Streamline 2.14.1 (vendored subset)

DLSS Super Resolution and DLAA (D3D11 and D3D12), DLSS Frame Generation (D3D12 only) and Reflex
for the Dead Air: Refined renderers. Plain vendor drop, not a submodule; nothing here is modified.

| | |
| --- | --- |
| Version | 2.14.1 (`include/sl_version.h`), tag `v2.14.1`, released 2026-09-08 |
| Source | https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip |
| Archive | 275,994,000 bytes, SHA-256 `92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b` (checked after download) |
| Licences | Streamline: MIT (`license.txt`, `3rd-party-licenses.md`). `nvngx_dlss.dll`, `nvngx_dlssg.dll`: NVIDIA RTX SDKs licence (`bin/x64/nvngx_dlss.license.txt`). Reflex: `bin/x64/reflex.license.txt`. |

## What was taken

- `include/`: all 24 public headers of the archive, unchanged.
- `lib/x64/sl.interposer.lib`: import library of the interposer (65,176 bytes).
- `bin/x64/`: the **production** binaries only (never `bin/x64/development/`, which draw a watermark).
  Every file carries a valid Authenticode signature of NVIDIA Corporation (checked).

| File | Bytes | File version | SHA-256 |
| --- | ---: | --- | --- |
| `sl.interposer.dll` | 652,928 | 2.14.1.0 | `8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c` |
| `sl.common.dll` | 843,392 | 2.14.1.0 | `82924a8954dd671e09351c5de0eb87ad0eb25b944cc9f9ab955ca1d9950de15d` |
| `sl.pcl.dll` | 360,064 | 2.14.1.0 | `f13d51cfa05f4cd514df2026049e2db8adf359221713170ad386fd499915b582` |
| `sl.reflex.dll` | 388,736 | 2.14.1.0 | `0ce9725e3e03ea9e7f81d008b57f33ee365973d2e349131c8b1c3e3378fe2db0` |
| `sl.dlss.dll` | 422,016 | 2.14.1.0 | `73bf52c0cfaa5900a8f3f4a91306e4625e7cca696dfb305aae44c9f97b582e1f` |
| `sl.dlss_g.dll` | 636,032 | 2.14.1.0 | `f4a6b2b14dcc0b1485989e430d3b4e3a44ac1800b92ba1ad74f476e64fb2b09c` |
| `nvngx_dlss.dll` | 58,956,912 | 310.9.1.0 | `3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983` |
| `nvngx_dlssg.dll` | 7,460,976 | 310.9.1.0 | `ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82` |

Together 69,721,056 bytes (66.5 MiB) added to the game package.

- Licence files: `license.txt` (MIT), `3rd-party-licenses.md`, `bin/x64/nvngx_dlss.license.txt`,
  `bin/x64/reflex.license.txt`.

Left out on purpose: `bin/x64/development/*`, the plugins the game does not use (`sl.nis`, `sl.dlss_d`,
`sl.deepdvc`, `sl.directsr`, `sl.nvperf` - the latter under the Nsight Perf SDK licence),
`NvLowLatencyVk.dll` (Vulkan only), `nvngx_dlssd.dll`, `nvngx_deepdvc.dll`, sources, symbols, docs,
`scripts/*.json` (development configuration, ignored by production DLLs).

## Build and runtime use

CMake (`cmake/XRay.Dependencies.cmake`):

- `NVIDIA::StreamlineHeaders`: include directory plus `wintrust`/`crypt32` for `sl_security.h`. For a
  module that loads `sl.interposer.dll` itself (`LoadLibraryW` + `GetProcAddress("slInit")` ...).
- `NVIDIA::Streamline`: the above plus `sl.interposer.lib` linked with `/DELAYLOAD:sl.interposer.dll`
  (and `delayimp`). The module loads without the DLL; the first `sl*` call binds the delay-load. The
  `sl_*.h` helper functions (`slDLSSGetOptimalSettings`, `slDLSSGSetOptions`, ...) need
  `slGetFeatureFunction` at link time, so this is the target a renderer normally links.
- `xray_stage_windows_runtime` copies the eight DLLs above next to `xrEngine.exe`, and
  `packaging/dead-air-x64/installer/runtime-files.txt` lists them.

Rules for the integration (checked with `tools/qa/sdk/Run-SdkSmoke.ps1` on an RTX 3070 Laptop GPU,
driver 616.92):

1. Verify, then load by absolute path, before the first `sl*` call:
   `sl::security::verifyEmbeddedSignature(<exe dir>\sl.interposer.dll)`, then
   `LoadLibraryExW(<exe dir>\sl.interposer.dll)`. The delay-load helper then binds to that module.
   The interposer verifies every plugin it loads itself.
2. `sl_security.h` defines functions and globals (not `inline`): include it in exactly one
   translation unit per module, otherwise the link fails with LNK2005. It also prints its verdict
   with `printf`.
3. `slInit` runs early, never inside `DllMain`. Manual hooking (`eUseManualHooking`), plugin path =
   the exe directory, `pathToLogsAndData = nullptr` (no log file). In the test it took about 2-5 s
   (NGX probing of the DLSS-G minimum spec), so it belongs off the critical path of the first frame.
   The default flags also enable OTA updates (`eAllowOTA | eLoadDownloadedPlugins`), which NVIDIA
   recommends keeping; the test switched them off.
4. Result on that GPU: `slIsFeatureSupported` DLSS = ok, Reflex = ok, PCL = ok,
   DLSS-G = `eErrorNoSupportedAdapterFound` (RTX 40 or newer is required, plus HAGS);
   `slGetFeatureRequirements` flags DLSS 0x7 (D3D11 | D3D12 | Vulkan), DLSS-G 0x16 (D3D12 | Vulkan |
   hardware scheduling required).
5. `slShutdown` before the device and swap chain are released.

## Updating

Download the new release zip from the URL pattern above, check its size and SHA-256 against the
GitHub release, replace the files listed here from `include/`, `lib/x64/` and the production
`bin/x64/` (never `development/`), check every DLL's NVIDIA signature, run `tools/qa/sdk/Run-SdkSmoke.ps1` (Release and Debug), update
this file and the Streamline entry in `docs/dead-air/DEPENDENCIES.md`. Never re-sign or modify the DLLs: NGX refuses a
DLL whose NVIDIA signature is broken.
