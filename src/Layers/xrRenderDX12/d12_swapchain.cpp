// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Swap chain of xrD3D12: a flip-model DXGI swap chain on the device's direct queue behind the
// IDXGISwapChain1 the engine holds. The engine only ever sees one back buffer (GetBuffer(0), as
// with D3D11 flip chains); it is a proxy texture that resolves to the buffer DXGI hands out next.
// Frame generation replaces the native chain through a SwapChainProvider; the wrapper stays.
#include "d12_layer.h"

#include <algorithm>
#include <new>

namespace xrd3d12
{
namespace
{
// Flags DXGI refuses to add or remove through ResizeBuffers: they stay as the chain was created.
constexpr UINT kCreationFixedFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER;

// The flip model rejects sRGB buffer formats; sRGB views of the UNORM buffer are the replacement.
DXGI_FORMAT FlipFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

// Three at least. On a D3D12 queue DXGI holds every submission after a Present until the next
// back buffer leaves the screen - the whole next frame, not only its last pass as a D3D11 driver
// does. With two buffers under vsync that rounds each frame up to whole refresh intervals: 12.5 ms
// of GPU work shown every 18.2 ms at 165 Hz, a third of the frame rate gone.
UINT FlipBufferCount(UINT count)
{
    return std::clamp<UINT>(count, 3, DXGI_MAX_SWAP_CHAIN_BUFFERS);
}

bool TearingSupported(IDXGIFactory2* factory)
{
    IDXGIFactory5* factory5 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        return false;
    BOOL allow = FALSE;
    const bool supported = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;
    factory5->Release();
    return supported;
}

// Maps whatever the engine asked for onto a description a D3D12 flip-model HWND chain accepts.
DXGI_SWAP_CHAIN_DESC1 FlipModelDesc(const DXGI_SWAP_CHAIN_DESC1& requested, bool tearingSupported)
{
    DXGI_SWAP_CHAIN_DESC1 desc = requested;
    if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.BufferCount = FlipBufferCount(desc.BufferCount);
    desc.SampleDesc = {1, 0};
    desc.BufferUsage |= DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.Format = FlipFormat(desc.Format);
    // ASPECT_RATIO_STRETCH exists for composition chains only; HWND chains know no alpha modes.
    if (desc.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH)
        desc.Scaling = DXGI_SCALING_STRETCH;
    if (desc.AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED && desc.AlphaMode != DXGI_ALPHA_MODE_IGNORE)
        desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE); // not applicable to D3D12
    if (!tearingSupported)
        desc.Flags &= ~UINT(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    return desc;
}

// Takes what DXGI (or a provider) actually made: zero sizes become the window size, and a provider
// may pick its own buffer count.
void ReadActualDesc(IDXGISwapChain1* native, DXGI_SWAP_CHAIN_DESC1& desc)
{
    DXGI_SWAP_CHAIN_DESC1 actual{};
    if (FAILED(native->GetDesc1(&actual)) || !actual.BufferCount)
        return;
    desc.Width = actual.Width;
    desc.Height = actual.Height;
    desc.Format = actual.Format;
    desc.BufferCount = actual.BufferCount;
    desc.Flags = actual.Flags;
}

// Goes exclusive fullscreen. Must run while no buffer is held: a flip chain realises the
// transition only through ResizeBuffers.
void EnterFullscreen(IDXGISwapChain4* native, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC& fullscreen, IDXGIOutput* target)
{
    HRESULT hr = native->SetFullscreenState(TRUE, target);
    if (FAILED(hr))
    {
        Log("swap chain: exclusive fullscreen refused (0x%08X), staying windowed", unsigned(hr));
        return;
    }
    DXGI_MODE_DESC mode{};
    mode.Width = desc.Width;
    mode.Height = desc.Height;
    mode.RefreshRate = fullscreen.RefreshRate;
    mode.Format = desc.Format;
    mode.ScanlineOrdering = fullscreen.ScanlineOrdering;
    mode.Scaling = fullscreen.Scaling;
    hr = native->ResizeTarget(&mode);
    if (FAILED(hr))
        Log("swap chain: ResizeTarget %ux%u failed (0x%08X)", desc.Width, desc.Height, unsigned(hr));
    hr = native->ResizeBuffers(0, desc.Width, desc.Height, DXGI_FORMAT_UNKNOWN, desc.Flags);
    if (FAILED(hr))
        Log("swap chain: ResizeBuffers after the fullscreen transition failed (0x%08X)", unsigned(hr));
}

bool IsFullscreen(IDXGISwapChain* native)
{
    BOOL fullscreen = FALSE;
    return SUCCEEDED(native->GetFullscreenState(&fullscreen, nullptr)) && fullscreen;
}

ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

// Submits the immediate context and waits until the GPU is idle.
void Idle(Device* device)
{
    if (device->immediate)
        device->immediate->Submit();
    device->WaitIdle();
}

// Idle, then runs every deferred release queued so far: views the engine released drop their
// back buffer references, and released buffer wrappers really let go of the DXGI buffers.
void Drain(Device* device)
{
    Idle(device);
    device->fence.Wait(device->fence.Signal(device->queue));
    device->releaser.Collect(false);
}

UINT BindFlagsOf(DXGI_USAGE usage)
{
    UINT bind = 0;
    if (usage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
        bind |= D3D11_BIND_RENDER_TARGET;
    if (usage & DXGI_USAGE_SHADER_INPUT)
        bind |= D3D11_BIND_SHADER_RESOURCE;
    if (usage & DXGI_USAGE_UNORDERED_ACCESS)
        bind |= D3D11_BIND_UNORDERED_ACCESS;
    return bind;
}
} // namespace

// ============================================================================================
// Creation and lifetime
// ============================================================================================

HRESULT SwapChain::Create(Device* device, IDXGIFactory2* factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, SwapChain** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!device || !factory || !hwnd)
        return DXGI_ERROR_INVALID_CALL;

