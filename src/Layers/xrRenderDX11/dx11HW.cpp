#include "stdafx.h"
#include "xrCore/Debug/CrashReport.h"

#include "dx11HW.h"

#include "StateManager/dx11SamplerStateCache.h"
#include "dx11TextureUtils.h"

#include <dwmapi.h>
#include <SDL_syswm.h>
#include <d3d11sdklayers.h>

#ifdef XR_D3D12
#include "Layers/xrRenderDX12/d12_api.h"
#endif
#ifdef XR_VULKAN
#include "Layers/xrRenderVK/vk_api.h"
#endif
#include "Layers/xrRenderPC_R4/da_streamline.h"
#include "Layers/xrRenderPC_R4/da_native_caps.h"
#include "Layers/xrRenderPC_R4/da_dxc.h"
#include "Layers/xrRenderPC_R4/da_dstorage.h"

extern ENGINE_API int g_pause_in_background;

// Diagnostics (-dxdebug): validator message queue of the debug-layer device. Drained into
// the engine log once per frame from the renderer, so live pipeline errors are named.
static ID3D11InfoQueue* s_dx11_info_queue = nullptr;

void dx11_debug_bind_info_queue(ID3D11Device* device)
{
    if (device)
        device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&s_dx11_info_queue));
    if (s_dx11_info_queue)
        Msg("* -dxdebug: D3D11 debug layer active, validator messages will be logged");
}

void dx11_debug_drain_messages(pcstr tag)
{
    if (!s_dx11_info_queue)
        return;

    // Deduplicate within the drain: frames repeat the same handful of complaints hundreds
    // of times, and the counts per unique message carry all the signal.
    struct entry
    {
        int id;
        int severity;
        u32 count;
        string512 text;
    };
    static xr_vector<entry> unique;
    unique.clear();

    const u64 stored = s_dx11_info_queue->GetNumStoredMessages();
    for (u64 i = 0; i < stored; ++i)
    {
        SIZE_T length = 0;
        if (FAILED(s_dx11_info_queue->GetMessage(i, nullptr, &length)) || !length)
            continue;
        auto* message = static_cast<D3D11_MESSAGE*>(xr_malloc(length));
        if (SUCCEEDED(s_dx11_info_queue->GetMessage(i, message, &length)))
        {
            bool found = false;
            for (entry& e : unique)
            {
                if (e.id == int(message->ID))
                {
                    ++e.count;
                    found = true;
                    break;
                }
            }
            if (!found && unique.size() < 32)
            {
                entry& e = unique.emplace_back();
                e.id = int(message->ID);
                e.severity = int(message->Severity);
                e.count = 1;
                xr_strcpy(e.text, message->pDescription);
            }
        }
        xr_free(message);
    }
    for (const entry& e : unique)
        Msg("# DXDBG f=%u [%s] sev=%d id=%d x%u %s", Device.dwFrame, tag, e.severity, e.id,
            e.count, e.text);
    s_dx11_info_queue->ClearStoredMessages();
}

namespace xray::render::RENDER_NAMESPACE
{
#ifdef XR_D3D12
void xrRender_d3d12_failed(); // r2_test_hw.cpp
#endif
#ifdef XR_VULKAN
void xrRender_vulkan_failed(); // r2_test_hw.cpp
// The -vk_no_* switches as the layer's NativeFeature bits: every fallback can be tested on hardware
// that has everything.
UINT xrRender_vulkan_disabled_features()
{
    struct Switch
    {
        pcstr key;
        UINT bit;
    };
    static constexpr Switch switches[] = {
        {"-vk_no_async", xrvk::NativeFeatureAsyncCompute},
        {"-vk_no_transfer", xrvk::NativeFeatureTransferQueue},
        {"-vk_no_eds3", xrvk::NativeFeatureDynamicState3},
        {"-vk_no_vertex_input", xrvk::NativeFeatureVertexInput},
        {"-vk_no_rebar", xrvk::NativeFeatureRebar},
        {"-vk_no_vrs", xrvk::NativeFeatureShadingRate},
        {"-vk_no_ms", xrvk::NativeFeatureMeshShaders},
        {"-vk_no_gpl", xrvk::NativeFeaturePipelineLibrary},
        {"-vk_no_multidraw", xrvk::NativeFeatureMultiDraw},
        {"-vk_no_d24", xrvk::NativeFeatureDepth24},
    };
    UINT bits = 0;
    for (const Switch& s : switches)
    {
        if (strstr(Core.Params, s.key))
            bits |= s.bit;
    }
    return bits;
}
#endif

CHW HW;

CHW::CHW()
{
    if (!ThisInstanceIsGlobal())
        return;

    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);
}

CHW::~CHW()
{
    if (!ThisInstanceIsGlobal())
        return;

    Device.seqAppActivate.Remove(this);
    Device.seqAppDeactivate.Remove(this);
}

void CHW::OnAppActivate()
{
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        ShowWindow(m_ChainDesc.OutputWindow, SW_RESTORE);
        m_pSwapChain->SetFullscreenState(ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle == rsFullscreen : false, NULL);
    }
}

