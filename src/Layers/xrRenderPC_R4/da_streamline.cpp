// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// NVIDIA Streamline core (da_streamline.h, da_streamline_sl.h). Manual hooking: the engine keeps
// its native device and factory; Streamline sees what it must through slSetD3DDevice, the
// upgraded swap chain (DX11) or factory (DX12) and, for DLSS frame generation, the direct queue
// made through its device proxy. Resources are tagged per frame (frame tokens).

#include "stdafx.h"

#include "da_streamline_sl.h"
#include "da_upscaler.h"
#include "xrCore/ProductVersion.h"

#include <nvapi.h>
#include <sl_matrix_helpers.h>

// Defines functions and globals: exactly one translation unit per module may include it. Its
// secondary-signature check uses the Windows 8.1 WinTrust structures, above the engine's SDK level.
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WINBLUE
#include <sl_security.h>
#pragma pop_macro("NTDDI_VERSION")

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Identifies the game to NVIDIA's services (OTA model updates, driver profiles). Never change it.
constexpr pcstr da_sl_project_id = "5b0c3f7e-8a21-4d6b-9e44-2f7d1c9a6e38";

constexpr u32 da_vendor_nvidia = 0x10DE;

struct streamline_state
{
    bool loaded{};      // the verified interposer is in the process: sl* calls bind to it
    bool initialised{}; // slInit succeeded
    bool device{};      // slSetD3DDevice succeeded
    LUID luid{};
    bool hags{};

    bool dlss{}, dlssg{}, reflex{}, pcl{};
    u32 dlssgMax{};
    StreamlineFunctions fn;

    sl::FrameToken* token{};
    bool constantsSet{};

    // Reflex: wanted and last applied options.
    bool lowLatency{};
    u32 limitUs{};
    bool reflexApplied{};
    bool appliedLowLatency{};
    u32 appliedLimitUs{};

    UINT pingMessage{};
    std::atomic<bool> pingPending{};

#ifdef XR_D3D12
    ID3D12Device* proxyDevice{}; // Streamline's device proxy: the direct queue is made through it
    bool dlssgLoaded{};          // sl.dlss_g is loaded (it hooks upgraded swap chains)
    bool dlssgWanted{};
    bool dlssgChain{};           // a DLSS-G swap chain is alive: sl.dlss_g must stay loaded
#endif
};
streamline_state g_sl;

wchar_t g_sl_directory[MAX_PATH]{};
const wchar_t* g_sl_plugin_paths[] = { g_sl_directory };

// Streamline logs from its own threads too; identical lines in a row and a flood after the start
// are dropped (a per-frame warning would otherwise fill the log).
void sl_log(sl::LogType type, const char* message)
{
    static std::mutex lock;
    static size_t last_hash = 0;
    static u32 lines = 0;
    if (!message)
        return;

    std::string_view text = message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    if (text.empty())
        return;

    const bool problem = type != sl::LogType::eInfo;
    {
        std::lock_guard guard(lock);
        const size_t hash = std::hash<std::string_view>{}(text);
        if (hash == last_hash)
            return;
        last_hash = hash;
        if (++lines > 400 && !problem)
            return;
        if (lines > 2000)
            return;
    }
    Msg("%s [Streamline] %.*s", problem ? "!" : "*", int(text.size()), text.data());
}

pcstr sl_result_text(sl::Result result) { return sl::getResultAsStr(result); }

// Why a feature does not run, in the player's terms.
pcstr sl_reason(sl::Result result)
{
    switch (result)
    {
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off (Windows graphics settings)";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported: return "the GPU does not support it";
    case sl::Result::eErrorFeatureMissing:
    case sl::Result::eErrorFeatureFailedToLoad: return "its plugin is missing or did not load";
    default: return sl_result_text(result);
    }
}

sl::AdapterInfo sl_adapter()
{
    sl::AdapterInfo info{};
    info.deviceLUID = reinterpret_cast<uint8_t*>(&g_sl.luid);
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    return info;
}

sl::Result sl_supported(sl::Feature feature)
{
    const sl::AdapterInfo adapter = sl_adapter();
    return slIsFeatureSupported(feature, adapter);
}