    SwapChain* chain = new (std::nothrow) SwapChain();
    if (!chain)
        return E_OUTOFMEMORY;
    chain->m_device = device;
    device->AddRef();
    chain->m_factory = factory;
    factory->AddRef();
    chain->m_hwnd = hwnd;
    chain->m_desc = FlipModelDesc(desc, TearingSupported(factory));
    if (desc.SwapEffect != chain->m_desc.SwapEffect || desc.BufferCount != chain->m_desc.BufferCount)
        Log("swap chain: swap effect %u x%u buffers mapped to flip model x%u", unsigned(desc.SwapEffect),
            desc.BufferCount, chain->m_desc.BufferCount);
    if (fullscreen)
    {
        chain->m_fullscreenDesc = *fullscreen;
        chain->m_hasFullscreenDesc = true;
    }

    HRESULT hr = chain->CreateNative();
    if (SUCCEEDED(hr))
    {
        ReadActualDesc(chain->m_native, chain->m_desc);
        chain->AttachPacer();
        // Flip chains are born windowed and then taken to fullscreen (DXGI's recommendation).
        if (fullscreen && !fullscreen->Windowed)
            EnterFullscreen(chain->m_native, chain->m_desc, chain->m_fullscreenDesc, nullptr);
        try
        {
            chain->AcquireBuffers();
            if (chain->m_buffers.empty() || !chain->m_proxy)
                hr = E_FAIL;
        }
        catch (const std::bad_alloc&)
        {
            hr = E_OUTOFMEMORY;
        }
    }
    if (FAILED(hr))
    {
        Log("swap chain: creation failed (0x%08X)", unsigned(hr));
        chain->Release();
        return hr;
    }