void CHW::OnAppDeactivate()
{
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
        if (psDeviceMode.WindowStyle == rsFullscreen || psDeviceMode.WindowStyle == rsFullscreenBorderless)
            ShowWindow(m_ChainDesc.OutputWindow, SW_MINIMIZE);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CHW::CreateD3D()
{
    ZoneScoped;

    // Driver workers can outlive device teardown and still execute runtime callbacks.
    hDXGI = XRay::LoadModule("dxgi", true);
    hD3D = XRay::LoadModule("d3d11", true);
    if (!hD3D->IsLoaded() || !hDXGI->IsLoaded())
    {
        Valid = false;
        return;
    }

    // Минимально поддерживаемая версия Windows => Windows Vista SP2 или Windows 7.
    const auto createDXGIFactory = static_cast<decltype(&CreateDXGIFactory1)>(hDXGI->GetProcAddress("CreateDXGIFactory1"));
    if (createDXGIFactory)
        createDXGIFactory(__uuidof(IDXGIFactory1), (void**)(&m_pFactory));

    if (m_pFactory)
        m_pFactory->EnumAdapters1(0, &m_pAdapter);

    Valid = m_pAdapter;
}

void CHW::DestroyD3D()
{
    _SHOW_REF("refCount:m_pAdapter", m_pAdapter);
    _RELEASE(m_pAdapter);

    _SHOW_REF("refCount:m_pFactory", m_pFactory);
    _RELEASE(m_pFactory);
}

void CHW::CreateDevice(SDL_Window* sdlWnd)
{
    ZoneScoped;

    presentTestState = PresentTestState::None;
    deviceRemovedReported = false;

    CreateD3D();
    if (!Valid)
        return;

    m_DriverType = Caps.bForceGPU_REF ? D3D_DRIVER_TYPE_REFERENCE : D3D_DRIVER_TYPE_HARDWARE;

    // Display the name of video board
    DXGI_ADAPTER_DESC1 Desc{};
    if (FAILED(m_pAdapter->GetDesc1(&Desc)))
        Msg("! [%s] failed to retrieve adapter description", __FUNCTION__);
    //  Warning: Desc.Description is wide string
    Msg("* GPU [vendor:%X]-[device:%X]: %S", Desc.VendorId, Desc.DeviceId, Desc.Description);

    Caps.id_vendor = Desc.VendorId;
    Caps.id_device = Desc.DeviceId;

    // NVIDIA Streamline (DLSS, Reflex) is initialised before the device exists; its Vulkan side hooks
    // the instance and device the layer creates (da_streamline.cpp).
    if (ThisInstanceIsGlobal())
        da_streamline_init(m_pAdapter);

#ifdef XR_D3D12
    // Direct3D 12 through xrD3D12: the native device comes back behind the D3D11 interfaces the
    // renderer is written against. -dxdebug: debug layer, -dx12gbv: GPU-based validation (slow),
    // -dx12dred: DRED breadcrumbs and page fault data when the device is removed.
    HRESULT R;
    auto& pContext = d3d_contexts_pool[CHW::IMM_CTX_ID];
    {
        ZoneScopedN("CreateDevice");

        // Pipeline state objects are compiled from the shaders' content; the library keeps them
        // across runs next to the shader cache.
        string_path pipelineCache;
        FS.update_path(pipelineCache, "$app_data_root$", "shaders_cache_grizzly" DELIMITER "r5" DELIMITER "pipelines.bin");
        VerifyPath(pipelineCache);
        wchar_t pipelineCacheW[sizeof(string_path)]{};
        MultiByteToWideChar(CP_ACP, 0, pipelineCache, -1, pipelineCacheW, int(std::size(pipelineCacheW)));

        xrd3d12::DeviceCreateInfo info;
        info.adapter = m_pAdapter;
        info.debugLayer = !!strstr(Core.Params, "-dxdebug");
        info.gpuValidation = !!strstr(Core.Params, "-dx12gbv");
        info.dred = !!strstr(Core.Params, "-dx12dred");
        info.stats = !!strstr(Core.Params, "-dxstats");
        info.gpuUploadHeaps = !strstr(Core.Params, "-dx12_no_rebar");
        info.pipelineCachePath = ThisInstanceIsGlobal() ? pipelineCacheW : nullptr;
        info.log = [](const char* line) { Msg("%s", line); };
        if (ThisInstanceIsGlobal())
        {
            info.deviceCreated = &da_streamline_d3d12_device_created;
            info.createQueue = &da_streamline_d3d12_create_queue;
        }
        // [dx12:infra] begin create-info
        static_assert(DaNativeDisableRT == xrd3d12::NativeFeatureRaytracing && DaNativeDisableVRS == xrd3d12::NativeFeatureVrs &&
                DaNativeDisableMS == xrd3d12::NativeFeatureMeshShaders &&
                DaNativeDisableAsync == xrd3d12::NativeFeatureAsyncCompute && DaNativeDisableSM6 == xrd3d12::NativeFeatureSm6,
            "the -dx12_no_* bits are the layer's NativeFeature bits");
        info.disabledFeatures = da_native_disabled_features();
        info.copyQueueUploads = !strstr(Core.Params, "-dx12_no_copy_queue");
        // [dx12:infra] end create-info
        R = xrd3d12::CreateDevice(info, &pDevice, &pContext, &FeatureLevel);
    }
#elif defined(XR_VULKAN)
    // Vulkan through xrVulkan: the device comes back behind the D3D11 interfaces like the DX12 one.
    // -vkdebug: validation layer (needs the Vulkan SDK), -vksync: synchronization validation as well,
    // -vkstats: the layer's work counters, -vklabels: debug labels for capture tools.
    HRESULT R;
    auto& pContext = d3d_contexts_pool[CHW::IMM_CTX_ID];
    {
        ZoneScopedN("CreateDevice");

        // The driver's pipeline binaries persist next to the shader cache.
        string_path pipelineCache;
        FS.update_path(pipelineCache, "$app_data_root$", "shaders_cache_grizzly" DELIMITER "r6" DELIMITER "pipelines.bin");
        VerifyPath(pipelineCache);
        wchar_t pipelineCacheW[sizeof(string_path)]{};
        MultiByteToWideChar(CP_ACP, 0, pipelineCache, -1, pipelineCacheW, int(std::size(pipelineCacheW)));

        xrvk::DeviceCreateInfo info;
        info.adapterLuid = Desc.AdapterLuid;
        info.adapter = m_pAdapter;
        info.validation = !!strstr(Core.Params, "-vkdebug");
        info.syncValidation = !!strstr(Core.Params, "-vksync");
        info.stats = !!strstr(Core.Params, "-vkstats");
        info.labels = !!strstr(Core.Params, "-vklabels");
        info.pipelineCachePath = ThisInstanceIsGlobal() ? pipelineCacheW : nullptr;
        info.log = [](const char* line) { Msg("%s", line); };
        info.disabledFeatures = xrRender_vulkan_disabled_features();
        // DLSS through Streamline: its extensions and features go into the device, then it gets the device.
        if (ThisInstanceIsGlobal())
        {
            info.externalRequirements = &da_streamline_vk_requirements;
            info.deviceCreated = &da_streamline_vk_device_created;
            info.deviceProc = &da_streamline_vk_device_proc;
            // AMD FSR frame generation presents and acquires on queues of its own.
            info.presentQueues = 2;
        }
        R = xrvk::CreateDevice(info, &pDevice, &pContext, &FeatureLevel);
    }
#else
    u32 createDeviceFlags = 0;

#ifdef DEBUG
    if (xrDebug::DebuggerIsPresent())
        createDeviceFlags |= D3D_CREATE_DEVICE_DEBUG;
#endif

    // Diagnostics: -dxdebug turns the D3D11 debug layer on in any configuration, so the
    // validator can name broken pipeline state on live defects. Requires the "Graphics
    // Tools" optional Windows feature (installed with Visual Studio).
    if (!!strstr(Core.Params, "-dxdebug"))
        createDeviceFlags |= D3D_CREATE_DEVICE_DEBUG;

    HRESULT R;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
#ifdef HAS_DX11_3
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
#endif
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevels2[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevels3[] =
    {
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    auto& pContext = d3d_contexts_pool[CHW::IMM_CTX_ID];

    const auto createDevice = [&](const D3D_FEATURE_LEVEL* level, const u32 levels)
    {
        ZoneScopedN("CreateDevice");

        static const auto d3d11CreateDevice = static_cast<PFN_D3D11_CREATE_DEVICE>(hD3D->GetProcAddress("D3D11CreateDevice"));
        return d3d11CreateDevice(m_pAdapter, D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, createDeviceFlags, level, levels,
            D3D11_SDK_VERSION, &pDevice, &FeatureLevel, &pContext);
    };

    if (DX10Only)
        R = createDevice(featureLevels3, std::size(featureLevels3));
    else
    {
        R = createDevice(featureLevels, std::size(featureLevels));
        if (FAILED(R))
            R = createDevice(featureLevels2, std::size(featureLevels2));
    }

    // The debug layer needs the "Graphics Tools" Windows feature; fall back to a plain
    // device instead of failing the whole renderer when it is missing.
    if (FAILED(R) && (createDeviceFlags & D3D_CREATE_DEVICE_DEBUG))
    {
        Msg("! -dxdebug: debug layer unavailable, creating a regular device");
        createDeviceFlags &= ~u32(D3D_CREATE_DEVICE_DEBUG);
        if (DX10Only)
            R = createDevice(featureLevels3, std::size(featureLevels3));
        else
        {
            R = createDevice(featureLevels, std::size(featureLevels));
            if (FAILED(R))
                R = createDevice(featureLevels2, std::size(featureLevels2));
        }
    }
#endif

    if (SUCCEEDED(R))
    {
#ifndef XR_NATIVE
        if (createDeviceFlags & D3D_CREATE_DEVICE_DEBUG)
            dx11_debug_bind_info_queue(pDevice);
        if (ThisInstanceIsGlobal())
            da_streamline_d3d11_device_created(pDevice);
#endif

        pContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&pContext1));
#ifdef HAS_DX11_3
        pDevice->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&pDevice3));
#endif
        if (FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            D3DCompile = &::D3DCompile;
            ComputeShadersSupported = true;
        }
        else
        {
            if (ClearSkyMode)
            {
                hD3DCompiler = XRay::LoadModule("d3dcompiler_37");
                D3DCompile = static_cast<D3DCompileFunc>(hD3DCompiler->GetProcAddress("D3DCompileFromMemory"));
            }
            else
            {
                D3DCompile = &::D3DCompile;
            }

            D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS data;
            pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                &data, sizeof(data));
            ComputeShadersSupported = data.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
        }
        D3D11_FEATURE_DATA_D3D11_OPTIONS options;
        pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));

        D3D11_FEATURE_DATA_DOUBLES doubles;
        pDevice->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles));

        DoublePrecisionFloatShaderOps = doubles.DoublePrecisionFloatShaderOps;
        SAD4ShaderInstructions = options.SAD4ShaderInstructions;
        ExtendedDoublesShaderInstructions = options.ExtendedDoublesShaderInstructions;

        // The DirectX 12 capabilities, then the shader compiler that depends on them; the caps are
        // read again for the shader model the loaded compiler targets.
        if (ThisInstanceIsGlobal())
        {
            da_native_caps_on_device(pDevice);
#ifdef XR_NATIVE
            da_dxc_init(pDevice);
            da_native_caps_on_device(pDevice);
#endif
#ifdef XR_VULKAN
            // Every Vulkan shader is DXC's SPIR-V: without the compiler the device is of no use.
            if (!da_dxc_active())
            {
                _RELEASE(pContext);
                _RELEASE(pDevice);
                R = E_FAIL;
            }
#endif
        }
    }

    if (FAILED(R))
    {
        Valid = false;
        if (!ThisInstanceIsGlobal())
            return;
#ifdef XR_D3D12
        // The adapter passed the probe and still failed: the next start runs DirectX 11.
        xrRender_d3d12_failed();
        Msg("Failed to initialize DirectX 12.\n"
            "The game will start with DirectX 11 next time.\n"
            "CreateDevice returned 0x%08x", R);
        xrDebug::DoExit("Failed to initialize DirectX 12.\nThe game will start with DirectX 11 next time.");
#endif
#ifdef XR_VULKAN
        // The adapter passed the probe and still failed: the next start runs DirectX 11.
        xrRender_vulkan_failed();
        Msg("Failed to initialize Vulkan.\n"
            "The game will start with DirectX 11 next time.\n"
            "CreateDevice returned 0x%08x", R);
        xrDebug::DoExit("Failed to initialize Vulkan.\nThe game will start with DirectX 11 next time.");
#endif
        // Fatal error! Cannot create rendering device AT STARTUP !!!
        Msg("Failed to initialize graphics hardware.\n"
            "Please try to restart the game.\n"
            "CreateDevice returned 0x%08x", R);
        xrDebug::DoExit("Failed to initialize graphics hardware.\nPlease try to restart the game.");
    }

    _SHOW_REF("* CREATE: DeviceREF:", pDevice);

