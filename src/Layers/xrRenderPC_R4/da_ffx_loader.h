// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The AMD FSR SDK 2.3 loader (amd_fidelityfx_loader_dx12.dll, ffx-api), shared by FSR upscaling
// and FSR frame generation on the DirectX 12 module: loaded once by its absolute path next to the
// executable, never unloaded. The loader picks the provider per adapter (FSR 4 on RX 7000/9000,
// FSR 3.1 elsewhere) from the DLLs next to it. See Externals/fidelityfx-sdk/README.md.
#pragma once

#ifdef XR_D3D12
#include <ffx_api.h>
#include <ffx_api_loader.h>

struct ID3D12Device;

namespace xray::render::RENDER_NAMESPACE
{
// The five ffx-api entry points; null when the loader is missing or incomplete (FSR is off then).
const ffxFunctions* da_ffx();
// How many providers the loader offers for a context type (FFX_API_CREATE_CONTEXT_DESC_TYPE_*)
// on this device: 0 = none, no loader or no device. Asked once per type and device; the first
// answer goes to the log under `what` ("upscaling", "frame generation").
u32 da_ffx_providers(u64 createDescType, ID3D12Device* device, pcstr what);
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_D3D12
