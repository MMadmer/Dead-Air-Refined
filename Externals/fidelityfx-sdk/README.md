# AMD FSR SDK 2.3.0 (ffx-api, vendored subset)

FSR upscaling (FSR 3.1.5 on any DX12 GPU, FSR 4 chosen by the loader on supported Radeons) and FSR
frame generation for the DirectX 12 renderer. Plain vendor drop, not a submodule; nothing here is
modified.

| | |
| --- | --- |
| Version | AMD FSR SDK 2.3.0, tag `v2.3.0` of https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (upscaler 4.1.1, frame generation swap chain 3.1.7) |
| Source | `https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/<path>`, each file checked against the git blob hash of the tag's tree (GitHub API) |
| Licence | `license.md` (AMD FSR SDK licence; the files taken here are all in its MIT list), `3rdpartynotice.md` |

## What was taken

| Here | SDK path | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `bin/amd_fidelityfx_loader_dx12.dll` | `Kits/FidelityFX/signedbin/` | 26,376 | `e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa` |
| `bin/amd_fidelityfx_upscaler_dx12.dll` | `Kits/FidelityFX/signedbin/` | 28,761,864 | `d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46` |
| `bin/amd_fidelityfx_framegeneration_dx12.dll` | `Kits/FidelityFX/signedbin/` | 40,085,776 | `02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18` |
| `lib/amd_fidelityfx_loader_dx12.lib` | `Kits/FidelityFX/signedbin/` | 2,880 | `01d76ff0ae9db6dbb335a765991cc14cda207d2597803d9df983dc14ab504902` |
| `include/api/include/ffx_api.h` | `Kits/FidelityFX/api/include/` | 10,726 | `e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd` |
| `include/api/include/ffx_api.hpp` | same | 5,884 | `e1fab07fa9ab489db74813e6b754fc91dad6ded68870ab558002e92b6a32566a` |
| `include/api/include/ffx_api_loader.h` | same | 2,977 | `18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e` |
| `include/api/include/ffx_api_types.h` | same | 13,636 | `cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49` |
| `include/api/include/dx12/ffx_api_dx12.h` | same `dx12/` | 11,323 | `c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530` |
| `include/api/include/dx12/ffx_api_dx12.hpp` | same `dx12/` | 1,859 | `31972deed9c5b94243eb72c97e24c1b4e1a0171ae0d0109f7fe842b6efdeddc7` |
| `include/upscalers/include/ffx_upscale.h` | `Kits/FidelityFX/upscalers/include/` | 19,313 | `65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf` |
| `include/upscalers/include/ffx_upscale.hpp` | same | 4,580 | `52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0` |
| `include/framegeneration/include/ffx_framegeneration.h` | `Kits/FidelityFX/framegeneration/include/` | 26,206 | `e6c42f8a38aab2c7c4fc6931545a8e5463fdf87349902ec9687d9fe4949a0566` |
| `include/framegeneration/include/ffx_framegeneration.hpp` | same | 5,786 | `826ba0a0385971ff5aa5651b51d4f4315c1ec40cbc391e2c4b48f4fe00a449cb` |
| `include/framegeneration/include/ffx_framegeneration_api_types.h` | same | 4,976 | `396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4` |
| `include/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` | same `dx12/` | 10,575 | `d533810bbd6fd4f87dfa2c1657c1457c7b1597b66345827d375b081d67127f66` |
| `include/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp` | same `dx12/` | 5,199 | `c4a4e2d7a97e92216d66d698216dcff690babce58e92671296353f479e27139c` |
| `license.md` | `Kits/FidelityFX/docs/license.md` | 60,891 | `6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963` |
| `3rdpartynotice.md` | `3rdpartynotice.md` | 2,441 | `94401a90486d1b53b528bb7f845a7b8daf7227b60eb7c0fc3b8c5571ba202a27` |

The three DLLs carry a valid Authenticode signature of Advanced Micro Devices (checked); file
versions: loader 2.3.0.2740, upscaler 4.1.1.2740, frame generation 4.0.1.2740. Together
68,874,016 bytes (65.7 MiB) added to the game package. The headers keep the SDK's directory
layout under `include/`, because they include each other by relative paths
(`../../api/include/ffx_api.h`).

## Build and runtime use

CMake (`cmake/XRay.Dependencies.cmake`):

- `AMD::FidelityFXAPI`: headers only. Include directories are the three `include/*/include` roots,
  so code includes `<ffx_api.h>`, `<ffx_api_loader.h>`, `<dx12/ffx_api_dx12.h>`, `<ffx_upscale.h>`,
  `<ffx_framegeneration.h>`, `<dx12/ffx_api_framegeneration_dx12.h>`. The renderer loads
  `<exe dir>\amd_fidelityfx_loader_dx12.dll` by absolute path and fills an `ffxFunctions` table with
  `ffxLoadFunctions()`. `ffx_api_loader.h` compiles to nothing unless `_WINDOWS` is defined; the
  engine's MSVC flags define it (`/D_WINDOWS`).
- `AMD::FidelityFXLoader`: the above plus `amd_fidelityfx_loader_dx12.lib` linked with
  `/DELAYLOAD:amd_fidelityfx_loader_dx12.dll`, for the `ffx::` C++ helpers of the `.hpp` headers,
  which call `ffxCreateContext` and friends directly. Load the DLL by absolute path first.
- `xray_stage_windows_runtime` copies the three DLLs next to `xrEngine.exe`, and
  `packaging/dead-air-x64/installer/runtime-files.txt` lists them. The loader finds the upscaler and
  frame generation DLLs next to itself.

Checked with `tools/qa/sdk/Run-SdkSmoke.ps1` on an RTX 3070 Laptop GPU (DX12, feature level 11_0): the loader
exports the five entry points; `ffxQueryDescGetVersions` lists upscaler providers `3.1.5`, `2.3.4`
and frame generation provider `3.1.6`; an upscaler context (with the mandatory
`ffxCreateContextDescUpscaleVersion`) is created on provider 3.1.5 and destroyed.

`ffx_framegeneration.h` and the FSR 3.1 DX11 host headers of `Externals/fidelityfx-dx11` both
define `FfxFrameGenerationConfig`: they cannot be included in the same translation unit. Every other
header of the two sets compiles together.

## Updating

List the tag's tree through `https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/git/trees/<tag>?recursive=1`,
download the same paths from `raw.githubusercontent.com`, compare each file with the blob hash of the
tree, check the AMD signatures of the DLLs, replace the files here, run `tools/qa/sdk/Run-SdkSmoke.ps1`
and update this file and the entry in `docs/dead-air/DEPENDENCIES.md`.