#ifndef XR_NATIVE // Tracy's D3D11 GPU zones need a real D3D11 device
    // Register immediate context in profiler
    if (ThisInstanceIsGlobal())
    {
        TaskScheduler->AddTask([this]
        {
            ZoneScopedN("TracyD3D11Context");
            profiler_ctx = TracyD3D11Context(pDevice, get_context(CHW::IMM_CTX_ID));
        });
    }
#endif

    // Create deferred contexts
    if (ThisInstanceIsGlobal())
    {
        ZoneScopedN("Create deferred contexts");
        for (int id = 0; id < R__NUM_PARALLEL_CONTEXTS; ++id)
        {
            R = pDevice->CreateDeferredContext(0, &d3d_contexts_pool[id]);
            VERIFY(SUCCEEDED(R));
        }
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (!SDL_GetWindowWMInfo(sdlWnd, &info))
    {
        Msg("! Failed to retrieve SDL window handle: %s", SDL_GetError());
        Valid = false;
        return;
    }

    const HWND hwnd = info.info.win.window;
    InstallWindowProc(hwnd);

    if (!CreateSwapChain2(hwnd))
    {
#if defined(XR_D3D12)
        // D3D12 has no legacy DXGI 1.1 swap chain to fall back to.
        Valid = false;
        if (ThisInstanceIsGlobal())
        {
            xrRender_d3d12_failed();
            Log("Failed to create the DirectX 12 swap chain.\n"
                "The game will start with DirectX 11 next time.");
            xrDebug::DoExit("Failed to initialize DirectX 12.\nThe game will start with DirectX 11 next time.");
        }
#elif defined(XR_VULKAN)
        Valid = false;
        if (ThisInstanceIsGlobal())
        {
            xrRender_vulkan_failed();
            Log("Failed to create the Vulkan swap chain.\n"
                "The game will start with DirectX 11 next time.");
            xrDebug::DoExit("Failed to initialize Vulkan.\nThe game will start with DirectX 11 next time.");
        }
#else
        if (!CreateSwapChain(hwnd))
            Valid = false;
#endif
    }

    // Select depth-stencil format
    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    const DXGI_FORMAT selectedFormat = SelectFormat(D3D_FORMAT_SUPPORT_DEPTH_STENCIL, formats);
    if (selectedFormat == DXGI_FORMAT_UNKNOWN)
    {
        Valid = false;
        if (!ThisInstanceIsGlobal())
            return;
        Log("Failed to initialize graphics hardware: "
            "failed to select depth-stencil format.\n"
            "Please try to restart the game.");
        xrDebug::DoExit("Failed to initialize graphics hardware.\nPlease try to restart the game.");
    }
    Caps.fDepth = dx11TextureUtils::ConvertTextureFormat(selectedFormat);

    const auto memory = Desc.DedicatedVideoMemory;
    Msg("*   Texture memory: %d M", memory / (1024 * 1024));

    string256 adapterName{};
    WideCharToMultiByte(CP_ACP, 0, Desc.Description, -1, adapterName, std::size(adapterName), nullptr, nullptr);
    LARGE_INTEGER driverVersion{};
    m_pAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion);
    CrashReporter::SetGpuInfo(adapterName, Desc.VendorId, Desc.DeviceId,
        Desc.DedicatedVideoMemory, Desc.SharedSystemMemory, FeatureLevel, driverVersion.QuadPart);
}