// Hardware-accelerated GPU scheduling of the adapter, from the kernel thunks (any vendor):
// D3DKMT_WDDM_2_7_CAPS.HwSchEnabled. The structures are declared here; d3dkmthk.h needs the
// driver kit's NTSTATUS environment.
bool query_hags(const LUID& luid)
{
    struct open_from_luid
    {
        LUID AdapterLuid;
        UINT hAdapter;
    };
    struct query_adapter_info
    {
        UINT hAdapter;
        UINT Type;
        void* pPrivateDriverData;
        UINT PrivateDriverDataSize;
    };
    struct close_adapter
    {
        UINT hAdapter;
    };
    constexpr UINT kmt_wddm_2_7_caps = 70;     // KMTQAITYPE_WDDM_2_7_CAPS
    constexpr UINT kmt_hw_sch_enabled = 1u << 1; // D3DKMT_WDDM_2_7_CAPS::HwSchEnabled

    const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi)
        return false;
    const auto open = reinterpret_cast<LONG(APIENTRY*)(open_from_luid*)>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto query = reinterpret_cast<LONG(APIENTRY*)(const query_adapter_info*)>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    const auto close = reinterpret_cast<LONG(APIENTRY*)(const close_adapter*)>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close)
        return false;

    open_from_luid adapter{ luid, 0 };
    if (open(&adapter) < 0)
        return false;
    UINT caps = 0;
    const query_adapter_info info{ adapter.hAdapter, kmt_wddm_2_7_caps, &caps, sizeof(caps) };
    const bool known = query(&info) >= 0;
    const close_adapter closing{ adapter.hAdapter };
    close(&closing);
    return known && (caps & kmt_hw_sch_enabled);
}

// Frames DLSS-G can generate by architecture when its state cannot tell: RTX 40 (Ada) 2x,
// RTX 50 (Blackwell) up to 4x.
u32 nvapi_dlssg_max()
{
    if (NvAPI_Initialize() != NVAPI_OK)
        return 0;
    NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS]{};
    NvU32 count = 0;
    if (NvAPI_EnumPhysicalGPUs(gpus, &count) != NVAPI_OK)
        return 0;
    NvU32 architecture = 0;
    for (NvU32 i = 0; i < count; ++i)
    {
        NV_GPU_ARCH_INFO info{};
        info.version = NV_GPU_ARCH_INFO_VER;
        if (NvAPI_GPU_GetArchInfo(gpus[i], &info) == NVAPI_OK)
            architecture = std::max(architecture, info.architecture);
    }
    if (architecture >= NV_GPU_ARCHITECTURE_GB200)
        return 3;
    return architecture >= NV_GPU_ARCHITECTURE_AD100 ? 1 : 0;
}

