// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#include "vk_loader.h"

#include <mutex>

namespace xrvk
{
Api g_vk;

namespace
{
std::mutex g_loaderLock;
HMODULE g_module = nullptr;
bool g_loaded = false;
} // namespace

bool Api::LoadLoader()
{
    std::lock_guard lock(g_loaderLock);
    if (g_loaded)
        return true;
    if (!g_module)
        g_module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_module)
        g_module = LoadLibraryW(L"vulkan-1.dll");
    if (!g_module)
        return false;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_module, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr)
        return false;
#define XRVK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    XRVK_GLOBAL_FUNCTIONS(XRVK_LOAD_GLOBAL)
#undef XRVK_LOAD_GLOBAL
    // vkEnumerateInstanceVersion is absent on a 1.0 loader; everything else must be there.
    g_loaded = vkCreateInstance && vkEnumerateInstanceExtensionProperties && vkEnumerateInstanceLayerProperties;
    return g_loaded;
}

void Api::LoadInstance(VkInstance instance)
{
#define XRVK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    XRVK_INSTANCE_FUNCTIONS(XRVK_LOAD_INSTANCE)
#undef XRVK_LOAD_INSTANCE
}

void Api::LoadDevice(VkDevice device)
{
#define XRVK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    XRVK_DEVICE_FUNCTIONS(XRVK_LOAD_DEVICE)
#undef XRVK_LOAD_DEVICE
}

const char* ResultText(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_ERROR_UNKNOWN";
    }
}
} // namespace xrvk
