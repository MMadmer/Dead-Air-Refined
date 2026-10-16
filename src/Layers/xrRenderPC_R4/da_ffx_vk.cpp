// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The ffx-api runtime of the Vulkan module (da_ffx_vk.h).

#include "stdafx.h"

#ifdef XR_VULKAN
#include "da_ffx_vk.h"
#include "Layers/xrRenderVK/vk_api.h"
#include "da_streamline.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
HMODULE g_module{};

// The runtime, loaded once by its absolute path next to the executable and never unloaded (contexts and its
// threads may outlive any owner). Null when it is missing or incomplete: FSR is off then.
const ffxFunctions* load_runtime()
{
    static ffxFunctions functions{};
    static const ffxFunctions* loaded = []() -> const ffxFunctions* {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = length && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
        if (!slash || wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"amd_fidelityfx_vk.dll"))
            return nullptr;
        const HMODULE module = LoadLibraryW(path);
        g_module = module;
        if (!module)
        {
            Msg("~ [FFX] amd_fidelityfx_vk.dll cannot be loaded (error %u): AMD FSR is off on Vulkan", u32(GetLastError()));
            return nullptr;
        }
        ffxLoadFunctions(&functions, module);
        if (!functions.CreateContext || !functions.DestroyContext || !functions.Query || !functions.Dispatch)
        {
            Msg("! [FFX] amd_fidelityfx_vk.dll lacks ffx-api entry points: AMD FSR is off on Vulkan");
            return nullptr;
        }
        Msg("* [FFX] AMD FidelityFX Vulkan runtime ready");
        return &functions;
    }();
    return loaded;
}

PFN_vkGetDeviceProcAddr s_device_proc{};

void VKAPI_PTR fsr_no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
void VKAPI_PTR fsr_no_label_end(VkCommandBuffer) {}

// ffx-api asks for extension aliases of core entry points (vkGetBufferMemoryRequirements2KHR) and for debug labels;
// a Vulkan 1.3 device without those extensions enabled answers null, which ffx-api calls through. The core name
// is tried next, and labels fall back to no-ops.
PFN_vkVoidFunction VKAPI_PTR fsr_device_proc(VkDevice device, const char* name)
{
    if (const PFN_vkVoidFunction f = s_device_proc(device, name))
        return f;
    const size_t length = xr_strlen(name);
    string128 core;
    if (length > 3 && length < sizeof(core) && (!xr_strcmp(name + length - 3, "KHR") || !xr_strcmp(name + length - 3, "EXT")))
    {
        xr_strcpy(core, name);
        core[length - 3] = 0;
        if (const PFN_vkVoidFunction f = s_device_proc(device, core))
            return f;
    }
    if (!xr_strcmp(name, "vkCmdBeginDebugUtilsLabelEXT") || !xr_strcmp(name, "vkCmdInsertDebugUtilsLabelEXT"))
        return reinterpret_cast<PFN_vkVoidFunction>(&fsr_no_label);
    if (!xr_strcmp(name, "vkCmdEndDebugUtilsLabelEXT"))
        return reinterpret_cast<PFN_vkVoidFunction>(&fsr_no_label_end);
    // AMD-only entry points (buffer markers) are optional to ffx-api.
    if (length <= 3 || xr_strcmp(name + length - 3, "AMD"))
        Msg("~ [FSR] %s is not available on the device", name);
    return nullptr;
}

} // namespace

const ffxFunctions* da_ffx_vk() { return load_runtime(); }

void da_ffx_vk_route_swapchain()
{
    static bool routed = false;
    if (routed || !load_runtime() || !g_module)
        return;
    routed = true;
    static constexpr pcstr names[] = { "vkCreateSwapchainKHR", "vkDestroySwapchainKHR", "vkGetSwapchainImagesKHR",
        "vkAcquireNextImageKHR", "vkQueuePresentKHR" };
    u32 patched = 0;
    BYTE* base = reinterpret_cast<BYTE*>(g_module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "vulkan-1.dll") || !imp->OriginalFirstThunk)
            continue;
        const auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++slot)
        {
            if (IMAGE_SNAP_BY_ORDINAL(lookup->u1.Ordinal))
                continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + lookup->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(byName->Name);
            if (std::none_of(std::begin(names), std::end(names), [&](pcstr n) { return !xr_strcmp(n, name); }))
                continue;
            void* proxy = da_streamline_vk_device_proc(name);
            if (!proxy)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &old))
                continue;
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(proxy);
            VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), old, &old);
            ++patched;
        }
    }
    if (patched)
        Msg("* [FFX] the runtime's swap chain calls go through Streamline (%u)", patched);
}

PFN_vkGetDeviceProcAddr da_ffx_vk_device_proc()
{
    if (!s_device_proc)
    {
        xrvk::NativeDeviceInfo device;
        void* proc = nullptr;
        if (!HW.pDevice || !xrvk::GetNativeDevice(HW.pDevice, device, &proc) || !proc)
            return nullptr;
        s_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(proc);
    }
    return &fsr_device_proc;
}
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_VULKAN