bool sl_load()
{
    if (g_sl.loaded)
        return true;

    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    wchar_t* slash = length && length < std::size(path) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash)
        return false;
    slash[1] = 0;
    wcscpy_s(g_sl_directory, path);

    wchar_t interposer[MAX_PATH]{};
    swprintf_s(interposer, L"%ssl.interposer.dll", g_sl_directory);
    if (GetFileAttributesW(interposer) == INVALID_FILE_ATTRIBUTES)
    {
        Msg("* [Streamline] sl.interposer.dll is not installed: DLSS and Reflex are off");
        return false;
    }
    // Both signatures (Windows and NVIDIA's own) by absolute path, before anything of it runs.
    if (!sl::security::verifyEmbeddedSignature(interposer))
    {
        Msg("! [Streamline] sl.interposer.dll is not signed by NVIDIA: DLSS and Reflex are off");
        return false;
    }
    // The delay-load helper binds the sl* imports to this module by its name.
    if (!LoadLibraryExW(interposer, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
    {
        Msg("! [Streamline] sl.interposer.dll could not be loaded (%lu): DLSS and Reflex are off", GetLastError());
        return false;
    }
    g_sl.loaded = true;
    return true;
}

template <typename Function>
void sl_resolve(sl::Feature feature, pcstr name, Function*& function)
{
    void* address = nullptr;
    function = slGetFeatureFunction(feature, name, address) == sl::Result::eOk ? reinterpret_cast<Function*>(address) : nullptr;
}

void sl_resolve_dlssg()
{
    sl_resolve(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl.fn.dlssgSetOptions);
    sl_resolve(sl::kFeatureDLSS_G, "slDLSSGGetState", g_sl.fn.dlssgGetState);
}

// What the device runs, once it is known to Streamline: support can only shrink from here.
void sl_device_ready()
{
    const auto check = [](sl::Feature feature, pcstr name, bool& supported)
    {
        const sl::Result result = sl_supported(feature);
        supported = result == sl::Result::eOk;
        if (!supported)
            Msg("~ [Streamline] %s is unavailable: %s", name, sl_reason(result));
    };
    check(sl::kFeatureDLSS, "DLSS Super Resolution", g_sl.dlss);
    check(sl::kFeatureReflex, "Reflex", g_sl.reflex);
    check(sl::kFeaturePCL, "PC latency stats", g_sl.pcl);

    StreamlineFunctions& fn = g_sl.fn;
    fn = {};
    if (g_sl.dlss)
    {
        sl_resolve(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn.dlssGetOptimalSettings);
        sl_resolve(sl::kFeatureDLSS, "slDLSSSetOptions", fn.dlssSetOptions);
        g_sl.dlss = fn.dlssGetOptimalSettings && fn.dlssSetOptions;
    }
    if (g_sl.reflex)
    {
        sl_resolve(sl::kFeatureReflex, "slReflexSleep", fn.reflexSleep);
        sl_resolve(sl::kFeatureReflex, "slReflexSetOptions", fn.reflexSetOptions);
        g_sl.reflex = fn.reflexSleep && fn.reflexSetOptions;
    }
    if (g_sl.pcl)
    {
        sl_resolve(sl::kFeaturePCL, "slPCLSetMarker", fn.pclSetMarker);
        sl_resolve(sl::kFeaturePCL, "slPCLGetState", fn.pclGetState);
        sl::PCLState state{};
        if (fn.pclGetState && fn.pclGetState(state) == sl::Result::eOk)
            g_sl.pingMessage = state.statsWindowMessage;
        g_sl.pcl = fn.pclSetMarker != nullptr;
    }

    g_sl.dlssg = false;
    g_sl.dlssgMax = 0;
#ifdef XR_D3D12
    bool loaded = false;
    g_sl.dlssgLoaded = slIsFeatureLoaded(sl::kFeatureDLSS_G, loaded) == sl::Result::eOk && loaded;
    const sl::Result dlssg = sl_supported(sl::kFeatureDLSS_G);
    if (dlssg != sl::Result::eOk)
    {
        Msg("~ [Streamline] DLSS frame generation is unavailable: %s", sl_reason(dlssg));
        sl::FeatureRequirements requirements{};
        if (dlssg == sl::Result::eErrorDriverOutOfDate &&
            slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements) == sl::Result::eOk)
        {
            Msg("~ [Streamline] DLSS frame generation needs driver %u.%u, found %u.%u",
                requirements.driverVersionRequired.major, requirements.driverVersionRequired.minor,
                requirements.driverVersionDetected.major, requirements.driverVersionDetected.minor);
        }
    }
    else if (g_sl.dlssgLoaded)
    {
        sl_resolve_dlssg();
        g_sl.dlssg = g_sl.fn.dlssgSetOptions && g_sl.fn.dlssgGetState;
    }

    if (g_sl.dlssg)
    {
        sl::DLSSGState state{};
        const sl::ViewportHandle viewport{ da_sl_viewport_id };
        if (g_sl.fn.dlssgGetState(viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax)
            g_sl.dlssgMax = state.numFramesToGenerateMax;
        else
            g_sl.dlssgMax = nvapi_dlssg_max();
        // Up to 4x: the r__framegen tokens end there.
        g_sl.dlssgMax = std::clamp(g_sl.dlssgMax, 1u, 3u);
    }
    // Loaded but never used: out before a swap chain exists, so none gets its proxy.
    if (g_sl.dlssgLoaded && !g_sl.dlssg)
    {
        if (slSetFeatureLoaded(sl::kFeatureDLSS_G, false) == sl::Result::eOk)
            g_sl.dlssgLoaded = false;
    }
#endif

    Msg("* [Streamline] DLSS %s, DLSS frame generation %s (up to %ux), Reflex %s, PC latency stats %s",
        g_sl.dlss ? "yes" : "no", g_sl.dlssg ? "yes" : "no", g_sl.dlssg ? g_sl.dlssgMax + 1 : 0,
        g_sl.reflex ? "yes" : "no", g_sl.pcl ? "yes" : "no");
}

// Reflex options change rarely: only a difference reaches the driver. They must be set once
// even while everything is off.
void sl_apply_reflex()
{
    if (!g_sl.reflex)
        return;
    if (g_sl.reflexApplied && g_sl.appliedLowLatency == g_sl.lowLatency && g_sl.appliedLimitUs == g_sl.limitUs)
        return;
    sl::ReflexOptions options{};
    options.mode = g_sl.lowLatency ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    options.frameLimitUs = g_sl.limitUs;
    const sl::Result result = g_sl.fn.reflexSetOptions(options);
    if (result != sl::Result::eOk)
        da_sl_report("slReflexSetOptions", result);
    // Also on a failure: a refused option is not retried every frame.
    g_sl.reflexApplied = true;
    g_sl.appliedLowLatency = g_sl.lowLatency;
    g_sl.appliedLimitUs = g_sl.limitUs;
}

#ifdef XR_D3D12
// Load state of sl.dlss_g towards the wanted one. Unloading waits for the DLSS-G swap chain to go.
// `flush`: the GPU still works on the frame (the swap chain recreation has drained it already).
bool sl_apply_dlssg_load(bool flush)
{
    if (!g_sl.device || !g_sl.dlssg)
        return !g_sl.dlssgWanted;
    if (g_sl.dlssgLoaded == g_sl.dlssgWanted)
        return true;
    if (!g_sl.dlssgWanted && g_sl.dlssgChain)
        return false;

    // Nothing may run on the GPU or through DXGI while a plugin is (un)hooked.
    if (flush)
        xrd3d12::Flush(HW.get_context(CHW::IMM_CTX_ID), true);
    const sl::Result result = slSetFeatureLoaded(sl::kFeatureDLSS_G, g_sl.dlssgWanted);
    if (result != sl::Result::eOk)
    {
        da_sl_report(g_sl.dlssgWanted ? "slSetFeatureLoaded(DLSS-G, on)" : "slSetFeatureLoaded(DLSS-G, off)", result);
        return false;
    }
    g_sl.dlssgLoaded = g_sl.dlssgWanted;
    // A reloaded plugin may sit elsewhere in memory.
    if (g_sl.dlssgLoaded)
        sl_resolve_dlssg();
    return true;
}

// The swap chain made through Streamline's upgraded factory. Without frame generation it gives
// sl.common its present hook; the DLSS-G variant lets sl.dlss_g take the chain over.
class sl_swapchain_provider final : public xrd3d12::SwapChainProvider
{
    const bool m_dlssg;

public:
    explicit sl_swapchain_provider(bool dlssg) : m_dlssg(dlssg) {}

    HRESULT Create(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain4** swapChain) override
    {
        *swapChain = nullptr;
        if (!g_sl.device)
            return E_FAIL;
        // sl.dlss_g attaches to every chain an upgraded factory makes while it is loaded.
        g_sl.dlssgWanted = m_dlssg;
        if (!sl_apply_dlssg_load(false) && m_dlssg)
            return E_FAIL;

        // The proxy takes its own reference to the factory; the layer keeps its one.
        void* upgraded = factory;
        const sl::Result result = slUpgradeInterface(&upgraded);
        if (result != sl::Result::eOk || !upgraded)
        {
            da_sl_report("slUpgradeInterface(IDXGIFactory)", result);
            return E_FAIL;
        }
        IDXGIFactory2* proxy = static_cast<IDXGIFactory2*>(upgraded);
        // DLSS-G presents from Streamline's thread and paces its frames itself: the layer's vsync
        // pacing stays off its chain.
        DXGI_SWAP_CHAIN_DESC1 chainDesc = desc;
        if (m_dlssg)
            chainDesc.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        IDXGISwapChain1* chain = nullptr;
        HRESULT hr = proxy->CreateSwapChainForHwnd(queue, hwnd, &chainDesc, fullscreen, nullptr, &chain);
        if (proxy != factory)
            proxy->Release();
        if (FAILED(hr))
            return hr;
        hr = chain->QueryInterface(IID_PPV_ARGS(swapChain));
        chain->Release();
        if (SUCCEEDED(hr) && m_dlssg)
            g_sl.dlssgChain = true;
        return hr;
    }

    void Destroy(IDXGISwapChain4* /*swapChain*/) override
    {
        if (!m_dlssg)
            return;
        // Presenting runs on Streamline's thread while DLSS-G is on: off before the chain goes.
        if (g_sl.fn.dlssgSetOptions)
        {
            sl::DLSSGOptions options{};
            options.mode = sl::DLSSGMode::eOff;
            g_sl.fn.dlssgSetOptions(sl::ViewportHandle{ da_sl_viewport_id }, options);
        }
        // Released right after this call: from here the plugin may be unloaded.
        g_sl.dlssgChain = false;
    }
};

sl_swapchain_provider g_sl_common_provider{ false };
#endif

void sl_marker(sl::PCLMarker marker)
{
    if (g_sl.pcl && g_sl.token)
        g_sl.fn.pclSetMarker(marker, *g_sl.token);
}

void sl_new_frame()
{
    sl::FrameToken* token = nullptr;
    if (slGetNewFrameToken(token, nullptr) == sl::Result::eOk)
        g_sl.token = token;
    g_sl.constantsSet = false;
}

sl::float4x4 sl_matrix(const Fmatrix& m)
{
    sl::float4x4 r;
    r.setRow(0, { m._11, m._12, m._13, m._14 });
    r.setRow(1, { m._21, m._22, m._23, m._24 });
    r.setRow(2, { m._31, m._32, m._33, m._34 });
    r.setRow(3, { m._41, m._42, m._43, m._44 });
    return r;
}

// r__caps: dlssg, dlssg_max, hags (dlss comes from the upscaler registry).
void sl_caps(UpscalerCaps& caps)
{
    caps.dlssg = da_streamline_dlssg();
    caps.dlssg_max = caps.dlssg ? da_streamline_dlssg_max() : 0;
    caps.hags = g_sl.hags;
}

const struct sl_caps_registrar
{
    sl_caps_registrar() { da_upscaler_register_caps_hook(&sl_caps); }
} s_sl_caps;
} // namespace

