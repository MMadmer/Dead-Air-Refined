#include "stdafx.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include "da_upscaler.h"
#include "da_native_caps.h"
#include "xrCore/ProductVersion.h"

#ifdef XR_D3D12
#include "Layers/xrRenderDX12/d12_api.h"
#endif
#ifdef XR_VULKAN
#include "Layers/xrRenderVK/vk_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
constexpr u32 HARDWARE_CACHE_MAGIC = 0x31434852;
constexpr u32 HARDWARE_CACHE_VERSION = 1;

bool GetHardwareSignature(u32& signature)
{
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi)
        return false;

    using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory =
        reinterpret_cast<CreateFactoryFunction>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory)
    {
        FreeLibrary(dxgi);
        return false;
    }

    IDXGIFactory1* factory{};
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        FreeLibrary(dxgi);
        return false;
    }

    signature = crc32(&HARDWARE_CACHE_VERSION, sizeof(HARDWARE_CACHE_VERSION));
    u32 adapterCount = 0;
    bool valid = true;
    for (UINT index = 0;; ++index)
    {
        IDXGIAdapter1* adapter{};
        const HRESULT enumerationResult = factory->EnumAdapters1(index, &adapter);
        if (enumerationResult == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(enumerationResult) || !adapter)
        {
            valid = false;
            break;
        }

        DXGI_ADAPTER_DESC1 description{};
        LARGE_INTEGER driverVersion{};
        valid = SUCCEEDED(adapter->GetDesc1(&description));
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion);
        adapter->Release();
        if (!valid)
            break;

        signature = crc32(&description, sizeof(description), signature);
        signature = crc32(&driverVersion, sizeof(driverVersion), signature);
        ++adapterCount;
    }

    factory->Release();
    FreeLibrary(dxgi);
    if (!valid || !adapterCount)
        return false;

    signature = crc32(&adapterCount, sizeof(adapterCount), signature);
    return true;
}

bool LoadHardwareCache(pcstr cachePath, u32 signature, BOOL& result)
{
    IReader* reader = FS.r_open(cachePath);
    if (!reader)
        return false;

    bool valid = reader->elapsed() == static_cast<intptr_t>(sizeof(u32) * 4);
    u32 cachedResult = 0;
    if (valid)
    {
        valid = reader->r_u32() == HARDWARE_CACHE_MAGIC;
        valid = valid && reader->r_u32() == HARDWARE_CACHE_VERSION;
        valid = valid && reader->r_u32() == signature;
        cachedResult = reader->r_u32();
        valid = valid && cachedResult <= 2 && reader->eof();
    }
    FS.r_close(reader);

    if (!valid)
        return false;

    result = static_cast<BOOL>(cachedResult);
    return true;
}

void SaveHardwareCache(pcstr cachePath, u32 signature, BOOL result)
{
    CMemoryWriter writer;
    writer.w_u32(HARDWARE_CACHE_MAGIC);
    writer.w_u32(HARDWARE_CACHE_VERSION);
    writer.w_u32(signature);
    writer.w_u32(static_cast<u32>(result));

    xr_string temporaryPath = cachePath;
    temporaryPath += ".tmp";
    if (writer.save_to(temporaryPath.c_str()))
        FS.file_rename(temporaryPath.c_str(), cachePath, true);
}
}

class DX11TestHelper
{
    SDL_Window* m_window = nullptr;
    CHW m_hw;

public:
    DX11TestHelper()
    {
        m_window = SDL_CreateWindow("TestDX11Window", 0, 0, 1, 1, SDL_WINDOW_HIDDEN);
        if (!m_window)
        {
            Log("~ Cannot create helper window for DirectX 11 test:", SDL_GetError());
            return;
        }
        m_hw.CreateDevice(m_window);
    }

    bool Successful() const
    {
        return m_window && m_hw.Valid;
    }

    const CHW& GetHW() const { return m_hw; }

    ~DX11TestHelper()
    {
        m_hw.DestroyDevice();
        SDL_DestroyWindow(m_window);
    }
};