bool CHW::CreateSwapChain(HWND hwnd)
{
    ZoneScoped;

    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC& sd = m_ChainDesc;
    ZeroMemory(&sd, sizeof(sd));

    // Back buffer
    sd.BufferDesc.Width = Device.dwWidth;
    sd.BufferDesc.Height = Device.dwHeight;

    //  TODO: DX11: implement dynamic format selection
    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT, // Do we even need this?
        //DXGI_FORMAT_R10G10B10A2_UNORM, // D3DX11SaveTextureToMemory fails on this format
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    sd.BufferDesc.Format = SelectFormat(D3D_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx11TextureUtils::ConvertTextureFormat(sd.BufferDesc.Format);

    // Buffering
    BackBufferCount = 1;
    sd.BufferCount = BackBufferCount;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    // Windoze
    /* XXX:
       Probably the reason of weird tearing
       glitches reported by Shoker in windowed
       mode with VSync enabled.
       XXX: Fix this windoze stuff!!!
    */
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    sd.OutputWindow = hwnd;

    sd.Windowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;

    //  Additional set up
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    const auto hr = m_pFactory->CreateSwapChain(pDevice, &sd, &m_pSwapChain);
    if (FAILED(hr))
        return false;
#ifndef XR_NATIVE
    // sl.common sees every present through Streamline's proxy of the chain.
    if (ThisInstanceIsGlobal())
        da_streamline_d3d11_swapchain_created(m_pSwapChain);
#endif

    // sd already describes what was asked for, so a failed refresh is not fatal here.
    UpdateChainDesc();
    return true;
}

// m_ChainDesc drives every later fullscreen decision (OnAppActivate, OnAppDeactivate, teardown),
// so it has to mirror the swap chain rather than the request that was made of it: DXGI is free to
// refuse exclusive fullscreen, and it silently leaves it when the display mode changes underneath.
bool CHW::UpdateChainDesc()
{
    if (!m_pSwapChain || FAILED(m_pSwapChain->GetDesc(&m_ChainDesc)))
        return false;

    // Only GetFullscreenState knows whether DXGI holds the output right now.
    BOOL fullscreen{};
    if (SUCCEEDED(m_pSwapChain->GetFullscreenState(&fullscreen, nullptr)))
        m_ChainDesc.Windowed = !fullscreen;

    if (ThisInstanceIsGlobal())
        Msg("* Swap chain: %ux%u, %s", m_ChainDesc.BufferDesc.Width, m_ChainDesc.BufferDesc.Height,
            m_ChainDesc.Windowed ? "windowed" : "exclusive fullscreen");

    return true;
}

bool CHW::CreateSwapChain2(HWND hwnd)
{
#ifndef XR_NATIVE // the only swap chain D3D12 and Vulkan have
    if (strstr(Core.Params, "-no_dx11_2"))
        return false;
#endif

    ZoneScoped;

#ifdef HAS_DX11_2
    IDXGIFactory2* pFactory2{};
    m_pAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pFactory2);
    if (!pFactory2)
        return false;

    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC1 desc{};

    // Back buffer
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT,
        //DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    desc.Format = SelectFormat(D3D11_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx11TextureUtils::ConvertTextureFormat(desc.Format);

    // Buffering. Only buffer 0 is ever rendered into; the flip model needs a second one to hand
    // to the display while the game draws, and DXGI rotates them internally.
    BackBufferCount = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    desc.Scaling = DXGI_SCALING_STRETCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fulldesc{};
    fulldesc.Windowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;

    // Copy-based chain: the shape every release before the flip model shipped with, and the
    // fallback whenever a flip chain cannot be created. These must be set unconditionally -
    // leaving BufferCount at zero makes creation fail outright.
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.BufferCount = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // Flip presentation lets the game drive the display directly instead of routing every
    // windowed frame through the desktop compositor. That is what makes variable refresh work in
    // borderless and stops a frame limit from beating against DWM's fixed grid - a 120 fps limit
    // on a 165 Hz panel used to alternate between one and two refreshes per frame. It needs
    // Windows 10, at least two buffers and a non-MSAA back buffer, so old systems simply fall
    // back to the copy-based chain above; -no_flip forces that fallback everywhere.
    //
    // Exclusive fullscreen never gets a flip chain at creation. A flip chain has to be born
    // windowed, and its tearing flag is only legal while windowed, so asking DXGI for both at
    // once yields a swap chain that reports success and presents nowhere: the engine renders
    // every frame and the player keeps looking at the desktop.
    m_tearingSupported = false;
    const bool flipAllowed = !strstr(Core.Params, "-no_flip") && fulldesc.Windowed;
    bool flipModel = flipAllowed;
    if (flipModel)
    {
#ifdef HAS_DXGI1_5
        // Tearing support doubles as the "this is a modern DXGI" probe, and the flag is what
        // allows an uncapped frame to reach the panel without waiting for the next refresh.
        IDXGIFactory5* pFactory5{};
        if (SUCCEEDED(pFactory2->QueryInterface(__uuidof(IDXGIFactory5), (void**)&pFactory5)))
        {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(pFactory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
                m_tearingSupported = allowTearing != FALSE;
            _RELEASE(pFactory5);
        }
#endif
        // Three buffers: with two, a vsync'd frame slower than the refresh waits a whole refresh for the
        // buffer the display still shows. Paced on the frame latency object, so the third one never queues
        // more than the frame rate allows (present_pacing.h).
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.BufferCount = 3;
#ifndef XR_NATIVE
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#endif
#ifdef HAS_DXGI1_5
        if (m_tearingSupported)
            desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
#endif
    }

    IDXGISwapChain1* swapchain{};
#if defined(XR_D3D12)
    // The layer creates the chain on its command queue, always flip model: a copy-model
    // description (exclusive fullscreen at creation, -no_flip) becomes FLIP_DISCARD there.
    HRESULT result = xrd3d12::CreateSwapChainForHwnd(pDevice, pFactory2, hwnd, &desc,
        fulldesc.Windowed ? nullptr : &fulldesc, &swapchain);
#elif defined(XR_VULKAN)
    // A VkSwapchainKHR of the window behind IDXGISwapChain1 (flip semantics, three images).
    HRESULT result = xrvk::CreateSwapChainForHwnd(pDevice, pFactory2, hwnd, &desc,
        fulldesc.Windowed ? nullptr : &fulldesc, &swapchain);
#else
    HRESULT result = pFactory2->CreateSwapChainForHwnd(pDevice, hwnd, &desc,
        fulldesc.Windowed ? nullptr : &fulldesc, nullptr, &swapchain);
#endif

    if (FAILED(result) && flipModel)
    {
        // Pre-Windows 10, or a driver that refuses flip: keep the old behaviour rather than
        // dropping to the legacy DXGI 1.1 path.
        flipModel = false;
        m_tearingSupported = false;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
#if defined(XR_D3D12)
        result = xrd3d12::CreateSwapChainForHwnd(pDevice, pFactory2, hwnd, &desc,
            fulldesc.Windowed ? nullptr : &fulldesc, &swapchain);
#elif defined(XR_VULKAN)
        result = xrvk::CreateSwapChainForHwnd(pDevice, pFactory2, hwnd, &desc,
            fulldesc.Windowed ? nullptr : &fulldesc, &swapchain);
#else
        result = pFactory2->CreateSwapChainForHwnd(pDevice, hwnd, &desc,
            fulldesc.Windowed ? nullptr : &fulldesc, nullptr, &swapchain);
#endif
    }

    _RELEASE(pFactory2);

    if (FAILED(result))
        return false;

#ifdef XR_NATIVE
    flipModel = true;
#endif

    m_pSwapChain = swapchain;

    // sl.common sees every present only through Streamline: its proxy of the chain (DX11), or a
    // chain made through its upgraded factory (DX12; frame generation swaps it again later).
    if (ThisInstanceIsGlobal())
    {
#ifdef XR_D3D12
        if (xrd3d12::SwapChainProvider* provider = da_streamline_swapchain_provider())
        {
            if (FAILED(xrd3d12::RecreateSwapChain(m_pSwapChain, provider)))
                Msg("! [Streamline] the swap chain could not be made through Streamline: DLSS may leak memory");
        }
#elif !defined(XR_VULKAN)
        da_streamline_d3d11_swapchain_created(m_pSwapChain);
#endif
    }

    m_pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&m_pSwapChain2));