// ---- lifetime -------------------------------------------------------------------------------

void da_streamline_init(IDXGIAdapter1* adapter)
{
    DXGI_ADAPTER_DESC1 desc{};
    if (!adapter || FAILED(adapter->GetDesc1(&desc)))
        return;
    g_sl.luid = desc.AdapterLuid;
    g_sl.hags = query_hags(desc.AdapterLuid);
    if (g_sl.initialised)
        return;

    // Everything Streamline offers here needs an NVIDIA adapter; the 2-5 s of slInit are spared
    // everywhere else.
    if (desc.VendorId != da_vendor_nvidia || strstr(Core.Params, "-no_streamline") || !sl_load())
        return;

    static const sl::Feature features[] =
    {
        sl::kFeatureDLSS,
        sl::kFeatureReflex,
        sl::kFeaturePCL,
#ifdef XR_D3D12
        sl::kFeatureDLSS_G,
#endif
    };

    sl::Preferences prefs{};
    prefs.showConsole = false;
    prefs.logLevel = sl::LogLevel::eDefault;
    prefs.pathsToPlugins = g_sl_plugin_paths;
    prefs.numPathsToPlugins = u32(std::size(g_sl_plugin_paths));
    prefs.pathToLogsAndData = nullptr;
    prefs.logMessageCallback = &sl_log;
    // Defaults kept: the host restores command list state, over-the-air model updates. Plugins
    // downloaded into ProgramData are not loaded: ours are newer, and mapping both doubled the
    // plugin start-up (about two seconds of every start on NVIDIA).
    prefs.flags |= sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = u32(std::size(features));
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = DAR_VERSION_STRING;
    prefs.projectId = da_sl_project_id;
#ifdef XR_D3D12
    prefs.renderAPI = sl::RenderAPI::eD3D12;
#else
    prefs.renderAPI = sl::RenderAPI::eD3D11;
#endif

    const sl::Result result = slInit(prefs, sl::kSDKVersion);
    if (result != sl::Result::eOk)
    {
        Msg("! [Streamline] slInit failed (%s): DLSS and Reflex are off", sl_result_text(result));
        return;
    }
    g_sl.initialised = true;
    Msg("* [Streamline] %d.%d.%d ready", SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
}

namespace
{
bool sl_set_device(void* device)
{
    if (!g_sl.initialised || !device)
        return false;
    const sl::Result result = slSetD3DDevice(device);
    if (result != sl::Result::eOk)
    {
        Msg("! [Streamline] slSetD3DDevice failed (%s): DLSS and Reflex are off", sl_result_text(result));
        return false;
    }
    g_sl.device = true;
    sl_device_ready();
    sl_apply_reflex();
    return true;
}
} // namespace

#ifdef XR_D3D12
void da_streamline_d3d12_device_created(ID3D12Device* device)
{
    if (!sl_set_device(device) || !g_sl.dlssg)
        return;
    // DLSS-G has to see the direct queue being created: it is made through the device proxy.
    void* proxy = device;
    const sl::Result result = slUpgradeInterface(&proxy);
    if (result == sl::Result::eOk && proxy && proxy != device)
        g_sl.proxyDevice = static_cast<ID3D12Device*>(proxy);
    else
        da_sl_report("slUpgradeInterface(ID3D12Device)", result);
}

HRESULT da_streamline_d3d12_create_queue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
    ID3D12CommandQueue** queue)
{
    ID3D12Device* creator = g_sl.proxyDevice ? g_sl.proxyDevice : device;
    return creator->CreateCommandQueue(desc, IID_PPV_ARGS(queue));
}

