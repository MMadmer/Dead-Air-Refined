// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// AMD FSR frame generation on Vulkan (docs/dead-air/VULKAN_RENDERER.md, 7) through the signed amd_fidelityfx_vk.dll
// of FidelityFX SDK 1.1.4: FFX's frame interpolation swap chain takes the place of the layer's VkSwapchainKHR
// (xrvk::SwapChainProvider) and presents a generated frame between every two rendered ones from its own presenter
// thread, on two graphics-family queues the device keeps for it. The frame generation context prepares from the
// scene depth and the motion vectors on the immediate command buffer; the UI is told apart from the scene by the
// difference between the back buffer and its hudless copy. Per frame: ffxConfigure -> Prepare (here) -> the
// generation inside Present, through the swap chain's callback. The DirectX 12 twin is da_framegen_fsr.cpp.

#include "stdafx.h"

#ifdef XR_VULKAN
#include "da_framegen.h"
#include "da_ffx_vk.h"

#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
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
    const ffxFunctions* ffx = da_ffx_vk();
    return ffx ? ffx->Dispatch(static_cast<ffxContext*>(context), &params->header) : FFX_API_RETURN_ERROR;
}

// FFX submits to the layer's queues from its presenter thread too: under the layer's locks.
VkResult fsr_fg_submit_graphics(uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    return VkResult(xrvk::SubmitToQueue(HW.pDevice, xrvk::Queue::Graphics, count, submits, fence));
}

VkResult fsr_fg_submit_compute(uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    return VkResult(xrvk::SubmitToQueue(HW.pDevice, xrvk::Queue::Compute, count, submits, fence));
}

// An image as ffx-api describes it (the format it was made with is the one it is read as).
FfxApiResource fsr_fg_resource(const xrvk::NativeImage& image, uint32_t state)
{
    if (!image.image)
        return {};
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VkFormat(image.format);
    info.extent = { image.width, image.height, 1 };
    info.mipLevels = image.mipLevels;
    info.arrayLayers = image.arrayLayers;
    info.flags = VkImageCreateFlags(image.createFlags) & ~VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    info.usage = VkImageUsageFlags(image.usage);
    return ffxApiGetResourceVK(image.image, ffxApiGetImageResourceDescriptionVK(static_cast<VkImage>(image.image), info, 0),
        state);
}

// FSR's frame interpolation swap chain in place of the layer's. A static object: the layer keeps it until the
// chain goes (da_framegen.h). One context for the life of the provider's chains: a resize or a vsync change
// makes a new chain of the same context, only the last chain takes the context with it.
class fsr_vk_swapchain_provider final : public xrvk::SwapChainProvider
{
    ffxContext m_context{};
    VkDevice m_device{};
    VkSwapchainKHR m_chain{};
    VkFormat m_format{};
    ffxQueryDescSwapchainReplacementFunctionsVK m_fn{};

    // Presents FFX has queued finish before a chain goes.
    void WaitForPresents()
    {
        const ffxFunctions* ffx = da_ffx_vk();
        if (!ffx || !m_context)
            return;
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        ffx->Dispatch(&m_context, &wait.header);
    }

    void DestroyContext()
    {
        if (const ffxFunctions* ffx = da_ffx_vk(); ffx && m_context)
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR FG] swap chain context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
        m_chain = VK_NULL_HANDLE;
        m_fn = {};
    }