#ifndef XR_NATIVE
    // Null for a chain without the flag (the copy model).
    if (ThisInstanceIsGlobal() && flipModel && m_pSwapChain2)
        m_pacer.Attach(m_pSwapChain2->GetFrameLatencyWaitableObject(), hwnd);
#endif
    if (ThisInstanceIsGlobal())
        Msg("* Presentation: %s%s%s", flipModel ? "flip" : "copy (legacy)",
            m_tearingSupported ? ", tearing allowed" : "", m_pacer.Attached() ? ", vsync paced" : "");

    if (!UpdateChainDesc())
    {
        _RELEASE(m_pSwapChain2);
        _RELEASE(m_pSwapChain);
        return false;
    }

    return true;
#else // #ifdef HAS_DX11_2
    UNUSED(hwnd);
#endif

    return false;
}

bool CHW::ThisInstanceIsGlobal() const
{
    return this == &HW;
}

void CHW::DestroyDevice()
{
    RestoreWindowProc();
    ReleaseBackgroundPreviewResources();

    // DirectStorage drains its queues on the device, then Streamline goes before the swap chain and
    // the device it was given.
    if (ThisInstanceIsGlobal())
    {
        da_dxc_log_stats(); // [dx12:dxc] hook
        da_dstorage_shutdown();
        da_streamline_shutdown();
    }

    if (ThisInstanceIsGlobal()) // only if we are global HW
    {
        RSManager.ClearStateArray();
        DSSManager.ClearStateArray();
        BSManager.ClearStateArray();
        SSManager.ClearStateArray();
    }
    //  Must switch to windowed mode to release swap chain
    if (!m_ChainDesc.Windowed && m_pSwapChain)
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
    m_pacer.Detach();
    m_adaptiveClock.Reset();
#ifdef HAS_DX11_2
    _RELEASE(m_pSwapChain2);
#endif
    _SHOW_REF("refCount:m_pSwapChain", m_pSwapChain);
    _RELEASE(m_pSwapChain);

    if (profiler_ctx)
        TracyD3D11Destroy(profiler_ctx);

    _RELEASE(pContext1);
    for (int id = 0; id < R__NUM_CONTEXTS; ++id)
    {
        _SHOW_REF("refCount:pContext", d3d_contexts_pool[id]);
        _RELEASE(d3d_contexts_pool[id]);
    }

#ifdef HAS_DX11_3
    _RELEASE(pDevice3);
#endif
    _SHOW_REF("refCount:pDevice:", pDevice);
    _RELEASE(pDevice);
    DestroyD3D();
    if (ThisInstanceIsGlobal())
        da_streamline_device_released();
}