xrd3d12::SwapChainProvider* da_streamline_swapchain_provider()
{
    return g_sl.device ? &g_sl_common_provider : nullptr;
}

IDXGIFactory2* da_streamline_factory(IDXGIFactory2* factory)
{
    if (!g_sl.device || !factory)
        return nullptr;
    g_sl.dlssgWanted = false;
    if (!sl_apply_dlssg_load(false))
        return nullptr;
    void* upgraded = factory;
    const sl::Result result = slUpgradeInterface(&upgraded);
    if (result != sl::Result::eOk || !upgraded)
    {
        da_sl_report("slUpgradeInterface(IDXGIFactory)", result);
        return nullptr;
    }
    // A proxy comes with its own reference; the factory itself does not.
    if (upgraded == factory)
        factory->AddRef();
    return static_cast<IDXGIFactory2*>(upgraded);
}

bool da_streamline_dlssg_load(bool loaded)
{
    g_sl.dlssgWanted = loaded;
    return sl_apply_dlssg_load(true);
}

void da_streamline_dlssg_chain(bool alive) { g_sl.dlssgChain = alive; }

// The DLSS-G swap chain provider (da_framegen_dlss.cpp).
xrd3d12::SwapChainProvider* da_streamline_dlssg_provider()
{
    static sl_swapchain_provider provider{ true };
    return &provider;
}
#else
void da_streamline_d3d11_device_created(ID3D11Device* device) { sl_set_device(device); }