BOOL xrRender_test_hw()
{
    ZoneScoped;

    string_path cachePath;
    FS.update_path(cachePath, "$app_data_root$", "render-hardware.cache");
    u32 signature = 0;
    BOOL result = FALSE;
    const bool signatureAvailable = GetHardwareSignature(signature);
    if (signatureAvailable && LoadHardwareCache(cachePath, signature, result))
        return result;

    const DX11TestHelper helper;
    if (!helper.Successful())
        return FALSE;

    const auto level = helper.GetHW().FeatureLevel;
    if (level >= D3D_FEATURE_LEVEL_11_0)
        result = TRUE + TRUE; // XXX: remove hack
    else if (level >= D3D_FEATURE_LEVEL_10_0)
        result = TRUE;

    if (signatureAvailable)
        SaveHardwareCache(cachePath, signature, result);
    return result;
}

namespace
{
// r__caps "dx12": the DirectX 12 mode made the engine's renderer list (xrRender_R5's probe
// passed). Both modules read the same list, so the DX11 menu knows it too.
void render_caps_dx12(UpscalerCaps& caps)
{
    IConsole_Command* renderer = Console ? Console->GetCommand("renderer") : nullptr;
    if (!renderer)
        return;
    IConsole_Command::TInfo modes;
    renderer->Info(modes);
    caps.dx12 = strstr(modes, "renderer_r5") != nullptr;
    caps.vk = strstr(modes, "renderer_r6") != nullptr;
}

const struct render_caps_dx12_registrar
{
    render_caps_dx12_registrar() { da_upscaler_register_caps_hook(&render_caps_dx12); }
} s_render_caps_dx12;
} // namespace

