// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Intel XeSS Super Resolution's runtime (da_xess.h).

#include "stdafx.h"

#include "da_xess.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
#if defined(XR_D3D12) || defined(XR_VULKAN)
constexpr const wchar_t* kXessDll = L"libxess.dll";
constexpr pcstr kXessDllName = "libxess.dll";
#else
constexpr const wchar_t* kXessDll = L"libxess_dx11.dll";
constexpr pcstr kXessDllName = "libxess_dx11.dll";
#endif

template <typename T>
bool take(HMODULE module, T& to, pcstr name)
{
    to = reinterpret_cast<T>(GetProcAddress(module, name));
    return to != nullptr;
}

const DaXess* load()
{
    static DaXess api{};
    static const DaXess* loaded = []() -> const DaXess* {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
        if (!slash || wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), kXessDll))
            return nullptr;
        const HMODULE module = LoadLibraryW(path);
        if (!module)
        {
            Msg("~ [XeSS] %s cannot be loaded (error %u): XeSS is off", kXessDllName, u32(GetLastError()));
            return nullptr;
        }
        bool ok = take(module, api.GetVersion, "xessGetVersion") && take(module, api.DestroyContext, "xessDestroyContext") &&
            take(module, api.SetVelocityScale, "xessSetVelocityScale") &&
            take(module, api.SetJitterScale, "xessSetJitterScale") &&
            take(module, api.SetLoggingCallback, "xessSetLoggingCallback") &&
            take(module, api.IsOptimalDriver, "xessIsOptimalDriver");
#if defined(XR_D3D12)
        ok = ok && take(module, api.CreateContext, "xessD3D12CreateContext") && take(module, api.Init, "xessD3D12Init") &&
            take(module, api.Execute, "xessD3D12Execute");
#elif defined(XR_VULKAN)
        ok = ok && take(module, api.CreateContext, "xessVKCreateContext") && take(module, api.Init, "xessVKInit") &&
            take(module, api.Execute, "xessVKExecute") &&
            take(module, api.RequiredInstanceExtensions, "xessVKGetRequiredInstanceExtensions") &&
            take(module, api.RequiredDeviceExtensions, "xessVKGetRequiredDeviceExtensions") &&
            take(module, api.RequiredDeviceFeatures, "xessVKGetRequiredDeviceFeatures");
#else
        ok = ok && take(module, api.CreateContext, "xessD3D11CreateContext") && take(module, api.Init, "xessD3D11Init") &&
            take(module, api.Execute, "xessD3D11Execute");
#endif
        if (!ok)
        {
            Msg("! [XeSS] %s lacks entry points: XeSS is off", kXessDllName);
            return nullptr;
        }
        api.GetVersion(&api.version);
        Msg("* [XeSS] %s %u.%u.%u ready", kXessDllName, api.version.major, api.version.minor, api.version.patch);
        return &api;
    }();
    return loaded;
}

#if defined(XR_VULKAN)
bool g_vk_device_ready{};
#endif
} // namespace

const DaXess* da_xess() { return load(); }

xess_quality_settings_t da_xess_quality(int quality)
{
    static constexpr xess_quality_settings_t settings[] = { XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_AA,
        XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY,
        XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE };
    return settings[std::clamp(quality, 0, 7)];
}

#if defined(XR_VULKAN)
void da_xess_vk_instance_extensions(std::vector<const char*>& out)
{
    const DaXess* xess = load();
    if (!xess)
        return;
    uint32_t count = 0, version = 0;
    const char* const* names = nullptr;
    const xess_result_t result = xess->RequiredInstanceExtensions(&count, &names, &version);
    if (result != XESS_RESULT_SUCCESS)
    {
        Msg("~ [XeSS] the Vulkan instance cannot have what XeSS needs (%d)", int(result));
        return;
    }
    // The names stay valid while the DLL is loaded (it never unloads); one asked twice is asked once.
    for (uint32_t i = 0; i < count; ++i)
    {
        if (std::none_of(out.begin(), out.end(), [&](const char* n) { return !strcmp(n, names[i]); }))
            out.push_back(names[i]);
    }
}

void da_xess_vk_device_extensions(void* instance, void* physicalDevice, std::vector<const char*>& out)
{
    g_vk_device_ready = false;
    const DaXess* xess = load();
    if (!xess || !instance || !physicalDevice)
        return;
    uint32_t count = 0;
    const char* const* names = nullptr;
    const xess_result_t result = xess->RequiredDeviceExtensions(static_cast<VkInstance>(instance),
        static_cast<VkPhysicalDevice>(physicalDevice), &count, &names);
    if (result != XESS_RESULT_SUCCESS)
    {
        Msg("~ [XeSS] this Vulkan device lacks what XeSS needs (%d): XeSS is off on Vulkan", int(result));
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(names[i]);
}

void* da_xess_vk_device_features(void* instance, void* physicalDevice, void* features2)
{
    g_vk_device_ready = false;
    const DaXess* xess = load();
    if (!xess || !instance || !physicalDevice || !features2)
        return nullptr;
    // XeSS writes the features it needs into the chain it is given and adds the structures the chain lacks; the head
    // it hands back is the chain vkCreateDevice gets.
    void* chain = features2;
    const xess_result_t result = xess->RequiredDeviceFeatures(static_cast<VkInstance>(instance),
        static_cast<VkPhysicalDevice>(physicalDevice), &chain);
    if (result != XESS_RESULT_SUCCESS || !chain)
    {
        Msg("~ [XeSS] this Vulkan device lacks the features XeSS needs (%d): XeSS is off on Vulkan", int(result));
        return nullptr;
    }
    g_vk_device_ready = true;
    return chain;
}

bool da_xess_vk_device_ready() { return g_vk_device_ready; }
#endif
} // namespace xray::render::RENDER_NAMESPACE
