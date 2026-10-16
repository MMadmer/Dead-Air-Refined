// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Swap chain: IDXGISwapChain1 over VkSwapchainKHR. GetBuffer(0) is a texture of the layer the frame renders
// into; Present submits the frame first, then acquires an image and copies the back buffer into it. Only that
// copy waits for the display: a semaphore wait covers every command of its submission at its stages, so a
// frame recorded after a lazy acquire stalled whole until vsync freed an image. Three images under vsync
// (two cost DX12 a frame of throughput), the present mode switches without a new swap chain where
// VK_EXT_swapchain_maintenance1 allows it, and with present_wait a vsync'd present is queued only after the
// previous one reached the display, or the one before it while frames come slower than the refresh (the pacing
// of every API, ../xrRender/present_pacing.h).
#include "vk_layer.h"

#include <algorithm>
#include <cstring>

namespace xrvk
{
namespace
{
constexpr u64 kPresentWaitNs = 100ull * 1000 * 1000;

VkFormat SurfaceFormatFor(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

VkFormat SrgbOf(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// The bytes of an sRGB format are those of its UNORM twin: a copy between the two keeps the colours.
VkFormat UnormOf(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}
} // namespace

HRESULT SwapChain::Create(Device* device, IDXGIFactory2* factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1& desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, SwapChain** out)
{
    *out = nullptr;
    auto* chain = new (std::nothrow) SwapChain();
    if (!chain)
        return E_OUTOFMEMORY;
    chain->m_device = device;
    device->AddRef();
    chain->m_factory = factory;
    if (factory)
        factory->AddRef();
    chain->m_hwnd = hwnd;
    chain->m_desc = desc;
    // Only the first buffer is ever rendered into; the rest is the layer's business.
    if (!chain->m_desc.BufferCount)
        chain->m_desc.BufferCount = 2;
    if (fullscreen)
    {
        chain->m_fullscreenDesc = *fullscreen;
        chain->m_hasFullscreenDesc = true;
        chain->m_fullscreen = !fullscreen->Windowed;
    }
    VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface.hinstance = GetModuleHandleW(nullptr);
    surface.hwnd = hwnd;
    VkResult result = g_vk.vkCreateWin32SurfaceKHR(device->instance, &surface, nullptr, &chain->m_surface);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateWin32SurfaceKHR failed: %s", ResultText(result));
        chain->Release();
        return E_FAIL;
    }
    VkBool32 supported = VK_FALSE;
    g_vk.vkGetPhysicalDeviceSurfaceSupportKHR(device->physical, device->graphicsFamily, chain->m_surface, &supported);
    if (!supported)
    {
        Log("! the graphics queue cannot present to the window");
        chain->Release();
        return E_FAIL;
    }
    for (VkSemaphore& s : chain->m_acquire)
    {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (g_vk.vkCreateSemaphore(device->vk, &info, nullptr, &s) != VK_SUCCESS)
        {
            chain->Release();
            return E_FAIL;
        }
    }
    HRESULT hr = chain->CreateNative(1);
    if (SUCCEEDED(hr))
        hr = chain->CreateBackBuffer();
    if (FAILED(hr))
    {
        chain->Release();
        return hr;
    }
    *out = chain;
    return S_OK;
}

HRESULT SwapChain::CreateBackBuffer()
{
    if (m_backBuffer)
    {
        m_backBuffer->Release();
        m_backBuffer = nullptr;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_desc.Width;
    td.Height = m_desc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = m_desc.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
        td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &texture);
    if (FAILED(hr))
    {
        Log("! the back buffer (%ux%u, format %u) could not be created", m_desc.Width, m_desc.Height, unsigned(m_desc.Format));
        return hr;
    }
    m_backBuffer = static_cast<Texture2D*>(texture);
    return S_OK;
}

HRESULT SwapChain::CreateNative(UINT syncInterval)
{
    Device* dev = m_device;
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = g_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, m_surface, &caps);
    if (result != VK_SUCCESS)
        return E_FAIL;
    // A minimized window has no surface to present to: presents report it occluded until it is back.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
    {
        m_minimized = true;
        return S_OK;
    }
    m_minimized = false;