namespace
{
// An OS update that brings or replaces the D3D12 runtime changes this, and the probe runs again.
u32 GetD3D12RuntimeSignature(u32 signature)
{
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (length && length < MAX_PATH && wcscat_s(path, L"\\d3d12.dll") == 0 &&
        GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    {
        signature = crc32(&data.nFileSizeLow, sizeof(data.nFileSizeLow), signature);
        signature = crc32(&data.ftLastWriteTime, sizeof(data.ftLastWriteTime), signature);
    }
    return signature;
}

// Creating a D3D12 device takes long enough to matter at every start: the verdict is kept next
// to render-hardware.cache, under the same hardware signature plus the D3D12 runtime and the
// game's version, so a new release, driver or Windows update tries DirectX 12 again. Without a
// hardware signature the rest still keys it: a failure has to stick, or a machine whose device
// creation fails every time would stop at it on every start.
bool GetD3D12Cache(string_path& cachePath, u32& signature)
{
    constexpr u32 D3D12_CACHE_VERSION = 2;

    FS.update_path(cachePath, "$app_data_root$", "render-hardware-d3d12.cache");
    signature = 0;
    GetHardwareSignature(signature);
    signature = crc32(&D3D12_CACHE_VERSION, sizeof(D3D12_CACHE_VERSION), GetD3D12RuntimeSignature(signature));
    signature = crc32(DAR_VERSION_STRING, sizeof(DAR_VERSION_STRING) - 1, signature);
    return true;
}

// The adapter's DirectX 12 tiers as the probe found them (render-caps-d3d12.cache, same signature
// as the verdict): the DX11 module learns from it what the adapter could do on DirectX 12, so the
// menu says "needs DirectX 12" instead of "not supported" (infra.md 3.1).
constexpr u32 CAPS_CACHE_MAGIC = 0x31434344; // "DCC1"
constexpr u32 CAPS_CACHE_VERSION = 1;

struct D3D12ProbeCaps
{
    u32 raytracingTier = 0;
    u32 vrsTier = 0;
    u32 meshShaderTier = 0;
    u32 shaderModel = 0;
};

void GetCapsCachePath(string_path& path)
{
    FS.update_path(path, "$app_data_root$", "render-caps-d3d12.cache");
}

bool LoadCapsCache(u32 signature, D3D12ProbeCaps& caps)
{
    string_path path;
    GetCapsCachePath(path);
    IReader* reader = FS.r_open(path);
    if (!reader)
        return false;
    bool valid = reader->elapsed() == static_cast<intptr_t>(sizeof(u32) * 7);
    if (valid)
    {
        valid = reader->r_u32() == CAPS_CACHE_MAGIC;
        valid = valid && reader->r_u32() == CAPS_CACHE_VERSION;
        valid = valid && reader->r_u32() == signature;
        caps.raytracingTier = reader->r_u32();
        caps.vrsTier = reader->r_u32();
        caps.meshShaderTier = reader->r_u32();
        caps.shaderModel = reader->r_u32();
        valid = valid && reader->eof();
    }
    FS.r_close(reader);
    return valid;
}

// R4 (and R5 before the device exists): the tiers of the probe. Registered with da_native_caps,
// which applies the -dx12_no_* masks; on DX11 only the ray tracing tier is reported (PLAN C7).
void ReadProbeCaps(CHWNativeCaps& caps)
{
    string_path cachePath;
    u32 signature;
    D3D12ProbeCaps probe;
    if (GetD3D12Cache(cachePath, signature) && LoadCapsCache(signature, probe))
        caps.raytracingTier = probe.raytracingTier;
}

const struct probe_caps_registrar
{
    probe_caps_registrar() { da_native_caps_register_probe_reader(&ReadProbeCaps); }
} s_probe_caps_registrar;
} // namespace

#ifdef XR_D3D12
namespace
{
void SaveCapsCache(u32 signature, const xrd3d12::NativeCaps& caps)
{
    string_path path;
    GetCapsCachePath(path);
    CMemoryWriter writer;
    writer.w_u32(CAPS_CACHE_MAGIC);
    writer.w_u32(CAPS_CACHE_VERSION);
    writer.w_u32(signature);
    writer.w_u32(caps.raytracingTier);
    writer.w_u32(caps.vrsTier);
    writer.w_u32(caps.meshShaderTier);
    writer.w_u32(caps.shaderModel);
    xr_string temporaryPath = path;
    temporaryPath += ".tmp";
    if (writer.save_to(temporaryPath.c_str()))
        FS.file_rename(temporaryPath.c_str(), path, true);
}

bool ProbeD3D12(string256& reason, xrd3d12::NativeCaps& caps)
{
    // dxgi stays loaded: the probe creates a device, and driver threads may outlive it.
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto createFactory = dxgi ?
        reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) :
        nullptr;
    IDXGIFactory1* factory{};
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        xr_strcpy(reason, "no DXGI");
        return false;
    }

    // Adapter 0, the one the renderer creates its device on (dx11HW.cpp CreateD3D).
    IDXGIAdapter1* adapter{};
    bool supported = false;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter)
    {
        supported = xrd3d12::ProbeAdapter(adapter, reason, sizeof(reason), caps);
        adapter->Release();
    }
    else
        xr_strcpy(reason, "no adapter");
    factory->Release();
    return supported;
}
} // namespace

// The device or its swap chain failed on hardware that passed the probe: the next start leaves
// DirectX 12 off the mode list, so a saved choice falls back to DirectX 11 (Engine.cpp).
void xrRender_d3d12_failed()
{
    string_path cachePath;
    u32 signature;
    if (GetD3D12Cache(cachePath, signature))
        SaveHardwareCache(cachePath, signature, FALSE);
}

