# FSR 3.1.2 upscaler for DirectX 11 (static libraries)

AMD FSR 3.1.2 temporal upscaler with a DirectX 11 backend for the DirectX 11 renderer
(`xrRender_R4.dll`). AMD ships no DX11 backend for FSR 3.1; this is the community port
`optiscaler/FidelityFX-SDK-DX11` (MIT), built here with the engine's MSVC toolset. The shaders are
compiled into the backend library (FXC, `cs_5_0`), so nothing is shipped at runtime.

| | |
| --- | --- |
| Source | https://github.com/optiscaler/FidelityFX-SDK-DX11, branch `optiscaler-build`, commit `9b04fa49abae5aa341c0007af13c8ba1b4ad9913` (2026-09-10, "Fix FSR3 sampler state binding in DX11 backend") |
| FSR version | 3.1.2 (`FFX_FSR3UPSCALER_VERSION_*` in `include/FidelityFX/host/ffx_fsr3upscaler.h`) |
| Changes | `fidelityfx-sdk-dx11-msvc.patch` (see below) |
| Toolset | Visual Studio 2026 18.5, MSVC 14.50.35717 (cl 19.50.35729), Windows SDK 10.0.26100.0, CMake 4.3.4 |
| Licence | MIT, `LICENSE.txt` (Copyright 2023 Advanced Micro Devices, Inc.); third-party code inside the libraries below |

## Contents

| File | Configuration | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `lib/ffx_fsr3upscaler_x64.lib` | Release `/MD` | 94,786 | `83642d6917464ed19897e4018c1e8f576cc0059699b990deaf317429e5203c7b` |
| `lib/ffx_backend_dx11_x64.lib` | Release `/MD` | 4,051,256 | `aeb40a83ad45b98faef09383d5323cd2cb54d44dad531d5822f3f2a6620cb660` |
| `lib/ffx_fsr3upscaler_x64d.lib` | Debug `/MDd` | 248,864 | `ace7889575cc186f3fcf1c812700b55702b23186d028b1d52de31ce3f6645bba` |
| `lib/ffx_backend_dx11_x64d.lib` | Debug `/MDd` | 5,480,798 | `c2e33f67f6bf2668f1b099e171f4f4c0b3aaf0566184afd53ad63a3e9af1e982` |

`include/FidelityFX/host/`: the public host headers the two libraries need, unchanged from the
commit (`ffx_assert.h`, `ffx_error.h`, `ffx_fsr3upscaler.h`, `ffx_interface.h`, `ffx_types.h`,
`ffx_util.h`, `backends/dx11/ffx_dx11.h`).

The Release libraries are `/MD` (Release, Mixed and ReleaseMasterGold configurations of the engine),
the Debug ones `/MDd`. They import nothing beyond the C runtime and `kernel32` (no `d3d11.lib` or
`dxguid.lib` needed: the backend works through the device it is given and defines its GUIDs itself).
Linked into a module they add about 3.4 MB (almost all of it shader blobs).

## The patch

`fidelityfx-sdk-dx11-msvc.patch` applies to the commit above (`git apply`):

1. `ffx_dx11.cpp`: `ffxGetResourceDX11_Fsr31` takes `const ID3D11Resource*`, as the header declares.
   Without it the definition is a C++ overload and the `extern "C"` symbol stays undefined.
2. `ffx_dx11.cpp`: `fp16Supported` is always false. The port derived it from D3D11 min-precision
   support, which is not SM 6.2 fp16; the FXC permutation tables have no 16-bit variants, so a
   16-bit lookup returned an empty blob and `CreateComputeShader` failed.
3. `ffx_dx11.cpp`: the binding names are widened with `MultiByteToWideChar` instead of the
   deprecated `std::wstring_convert`.
4. `sdk/CMakeLists.txt`: `FFX_SC_EXECUTABLE` can be passed in, so the shader compiler is built from the
   SDK's own source (`sdk/tools/ffx_shader_compiler`) instead of running the unsigned
   `FidelityFX_SC.exe` committed to the fork.
5. `sdk/src/backends/dx11/CMakeLists.txt` + `sdk/src/backends/shared/ffx_shader_blobs.cpp`: option
   `FFX_DX11_FSR3UPSCALER_ONLY` (define `FFX_FSR3UPSCALER_ONLY`) builds the backend with the FSR3
   upscaler's shader blobs only. The fork links the FSR 1, FSR 2, frame interpolation and optical flow
   blobs too, which the game never uses.