void da_streamline_d3d11_swapchain_created(IDXGISwapChain*& swapChain)
{
    if (!g_sl.device || !swapChain)
        return;
    void* upgraded = swapChain;
    const sl::Result result = slUpgradeInterface(&upgraded);
    if (result != sl::Result::eOk || !upgraded || upgraded == swapChain)
    {
        if (result != sl::Result::eOk)
            da_sl_report("slUpgradeInterface(IDXGISwapChain)", result);
        return;
    }
    // The proxy holds its own reference to the chain; the engine's moves to the proxy.
    swapChain->Release();
    swapChain = static_cast<IDXGISwapChain*>(upgraded);
}
#endif

void da_streamline_shutdown()
{
    if (!g_sl.initialised)
        return;
#ifdef XR_D3D12
    if (g_sl.device)
        xrd3d12::Flush(HW.get_context(CHW::IMM_CTX_ID), true);
#endif
    const sl::Result result = slShutdown();
    if (result != sl::Result::eOk)
        Msg("! [Streamline] slShutdown failed (%s)", sl_result_text(result));

    g_sl.initialised = false;
    g_sl.device = false;
    g_sl.dlss = g_sl.dlssg = g_sl.reflex = g_sl.pcl = false;
    g_sl.dlssgMax = 0;
    g_sl.fn = {};
    g_sl.token = nullptr;
    g_sl.constantsSet = false;
    g_sl.lowLatency = false;
    g_sl.limitUs = 0;
    g_sl.reflexApplied = false;
    g_sl.pingMessage = 0;
#ifdef XR_D3D12
    g_sl.dlssgLoaded = g_sl.dlssgWanted = g_sl.dlssgChain = false;
#endif
}

