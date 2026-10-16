# AMD FidelityFX SDK 1.1.4 ffx-api for Vulkan (vendored subset)

FSR 3.1 upscaling for the Vulkan renderer (`xrRender_R6.dll`). The AMD FSR SDK 2.x the DirectX 12 renderer uses
(`Externals/fidelityfx-sdk`) has no Vulkan runtime; FidelityFX SDK 1.1.4 ships one as a signed DLL behind the same
ffx-api. Plain vendor drop, not a submodule; nothing here is modified.

| | |
| --- | --- |
| Version | FidelityFX SDK 1.1.4, tag `v1.1.4` of https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (FSR 3.1 upscaler) |
| Source | `https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/<path>`, each file checked against the git blob hash of the tag's tree (GitHub API) |
| Signature | `amd_fidelityfx_vk.dll`: Authenticode, Advanced Micro Devices, Inc. |
| Licence | `LICENSE.txt` (MIT) |

## What was taken

| Here | SDK path | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `LICENSE.txt` | `LICENSE.txt` | 1,115 | `82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a` |
| `bin/amd_fidelityfx_vk.dll` | `PrebuiltSignedDLL/amd_fidelityfx_vk.dll` | 9,332,432 | `a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78` |
| `include/ffx_api/ffx_api.h` | `ffx-api/include/ffx_api/ffx_api.h` | 7,623 | `408b4203545e8299ed188fbb1d9ddfe0932b73ea6e8b14fe10edcf1f45771561` |
| `include/ffx_api/ffx_api.hpp` | `ffx-api/include/ffx_api/ffx_api.hpp` | 4,999 | `42470189c0241feca12eed959cf90ade15806bb9ecc578b65f8c20ca9365c34d` |
| `include/ffx_api/ffx_api_loader.h` | `ffx-api/include/ffx_api/ffx_api_loader.h` | 2,022 | `7be51b7bf1e640b84c0c7f22a9db50a6816debce3755b51224e56c89501659c3` |
| `include/ffx_api/ffx_api_types.h` | `ffx-api/include/ffx_api/ffx_api_types.h` | 14,748 | `dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe` |
| `include/ffx_api/ffx_upscale.h` | `ffx-api/include/ffx_api/ffx_upscale.h` | 13,946 | `1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1` |
| `include/ffx_api/ffx_upscale.hpp` | `ffx-api/include/ffx_api/ffx_upscale.hpp` | 3,729 | `6451754c2539a7e991429a7bd8365bb848f51abebad1982715f345989cc0a234` |
| `include/ffx_api/vk/ffx_api_vk.h` | `ffx-api/include/ffx_api/vk/ffx_api_vk.h` | 15,949 | `a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9` |
| `include/ffx_api/vk/ffx_api_vk.hpp` | `ffx-api/include/ffx_api/vk/ffx_api_vk.hpp` | 4,451 | `b4eab5ed043de7590c786c1904eba7064449ee8a8c9e302eb6363ccc7035172e` |

## How it is used

`src/Layers/xrRenderPC_R4/da_upscaler_fsr_vk.cpp` (Vulkan module only) loads `amd_fidelityfx_vk.dll` by its absolute
path next to the executable, takes the ffx-api entry points through `ffxLoadFunctions()` and creates the upscaler
with the `ffxCreateBackendVKDesc` of the renderer's own device (`xrvk::GetNativeDevice`). The headers are included
as `<ffx_api/...>`, so they never meet the SDK 2.3 headers of the DirectX 12 module. Nothing links against the
DLL, so the SDK's import library is not taken.
