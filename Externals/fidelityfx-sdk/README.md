# AMD FSR SDK 2.4.0 (ffx-api, vendored subset)

FSR upscaling (FSR 4.1 on Radeon RX 6000 and newer and on recent Ryzen AI graphics, FSR 3.1.6 on any other GPU, the
loader picks per adapter) and FSR frame generation for the DirectX 12 and Vulkan renderers. Plain vendor drop, not a
submodule; nothing here is modified.

| | |
| --- | --- |
| Version | AMD FSR SDK 2.4.0, tag `v2.4.0` of https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (upscaler 4.1.2, frame generation 4.0.2, frame generation swap chain 4.0.0, DX12 and Vulkan) |
| Source | `https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/<path>`, each file checked against the git blob hash of the tag's tree (GitHub API) |
| Licence | `license.md` (AMD FSR SDK licence; the files taken here are all in its MIT list, the Vulkan DLLs included), `3rdpartynotice.md` |

## What was taken

| Here | SDK path | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `bin/amd_fidelityfx_framegeneration_dx12.dll` | `Kits/FidelityFX/signedbin/` | 29,679,376 | `e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6` |
| `bin/amd_fidelityfx_framegeneration_vk.dll` | `Kits/FidelityFX/signedbin/` | 36,806,928 | `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4` |
| `bin/amd_fidelityfx_loader_dx12.dll` | `Kits/FidelityFX/signedbin/` | 39,184 | `e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb` |
| `bin/amd_fidelityfx_loader_vk.dll` | `Kits/FidelityFX/signedbin/` | 39,184 | `38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9` |
| `bin/amd_fidelityfx_upscaler_dx12.dll` | `Kits/FidelityFX/signedbin/` | 57,664,272 | `aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace` |
| `bin/amd_fidelityfx_upscaler_vk.dll` | `Kits/FidelityFX/signedbin/` | 36,135,696 | `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7` |
| `lib/amd_fidelityfx_loader_dx12.lib` | `Kits/FidelityFX/signedbin/` | 2,880 | `384123f21f43f263047250ac28d1b0e9a7ed4ea5b37391af2e89fdc3f964d50c` |
| `lib/amd_fidelityfx_loader_vk.lib` | `Kits/FidelityFX/signedbin/` | 2,846 | `7f0b0186c7c207779bee56f44ee6a7881b9c6ebb32038a95e8b960ccce9b3689` |
| `include/api/include/dx12/ffx_api_dx12.h` | `Kits/FidelityFX/api/include/dx12/` | 11,323 | `c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530` |
| `include/api/include/dx12/ffx_api_dx12.hpp` | `Kits/FidelityFX/api/include/dx12/` | 1,860 | `a4f02dcf86ed5c05f76b3cb83d8b3410f0b8c96f5d6042e48cca30dd25bac7f2` |
| `include/api/include/ffx_api.h` | `Kits/FidelityFX/api/include/` | 10,848 | `4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf` |
| `include/api/include/ffx_api.hpp` | `Kits/FidelityFX/api/include/` | 5,980 | `af441486342abfeb189c5672f3216a28446b582ad314ec4e754b81e20bb8a840` |
| `include/api/include/ffx_api_loader.h` | `Kits/FidelityFX/api/include/` | 2,977 | `18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e` |
| `include/api/include/ffx_api_matrix_helpers.h` | `Kits/FidelityFX/api/include/` | 3,343 | `f37055b1980562b75295ac0ee3f4b60dd314c8c2f21a41b7d71d9f9070858bd5` |
| `include/api/include/ffx_api_matrix_helpers.hpp` | `Kits/FidelityFX/api/include/` | 1,923 | `8f671dadf9e1267dd8a51d224e11b27656ddb9da51ef9524a177b89114548737` |
| `include/api/include/ffx_api_types.h` | `Kits/FidelityFX/api/include/` | 13,637 | `53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c` |
| `include/api/include/vk/ffx_api_vk.h` | `Kits/FidelityFX/api/include/vk/` | 12,253 | `09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad` |
| `include/api/include/vk/ffx_api_vk.hpp` | `Kits/FidelityFX/api/include/vk/` | 1,866 | `7757c3ffa6b1e189bca5eec3b8c93aa8f802386642648a0dfeb8c2c52502e5ba` |
| `include/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` | `Kits/FidelityFX/framegeneration/include/dx12/` | 10,575 | `8ddb1cb78f83caf3598509df4968e84c851504cd6c00d9923261a5fc2c7ef0ed` |
| `include/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp` | `Kits/FidelityFX/framegeneration/include/dx12/` | 5,199 | `c4a4e2d7a97e92216d66d698216dcff690babce58e92671296353f479e27139c` |
| `include/framegeneration/include/ffx_framegeneration.h` | `Kits/FidelityFX/framegeneration/include/` | 26,606 | `bc78a18aa95f5157b4699b0602700592b624788c5bc55d1762cb46a1b6557ca1` |
| `include/framegeneration/include/ffx_framegeneration.hpp` | `Kits/FidelityFX/framegeneration/include/` | 5,939 | `43854713e6e95f28a1dab783c20879f3c6cc465281ef8ad1c1a55d81c91a4a8d` |
| `include/framegeneration/include/ffx_framegeneration_api_types.h` | `Kits/FidelityFX/framegeneration/include/` | 4,976 | `396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4` |
| `include/framegeneration/include/vk/ffx_api_framegeneration_vk.h` | `Kits/FidelityFX/framegeneration/include/vk/` | 11,044 | `321d3138005757759d9ee05281d3dcd4cc2c3608bc1d524f9b1d0a6dadd49f61` |
| `include/framegeneration/include/vk/ffx_api_framegeneration_vk.hpp` | `Kits/FidelityFX/framegeneration/include/vk/` | 5,067 | `418b481f9128ed79205c602b0bb6804b3f495258d82a5b887d36db5c04bf7239` |
| `include/upscalers/include/ffx_upscale.h` | `Kits/FidelityFX/upscalers/include/` | 19,576 | `13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d` |
| `include/upscalers/include/ffx_upscale.hpp` | `Kits/FidelityFX/upscalers/include/` | 4,580 | `52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0` |
| `license.md` | `Kits/FidelityFX/docs/license.md` | 65,770 | `32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb` |
| `3rdpartynotice.md` | `3rdpartynotice.md` | 2,519 | `3b69b1a1011622557a81b55a9a211dd797d01b8160924a5ec51b4c3432d33a4f` |