//////////////////////////////////////////////////////////////////////
// Resetting device
//////////////////////////////////////////////////////////////////////
void CHW::Reset()
{
    ZoneScoped;
    SetBackgroundPreviewEnabled(false);
    ReleaseBackgroundPreviewResources();

    DXGI_SWAP_CHAIN_DESC& cd = m_ChainDesc;
    const bool bWindowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;

#ifndef XR_NATIVE
    // The chain a fresh start would make for the new window mode: a paced flip chain windowed, the copy model
    // in exclusive fullscreen (Direct3D 11 does not take the frame latency object there, so SetFullscreenState
    // failed on the flip chain). Nothing holds the back buffer here: ResizeBuffers below needs that as well.
    const bool flipWanted = bWindowed && !strstr(Core.Params, "-no_flip") && !strstr(Core.Params, "-no_dx11_2");
    if (ThisInstanceIsGlobal() && outputWindow && flipWanted != UsingFlipPresentationModel())
    {
        // A chain is released windowed; only the copy model can be in exclusive fullscreen here.
        if (!UsingFlipPresentationModel())
            m_pSwapChain->SetFullscreenState(FALSE, nullptr);
        m_pacer.Detach();
        m_adaptiveClock.Reset();
        _RELEASE(m_pSwapChain2);
        _RELEASE(m_pSwapChain);
        if (!CreateSwapChain2(outputWindow) && !CreateSwapChain(outputWindow))
            FATAL("Failed to create the swap chain for the new window mode.");
    }
#endif

    // An occupied output, an unsupported mode or a rendering adapter without outputs all make this
    // fail; swallowing the result leaves the engine convinced it is exclusive when it is not. A chain
    // already in the state is left alone: a waitable one refuses even the call that changes nothing.
    BOOL fullscreenNow{};
    if (FAILED(m_pSwapChain->GetFullscreenState(&fullscreenNow, nullptr)) || !fullscreenNow != bWindowed)
    {
        const HRESULT fullscreenResult = m_pSwapChain->SetFullscreenState(!bWindowed, nullptr);
        if (FAILED(fullscreenResult))
            Msg("! [%s] failed to %s exclusive fullscreen: 0x%08x", __FUNCTION__,
                bWindowed ? "leave" : "enter", fullscreenResult);
    }

#ifdef HAS_DXGI1_5
    // A chain that was born windowed may now be going exclusive fullscreen, where the tearing
    // flag is not legal. Carrying it across the transition is the same defect as creating a
    // tearing chain in fullscreen: presentation silently stops reaching the display.
    if (m_tearingSupported)
    {
        if (bWindowed)
            cd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        else
            cd.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    }
#endif

    DXGI_MODE_DESC& desc = m_ChainDesc.BufferDesc;
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    CHK_DX(m_pSwapChain->ResizeTarget(&desc));
    CHK_DX(m_pSwapChain->ResizeBuffers(
        cd.BufferCount, desc.Width, desc.Height, desc.Format, cd.Flags));

    UpdateChainDesc();
}

void CHW::SetPrimaryAttributes(u32& /*windowFlags*/)
{

}

bool CHW::CheckFormatSupport(const DXGI_FORMAT format, const u32 feature) const
{
    u32 supports;

    if (SUCCEEDED(pDevice->CheckFormatSupport(format, &supports)))
    {
        if (supports & feature)
            return true;
    }

    return false;
}

DXGI_FORMAT CHW::SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const
{
    for (size_t i = 0; i < count; ++i)
        if (CheckFormatSupport(formats[i], feature))
            return formats[i];

    return DXGI_FORMAT_UNKNOWN;
}

bool CHW::UsingFlipPresentationModel() const
{
    return m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
#ifdef HAS_DXGI1_4
        || m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
#endif
    ;
}

std::pair<u32, u32> CHW::GetSurfaceSize() const
{
    return
    {
        m_ChainDesc.BufferDesc.Width,
        m_ChainDesc.BufferDesc.Height
    };
}

void CHW::BeginScene() { }
void CHW::EndScene() { }

void CHW::Present()
{
    // rs_vsync. Adaptive keeps vsync while frames keep up with the refresh and tears the late ones: Vulkan's
    // FIFO_RELAXED where the surface has it (else plain vsync: a mode switch there may recreate the chain),
    // DXGI decided here from the present rate. Frame generation paces its own presents: adaptive is vsync there.
    bool bUseVSync = psDeviceFlags.test(rsVSync);
    const bool adaptive = bUseVSync && psVSyncMode == VSyncAdaptive;
#ifdef XR_VULKAN
    xrvk::SetAdaptiveVsync(m_pSwapChain, adaptive);
#else
    if (adaptive && Device.dwFramesPerRender <= 1)
    {
        m_adaptiveClock.Tick(outputWindow);
        bUseVSync = !m_adaptiveClock.Slower();
    }
#endif

    UpdateBackgroundPreview();

    // Without vsync a windowed flip chain still waits for the next refresh unless it is told
    // tearing is acceptable - which is exactly what "vsync off" asks for. Exclusive fullscreen
    // must not carry the flag.
    UINT presentFlags = 0;
#ifdef HAS_DXGI1_5
    if (!bUseVSync && m_tearingSupported && m_ChainDesc.Windowed)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
#endif

    m_pacer.BeforePresent(bUseVSync);

    switch (m_pSwapChain->Present(bUseVSync ? 1 : 0, presentFlags))
    {
    case DXGI_STATUS_OCCLUDED:
        if (g_pause_in_background)
            presentTestState = PresentTestState::Occluded;
        break;

    case DXGI_ERROR_DEVICE_RESET:
        presentTestState = PresentTestState::DeviceReset;
        break;

    case DXGI_ERROR_DEVICE_REMOVED:
        presentTestState = PresentTestState::DeviceRemoved;
        break;
    }

    CurrentBackBuffer = (CurrentBackBuffer + 1) % BackBufferCount;

#ifndef XR_NATIVE
    TracyD3D11Collect(profiler_ctx);
#endif
}