## Build

From a VS x64 developer prompt (`VsDevCmd.bat -arch=x64 -host_arch=x64`, found through `vswhere`
like `tools\build\build_x64.ps1`) in a checkout on a short path outside `%TEMP%` (the fork's
shader generation fails silently under `%TEMP%`):

```bat
git clone -b optiscaler-build https://github.com/optiscaler/FidelityFX-SDK-DX11 ffx-dx11
cd ffx-dx11
git checkout 9b04fa49abae5aa341c0007af13c8ba1b4ad9913
git apply <repo>\Externals\fidelityfx-dx11\fidelityfx-sdk-dx11-msvc.patch

cmake -S sdk\tools\ffx_shader_compiler -B _build\sc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev
cmake --build _build\sc
cmake -S sdk -B _build\fsr3 -G "Visual Studio 18 2026" -A x64 -Wno-dev -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DFFX_API_CUSTOM=OFF -DFFX_API_VK=OFF -DFFX_API_DX12=OFF -DFFX_ALL=OFF -DFFX_API_DX11=ON ^
  -DFFX_FSR=ON -DFFX_FSR1=OFF -DFFX_FSR2=OFF -DFFX_FI=OFF -DFFX_OF=OFF -DFFX_FSR3UPSCALER=ON ^
  -DFFX_DX11_FSR3UPSCALER_ONLY=ON -DFFX_AUTO_COMPILE_SHADERS=1 ^
  "-DFFX_SC_EXECUTABLE=%CD%\sdk\tools\ffx_shader_compiler\bin\FidelityFX_SC.exe"
cmake --build _build\fsr3 --config Release --target ffx_fsr3upscaler_x64 ffx_backend_dx11_x64 --parallel 8 -- /p:CL_MPcount=16
cmake --build _build\fsr3 --config Debug --target ffx_fsr3upscaler_x64 ffx_backend_dx11_x64 --parallel 8 -- /p:CL_MPcount=16
```

The libraries land in `sdk\bin\ffx_sdk\` (`*_x64.lib` Release, `*_x64d.lib` Debug). The shader
permutations take about ten minutes per configuration (the Visual Studio generator regenerates them
for each one). `FFX_FSR3UPSCALER=ON` has to be given explicitly: the fork's `BuildFidelityFXDX11.bat`
builds the backend alone.

## Use

CMake target `AMD::FidelityFXDX11` (`cmake/XRay.Dependencies.cmake`): the upscaler library, the
backend library (`AMD::FidelityFXDX11Backend`) and the include directory; code includes
`<FidelityFX/host/ffx_fsr3upscaler.h>` and `<FidelityFX/host/backends/dx11/ffx_dx11.h>`.

These headers and `ffx_framegeneration.h` of the AMD FSR SDK 2.3.0 (`Externals/fidelityfx-sdk`)
both define `FfxFrameGenerationConfig`, so the two cannot share a translation unit.

Checked with `tools/qa/sdk/Run-SdkSmoke.ps1` (Release and Debug) on an RTX 3070 Laptop GPU: a D3D11
device, `ffxGetInterfaceDX11`, `ffxFsr3UpscalerContextCreate` (which creates every pass's compute
shader from the blobs), three `ffxFsr3UpscalerContextDispatch` calls from 1280x720 RGBA8 colour,
R32F depth and RG16F motion vectors to a 1920x1080 RGBA16F UAV, `ffxFsr3UpscalerContextDestroy`; all
`FFX_OK` and the device stays alive. After a dispatch the backend unbinds its compute UAVs and SRVs
and restores the compute samplers, but leaves its compute shader and constant buffers bound on the
context it was given: the renderer's state cache has to be invalidated afterwards
(`RCache.Invalidate()`).

## Third-party code inside the libraries

- `DXBCChecksum.c` (backend): DXBC checksum by Seth Sowerby, derived from the RSA Data Security, Inc.
  MD5 Message Digest Algorithm.
- `md5.c` (backend): the RSA Data Security, Inc. MD5 Message Digest Algorithm, Copyright (C) 1990,
  RSA Data Security, Inc. All rights reserved. Its licence permits copying and use provided it is
  identified as the "RSA Data Security, Inc. MD5 Message Digest Algorithm" in all material
  mentioning or referencing it; RSA Data Security, Inc. makes no representations concerning its
  merchantability or suitability for any purpose, and it is provided "as is" without express or
  implied warranty of any kind.