    Log("swap chain: %ux%u, format %u, %u buffers, %s%s%s", chain->m_desc.Width, chain->m_desc.Height,
        unsigned(chain->m_desc.Format), chain->BufferCount(),
        chain->m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip discard" : "flip sequential",
        (chain->m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? ", tearing allowed" : "",
        chain->m_pacer.Attached() ? ", vsync paced" : "");
    *out = chain;
    return S_OK;
}

HRESULT SwapChain::CreateNative()
{
    // Always created windowed; exclusive fullscreen is entered afterwards (EnterFullscreen).
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen = m_fullscreenDesc;
    fullscreen.Windowed = TRUE;
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenPtr = m_hasFullscreenDesc ? &fullscreen : nullptr;

    // Waitable for the vsync pacing (d12_pacing.h). A provider that paces its presents itself strips
    // the flag from its chain; ReadActualDesc then takes what the chain really has.
    m_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (m_provider)
    {
        IDXGISwapChain4* native = nullptr;
        const HRESULT hr = m_provider->Create(m_factory, m_device->queue, m_hwnd, m_desc, fullscreenPtr, &native);
        if (FAILED(hr) || !native)
        {
            if (native)
                native->Release();
            return FAILED(hr) ? hr : E_FAIL;
        }
        m_native = native;
        return S_OK;
    }

    IDXGISwapChain1* chain = nullptr;
    HRESULT hr = m_factory->CreateSwapChainForHwnd(m_device->queue, m_hwnd, &m_desc, fullscreenPtr, nullptr, &chain);
    if (FAILED(hr))
        return hr;
    hr = chain->QueryInterface(IID_PPV_ARGS(&m_native));
    chain->Release();
    return hr;
}

void SwapChain::AttachPacer()
{
    const bool waitable = m_native && (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    m_pacer.Attach(waitable ? m_native->GetFrameLatencyWaitableObject() : nullptr);
}

void SwapChain::AcquireBuffers()
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_desc.Width;
    desc.Height = m_desc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = m_desc.Format;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = BindFlagsOf(m_desc.BufferUsage);

    m_buffers.reserve(m_desc.BufferCount);
    for (UINT i = 0; i < m_desc.BufferCount; ++i)
    {
        ID3D12Resource* resource = nullptr;
        const HRESULT hr = m_native->GetBuffer(i, IID_PPV_ARGS(&resource));
        if (FAILED(hr))
        {
            Log("swap chain: GetBuffer(%u) failed (0x%08X)", i, unsigned(hr));
            break;
        }
        if (!i)
        {
            const D3D12_RESOURCE_DESC native = resource->GetDesc();
            desc.Width = UINT(native.Width);
            desc.Height = native.Height;
            desc.Format = native.Format;
        }
        // The wrapper keeps its own reference to the buffer.
        Texture2D* texture = Texture2D::CreateForSwapChain(m_device, this, resource, desc);
        resource->Release();
        if (!texture)
        {
            Log("swap chain: cannot wrap buffer %u", i);
            break;
        }
        m_buffers.push_back(texture);
    }

    // The proxy lives as long as its description holds; a new size or format needs a new one,
    // which is safe here because nobody else may reference it (ResizeBuffers/Recreate check that).
    if (m_proxy)
    {
        const D3D11_TEXTURE2D_DESC& old = m_proxy->Desc();
        if (old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format || old.BindFlags != desc.BindFlags)
        {
            m_proxy->Release();
            m_proxy = nullptr;
        }
    }
    if (!m_proxy)
    {
        m_proxy = Texture2D::CreateForSwapChain(m_device, this, nullptr, desc);
        if (!m_proxy)
            Log("swap chain: cannot create the back buffer proxy");
    }

    const u32 current = m_native->GetCurrentBackBufferIndex();
    m_current = current < m_buffers.size() ? current : 0;
    m_device->swapChain = this;
}

void SwapChain::ReleaseBuffers()
{
    for (Texture2D* buffer : m_buffers)
        buffer->Release();
    m_buffers.clear();
    m_current = 0;
}

HRESULT SwapChain::Recreate(SwapChainProvider* provider)
{
    if (!m_native)
        return DXGI_ERROR_INVALID_CALL;
    try
    {
        Drain(m_device);
        if (m_proxy && RefCount(m_proxy) > 1)
        {
            Log("swap chain: recreate refused, the back buffer is still referenced (%u)", unsigned(RefCount(m_proxy) - 1));
            return DXGI_ERROR_INVALID_CALL;
        }

        BOOL fullscreen = FALSE;
        IDXGIOutput* output = nullptr;
        if (FAILED(m_native->GetFullscreenState(&fullscreen, &output)))
            fullscreen = FALSE;
        if (fullscreen)
            m_native->SetFullscreenState(FALSE, nullptr);

        ReleaseBuffers();
        Drain(m_device);
        m_pacer.Detach();
        if (m_provider)
            m_provider->Destroy(m_native);
        m_native->Release();
        m_native = nullptr;

        m_provider = provider;
        HRESULT hr = CreateNative();
        if (FAILED(hr) && provider)
        {
            // Keep a working chain: the caller learns from the result that the provider failed.
            Log("swap chain: provider failed to create its swap chain (0x%08X), using a plain one", unsigned(hr));
            m_provider = nullptr;
            const HRESULT plain = CreateNative();
            if (FAILED(plain))
                hr = plain;
        }
        if (m_native)
        {
            ReadActualDesc(m_native, m_desc);
            AttachPacer();
            if (fullscreen)
                EnterFullscreen(m_native, m_desc, m_fullscreenDesc, output);
        }
        if (output)
            output->Release();
        if (!m_native)
        {
            Log("swap chain: recreation failed (0x%08X)", unsigned(hr));
            return hr;
        }

        AcquireBuffers();
        if (m_buffers.empty() || !m_proxy)
            return FAILED(hr) ? hr : E_FAIL;
        Log("swap chain: recreated %s, %ux%u, %u buffers%s", m_provider ? "through a provider" : "plain",
            m_desc.Width, m_desc.Height, BufferCount(), m_pacer.Attached() ? ", vsync paced" : "");
        return hr;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

// ============================================================================================
// IUnknown / IDXGIObject / IDXGIDeviceSubObject
// ============================================================================================

HRESULT STDMETHODCALLTYPE SwapChain::QueryInterface(REFIID riid, void** object)
{
    if (!object)
        return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1))
    {
        *object = static_cast<IDXGISwapChain1*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SwapChain::AddRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE SwapChain::Release()
{
    const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs)
        return refs;

    Device* device = m_device;
    try
    {
        if (device)
            Idle(device);
        // DXGI cannot release a chain that holds the output.
        if (m_native && IsFullscreen(m_native))
            m_native->SetFullscreenState(FALSE, nullptr);
        ReleaseBuffers();
        if (m_proxy)
        {
            const ULONG proxyRefs = RefCount(m_proxy);
            if (proxyRefs > 1)
                Log("swap chain: released while the engine still holds the back buffer (%u)", unsigned(proxyRefs - 1));
            m_proxy->Release();
            m_proxy = nullptr;
        }
        if (device)
            Drain(device);
    }
    catch (const std::bad_alloc&)
    {
        Log("swap chain: out of memory during release");
    }
    m_pacer.Detach();
    if (m_native)
    {
        if (m_provider)
            m_provider->Destroy(m_native);
        m_native->Release();
    }
    if (m_factory)
        m_factory->Release();
    if (device && device->swapChain == this)
        device->swapChain = nullptr;
    delete this;
    if (device)
        device->Release();
    return 0;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateData(REFGUID guid, UINT size, const void* data)
{
    try
    {
        return m_private.Set(guid, size, data);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateDataInterface(REFGUID guid, const IUnknown* data)
{
    try
    {
        return m_private.SetInterface(guid, data);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::GetPrivateData(REFGUID guid, UINT* size, void* data)
{
    try
    {
        return m_private.Get(guid, size, data);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::GetParent(REFIID riid, void** parent)
{
    if (!parent)
        return E_POINTER;
    *parent = nullptr;
    return m_factory->QueryInterface(riid, parent);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDevice(REFIID riid, void** device)
{
    if (!device)
        return E_POINTER;
    *device = nullptr;
    return m_device->QueryInterface(riid, device);
}

// ============================================================================================
// IDXGISwapChain / IDXGISwapChain1
// ============================================================================================

HRESULT STDMETHODCALLTYPE SwapChain::Present(UINT syncInterval, UINT flags)
{
    return Present1(syncInterval, flags, nullptr);
}

HRESULT STDMETHODCALLTYPE SwapChain::Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    if (!m_native)
        return DXGI_ERROR_INVALID_CALL;
    try
    {
        // Tearing is legal only for chains created with it, with interval 0 and while windowed
        // (DXGI may have gone exclusive on its own through Alt+Enter).
        if ((flags & DXGI_PRESENT_ALLOW_TEARING) &&
            (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) || syncInterval || IsFullscreen(m_native)))
            flags &= ~UINT(DXGI_PRESENT_ALLOW_TEARING);

        const auto presentNative = [&] {
            return params ? m_native->Present1(syncInterval, flags, params) : m_native->Present(syncInterval, flags);
        };
        // An occlusion test presents nothing: no submission, no frame boundary.
        if (flags & DXGI_PRESENT_TEST)
            return presentNative();

        Device* device = m_device;
        Context* immediate = device->immediate;
        u64 fenceValue = 0;
        if (immediate)
        {
            // Every async job is joined before the frame's final signal.
            device->AsyncFrameBoundary();
            if (!m_buffers.empty())
                immediate->RequireAll(CurrentBuffer(), D3D12_RESOURCE_STATE_PRESENT);
            if (!immediate->barriers.empty())
            {
                immediate->List();
                immediate->FlushBarriers();
            }
            fenceValue = immediate->Submit();
        }
        if (!fenceValue)
            fenceValue = device->fence.Signal(device->queue);

        // One present per refresh under vsync; the frame's GPU work is already submitted.
        m_pacer.BeforePresent(syncInterval > 0);
        if (m_provider)
            m_provider->BeforePresent(m_native);
        const HRESULT hr = presentNative();
        device->EndFrame(fenceValue);

        const u32 current = m_native->GetCurrentBackBufferIndex();
        m_current = current < m_buffers.size() ? current : 0;
        // Bound back buffer views now resolve to another buffer.
        if (immediate)
            immediate->InvalidateApplied();
        ++m_presentCount;

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            device->ReportRemoval(device->d12->GetDeviceRemovedReason());
        return hr;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBuffer(UINT buffer, REFIID riid, void** surface)
{
    if (!surface)
        return DXGI_ERROR_INVALID_CALL;
    *surface = nullptr;
    if (!m_proxy)
        return DXGI_ERROR_INVALID_CALL;
    // D3D11 flip chains expose buffer 0 only, and it always means the current back buffer.
    if (buffer)
    {
        static const char message[] = "swap chain: GetBuffer(%u) answered with the current back buffer";
        LogOnce(message, message, buffer);
    }
    return m_proxy->QueryInterface(riid, surface);
}

HRESULT STDMETHODCALLTYPE SwapChain::SetFullscreenState(BOOL fullscreen, IDXGIOutput* target)
{
    if (!m_native)
        return DXGI_ERROR_INVALID_CALL;
    try
    {
        Idle(m_device);
        return m_native->SetFullscreenState(fullscreen, target);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target)
{
    return m_native ? m_native->GetFullscreenState(fullscreen, target) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* desc)
{
    if (!desc)
        return E_INVALIDARG;
    if (!m_native)
        return DXGI_ERROR_INVALID_CALL;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    if (FAILED(m_native->GetFullscreenDesc(&fullscreen)))
        fullscreen = m_fullscreenDesc;

    *desc = {};
    desc->BufferDesc.Width = m_desc.Width;
    desc->BufferDesc.Height = m_desc.Height;
    desc->BufferDesc.RefreshRate = fullscreen.RefreshRate;
    desc->BufferDesc.Format = m_desc.Format;
    desc->BufferDesc.ScanlineOrdering = fullscreen.ScanlineOrdering;
    desc->BufferDesc.Scaling = fullscreen.Scaling;
    desc->SampleDesc = m_desc.SampleDesc;
    desc->BufferUsage = m_desc.BufferUsage;
    desc->BufferCount = m_desc.BufferCount;
    desc->OutputWindow = m_hwnd;
    desc->Windowed = !IsFullscreen(m_native);
    desc->SwapEffect = m_desc.SwapEffect;
    desc->Flags = m_desc.Flags;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (!m_native)
        return DXGI_ERROR_INVALID_CALL;
    try
    {
        // Deferred view releases must have run before the reference check, as D3D11 has none.
        Drain(m_device);
        if (m_proxy && RefCount(m_proxy) > 1)
        {
            Log("swap chain: ResizeBuffers refused, the back buffer is still referenced (%u)", unsigned(RefCount(m_proxy) - 1));
            return DXGI_ERROR_INVALID_CALL;
        }

        ReleaseBuffers();
        Drain(m_device);

        const UINT nativeCount = count ? FlipBufferCount(count) : 0; // 0 keeps the current count
        const UINT nativeFlags = (flags & ~(kCreationFixedFlags | UINT(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE))) |
            (m_desc.Flags & kCreationFixedFlags);
        const HRESULT hr = m_native->ResizeBuffers(nativeCount, width, height, FlipFormat(format), nativeFlags);
        if (FAILED(hr))
            Log("swap chain: ResizeBuffers(%u, %ux%u, format %u, flags 0x%X) failed (0x%08X)", nativeCount, width, height,
                unsigned(format), nativeFlags, unsigned(hr));
        else
            ReadActualDesc(m_native, m_desc);

        // On failure DXGI keeps the old buffers: take them back so the chain stays usable.
        AcquireBuffers();
        if (FAILED(hr))
            return hr;
        return m_buffers.empty() || !m_proxy ? E_FAIL : S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeTarget(const DXGI_MODE_DESC* params)
{
    return m_native ? m_native->ResizeTarget(params) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetContainingOutput(IDXGIOutput** output)
{
    return m_native ? m_native->GetContainingOutput(output) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* stats)
{
    return m_native ? m_native->GetFrameStatistics(stats) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetLastPresentCount(UINT* count)
{
    if (!count)
        return E_INVALIDARG;
    *count = m_presentCount;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (!desc)
        return E_INVALIDARG;
    *desc = m_desc;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc)
{
    if (!desc)
        return E_INVALIDARG;
    if (m_native && SUCCEEDED(m_native->GetFullscreenDesc(desc)))
        return S_OK;
    *desc = m_fullscreenDesc;
    desc->Windowed = !m_native || !IsFullscreen(m_native);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetHwnd(HWND* hwnd)
{
    if (!hwnd)
        return E_INVALIDARG;
    *hwnd = m_hwnd;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetCoreWindow(REFIID /*riid*/, void** unk)
{
    if (unk)
        *unk = nullptr;
    return DXGI_ERROR_INVALID_CALL; // an HWND chain
}

BOOL STDMETHODCALLTYPE SwapChain::IsTemporaryMonoSupported()
{
    return FALSE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRestrictToOutput(IDXGIOutput** output)
{
    return m_native ? m_native->GetRestrictToOutput(output) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetBackgroundColor(const DXGI_RGBA* color)
{
    return m_native ? m_native->SetBackgroundColor(color) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBackgroundColor(DXGI_RGBA* color)
{
    return m_native ? m_native->GetBackgroundColor(color) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetRotation(DXGI_MODE_ROTATION rotation)
{
    return m_native ? m_native->SetRotation(rotation) : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRotation(DXGI_MODE_ROTATION* rotation)
{
    return m_native ? m_native->GetRotation(rotation) : DXGI_ERROR_INVALID_CALL;
}

// ============================================================================================
// Public API (d12_api.h)
// ============================================================================================

HRESULT CreateSwapChainForHwnd(ID3D11Device* device, IDXGIFactory2* factory, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
    IDXGISwapChain1** swapChain)
{
    if (!swapChain)
        return E_POINTER;
    *swapChain = nullptr;
    if (!device || !factory || !desc || !IsLayerDevice(device))
        return DXGI_ERROR_INVALID_CALL;
    SwapChain* chain = nullptr;
    const HRESULT hr = SwapChain::Create(Device::From(device), factory, hwnd, *desc, fullscreen, &chain);
    if (SUCCEEDED(hr))
        *swapChain = chain;
    return hr;
}

HRESULT RecreateSwapChain(IDXGISwapChain* swapChain, SwapChainProvider* provider)
{
    return swapChain ? static_cast<SwapChain*>(swapChain)->Recreate(provider) : DXGI_ERROR_INVALID_CALL;
}

IDXGISwapChain4* NativeSwapChain(IDXGISwapChain* swapChain)
{
    return swapChain ? static_cast<SwapChain*>(swapChain)->Native() : nullptr;
}

ID3D12Resource* NativeResource(ID3D11Resource* resource)
{
    if (!resource)
        return nullptr;
    Resource* layerResource = Resource::From(resource);
    return layerResource ? layerResource->ActualNative() : nullptr;
}

// The old native API (SDK integrations): a section that re-applies everything afterwards.
ID3D12GraphicsCommandList* BeginNative(ID3D11DeviceContext* immediate)
{
    NativeScope scope;
    return BeginNative(immediate, scope) ? scope.list : nullptr;
}

void EndNative(ID3D11DeviceContext* immediate)
{
    EndNative(immediate, TouchedAll);
}
} // namespace xrd3d12