DeviceState CHW::GetDeviceState()
{
    if (presentTestState == PresentTestState::DeviceReset)
        return DeviceState::NeedReset;

    if (presentTestState == PresentTestState::DeviceRemoved)
    {
        // The frame loop keeps polling a lost device, and this state is never cleared, so the
        // report has to be latched: otherwise it repeats every frame and the diagnostic log
        // carries several identical fatals instead of the one that matters.
        if (deviceRemovedReported)
            return DeviceState::Lost;
        deviceRemovedReported = true;

        // Report what DXGI actually says: a driver update and a physical removal are only two of
        // the reasons, and the interesting ones (a hung or faulting GPU, an internal driver error,
        // running out of video memory) are indistinguishable without this.
        const HRESULT reason = pDevice ? pDevice->GetDeviceRemovedReason() : E_FAIL;
        pcstr description;
        switch (reason)
        {
        case DXGI_ERROR_DEVICE_HUNG: description = "the GPU stopped responding to commands"; break;
        case DXGI_ERROR_DEVICE_RESET: description = "the GPU was reset"; break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: description = "an internal graphics driver error"; break;
        case DXGI_ERROR_INVALID_CALL: description = "an invalid rendering command"; break;
        case E_OUTOFMEMORY: description = "the graphics driver ran out of memory"; break;
        case DXGI_ERROR_DEVICE_REMOVED: description = "the device was removed"; break;
        default: description = "an unknown reason"; break;
        }
        FATAL_F("Rendering device lost: %s (DXGI reason 0x%08X).\n"
                "This also happens when the graphics driver is updated or the GPU is removed.\n"
                "Please, restart the game.",
            description, u32(reason));
        return DeviceState::Lost;
    }

    if (presentTestState == PresentTestState::Occluded)
    {
        if (!g_pause_in_background)
        {
            presentTestState = PresentTestState::None;
            return DeviceState::Normal;
        }

        switch (m_pSwapChain->Present(0, DXGI_PRESENT_TEST))
        {
        case S_OK:
            presentTestState = PresentTestState::None;
            break;

        case DXGI_STATUS_OCCLUDED:
            // Do not render until we become visible again
            return DeviceState::Lost;

        case DXGI_ERROR_DEVICE_RESET:
            presentTestState = PresentTestState::DeviceReset;
            return DeviceState::NeedReset;

        case DXGI_ERROR_DEVICE_REMOVED:
            // Reported by the latched path above on the next poll, which names the real DXGI
            // reason. Returning Normal here would have sent the frame loop on to render against
            // a device that is already gone.
            presentTestState = PresentTestState::DeviceRemoved;
            return DeviceState::Lost;
        }
    }

    return DeviceState::Normal;
}

LRESULT CALLBACK CHW::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT result{};
    if (HW.outputWindow == hwnd)
        da_streamline_window_message(message);
    if (HW.outputWindow == hwnd && HW.HandleDwmMessage(message, lParam, result))
        return result;

    if (HW.previousWindowProc)
        return CallWindowProc(HW.previousWindowProc, hwnd, message, wParam, lParam);

    return DefWindowProc(hwnd, message, wParam, lParam);
}

bool CHW::HandleDwmMessage(const UINT message, const LPARAM lParam, LRESULT& result)
{
    if (!backgroundPreviewEnabled)
        return false;

    if (message != WM_DWMSENDICONICTHUMBNAIL && message != WM_DWMSENDICONICLIVEPREVIEWBITMAP)
        return false;

    backgroundPreviewRequestedUntil = GetTickCount64() + 250;
    if (!backgroundPreviewBitmap)
        return false;

    HRESULT status{};
    if (message == WM_DWMSENDICONICTHUMBNAIL)
    {
        const auto thumbnail = CreateScaledBackgroundPreview(HIWORD(lParam), LOWORD(lParam));
        if (!thumbnail)
            return false;

        status = DwmSetIconicThumbnail(outputWindow, thumbnail, 0);
        DeleteObject(thumbnail);
    }
    else
    {
        RECT clientRect{};
        if (!GetClientRect(outputWindow, &clientRect))
            return false;

        const u32 clientWidth = static_cast<u32>(std::max(clientRect.right - clientRect.left, 1L));
        const u32 clientHeight = static_cast<u32>(std::max(clientRect.bottom - clientRect.top, 1L));
        HBITMAP preview = backgroundPreviewBitmap;
        if (backgroundPreviewWidth > clientWidth || backgroundPreviewHeight > clientHeight)
            preview = CreateScaledBackgroundPreview(clientWidth, clientHeight);
        if (!preview)
            return false;

        status = DwmSetIconicLivePreviewBitmap(outputWindow, preview, nullptr, DWM_SIT_DISPLAYFRAME);
        if (preview != backgroundPreviewBitmap)
            DeleteObject(preview);
    }

    if (FAILED(status))
        return false;

    result = 0;
    return true;
}

void CHW::InstallWindowProc(const HWND hwnd)
{
    if (!ThisInstanceIsGlobal() || outputWindow)
        return;

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc));
    if (!previous && GetLastError() != ERROR_SUCCESS)
    {
        Msg("! Failed to install the background preview window procedure: %lu", GetLastError());
        return;
    }

    outputWindow = hwnd;
    previousWindowProc = reinterpret_cast<WNDPROC>(previous);
}

void CHW::RestoreWindowProc()
{
    SetBackgroundPreviewEnabled(false);
    if (outputWindow && previousWindowProc)
        SetWindowLongPtr(outputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));

    outputWindow = nullptr;
    previousWindowProc = nullptr;
}

