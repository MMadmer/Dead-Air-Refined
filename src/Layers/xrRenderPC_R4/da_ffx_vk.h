// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The ffx-api of AMD FidelityFX SDK 1.1.4's signed Vulkan runtime (Externals/fidelityfx-vk), shared by FSR
// upscaling (da_upscaler_fsr_vk.cpp) and FSR frame generation (da_framegen_fsr_vk.cpp). Vulkan module only.
#pragma once

#ifdef XR_VULKAN
#include <vulkan/vulkan.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>

namespace xray::render::RENDER_NAMESPACE
{
// amd_fidelityfx_vk.dll, loaded once by its absolute path next to the executable and never unloaded (contexts
// and their threads may outlive any owner). Null when it is missing or incomplete.
const ffxFunctions* da_ffx_vk();

// The runtime makes, presents and destroys its swap chains through its own imports of vulkan-1.dll. Where
// Streamline runs, those imports are pointed at its interposer, so its bookkeeping sees every present (the
// DirectX 12 twin makes FFX's chain through Streamline's factory). Once, before the first swap chain.
void da_ffx_vk_route_swapchain();

// The vkGetDeviceProcAddr an ffx-api backend gets: the layer's, with the core names of extension aliases.
// Null without a Vulkan device.
PFN_vkGetDeviceProcAddr da_ffx_vk_device_proc();
} // namespace xray::render::RENDER_NAMESPACE
#endif
