// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Intel XeSS Super Resolution's runtime (Externals/xess/README.md): libxess.dll for the DirectX 12 and Vulkan modules,
// libxess_dx11.dll for the DirectX 11 one, loaded once by the absolute path next to the executable and never unloaded
// (contexts may outlive any owner). The entry points are taken by name, so a missing DLL only leaves XeSS out.
#pragma once

#include <xess/xess.h>
#if defined(XR_D3D12)
#include <xess/xess_d3d12.h>
#elif defined(XR_VULKAN)
#include <vulkan/vulkan.h>
#include <xess/xess_vk.h>
#else
#include <xess/xess_d3d11.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
struct DaXess
{
    decltype(&xessGetVersion) GetVersion{};
    decltype(&xessDestroyContext) DestroyContext{};
    decltype(&xessSetVelocityScale) SetVelocityScale{};
    decltype(&xessSetJitterScale) SetJitterScale{};
    decltype(&xessSetLoggingCallback) SetLoggingCallback{};
    decltype(&xessIsOptimalDriver) IsOptimalDriver{};
#if defined(XR_D3D12)
    decltype(&xessD3D12CreateContext) CreateContext{};
    decltype(&xessD3D12Init) Init{};
    decltype(&xessD3D12Execute) Execute{};
#elif defined(XR_VULKAN)
    decltype(&xessVKCreateContext) CreateContext{};
    decltype(&xessVKInit) Init{};
    decltype(&xessVKExecute) Execute{};
    decltype(&xessVKGetRequiredInstanceExtensions) RequiredInstanceExtensions{};
    decltype(&xessVKGetRequiredDeviceExtensions) RequiredDeviceExtensions{};
    decltype(&xessVKGetRequiredDeviceFeatures) RequiredDeviceFeatures{};
#else
    decltype(&xessD3D11CreateContext) CreateContext{};
    decltype(&xessD3D11Init) Init{};
    decltype(&xessD3D11Execute) Execute{};
#endif
    xess_version_t version{};
};

// The module's runtime; null when its DLL is missing or incomplete.
const DaXess* da_xess();

// What r__xess's token value asks for: 1 native AA, 2 ultra quality plus, 3 ultra quality, 4 quality, 5 balanced,
// 6 performance, 7 ultra performance.
xess_quality_settings_t da_xess_quality(int quality);

#if defined(XR_VULKAN)
// The instance extensions XeSS needs (before the instance exists) and the device extensions and features (between
// the physical device's choice and vkCreateDevice), merged into the layer's own: dx11HW.cpp hands these to
// xrvk::CreateDevice next to Streamline's.
void da_xess_vk_instance_extensions(std::vector<const char*>& out);
void da_xess_vk_device_requirements(void* instance, void* physicalDevice, void* features2, std::vector<const char*>& out);
// Whether this device was made with what XeSS asked for (else it cannot run on it).
bool da_xess_vk_device_ready();
#endif
} // namespace xray::render::RENDER_NAMESPACE
