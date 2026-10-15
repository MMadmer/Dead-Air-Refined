// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// AMD FSR frame generation on DirectX 12 (docs/dead-air/DX12_RENDERER.md, 1) through the AMD FSR
// SDK 2.3 signed DLLs (ffx-api): FSR's frame interpolation swap chain presents a generated frame
// between every two rendered ones (2x), and the frame generation context prepares it from the
// scene depth and the motion vectors. The UI is told apart from the scene by the difference
// between the back buffer and its hudless copy, so it is composed onto the generated frames too.
// Per frame: ffxConfigure -> PrepareV2 (here) -> the generation dispatch inside Present, through
// the swap chain's callback. Every present FFX makes goes through our present callback, which
// paces it to the refresh under vsync on FFX's real swap chain (d12_pacing.h).

#include "stdafx.h"

#ifdef XR_D3D12
#include "da_framegen.h"
#include "da_ffx_loader.h"
#include "da_streamline.h"
#include "Layers/xrRenderDX12/d12_pacing.h"

#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
ID3D12Device* fsr_fg_device() { return HW.pDevice ? xrd3d12::NativeDevice(HW.pDevice) : nullptr; }

void fsr_fg_message(uint32_t type, const wchar_t* message)
{
    string512 text;
    const int written = WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    text[written > 0 ? written : 0] = 0;
    Msg("%s [FSR FG] %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "!" : "~", text);
}

// What the swap chain calls inside Present to generate the frame (the SDK's own recipe).
ffxReturnCode_t fsr_fg_generate(ffxDispatchDescFrameGeneration* params, void* context)
{
    const ffxFunctions* ffx = da_ffx();
    return ffx ? ffx->Dispatch(static_cast<ffxContext*>(context), &params->header) : FFX_API_RETURN_ERROR;
}

// Forwards everything to the factory FFX is given and keeps the swap chain FFX makes through it:
// the real one behind FFX's proxy, whose waitable object the vsync pacing needs (FFX exposes the
// real swap chain no other way). FFX uses the factory only while its context is being created.
class fsr_capture_factory final : public IDXGIFactory2
{
    std::atomic<ULONG> m_refs{ 1 };
    IDXGIFactory2* m_inner;
    IDXGISwapChain1* m_captured{};

public:
    explicit fsr_capture_factory(IDXGIFactory2* inner) : m_inner(inner) { m_inner->AddRef(); }
    ~fsr_capture_factory()
    {
        if (m_captured)
            m_captured->Release();
        m_inner->Release();
    }

    IDXGISwapChain1* Captured() const { return m_captured; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2))
        {
            *object = static_cast<IDXGIFactory2*>(this);
            AddRef();
            return S_OK;
        }
        // Newer interfaces (FFX checks tearing support through IDXGIFactory5) come from the factory.
        return m_inner->QueryInterface(riid, object);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (!refs)
            delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override
    {
        return m_inner->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    {
        return m_inner->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override
    {
        return m_inner->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** parent) override { return m_inner->GetParent(riid, parent); }

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter** adapter) override
    {
        return m_inner->EnumAdapters(index, adapter);
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND hwnd, UINT flags) override
    {
        return m_inner->MakeWindowAssociation(hwnd, flags);
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* hwnd) override { return m_inner->GetWindowAssociation(hwnd); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) override
    {
        return m_inner->CreateSwapChain(device, desc, swapChain);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter** adapter) override
    {
        return m_inner->CreateSoftwareAdapter(module, adapter);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override
    {
        return m_inner->EnumAdapters1(index, adapter);
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return m_inner->IsCurrent(); }

    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return m_inner->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swapChain) override
    {
        const HRESULT hr = m_inner->CreateSwapChainForHwnd(device, hwnd, desc, fullscreen, output, swapChain);
        if (SUCCEEDED(hr) && swapChain && *swapChain)
        {
            if (m_captured)
                m_captured->Release();
            m_captured = *swapChain;
            m_captured->AddRef();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* device, IUnknown* window,
        const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** swapChain) override
    {
        return m_inner->CreateSwapChainForCoreWindow(device, window, desc, output, swapChain);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID* luid) override
    {
        return m_inner->GetSharedResourceAdapterLuid(resource, luid);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND hwnd, UINT message, DWORD* cookie) override
    {
        return m_inner->RegisterStereoStatusWindow(hwnd, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return m_inner->RegisterStereoStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override { m_inner->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND hwnd, UINT message, DWORD* cookie) override
    {
        return m_inner->RegisterOcclusionStatusWindow(hwnd, message, cookie);
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD* cookie) override
    {
        return m_inner->RegisterOcclusionStatusEvent(event, cookie);
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override { m_inner->UnregisterOcclusionStatus(cookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
        IDXGIOutput* output, IDXGISwapChain1** swapChain) override
    {
        return m_inner->CreateSwapChainForComposition(device, desc, output, swapChain);
    }
};

// FSR's frame interpolation swap chain in place of the plain DXGI one. A static object: the swap
// chain keeps it until the device goes (da_framegen.h).
class fsr_swapchain_provider final : public xrd3d12::SwapChainProvider
{
    ffxContext m_context{};
    IDXGISwapChain4* m_chain{};
    // Streamline's factory proxy the chain was made through, kept while the context lives.
    IDXGIFactory2* m_factory{};
    // ffx-api may keep the pointers of a create description until the context is destroyed.
    DXGI_SWAP_CHAIN_DESC1 m_desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC m_fullscreen{};
    // Every present of the proxy, rendered or generated, ends on FFX's real swap chain; the present
    // callback paces them on its waitable object (d12_pacing.h). The callback runs on the thread
    // that presents: the render thread without generation, FFX's presenter thread with it.
    IDXGISwapChain2* m_real{};
    xrd3d12::PresentPacer m_pacer;
    std::atomic<bool> m_vsync{};

    // The context through a factory that keeps the real swap chain.
    ffxReturnCode_t CreateThrough(const ffxFunctions* ffx, ffxCreateContextDescFrameGenerationSwapChainForHwndDX12& create,
        IDXGIFactory2* factory)
    {
        fsr_capture_factory* capture = new fsr_capture_factory(factory);
        create.dxgiFactory = capture;
        const ffxReturnCode_t code = ffx->CreateContext(&m_context, &create.header, nullptr);
        if (code == FFX_API_RETURN_OK && capture->Captured())
            capture->Captured()->QueryInterface(IID_PPV_ARGS(&m_real));
        capture->Release();
        create.dxgiFactory = nullptr;
        return code;
    }

    void ReleaseReal()
    {
        m_pacer.Detach();
        if (m_real)
        {
            m_real->Release();
            m_real = nullptr;
        }
    }

public:
    HRESULT Create(IDXGIFactory2* factory, ID3D12CommandQueue* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain4** swapChain) override
    {
        *swapChain = nullptr;
        const ffxFunctions* ffx = da_ffx();
        if (!ffx || m_context)
            return E_FAIL;

        m_desc = desc;
        // FSR paces the generated frame itself; more than two buffers only add latency.
        m_desc.BufferCount = 2;
        // The waitable object FFX would offer follows its GPU fences, not the display: the pacing
        // waits on the real swap chain instead, and the layer leaves this chain alone.
        m_desc.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (fullscreen)
            m_fullscreen = *fullscreen;

        // Mandatory since SDK 2.1: the API version this code was written against.
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

        IDXGISwapChain4* chain = nullptr;
        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        create.header.pNext = &version.header;
        create.swapchain = &chain;
        create.hwnd = hwnd;
        create.desc = &m_desc;
        create.fullscreenDesc = fullscreen ? &m_fullscreen : nullptr;
        create.gameQueue = queue;

        // Through Streamline's factory where it runs: its per-frame bookkeeping lives on the
        // presents, and DLSS upscaling may run under this frame generation. The plain factory
        // is the fallback.
        IDXGIFactory2* streamline = da_streamline_factory(factory);
        ffxReturnCode_t code = CreateThrough(ffx, create, streamline ? streamline : factory);
        if (streamline)
        {
            if (code == FFX_API_RETURN_OK && chain)
                m_factory = streamline;
            else
            {
                Msg("! [FSR FG] the swap chain cannot be made through Streamline (%u): plain DXGI", u32(code));
                streamline->Release();
                if (m_context)
                    ffx->DestroyContext(&m_context, nullptr);
                m_context = nullptr;
                if (chain)
                    chain->Release();
                chain = nullptr;
                ReleaseReal();
                code = CreateThrough(ffx, create, factory);
            }
        }
        if (code != FFX_API_RETURN_OK || !chain)
        {
            Msg("! [FSR FG] the frame generation swap chain cannot be created (%u)", u32(code));
            if (m_context)
                ffx->DestroyContext(&m_context, nullptr);
            m_context = nullptr;
            if (chain)
                chain->Release();
            ReleaseReal();
            return E_FAIL;
        }
        // The caller owns this reference and releases it after Destroy.
        m_chain = chain;
        *swapChain = chain;
        // FFX always makes the real swap chain a waitable one.
        if (m_real)
            m_pacer.Attach(m_real->GetFrameLatencyWaitableObject());
        if (!m_pacer.Attached())
            Msg("! [FSR FG] the real swap chain is out of reach: vsync does not pace its presents");
        return S_OK;
    }

    void Destroy(IDXGISwapChain4* /*swapChain*/) override
    {
        if (!m_context)
            return;
        if (const ffxFunctions* ffx = da_ffx())
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR FG] swap chain context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
        m_chain = nullptr;
        ReleaseReal();
        if (m_factory)
        {
            m_factory->Release();
            m_factory = nullptr;
        }
    }

    // The render thread's vsync for the presents the callback paces (the interval CHW::Present uses).
    void BeforePresent(IDXGISwapChain4* /*swapChain*/) override
    {
        m_vsync.store(psDeviceFlags.test(rsVSync), std::memory_order_relaxed);
    }

    // From the present callback, right before FFX presents on the real swap chain.
    void PacePresent() { m_pacer.BeforePresent(m_vsync.load(std::memory_order_relaxed)); }

    // Presents FFX composed before the callback was set were not counted: once they have all left
    // the queue, counting starts afresh. Render thread, generation off.
    void ResyncPacing()
    {
        const ffxFunctions* ffx = da_ffx();
        if (!m_context || !ffx || !m_pacer.Attached())
            return;
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
        ffx->Dispatch(&m_context, &wait.header);
        m_pacer.Resync(100);
    }

    ffxContext* Context() { return m_context ? &m_context : nullptr; }
    IDXGISwapChain4* Chain() const { return m_chain; }
};
fsr_swapchain_provider g_fsr_swapchain;

D3D12_RESOURCE_STATES fsr_fg_state(uint32_t state)
{
    switch (state)
    {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    default: return D3D12_RESOURCE_STATE_COMMON; // COMMON and PRESENT
    }
}

// Called by the swap chain for every frame it presents, rendered or generated: FFX's own
// composition without a UI surface (none is registered, the UI is told apart through the hudless
// copy) - a copy into the real back buffer - and the vsync pacing of that present.
ffxReturnCode_t fsr_fg_present(ffxCallbackDescFrameGenerationPresent* params, void* context)
{
    auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto* source = static_cast<ID3D12Resource*>(params->currentBackBuffer.resource);
    auto* target = static_cast<ID3D12Resource*>(params->outputSwapChainBuffer.resource);
    if (!list || !source || !target)
        return FFX_API_RETURN_ERROR_PARAMETER;

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;
        D3D12_RESOURCE_BARRIER& barrier = barriers[count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
    };
    transition(source, fsr_fg_state(params->currentBackBuffer.state), D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(target, fsr_fg_state(params->outputSwapChainBuffer.state), D3D12_RESOURCE_STATE_COPY_DEST);
    if (count)
        list->ResourceBarrier(count, barriers);
    list->CopyResource(target, source);
    for (UINT i = 0; i < count; ++i)
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    if (count)
        list->ResourceBarrier(count, barriers);

    static_cast<fsr_swapchain_provider*>(context)->PacePresent();
    return FFX_API_RETURN_OK;
}

class fsr_framegen final : public IFrameGen
{
    ffxContext m_context{};
    u32 m_displayW{}, m_displayH{};
    u64 m_frameID{};
    bool m_active{};
    bool m_resetNext{};
    bool m_warned{};
    string64 m_name{ "AMD FSR FG" };

    // The swap chain learns whether to generate, what to call for it and where the UI is.
    bool Configure(bool enabled, ID3D12Resource* hudless)
    {
        const ffxFunctions* ffx = da_ffx();
        if (!ffx || !m_context)
            return false;
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = xrd3d12::NativeSwapChain(HW.m_pSwapChain);
        // Every present, generating or not, goes through the pacing composition; passing no callback
        // would restore FFX's default one, which does not pace.
        config.presentCallback = &fsr_fg_present;
        config.presentCallbackUserContext = &g_fsr_swapchain;
        config.frameGenerationCallback = enabled ? &fsr_fg_generate : nullptr;
        config.frameGenerationCallbackUserContext = enabled ? &m_context : nullptr;
        config.frameGenerationEnabled = enabled;
        config.allowAsyncWorkloads = false;
        if (hudless)
            config.HUDLessColor = ffxApiGetResourceDX12(hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        config.generationRect = { 0, 0, int32_t(m_displayW), int32_t(m_displayH) };
        config.frameID = m_frameID;
        const ffxReturnCode_t code = ffx->Configure(&m_context, &config.header);
        if (code != FFX_API_RETURN_OK && !m_warned)
        {
            Msg("! [FSR FG] configure failed (%u)", u32(code));
            m_warned = true;
        }
        return code == FFX_API_RETURN_OK;
    }

    // A frame that cannot be prepared must not be generated from stale input.
    void Fail(pcstr why)
    {
        if (!m_warned)
        {
            Msg("! [FSR FG] %s: the frame is shown without a generated one", why);
            m_warned = true;
        }
        if (m_active)
            Configure(false, nullptr);
        m_active = false;
        m_resetNext = true;
    }

public:
    ~fsr_framegen() override { Shutdown(); }

    pcstr Name() const override { return m_name; }

    xrd3d12::SwapChainProvider* Provider() override { return &g_fsr_swapchain; }

    bool Init(u32 displayW, u32 displayH, u32 generated) override
    {
        Shutdown();
        const ffxFunctions* ffx = da_ffx();
        ID3D12Device* device = fsr_fg_device();
        if (!ffx || !device || !displayW || !displayH)
            return false;
        // FSR generates one frame per rendered one.
        if (generated != 1)
        {
            Msg("! [FSR FG] %u generated frames asked, FSR makes one", generated);
            return false;
        }
        IDXGISwapChain4* chain = xrd3d12::NativeSwapChain(HW.m_pSwapChain);
        if (!chain || chain != g_fsr_swapchain.Chain())
        {
            Msg("! [FSR FG] the renderer does not present through FSR's swap chain");
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 chainDesc{};
        chain->GetDesc1(&chainDesc);

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;

        // Mandatory since SDK 2.1: the API version this code was written against.
        ffxCreateContextDescFrameGenerationVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_FRAMEGENERATION_VERSION;

        // LDR input, standard finite depth (0 = near), motion vectors at render size without the
        // jitter in them: no HDR, inverted, infinite or jitter-cancellation flags. The render size
        // never exceeds the display, so an upscaler change needs no new context.
        const bool debug = !!strstr(Core.Params, "-dxdebug");
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = &version.header;
        desc.flags = debug ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
        desc.displaySize = { displayW, displayH };
        desc.maxRenderSize = { displayW, displayH };
        desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(chainDesc.Format);

        const ffxReturnCode_t code = ffx->CreateContext(&m_context, &desc.header, nullptr);
        if (code != FFX_API_RETURN_OK || !m_context)
        {
            Msg("! [FSR FG] context creation failed (%u)", u32(code));
            m_context = nullptr;
            return false;
        }
        m_displayW = displayW;
        m_displayH = displayH;
        m_active = false;
        m_resetNext = true;
        m_warned = false;

        // The runtime's own messages: errors, and warnings under -dxdebug.
        ffxConfigureDescGlobalDebug1 messages{};
        messages.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        messages.fpMessage = &fsr_fg_message;
        messages.debugLevel = debug ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
        ffx->Configure(&m_context, &messages.header);

        // The swap chain starts with generation off and from here on presents through the callback.
        Configure(false, nullptr);
        g_fsr_swapchain.ResyncPacing();

        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx->Query(&m_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
            xr_sprintf(m_name, "AMD FSR FG %s", provider.versionName);
        return true;
    }

    void Shutdown() override
    {
        if (!m_context)
            return;
        const ffxFunctions* ffx = da_ffx();
        // The swap chain must stop calling into this context, and finish what it queued, first.
        Configure(false, nullptr);
        if (ffxContext* swapchain = g_fsr_swapchain.Context(); swapchain && ffx)
        {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
            ffx->Dispatch(swapchain, &wait.header);
        }
        // Recorded and in-flight work still references the context's resources.
        if (ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID))
            xrd3d12::Flush(imm, true);
        if (ffx)
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR FG] context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
        m_active = false;
        xr_strcpy(m_name, "AMD FSR FG");
    }

    void SetActive(bool active) override
    {
        if (m_active == active)
            return;
        m_active = active;
        if (active)
            m_resetNext = true;
        // Off at once: the next Present shows the rendered frame alone. On waits for Prepare.
        else
            Configure(false, nullptr);
    }

    void Prepare(const FrameGenFrame& frame) override
    {
        const ffxFunctions* ffx = da_ffx();
        if (!m_context || !ffx || !m_active)
            return;
        ID3D12Resource* depth = xrd3d12::NativeResource(frame.depth);
        ID3D12Resource* velocity = xrd3d12::NativeResource(frame.velocity);
        ID3D12Resource* hudless = xrd3d12::NativeResource(frame.hudless);
        if (!depth || !velocity || !hudless)
        {
            Fail("an input is missing");
            return;
        }
        if (frame.displayW != m_displayW || frame.displayH != m_displayH || frame.renderW > m_displayW ||
            frame.renderH > m_displayH)
        {
            Fail("the frame does not match the context");
            return;
        }

        // Configure first, then prepare: the order FSR 4 requires (fine for 3.1). The id steps by
        // exactly one per frame; anything else resets the generation.
        m_frameID = frame.frameIndex;
        if (!Configure(true, hudless))
        {
            Fail("configure failed");
            return;
        }

        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(imm);
        if (!list)
        {
            Fail("no command list");
            return;
        }
        // The layer tracks the states; FSR returns every resource to the state it was given. The
        // hudless copy stays there until the generation reads it inside Present.
        constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        xrd3d12::Require(imm, frame.depth, read);
        xrd3d12::Require(imm, frame.velocity, read);
        xrd3d12::Require(imm, frame.hudless, read);

        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID = m_frameID;
        prepare.flags = 0;
        prepare.commandList = list;
        prepare.renderSize = { frame.renderW, frame.renderH };
        // Render pixels, y down, the offset the projection carried (AMD's convention).
        prepare.jitterOffset = { frame.jitterX, frame.jitterY };
        // Stored NDC(cur) - NDC(prev), y up; FSR wants prev - cur in render pixels, y down.
        prepare.motionVectorScale = { -0.5f * float(frame.renderW), 0.5f * float(frame.renderH) };
        prepare.frameTimeDelta = std::max(frame.frameTimeMs, 0.1f);
        prepare.reset = frame.reset || m_resetNext;
        prepare.cameraNear = frame.nearZ;
        prepare.cameraFar = frame.farZ;
        prepare.cameraFovAngleVertical = frame.fovY;
        prepare.viewSpaceToMetersFactor = 1.f;
        prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prepare.motionVectors = ffxApiGetResourceDX12(velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        const auto put = [](float (&to)[3], const Fvector& from)
        {
            to[0] = from.x;
            to[1] = from.y;
            to[2] = from.z;
        };
        put(prepare.cameraPosition, frame.cameraPosition);
        put(prepare.cameraUp, frame.cameraUp);
        put(prepare.cameraRight, frame.cameraRight);
        put(prepare.cameraForward, frame.cameraForward);

        const ffxReturnCode_t code = ffx->Dispatch(&m_context, &prepare.header);
        // FSR set its own heaps, root signature and pipeline: the layer re-applies the engine's.
        xrd3d12::EndNative(imm);
        if (code != FFX_API_RETURN_OK)
        {
            Fail("prepare failed");
            return;
        }
        m_resetNext = false;
    }
};

IFrameGen* fsr_fg_create() { return xr_new<fsr_framegen>(); }

// The loader has a frame generation provider for this adapter (every DX12 GPU with typed UAV
// loads; FSR FG 4 on RX 9000, 3.1 elsewhere).
bool fsr_fg_supported()
{
    return da_ffx_providers(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, fsr_fg_device(), "frame generation") > 0;
}

const da_framegen_registrar s_fsr_fg{ { 1, "AMD FSR FG", &fsr_fg_create, &fsr_fg_supported, nullptr } };
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_D3D12