void da_streamline_device_released()
{
#ifdef XR_D3D12
    // The queue proxy points at it: it goes after the layer released the queue.
    _RELEASE(g_sl.proxyDevice);
#endif
}

void da_streamline_window_message(UINT message)
{
    if (message && message == g_sl.pingMessage)
        g_sl.pingPending = true;
}

// ---- the frame loop ------------------------------------------------------------------------

void da_streamline_frame_marker(IRender::FrameMarker marker)
{
    if (!g_sl.device)
        return;

    using Marker = IRender::FrameMarker;
    switch (marker)
    {
    case Marker::FrameStart:
        sl_new_frame();
#ifdef XR_D3D12
        // A deferred unload of sl.dlss_g (its swap chain went meanwhile).
        if (g_sl.dlssgLoaded != g_sl.dlssgWanted)
            sl_apply_dlssg_load(true);
#endif
        sl_apply_reflex();
        // Always, also with Reflex off: the driver measures the frame from here.
        if (g_sl.reflex && g_sl.token)
            g_sl.fn.reflexSleep(*g_sl.token);
        break;
    case Marker::SimulationStart:
        if (!g_sl.token)
            sl_new_frame();
        sl_marker(sl::PCLMarker::eSimulationStart);
        // The ping of the PC latency stats counts from the input the frame picks up.
        if (g_sl.pingPending.exchange(false))
            sl_marker(sl::PCLMarker::ePCLatencyPing);
        break;
    case Marker::SimulationEnd: sl_marker(sl::PCLMarker::eSimulationEnd); break;
    case Marker::RenderSubmitStart: sl_marker(sl::PCLMarker::eRenderSubmitStart); break;
    case Marker::RenderSubmitEnd: sl_marker(sl::PCLMarker::eRenderSubmitEnd); break;
    case Marker::PresentStart: sl_marker(sl::PCLMarker::ePresentStart); break;
    case Marker::PresentEnd: sl_marker(sl::PCLMarker::ePresentEnd); break;
    }
}

bool da_streamline_frame_limit(u32 fpsLimit)
{
    if (!g_sl.device || !g_sl.reflex)
        return false;
    // The limit counts frames on the screen; with frame generation the driver knows the
    // multiplier and paces the rendered frames for it.
    const bool owned = g_sl.lowLatency && fpsLimit > 0;
    g_sl.limitUs = owned ? u32(std::lround(1000000.0 / double(fpsLimit))) : 0;
    return owned;
}

// ---- capabilities ---------------------------------------------------------------------------

bool da_streamline_active() { return g_sl.device; }
bool da_streamline_dlss() { return g_sl.device && g_sl.dlss; }
bool da_streamline_dlssg() { return g_sl.device && g_sl.dlssg; }
u32 da_streamline_dlssg_max() { return da_streamline_dlssg() ? g_sl.dlssgMax : 0; }
bool da_streamline_hags() { return g_sl.hags; }