    u32 count = 0;
    g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, m_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    g_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, m_surface, &count, formats.data());
    const VkFormat wanted = SurfaceFormatFor(m_desc.Format);
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const VkSurfaceFormatKHR& f : formats)
    {
        if (f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = f;
    }
    // RGBA and BGRA hold the same colours: the image's own format is what views use.
    if (chosen.format == VK_FORMAT_UNDEFINED)
    {
        for (const VkSurfaceFormatKHR& f : formats)
        {
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                chosen = f;
                break;
            }
        }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED && !formats.empty())
        chosen = formats[0];
    m_vkFormat = chosen.format;

    count = 0;
    g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, m_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    g_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, m_surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    // Vsync off: tearing where the panel allows it (what DXGI_PRESENT_ALLOW_TEARING asked for), else the
    // newest frame without tearing.
    m_tearingMode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR :
        has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    m_relaxedSupported = has(VK_PRESENT_MODE_FIFO_RELAXED_KHR);
    m_vsyncMode = m_adaptive && m_relaxedSupported ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
    m_mode = syncInterval ? m_vsyncMode : m_tearingMode;
    m_syncInterval = syncInterval;

    u32 images = (std::max)(3u, caps.minImageCount);
    if (caps.maxImageCount)
        images = (std::min)(images, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = m_surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{m_desc.Width, m_desc.Height};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageUsage |= caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
                                                                                            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = m_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = m_swapchain;
    // sRGB views of the back buffer need a mutable swap chain.
    VkFormat viewFormats[2] = {chosen.format, SrgbOf(chosen.format)};
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (dev->mutableSwapchain && viewFormats[1] != VK_FORMAT_UNDEFINED)
    {
        info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        formatList.viewFormatCount = 2;
        formatList.pViewFormats = viewFormats;
        formatList.pNext = info.pNext;
        info.pNext = &formatList;
    }
    // Both present modes up front: vsync switches per present without a new swap chain.
    VkPresentModeKHR switchable[2] = {m_vsyncMode, m_tearingMode};
    VkSwapchainPresentModesCreateInfoEXT modesInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT};
    m_switchModes = false;
    if (dev->swapchainMaintenance && m_tearingMode != m_vsyncMode)
    {
        VkSurfacePresentModeEXT surfaceMode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};
        surfaceMode.presentMode = m_mode;
        VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, &surfaceMode};
        surfaceInfo.surface = m_surface;
        VkPresentModeKHR compatible[8]{};
        VkSurfacePresentModeCompatibilityEXT compat{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT};
        compat.presentModeCount = 8;
        compat.pPresentModes = compatible;
        VkSurfaceCapabilities2KHR caps2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &compat};
        if (g_vk.vkGetPhysicalDeviceSurfaceCapabilities2KHR &&
            g_vk.vkGetPhysicalDeviceSurfaceCapabilities2KHR(dev->physical, &surfaceInfo, &caps2) == VK_SUCCESS)
        {
            const VkPresentModeKHR other = m_mode == m_vsyncMode ? m_tearingMode : m_vsyncMode;
            for (u32 i = 0; i < compat.presentModeCount; ++i)
                m_switchModes |= compatible[i] == other;
        }
        if (m_switchModes)
        {
            modesInfo.presentModeCount = 2;
            modesInfo.pPresentModes = switchable;
            modesInfo.pNext = info.pNext;
            info.pNext = &modesInfo;
        }
    }
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (m_provider)
    {
        // A provider takes the plain description (no per-present modes, no sRGB views) and holds one chain
        // at a time: the old one goes first.
        info.flags &= ~VkSwapchainCreateFlagsKHR(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR);
        info.pNext = nullptr;
        m_switchModes = false;
        DestroyNative();
        info.oldSwapchain = VK_NULL_HANDLE;
        result = VkResult(m_provider->Create(&info, &created));
    }
    else
    {
        result = g_vk.vkCreateSwapchainKHR(dev->vk, &info, nullptr, &created);
        if (result != VK_SUCCESS && (info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR))
        {
            info.flags &= ~VkSwapchainCreateFlagsKHR(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR);
            info.pNext = m_switchModes ? &modesInfo : nullptr;
            modesInfo.pNext = nullptr;
            result = g_vk.vkCreateSwapchainKHR(dev->vk, &info, nullptr, &created);
        }
    }
    if (result != VK_SUCCESS || !created)
    {
        Log("! %s failed: %s", m_provider ? "the swap chain provider" : "vkCreateSwapchainKHR", ResultText(result));
        return E_FAIL;
    }
    // The old chain is retired: its images go with the wrappers.
    DestroyNative();
    m_swapchain = created;
    m_chainProvider = m_provider;
    m_chainFirstId = m_presentId + 1;
    m_clock.Reset();
    m_mutable = (info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0;
    m_extent = info.imageExtent;

    count = 0;
    std::vector<VkImage> handles;
    if (m_chainProvider)
    {
        m_chainProvider->GetImages(m_swapchain, &count, nullptr);
        handles.resize(count);
        m_chainProvider->GetImages(m_swapchain, &count, handles.data());
    }
    else
    {
        g_vk.vkGetSwapchainImagesKHR(dev->vk, m_swapchain, &count, nullptr);
        handles.resize(count);
        g_vk.vkGetSwapchainImagesKHR(dev->vk, m_swapchain, &count, handles.data());
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_extent.width;
    td.Height = m_extent.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = m_desc.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (VkImage image : handles)
    {
        Texture2D* t = Texture2D::CreateForSwapChain(dev, image, td);
        if (!t)
            return E_OUTOFMEMORY;
        // Views use the image's real format (a BGRA chain behind an RGBA description is fine).
        t->imageInfo.format = chosen.format;
        t->imageInfo.flags = m_mutable ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        t->imageInfo.usage = info.imageUsage;
        m_buffers.push_back(t);
        VkSemaphore done = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        g_vk.vkCreateSemaphore(dev->vk, &si, nullptr, &done);
        m_renderDone.push_back(done);
    }
    m_acquired = false;
    m_outOfDate = false;
    Log("* swap chain: %ux%u, %u images, %s, %s", m_extent.width, m_extent.height, count,
        m_mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : m_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "adaptive vsync" :
        m_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "tearing" : "mailbox",
        m_switchModes ? "mode switches in place" : "mode switches recreate it");
    return S_OK;
}

void SwapChain::DestroyNative(bool last)
{
    Device* dev = m_device;
    if (m_swapchain || !m_buffers.empty())
    {
        // Every frame that used the images has to finish first (present included).
        dev->WaitIdle();
    }
    for (Texture2D* t : m_buffers)
        t->Release();
    m_buffers.clear();
    for (VkSemaphore s : m_renderDone)
    {
        if (s)
            g_vk.vkDestroySemaphore(dev->vk, s, nullptr);
    }
    m_renderDone.clear();
    if (m_swapchain && m_chainProvider)
        m_chainProvider->Destroy(m_swapchain, last);
    else if (m_swapchain)
        g_vk.vkDestroySwapchainKHR(dev->vk, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_chainProvider = nullptr;
    m_acquired = false;
    m_pendingAcquire = VK_NULL_HANDLE;
}

bool SwapChain::Acquire()
{
    if (m_acquired)
        return true;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (m_outOfDate || !m_swapchain)
        {
            if (FAILED(CreateNative(m_syncInterval)) || m_minimized || !m_swapchain)
                return false;
        }
        const VkSemaphore semaphore = m_acquire[m_acquireIndex];
        u32 index = 0;
        const VkResult result = m_chainProvider ?
            VkResult(m_chainProvider->Acquire(m_swapchain, UINT64_MAX, semaphore, &index)) :
            g_vk.vkAcquireNextImageKHR(m_device->vk, m_swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            m_outOfDate = true;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            m_device->ReportLost(result);
            return false;
        }
        if (result == VK_SUBOPTIMAL_KHR)
            m_outOfDate = true; // after this frame's present
        m_current = index;
        m_acquireIndex = (m_acquireIndex + 1) % u32(std::size(m_acquire));
        m_pendingAcquire = semaphore;
        m_acquired = true;
        // The image's old contents are gone (flip discard): the copy transitions it from UNDEFINED after the
        // stage the acquire is waited at.
        Texture2D* t = m_buffers[index];
        t->states.assign(1, ImageState{VK_IMAGE_LAYOUT_UNDEFINED, kAcquireStages, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE});
        t->uniform = true;
        return true;
    }
    return false;
}


// ---- IUnknown / IDXGIObject ---------------------------------------------------------------------

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
    Device* dev = m_device;
    if (dev)
    {
        DestroyNative(true);
        if (m_backBuffer)
            m_backBuffer->Release();
        for (VkSemaphore s : m_acquire)
        {
            if (s)
                g_vk.vkDestroySemaphore(dev->vk, s, nullptr);
        }
        if (m_surface)
            g_vk.vkDestroySurfaceKHR(dev->instance, m_surface, nullptr);
    }
    if (m_factory)
        m_factory->Release();
    if (dev)
        dev->Release();
    delete this;
    return 0;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateData(REFGUID guid, UINT size, const void* data) { return m_private.Set(guid, size, data); }
HRESULT STDMETHODCALLTYPE SwapChain::SetPrivateDataInterface(REFGUID guid, const IUnknown* data) { return m_private.SetInterface(guid, data); }
HRESULT STDMETHODCALLTYPE SwapChain::GetPrivateData(REFGUID guid, UINT* size, void* data) { return m_private.Get(guid, size, data); }

HRESULT STDMETHODCALLTYPE SwapChain::GetParent(REFIID riid, void** parent)
{
    if (!parent)
        return E_POINTER;
    *parent = nullptr;
    return m_factory ? m_factory->QueryInterface(riid, parent) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDevice(REFIID riid, void** device)
{
    return m_device ? m_device->QueryInterface(riid, device) : E_FAIL;
}

// ---- presentation -------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE SwapChain::Present(UINT syncInterval, UINT flags)
{
    Device* dev = m_device;
    if (dev->lost)
        return DXGI_ERROR_DEVICE_REMOVED;
    if (flags & DXGI_PRESENT_TEST)
        return IsIconic(m_hwnd) ? DXGI_STATUS_OCCLUDED : S_OK;
    Context* imm = dev->immediate;
    syncInterval = (std::min)(syncInterval, 1u);
    if (syncInterval != m_syncInterval)
    {
        m_syncInterval = syncInterval;
        // Without mode switching in place a new chain carries the new mode.
        if (!m_switchModes)
            m_outOfDate = true;
    }
    // The frame goes to the GPU first and waits for nothing of the display.
    const u64 frameValue = imm->Submit();
    if (!Acquire())
    {
        // Minimized: the frame's work ran and its memory is recycled.
        dev->EndFrame(frameValue);
        return m_minimized ? DXGI_STATUS_OCCLUDED : S_OK;
    }
    Texture2D* image = m_buffers[m_current];
    Texture2D* source = m_backBuffer;
    if (source && imm->Cmd())
    {
        imm->RequireImageAll(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
        imm->RequireImageAll(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        imm->FlushBarriers();
        const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        const bool sameSize = source->width == m_extent.width && source->height == m_extent.height;
        if (sameSize && UnormOf(source->imageInfo.format) == UnormOf(image->imageInfo.format))
        {
            const VkImageCopy region{layers, {}, layers, {}, {m_extent.width, m_extent.height, 1}};
            g_vk.vkCmdCopyImage(imm->Cmd(), source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        else
        {
            // Another channel order, or a window resized ahead of ResizeBuffers: DXGI's stretch.
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = layers;
            blit.srcOffsets[1] = {int32_t(source->width), int32_t(source->height), 1};
            blit.dstOffsets[1] = {int32_t(m_extent.width), int32_t(m_extent.height), 1};
            g_vk.vkCmdBlitImage(imm->Cmd(), source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        imm->RequireImageAll(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        imm->FlushBarriers();
    }
    const VkSemaphore done = m_renderDone[m_current];
    const VkSemaphore acquired = m_pendingAcquire;
    m_pendingAcquire = VK_NULL_HANDLE;
    const u64 value = imm->Submit(acquired, done);

    // One present per refresh with vsync: the previous present has to reach the display first, or the one
    // before it while frames come slower than the refresh (present_pacing.h).
    m_clock.Tick(m_hwnd);
    const u64 queued = m_clock.Slower() && !xrpacing::StrictVsync() ? 1 : 0;
    // A provider's chain is no driver chain: it paces its own presents.
    if (dev->caps.presentWait && m_syncInterval && !m_chainProvider && m_presentId > queued &&
        m_presentId - queued >= m_chainFirstId)
        g_vk.vkWaitForPresentKHR(dev->vk, m_swapchain, m_presentId - queued, kPresentWaitNs);

    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &done;
    info.swapchainCount = 1;
    info.pSwapchains = &m_swapchain;
    info.pImageIndices = &m_current;
    // An SDK's frame id when it named one (present ids only grow).
    const u64 id = (std::max)(m_presentId + 1, m_requestedPresentId);
    m_requestedPresentId = 0;
    VkPresentIdKHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    if (dev->caps.presentWait && !m_chainProvider)
    {
        presentId.swapchainCount = 1;
        presentId.pPresentIds = &id;
        info.pNext = &presentId;
    }
    const VkPresentModeKHR mode = m_syncInterval ? m_vsyncMode : m_tearingMode;
    VkSwapchainPresentModeInfoEXT modeInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT};
    if (m_switchModes)
    {
        modeInfo.swapchainCount = 1;
        modeInfo.pPresentModes = &mode;
        modeInfo.pNext = info.pNext;
        info.pNext = &modeInfo;
    }
    VkResult result;
    if (m_chainProvider)
        result = VkResult(m_chainProvider->Present(dev->graphicsQueue, &info));
    else
    {
        std::lock_guard lock(dev->graphicsQueueLock);
        result = g_vk.vkQueuePresentKHR(dev->graphicsQueue, &info);
    }
    m_presentId = id;
    m_mode = mode;
    m_acquired = false;
    ++m_presentCount;
    dev->EndFrame(value);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        m_outOfDate = true;
    else if (result == VK_ERROR_DEVICE_LOST)
    {
        dev->ReportLost(result);
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*)
{
    return Present(syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetBuffer(UINT buffer, REFIID riid, void** surface)
{
    if (!surface)
        return E_POINTER;
    *surface = nullptr;
    if (buffer != 0)
    {
        static constexpr char kFormat[] = "~ GetBuffer(%u): only the current back buffer (0) exists for the engine";
        LogOnce(kFormat, kFormat, buffer);
    }
    return m_backBuffer ? m_backBuffer->QueryInterface(riid, surface) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetFullscreenState(BOOL fullscreen, IDXGIOutput*)
{
    // Borderless flip presentation is as fast as exclusive fullscreen on Windows 10+ (independent
    // flip); the state is kept for the engine, the window stays as it set it up.
    m_fullscreen = fullscreen != FALSE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target)
{
    if (fullscreen)
        *fullscreen = m_fullscreen;
    if (target)
        *target = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* desc)
{
    if (!desc)
        return E_INVALIDARG;
    *desc = {};
    desc->BufferDesc.Width = m_desc.Width;
    desc->BufferDesc.Height = m_desc.Height;
    desc->BufferDesc.Format = m_desc.Format;
    if (m_hasFullscreenDesc)
    {
        desc->BufferDesc.RefreshRate = m_fullscreenDesc.RefreshRate;
        desc->BufferDesc.Scaling = m_fullscreenDesc.Scaling;
        desc->BufferDesc.ScanlineOrdering = m_fullscreenDesc.ScanlineOrdering;
    }
    desc->SampleDesc = m_desc.SampleDesc;
    desc->BufferUsage = m_desc.BufferUsage;
    desc->BufferCount = m_desc.BufferCount;
    desc->OutputWindow = m_hwnd;
    desc->Windowed = !m_fullscreen;
    desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc->Flags = m_desc.Flags;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc)
{
    if (!desc)
        return E_INVALIDARG;
    *desc = m_desc;
    desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc)
{
    if (!desc)
        return E_INVALIDARG;
    *desc = m_fullscreenDesc;
    desc->Windowed = !m_fullscreen;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetHwnd(HWND* hwnd)
{
    if (!hwnd)
        return E_INVALIDARG;
    *hwnd = m_hwnd;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_desc.Width = width ? width : UINT((std::max)(1L, client.right - client.left));
    m_desc.Height = height ? height : UINT((std::max)(1L, client.bottom - client.top));
    if (format != DXGI_FORMAT_UNKNOWN)
        m_desc.Format = format;
    if (count)
        m_desc.BufferCount = count;
    m_desc.Flags = flags;
    DestroyNative();
    const HRESULT hr = CreateNative(m_syncInterval);
    if (FAILED(hr))
        return hr;
    // The engine released its references to the old back buffer (DXGI's rule for ResizeBuffers).
    return CreateBackBuffer();
}

HRESULT STDMETHODCALLTYPE SwapChain::ResizeTarget(const DXGI_MODE_DESC* params)
{
    if (!params)
        return E_INVALIDARG;
    // Windowed: the client area takes the size, as DXGI does; fullscreen keeps the window on its monitor.
    if (!m_fullscreen && params->Width && params->Height)
    {
        RECT rect{0, 0, LONG(params->Width), LONG(params->Height)};
        const DWORD style = DWORD(GetWindowLongW(m_hwnd, GWL_STYLE));
        const DWORD exStyle = DWORD(GetWindowLongW(m_hwnd, GWL_EXSTYLE));
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
        RECT current{};
        GetClientRect(m_hwnd, &current);
        if (current.right - current.left != LONG(params->Width) || current.bottom - current.top != LONG(params->Height))
            SetWindowPos(m_hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetContainingOutput(IDXGIOutput** output)
{
    if (!output)
        return E_INVALIDARG;
    *output = nullptr;
    IDXGIAdapter1* adapter = m_device ? m_device->adapter : nullptr;
    if (!adapter)
        return DXGI_ERROR_NOT_FOUND;
    // The output whose desktop holds most of the window.
    const HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIOutput* candidate = nullptr;
    for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, &candidate)); ++i)
    {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor)
        {
            *output = candidate;
            return S_OK;
        }
        candidate->Release();
    }
    return adapter->EnumOutputs(0, output);
}

HRESULT STDMETHODCALLTYPE SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* stats)
{
    if (stats)
        *stats = {};
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetLastPresentCount(UINT* count)
{
    if (!count)
        return E_INVALIDARG;
    *count = m_presentCount;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetCoreWindow(REFIID, void** unk)
{
    if (unk)
        *unk = nullptr;
    return DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE SwapChain::GetRestrictToOutput(IDXGIOutput** output)
{
    if (output)
        *output = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetBackgroundColor(const DXGI_RGBA*) { return S_OK; }

HRESULT STDMETHODCALLTYPE SwapChain::GetBackgroundColor(DXGI_RGBA* color)
{
    if (color)
        *color = {};
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SwapChain::SetRotation(DXGI_MODE_ROTATION) { return S_OK; }

HRESULT STDMETHODCALLTYPE SwapChain::GetRotation(DXGI_MODE_ROTATION* rotation)
{
    if (rotation)
        *rotation = DXGI_MODE_ROTATION_IDENTITY;
    return S_OK;
}

// ================================================================================================
// Public API
// ================================================================================================

namespace
{
// The chain behind an IDXGISwapChain of this library; null for another one.
SwapChain* LayerChain(IDXGISwapChain* chain)
{
    if (!chain)
        return nullptr;
    ID3D11Device* device = nullptr;
    chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    const bool ours = IsLayerDevice(device);
    if (device)
        device->Release();
    return ours ? static_cast<SwapChain*>(static_cast<IDXGISwapChain1*>(chain)) : nullptr;
}
} // namespace

bool SetSwapChainProvider(IDXGISwapChain* chain, SwapChainProvider* provider)
{
    SwapChain* s = LayerChain(chain);
    if (!s || s->m_device->lost)
        return false;
    if (s->m_provider == provider && (s->m_swapchain || s->m_minimized))
        return true;
    s->DestroyNative(true);
    s->m_provider = provider;
    if (SUCCEEDED(s->CreateNative(s->m_syncInterval)) && (s->m_swapchain || s->m_minimized))
        return true;
    // The layer's own chain again.
    s->m_provider = nullptr;
    s->CreateNative(s->m_syncInterval);
    return false;
}

void SetNextPresentId(IDXGISwapChain* chain, UINT64 id)
{
    if (SwapChain* s = LayerChain(chain))
        s->m_requestedPresentId = id;
}

bool SetAdaptiveVsync(IDXGISwapChain* chain, bool adaptive)
{
    SwapChain* s = LayerChain(chain);
    if (!s)
        return false;
    if (s->m_adaptive != adaptive)
    {
        s->m_adaptive = adaptive;
        // FIFO_RELAXED switches with no other mode in place.
        if (s->m_relaxedSupported)
            s->m_outOfDate = true;
    }
    return s->m_relaxedSupported;
}

HRESULT CreateSwapChainForHwnd(ID3D11Device* device, IDXGIFactory2* factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGISwapChain1** swapChain)
{
    if (swapChain)
        *swapChain = nullptr;
    if (!device || !hwnd || !desc || !swapChain || !IsLayerDevice(device))
        return E_INVALIDARG;
    SwapChain* chain = nullptr;
    const HRESULT hr = SwapChain::Create(Device::From(device), factory, hwnd, *desc, fullscreen, &chain);
    if (FAILED(hr))
        return hr;
    *swapChain = chain;
    return S_OK;
}
} // namespace xrvk