void CHW::UpdateBackgroundPreview()
{
    if (!outputWindow)
        return;

    const bool enabled = !g_pause_in_background && IsIconic(outputWindow);
    SetBackgroundPreviewEnabled(enabled);
    if (!enabled)
        return;

    ID3D11DeviceContext* context = get_context(CHW::IMM_CTX_ID);
    if (backgroundPreviewCopyPending)
    {
        // Never stall the render thread for a taskbar preview readback.
        const HRESULT copyState = context->GetData(
            backgroundPreviewReady, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (copyState == S_OK)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(backgroundPreviewStaging, 0, D3D11_MAP_READ, 0, &mapped)))
            {
                auto* destination = static_cast<u8*>(backgroundPreviewBits);
                for (u32 row = 0; row < backgroundPreviewHeight; ++row)
                {
                    const auto* source = static_cast<const u8*>(mapped.pData) + row * mapped.RowPitch;
                    auto* target = destination + row * backgroundPreviewWidth * 4;
                    for (u32 column = 0; column < backgroundPreviewWidth; ++column)
                    {
                        if (backgroundPreviewFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                            backgroundPreviewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                        {
                            target[0] = source[2];
                            target[1] = source[1];
                            target[2] = source[0];
                        }
                        else
                        {
                            target[0] = source[0];
                            target[1] = source[1];
                            target[2] = source[2];
                        }
                        target[3] = 255;
                        source += 4;
                        target += 4;
                    }
                }
                context->Unmap(backgroundPreviewStaging, 0);
                DwmInvalidateIconicBitmaps(outputWindow);
            }
            backgroundPreviewCopyPending = false;
        }
        else if (FAILED(copyState))
            backgroundPreviewCopyPending = false;
    }

    const u64 now = GetTickCount64();
    if (backgroundPreviewCopyPending || now > backgroundPreviewRequestedUntil ||
        now - backgroundPreviewLastCapture < 33)
        return;

    ID3D11Texture2D* backBuffer{};
    if (FAILED(m_pSwapChain->GetBuffer(
            CurrentBackBuffer, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
        return;

    if (EnsureBackgroundPreviewResources(backBuffer))
    {
        context->CopyResource(backgroundPreviewStaging, backBuffer);
        context->End(backgroundPreviewReady);
        backgroundPreviewCopyPending = true;
        backgroundPreviewLastCapture = now;
    }
    _RELEASE(backBuffer);
}

void CHW::SetBackgroundPreviewEnabled(const bool enabled)
{
    if (backgroundPreviewEnabled == enabled)
        return;

    backgroundPreviewEnabled = enabled;
    const BOOL value = enabled ? TRUE : FALSE;
    // DWM requests fresh bitmaps only while the minimized preview is active.
    DwmSetWindowAttribute(outputWindow, DWMWA_FORCE_ICONIC_REPRESENTATION, &value, sizeof(value));
    DwmSetWindowAttribute(outputWindow, DWMWA_HAS_ICONIC_BITMAP, &value, sizeof(value));

    if (enabled)
    {
        backgroundPreviewRequestedUntil = GetTickCount64() + 250;
        DwmInvalidateIconicBitmaps(outputWindow);
    }
    else
        ReleaseBackgroundPreviewResources();
}

bool CHW::EnsureBackgroundPreviewResources(ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    const bool supportedFormat = sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        sourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        sourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!supportedFormat)
        return false;

    if (backgroundPreviewStaging && backgroundPreviewWidth == sourceDescription.Width &&
        backgroundPreviewHeight == sourceDescription.Height &&
        backgroundPreviewFormat == sourceDescription.Format)
        return true;

    ReleaseBackgroundPreviewResources();

    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.SampleDesc.Count = 1;
    stagingDescription.SampleDesc.Quality = 0;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    if (FAILED(pDevice->CreateTexture2D(&stagingDescription, nullptr, &backgroundPreviewStaging)))
        return false;

    D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
    if (FAILED(pDevice->CreateQuery(&queryDescription, &backgroundPreviewReady)))
    {
        ReleaseBackgroundPreviewResources();
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(sourceDescription.Width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(sourceDescription.Height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    backgroundPreviewBitmap = CreateDIBSection(
        nullptr, &bitmapInfo, DIB_RGB_COLORS, &backgroundPreviewBits, nullptr, 0);
    if (!backgroundPreviewBitmap)
    {
        ReleaseBackgroundPreviewResources();
        return false;
    }

    backgroundPreviewFormat = sourceDescription.Format;
    backgroundPreviewWidth = sourceDescription.Width;
    backgroundPreviewHeight = sourceDescription.Height;
    return true;
}

void CHW::ReleaseBackgroundPreviewResources()
{
    backgroundPreviewCopyPending = false;
    _RELEASE(backgroundPreviewReady);
    _RELEASE(backgroundPreviewStaging);
    if (backgroundPreviewBitmap)
        DeleteObject(backgroundPreviewBitmap);
    backgroundPreviewBitmap = nullptr;
    backgroundPreviewBits = nullptr;
    backgroundPreviewFormat = DXGI_FORMAT_UNKNOWN;
    backgroundPreviewWidth = 0;
    backgroundPreviewHeight = 0;
    backgroundPreviewLastCapture = 0;
    backgroundPreviewRequestedUntil = 0;
}

HBITMAP CHW::CreateScaledBackgroundPreview(const u32 maxWidth, const u32 maxHeight) const
{
    if (!backgroundPreviewBits || !maxWidth || !maxHeight)
        return nullptr;

    u32 width = maxWidth;
    u32 height = static_cast<u32>(
        static_cast<u64>(backgroundPreviewHeight) * maxWidth / backgroundPreviewWidth);
    if (height > maxHeight)
    {
        height = maxHeight;
        width = static_cast<u32>(
            static_cast<u64>(backgroundPreviewWidth) * maxHeight / backgroundPreviewHeight);
    }
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    BITMAPINFO destinationInfo{};
    destinationInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    destinationInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    destinationInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    destinationInfo.bmiHeader.biPlanes = 1;
    destinationInfo.bmiHeader.biBitCount = 32;
    destinationInfo.bmiHeader.biCompression = BI_RGB;

    void* destinationBits{};
    const HBITMAP bitmap = CreateDIBSection(
        nullptr, &destinationInfo, DIB_RGB_COLORS, &destinationBits, nullptr, 0);
    if (!bitmap)
        return nullptr;

    BITMAPINFO sourceInfo{};
    sourceInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sourceInfo.bmiHeader.biWidth = static_cast<LONG>(backgroundPreviewWidth);
    sourceInfo.bmiHeader.biHeight = -static_cast<LONG>(backgroundPreviewHeight);
    sourceInfo.bmiHeader.biPlanes = 1;
    sourceInfo.bmiHeader.biBitCount = 32;
    sourceInfo.bmiHeader.biCompression = BI_RGB;

    const HDC deviceContext = CreateCompatibleDC(nullptr);
    const HGDIOBJ previousBitmap = SelectObject(deviceContext, bitmap);
    SetStretchBltMode(deviceContext, HALFTONE);
    SetBrushOrgEx(deviceContext, 0, 0, nullptr);
    const int copied = StretchDIBits(deviceContext, 0, 0, width, height, 0, 0,
        backgroundPreviewWidth, backgroundPreviewHeight, backgroundPreviewBits,
        &sourceInfo, DIB_RGB_COLORS, SRCCOPY);
    SelectObject(deviceContext, previousBitmap);
    DeleteDC(deviceContext);

    if (copied == GDI_ERROR)
    {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}
} // namespace xray::render::RENDER_NAMESPACE