void da_streamline_low_latency(bool on)
{
    g_sl.lowLatency = on && g_sl.reflex;
    if (!g_sl.lowLatency)
        g_sl.limitUs = 0;
    sl_apply_reflex();
}

// ---- the SDK side (da_streamline_sl.h) -----------------------------------------------------

const StreamlineFunctions& da_sl() { return g_sl.fn; }

const sl::FrameToken* da_sl_frame()
{
    if (!g_sl.device)
        return nullptr;
    if (!g_sl.token)
        sl_new_frame();
    return g_sl.token;
}

bool da_sl_constants(const StreamlineCamera& camera)
{
    const sl::FrameToken* frame = da_sl_frame();
    if (!frame)
        return false;
    // DLSS and DLSS-G read the same constants: a frame sets them once.
    if (g_sl.constantsSet)
        return true;

    sl::Constants constants{};
    constants.cameraViewToClip = sl_matrix(camera.project);
    sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);
    constants.clipToLensClip = sl_matrix(Fidentity);

    // Current clip -> previous clip through the camera motion, computed around the current camera
    // (large world positions would cost the fp32 precision otherwise).
    Fmatrix cameraToWorld, prevCameraToWorld;
    cameraToWorld.invert(camera.view);
    prevCameraToWorld.invert(camera.prevView);
    sl::float4x4 viewToPrevView, clipToPrevView;
    sl::calcCameraToPrevCamera(viewToPrevView, sl_matrix(cameraToWorld), sl_matrix(prevCameraToWorld));
    sl::matrixMul(clipToPrevView, constants.clipToCameraView, viewToPrevView);
    sl::matrixMul(constants.clipToPrevClip, clipToPrevView, sl_matrix(camera.prevProject));
    sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);

    constants.jitterOffset = { camera.jitterX, camera.jitterY };
    // The buffer holds NDC(current) - NDC(previous), y up; Streamline wants previous - current in
    // UV units (y down).
    constants.mvecScale = { -0.5f, 0.5f };
    constants.cameraPinholeOffset = { 0.f, 0.f };
    constants.cameraRight = { cameraToWorld._11, cameraToWorld._12, cameraToWorld._13 };
    constants.cameraUp = { cameraToWorld._21, cameraToWorld._22, cameraToWorld._23 };
    constants.cameraFwd = { cameraToWorld._31, cameraToWorld._32, cameraToWorld._33 };
    constants.cameraPos = { cameraToWorld._41, cameraToWorld._42, cameraToWorld._43 };
    constants.cameraNear = camera.nearZ;
    constants.cameraFar = camera.farZ;
    constants.cameraFOV = camera.fovY;
    constants.cameraAspectRatio = camera.renderH ? float(camera.renderW) / float(camera.renderH) : 1.f;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = camera.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;

    const sl::Result result = slSetConstants(constants, *frame, sl::ViewportHandle{ da_sl_viewport_id });
    if (result != sl::Result::eOk)
    {
        da_sl_report("slSetConstants", result);
        return false;
    }
    g_sl.constantsSet = true;
    return true;
}

sl::Resource da_sl_texture(ID3D11Resource* resource, u32 d3d12State)
{
#ifdef XR_D3D12
    sl::Resource texture(sl::ResourceType::eTex2d, resource ? xrd3d12::NativeResource(resource) : nullptr, d3d12State);
#else
    // D3D11 resources carry no state: Streamline expects 0 (COMMON) for all of them.
    UNUSED(d3d12State);
    sl::Resource texture(sl::ResourceType::eTex2d, resource, 0u);
#endif
    // The constructor leaves the Vulkan flags uninitialised.
    texture.flags = 0;
    return texture;
}

void da_sl_report(pcstr what, sl::Result result)
{
    static std::mutex lock;
    static xr_vector<std::pair<xr_string, sl::Result>> reported;
    std::lock_guard guard(lock);
    for (const auto& [name, code] : reported)
    {
        if (code == result && name == what)
            return;
    }
    reported.emplace_back(what, result);
    Msg("! [Streamline] %s failed: %s", what, sl_result_text(result));
}
} // namespace xray::render::RENDER_NAMESPACE