The six DLLs carry a valid Authenticode signature of Advanced Micro Devices (checked); file versions: loaders
2.3.0.4555 (DX12) and 1.0.2.4555 (Vulkan), upscalers 4.1.2.4555, frame generation 4.0.2.4555. Together
160,364,640 bytes (152.9 MiB) added to the game package. The headers keep the SDK's directory layout under
`include/`, because they include each other by relative paths (`../../api/include/ffx_api.h`).

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
- The Vulkan module includes the same roots (`<vk/ffx_api_vk.h>`, `<vk/ffx_api_framegeneration_vk.h>`) and loads
  `<exe dir>\amd_fidelityfx_loader_vk.dll` the same way (`da_ffx_vk.cpp`); it loads
  `amd_fidelityfx_framegeneration_vk.dll` itself first, to route that DLL's swap chain imports through Streamline.
- `xray_stage_windows_runtime` copies the six DLLs next to `xrEngine.exe`, and
  `packaging/dead-air-x64/installer/runtime-files.txt` lists them. Each loader finds the upscaler and
  frame generation DLLs of its API next to itself.

Checked in game with `tools/qa/water/upscaler_tour.lua` on DirectX 12 and Vulkan (FSR Quality, FSR with frame
generation): the provider each context got is in the log's `[FSR]` and `[FSR FG]` ready lines.

`ffx_framegeneration.h` and the FSR 3.1 DX11 host headers of `Externals/fidelityfx-dx11` both
define `FfxFrameGenerationConfig`: they cannot be included in the same translation unit. Every other
header of the two sets compiles together.

## Updating

List the tag's tree through `https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/git/trees/<tag>?recursive=1`,
download the same paths from `raw.githubusercontent.com`, compare each file with the blob hash of the
tree, check the AMD signatures of the DLLs, replace the files here, run `tools/qa/sdk/Run-SdkSmoke.ps1`
and update this file and the entry in `docs/dead-air/DEPENDENCIES.md`.