public:
    int Create(const void* createInfo, void* swapchain) override
    {
        const VkSwapchainCreateInfoKHR& info = *static_cast<const VkSwapchainCreateInfoKHR*>(createInfo);
        VkSwapchainKHR& out = *static_cast<VkSwapchainKHR*>(swapchain);
        out = VK_NULL_HANDLE;
        const ffxFunctions* ffx = da_ffx_vk();
        xrvk::NativeDeviceInfo device;
        const PFN_vkGetDeviceProcAddr proc = da_ffx_vk_device_proc();
        if (!ffx || !proc || !xrvk::GetNativeDevice(HW.pDevice, device, nullptr))
            return VK_ERROR_INITIALIZATION_FAILED;
        m_device = static_cast<VkDevice>(device.device);

        if (m_context)
        {
            const VkResult result = m_fn.pOutCreateSwapchainFFXAPI(m_device, &info, nullptr, &out, m_context);
            if (result == VK_SUCCESS)
            {
                m_chain = out;
                m_format = info.imageFormat;
            }
            else
                Msg("! [FSR FG] the frame generation swap chain cannot be made again (%d)", int(result));
            return result;
        }

        if (device.presentQueueCount < 2)
        {
            Msg("! [FSR FG] the device keeps no queues for the presenter");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        da_ffx_vk_route_swapchain();
        const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(proc(m_device, "vkGetDeviceQueue"));
        if (!getQueue)
            return VK_ERROR_INITIALIZATION_FAILED;
        VkQueue present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
        getQueue(m_device, device.graphicsFamily, device.presentQueueIndex, &present);
        getQueue(m_device, device.graphicsFamily, device.presentQueueIndex + 1, &acquire);

        VkSwapchainKHR chain = VK_NULL_HANDLE;
        ffxCreateContextDescFrameGenerationSwapChainVK create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        create.physicalDevice = static_cast<VkPhysicalDevice>(device.physicalDevice);
        create.device = m_device;
        create.swapchain = &chain;
        create.allocator = nullptr;
        create.createInfo = info;
        create.gameQueue = { static_cast<VkQueue>(xrvk::NativeQueue(HW.pDevice, xrvk::Queue::Graphics)),
            device.graphicsFamily, &fsr_fg_submit_graphics };
        create.asyncComputeQueue = { static_cast<VkQueue>(xrvk::NativeQueue(HW.pDevice, xrvk::Queue::Compute)),
            device.computeFamily, &fsr_fg_submit_compute };
        create.presentQueue = { present, device.graphicsFamily, nullptr };
        create.imageAcquireQueue = { acquire, device.graphicsFamily, nullptr };
        // The UI composition goes with the present (FFX's preferred mode): the game queue only interpolates.
        ffxCreateContextDescFrameGenerationSwapChainModeVK mode{};
        mode.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
        mode.composeOnPresentQueue = true;
        create.header.pNext = &mode.header;

        const ffxReturnCode_t code = ffx->CreateContext(&m_context, &create.header, nullptr);
        if (code != FFX_API_RETURN_OK || !m_context || !chain)
        {
            Msg("! [FSR FG] the frame generation swap chain cannot be created (%u)", u32(code));
            DestroyContext();
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_fn = {};
        m_fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        if (ffx->Query(&m_context, &m_fn.header) != FFX_API_RETURN_OK || !m_fn.pOutCreateSwapchainFFXAPI ||
            !m_fn.pOutDestroySwapchainFFXAPI || !m_fn.pOutGetSwapchainImagesKHR || !m_fn.pOutAcquireNextImageKHR ||
            !m_fn.pOutQueuePresentKHR)
        {
            Msg("! [FSR FG] the frame generation swap chain gives no replacement functions");
            DestroyContext();
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        m_chain = out = chain;
        m_format = info.imageFormat;
        return VK_SUCCESS;
    }

    void Destroy(void* swapchain, bool last) override
    {
        const VkSwapchainKHR chain = static_cast<VkSwapchainKHR>(swapchain);
        if (!m_context || chain != m_chain)
            return;
        WaitForPresents();
        if (last)
        {
            DestroyContext();
            return;
        }
        m_fn.pOutDestroySwapchainFFXAPI(m_device, chain, nullptr, m_context);
        m_chain = VK_NULL_HANDLE;
    }

    int GetImages(void* swapchain, UINT* count, void* images) override
    {
        return m_fn.pOutGetSwapchainImagesKHR(m_device, static_cast<VkSwapchainKHR>(swapchain), count,
            static_cast<VkImage*>(images));
    }

    int Acquire(void* swapchain, UINT64 timeout, void* semaphore, UINT* index) override
    {
        return m_fn.pOutAcquireNextImageKHR(m_device, static_cast<VkSwapchainKHR>(swapchain), timeout,
            static_cast<VkSemaphore>(semaphore), VK_NULL_HANDLE, index);
    }

    int Present(void* queue, const void* presentInfo) override
    {
        return m_fn.pOutQueuePresentKHR(static_cast<VkQueue>(queue), static_cast<const VkPresentInfoKHR*>(presentInfo));
    }

    ffxContext* Context() { return m_context ? &m_context : nullptr; }
    VkSwapchainKHR Chain() const { return m_chain; }
    VkFormat Format() const { return m_format; }
    // Presents of the current chain, generated ones included (a new chain counts from zero).
    u64 PresentCount() const
    {
        return m_chain && m_fn.pOutGetLastPresentCountFFXAPI ? m_fn.pOutGetLastPresentCountFFXAPI(m_chain) : 0;
    }
};
fsr_vk_swapchain_provider g_fsr_vk_swapchain;

// The format of the back buffer the hudless copy is made of (the layer's GetBuffer(0)).
VkFormat fsr_fg_backbuffer_format()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!HW.m_pSwapChain || FAILED(HW.m_pSwapChain->GetDesc(&desc)))
        return VK_FORMAT_R8G8B8A8_UNORM;
    switch (desc.BufferDesc.Format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

class fsr_vk_framegen final : public IFrameGen
{
    ffxContext m_context{};
    u32 m_displayW{}, m_displayH{};
    u64 m_frameID{};
    u64 m_idOffset{}; // SDK 1.1.4 resets on a frame id that does not step by one: a reset skips one
    static constexpr u32 kReportFrames = 600;
    u64 m_prepared{}, m_presentsAtInit{}; // the log line that shows the generation reached the display
    bool m_active{};
    bool m_resetNext{};
    bool m_warned{};
    string64 m_name{ "AMD FSR FG (Vulkan)" };

    // The swap chain learns whether to generate, what to call for it and where the UI is.
    bool Configure(bool enabled, const xrvk::NativeImage* hudless)
    {
        const ffxFunctions* ffx = da_ffx_vk();
        if (!ffx || !m_context)
            return false;
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = g_fsr_vk_swapchain.Chain();
        // FFX's own composition: the rendered or generated frame and the UI of the hudless difference.
        config.presentCallback = nullptr;
        config.presentCallbackUserContext = nullptr;
        config.frameGenerationCallback = enabled ? &fsr_fg_generate : nullptr;
        config.frameGenerationCallbackUserContext = enabled ? &m_context : nullptr;
        config.frameGenerationEnabled = enabled;
        config.allowAsyncWorkloads = false;
        if (hudless)
            config.HUDLessColor = fsr_fg_resource(*hudless, FFX_API_RESOURCE_STATE_COMPUTE_READ);
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
    ~fsr_vk_framegen() override { Shutdown(); }

    pcstr Name() const override { return m_name; }

    xrvk::SwapChainProvider* Provider() override { return &g_fsr_vk_swapchain; }

    bool Init(u32 displayW, u32 displayH, u32 generated) override
    {
        Shutdown();
        const ffxFunctions* ffx = da_ffx_vk();
        const PFN_vkGetDeviceProcAddr proc = da_ffx_vk_device_proc();
        xrvk::NativeDeviceInfo device;
        if (!ffx || !proc || !xrvk::GetNativeDevice(HW.pDevice, device, nullptr) || !displayW || !displayH)
            return false;
        // FSR generates one frame per rendered one.
        if (generated != 1)
        {
            Msg("! [FSR FG] %u generated frames asked, FSR makes one", generated);
            return false;
        }
        if (!g_fsr_vk_swapchain.Chain())
        {
            Msg("! [FSR FG] the renderer does not present through FSR's swap chain");
            return false;
        }

        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = static_cast<VkDevice>(device.device);
        backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.physicalDevice);
        backend.vkDeviceProcAddr = proc;

        // The hudless copy has the back buffer's format; the swap chain's may order the channels otherwise.
        const VkFormat hudlessFormat = fsr_fg_backbuffer_format();
        ffxCreateContextDescFrameGenerationHudless hudless{};
        hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(hudlessFormat);
        if (hudlessFormat != g_fsr_vk_swapchain.Format())
            backend.header.pNext = &hudless.header;

        // LDR input, standard finite depth (0 = near), motion vectors at render size without the jitter in them:
        // no HDR, inverted, infinite or jitter-cancellation flags. The render size never exceeds the display, so
        // an upscaler change needs no new context.
        const bool debug = !!strstr(Core.Params, "-vkdebug");
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = &backend.header;
        desc.flags = debug ? FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING : 0u;
        desc.displaySize = { displayW, displayH };
        desc.maxRenderSize = { displayW, displayH };
        desc.backBufferFormat = ffxApiGetSurfaceFormatVK(g_fsr_vk_swapchain.Format());

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

        // The runtime's own messages: errors, and warnings under -vkdebug.
        ffxConfigureDescGlobalDebug1 messages{};
        messages.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        messages.fpMessage = &fsr_fg_message;
        messages.debugLevel = debug ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
        ffx->Configure(&m_context, &messages.header);

        // The swap chain starts with generation off.
        Configure(false, nullptr);
        m_prepared = 0;
        m_presentsAtInit = g_fsr_vk_swapchain.PresentCount();

        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx->Query(&m_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
            xr_sprintf(m_name, "AMD FSR FG %s (Vulkan)", provider.versionName);
        return true;
    }

    void Shutdown() override
    {
        if (!m_context)
            return;
        const ffxFunctions* ffx = da_ffx_vk();
        // The swap chain must stop calling into this context, and finish what it queued, first.
        Configure(false, nullptr);
        if (ffxContext* swapchain = g_fsr_vk_swapchain.Context(); swapchain && ffx)
        {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
            wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
            ffx->Dispatch(swapchain, &wait.header);
        }
        // Recorded and in-flight work still references the context's resources.
        if (ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID))
            xrvk::Flush(imm, true);
        if (ffx)
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR FG] context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
        m_active = false;
        xr_strcpy(m_name, "AMD FSR FG (Vulkan)");
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
        const ffxFunctions* ffx = da_ffx_vk();
        if (!m_context || !ffx || !m_active)
            return;
        if (frame.displayW != m_displayW || frame.displayH != m_displayH || frame.renderW > m_displayW ||
            frame.renderH > m_displayH)
        {
            Fail("the frame does not match the context");
            return;
        }

        // The inputs read by compute in SHADER_READ_ONLY_OPTIMAL; the hudless copy stays there until the
        // generation reads it inside Present.
        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
        xrvk::NativeImage depth, velocity, hudless;
        if (!xrvk::PrepareNativeImage(imm, frame.depth, false, depth) ||
            !xrvk::PrepareNativeImage(imm, frame.velocity, false, velocity) ||
            !xrvk::PrepareNativeImage(imm, frame.hudless, false, hudless))
        {
            Fail("an input is missing");
            return;
        }

        // Configure first, then prepare. The id steps by exactly one per frame; anything else resets the
        // generation, which is how this SDK takes a reset.
        if (frame.reset || m_resetNext)
            ++m_idOffset;
        m_frameID = frame.frameIndex + m_idOffset;
        if (!Configure(true, &hudless))
        {
            Fail("configure failed");
            return;
        }
        void* command = xrvk::BeginNative(imm);
        if (!command)
        {
            Fail("no command buffer");
            return;
        }

        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.frameID = m_frameID;
        prepare.flags = 0;
        prepare.commandList = command;
        prepare.renderSize = { frame.renderW, frame.renderH };
        // Render pixels, y down, the offset the projection carried (AMD's convention).
        prepare.jitterOffset = { frame.jitterX, frame.jitterY };
        // Stored NDC(cur) - NDC(prev), y up; FSR wants prev - cur in render pixels, y down.
        prepare.motionVectorScale = { -0.5f * float(frame.renderW), 0.5f * float(frame.renderH) };
        prepare.frameTimeDelta = std::max(frame.frameTimeMs, 0.1f);
        prepare.cameraNear = frame.nearZ;
        prepare.cameraFar = frame.farZ;
        prepare.cameraFovAngleVertical = frame.fovY;
        prepare.viewSpaceToMetersFactor = 1.f;
        prepare.depth = fsr_fg_resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prepare.motionVectors = fsr_fg_resource(velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);

        // FSR 3.1.4 and later want the camera for their best quality.
        ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
        camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        const auto put = [](float (&to)[3], const Fvector& from)
        {
            to[0] = from.x;
            to[1] = from.y;
            to[2] = from.z;
        };
        put(camera.cameraPosition, frame.cameraPosition);
        put(camera.cameraUp, frame.cameraUp);
        put(camera.cameraRight, frame.cameraRight);
        put(camera.cameraForward, frame.cameraForward);
        prepare.header.pNext = &camera.header;

        const ffxReturnCode_t code = ffx->Dispatch(&m_context, &prepare.header);
        // FSR bound its own pipelines and descriptor sets: the layer applies the engine's again.
        xrvk::EndNative(imm);
        if (code != FFX_API_RETURN_OK)
        {
            Fail("prepare failed");
            return;
        }
        m_resetNext = false;
        // One line that shows the generation reached the display: about two presents per prepared frame.
        if (++m_prepared == kReportFrames)
            Msg("* [FSR FG] %llu presents for the first %u generating frames",
                g_fsr_vk_swapchain.PresentCount() - m_presentsAtInit, kReportFrames);
    }
};

IFrameGen* fsr_vk_fg_create() { return xr_new<fsr_vk_framegen>(); }

// The runtime is there and the device kept the presenter's queues.
bool fsr_vk_fg_supported()
{
    xrvk::NativeDeviceInfo device;
    return da_ffx_vk() && xrvk::GetNativeDevice(HW.pDevice, device, nullptr) && device.presentQueueCount >= 2;
}

const da_framegen_registrar s_fsr_vk_fg{ { 1, "AMD FSR FG (Vulkan)", &fsr_vk_fg_create, &fsr_vk_fg_supported, nullptr } };
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_VULKAN