bool xrRender_test_d3d12()
{
    ZoneScoped;

    string_path cachePath;
    u32 signature;
    const bool signatureAvailable = GetD3D12Cache(cachePath, signature);
    BOOL result = FALSE;
    D3D12ProbeCaps cachedCaps;
    // A cached verdict without the capabilities file (a build from before it) probes once more.
    if (signatureAvailable && LoadHardwareCache(cachePath, signature, result) && (!result || LoadCapsCache(signature, cachedCaps)))
    {
        if (!result)
            Msg("* DirectX 12: not available on this system (cached)");
        return result != FALSE;
    }

    string256 reason{};
    xrd3d12::NativeCaps caps;
    result = ProbeD3D12(reason, caps) ? TRUE : FALSE;
    if (result)
    {
        Msg("* DirectX 12: available (raytracing tier %u, VRS tier %u, mesh shader tier %u, shader model %X.%X)",
            caps.raytracingTier, caps.vrsTier, caps.meshShaderTier, caps.shaderModel >> 4, caps.shaderModel & 0xF);
    }
    else
        Msg("* DirectX 12: not available on this system: %s", reason[0] ? reason : "unknown reason");

    if (signatureAvailable)
    {
        SaveHardwareCache(cachePath, signature, result);
        if (result)
            SaveCapsCache(signature, caps);
    }
    return result != FALSE;
}
#endif

#ifdef XR_VULKAN
namespace
{
// The Vulkan loader of the system; a driver update changes the hardware signature already.
u32 GetVulkanRuntimeSignature(u32 signature)
{
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (length && length < MAX_PATH && wcscat_s(path, L"\\vulkan-1.dll") == 0 &&
        GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    {
        signature = crc32(&data.nFileSizeLow, sizeof(data.nFileSizeLow), signature);
        signature = crc32(&data.ftLastWriteTime, sizeof(data.ftLastWriteTime), signature);
    }
    return signature;
}

// The Vulkan verdict next to the DirectX 12 one, keyed the same way (hardware, loader, game version).
bool GetVulkanCache(string_path& cachePath, u32& signature)
{
    constexpr u32 VULKAN_CACHE_VERSION = 1;

    FS.update_path(cachePath, "$app_data_root$", "render-hardware-vulkan.cache");
    signature = 0;
    GetHardwareSignature(signature);
    signature = crc32(&VULKAN_CACHE_VERSION, sizeof(VULKAN_CACHE_VERSION), GetVulkanRuntimeSignature(signature));
    signature = crc32(DAR_VERSION_STRING, sizeof(DAR_VERSION_STRING) - 1, signature);
    return true;
}

bool ProbeVulkan(string256& reason)
{
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto createFactory = dxgi ?
        reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) :
        nullptr;
    IDXGIFactory1* factory{};
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    {
        xr_strcpy(reason, "no DXGI");
        return false;
    }
    // Adapter 0, the one the renderer creates its device on (dx11HW.cpp CreateD3D): its Vulkan twin by LUID.
    IDXGIAdapter1* adapter{};
    bool supported = false;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter && SUCCEEDED(adapter->GetDesc1(&desc)))
        supported = xrvk::ProbeAdapter(desc.AdapterLuid, reason, sizeof(reason));
    else
        xr_strcpy(reason, "no adapter");
    if (adapter)
        adapter->Release();
    factory->Release();
    return supported;
}
} // namespace

// The device or its swap chain failed on hardware that passed the probe: the next start leaves Vulkan
// off the mode list, so a saved choice falls back to DirectX 11 (Engine.cpp).
void xrRender_vulkan_failed()
{
    string_path cachePath;
    u32 signature;
    if (GetVulkanCache(cachePath, signature))
        SaveHardwareCache(cachePath, signature, FALSE);
}

bool xrRender_test_vulkan()
{
    ZoneScoped;

    string_path cachePath;
    u32 signature;
    const bool signatureAvailable = GetVulkanCache(cachePath, signature);
    BOOL result = FALSE;
    if (signatureAvailable && LoadHardwareCache(cachePath, signature, result))
    {
        if (!result)
            Msg("* Vulkan: not available on this system (cached)");
        return result != FALSE;
    }
    string256 reason{};
    result = ProbeVulkan(reason) ? TRUE : FALSE;
    if (result)
        Msg("* Vulkan: available");
    else
        Msg("* Vulkan: not available on this system: %s", reason[0] ? reason : "unknown reason");
    if (signatureAvailable)
        SaveHardwareCache(cachePath, signature, result);
    return result != FALSE;
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
