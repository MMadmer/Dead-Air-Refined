// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Intel XeSS Super Resolution (Externals/xess/README.md) in the upscale slot, on each renderer: DirectX 12 and Vulkan
// through libxess.dll (any GPU with Shader Model 6.4 and DP4a; the Intel-optimised model on Arc and Iris Xe), DirectX
// 11 through libxess_dx11.dll (Intel Arc only). The input is the same as FSR's: tonemapped LDR colour, standard depth,
// motion vectors at render size without the jitter, the reactive mask as XeSS's responsive pixel mask. DirectX 12 and
// Vulkan record into the immediate context's native command list between the layer's Begin/EndNative; DirectX 11
// executes on the immediate context itself.

#include "stdafx.h"

#include "da_upscaler.h"
#include "da_xess.h"
#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#elif defined(XR_VULKAN)
#include "Layers/xrRenderVK/vk_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
#if defined(XR_D3D12)
constexpr u32 kXessApi = 12;
constexpr pcstr kXessName = "XeSS (DX12)";
#elif defined(XR_VULKAN)
constexpr u32 kXessApi = 13;
constexpr pcstr kXessName = "XeSS (Vulkan)";
#else
constexpr u32 kXessApi = 11;
constexpr pcstr kXessName = "XeSS (DX11)";
#endif

void xess_message(const char* message, xess_logging_level_t level)
{
    Msg("%s [XeSS] %s", level >= XESS_LOGGING_LEVEL_ERROR ? "!" : "~", message);
}

// A context on this device, or null with the reason in the log.
xess_context_handle_t xess_create(const DaXess& xess, bool quiet)
{
    xess_context_handle_t context = nullptr;
    xess_result_t result = XESS_RESULT_ERROR_UNKNOWN;
#if defined(XR_D3D12)
    if (ID3D12Device* device = HW.pDevice ? xrd3d12::NativeDevice(HW.pDevice) : nullptr)
        result = xess.CreateContext(device, &context);
#elif defined(XR_VULKAN)
    xrvk::NativeDeviceInfo device;
    if (HW.pDevice && da_xess_vk_device_ready() && xrvk::GetNativeDevice(HW.pDevice, device, nullptr))
    {
        result = xess.CreateContext(static_cast<VkInstance>(device.instance),
            static_cast<VkPhysicalDevice>(device.physicalDevice), static_cast<VkDevice>(device.device), &context);
    }
#else
    if (HW.pDevice)
        result = xess.CreateContext(HW.pDevice, &context);
#endif
    if (result != XESS_RESULT_SUCCESS || !context)
    {
        if (!quiet)
            Msg("! [XeSS] context creation failed (%d)", int(result));
        if (context)
            xess.DestroyContext(context);
        return nullptr;
    }
    return context;
}

// Whether XeSS runs on this adapter: a context can be made (DirectX 11: Intel Arc only). Asked once per device.
bool xess_supported()
{
    static const void* asked{};
    static bool answer{};
    const DaXess* xess = da_xess();
    if (!xess || !HW.pDevice)
        return false;
    if (asked == HW.pDevice)
        return answer;
    asked = HW.pDevice;
    xess_context_handle_t context = xess_create(*xess, true);
    answer = context != nullptr;
    if (context)
        xess->DestroyContext(context);
    Msg("* [XeSS] %s on this adapter", answer ? "runs" : "does not run");
    return answer;
}

class xess_upscaler final : public IUpscaler
{
    xess_context_handle_t m_context{};
    u32 m_displayW{}, m_displayH{};
    bool m_warnedVelocity{};
    string64 m_name{};

public:
    xess_upscaler() { xr_strcpy(m_name, kXessName); }
    ~xess_upscaler() override { Shutdown(); }

    bool Init(u32 displayW, u32 displayH, int quality) override
    {
        Shutdown();
        const DaXess* xess = da_xess();
        if (!xess || !displayW || !displayH)
            return false;
        m_context = xess_create(*xess, false);
        if (!m_context)
            return false;
        xess->SetLoggingCallback(m_context, strstr(Core.Params, "-dxdebug") || strstr(Core.Params, "-vkdebug") ?
            XESS_LOGGING_LEVEL_WARNING : XESS_LOGGING_LEVEL_ERROR, &xess_message);

        // Tonemapped LDR input with standard depth and motion vectors at render size; the reactive mask is the
        // responsive one.
        constexpr uint32_t flags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
#if defined(XR_D3D12)
        xess_d3d12_init_params_t init{};
#elif defined(XR_VULKAN)
        xess_vk_init_params_t init{};
#else
        xess_d3d11_init_params_t init{};
#endif
        init.outputResolution = { displayW, displayH };
        init.qualitySetting = da_xess_quality(quality);
        init.initFlags = flags;
        const xess_result_t result = xess->Init(m_context, &init);
        if (result != XESS_RESULT_SUCCESS)
        {
            Msg("! [XeSS] initialisation failed (%d)", int(result));
            Shutdown();
            return false;
        }
        if (xess->IsOptimalDriver(m_context) == XESS_RESULT_WARNING_OLD_DRIVER)
            Msg("~ [XeSS] the graphics driver is older than XeSS recommends");
        m_displayW = displayW;
        m_displayH = displayH;
        m_warnedVelocity = false;
        xr_sprintf(m_name, "XeSS %u.%u.%u (%s)", xess->version.major, xess->version.minor, xess->version.patch,
            kXessApi == 12 ? "DX12" : kXessApi == 13 ? "Vulkan" : "DX11");
        Msg("* [XeSS] ready through [%s]: %ux%u -> %ux%u", m_name, Device.dwRenderWidth, Device.dwRenderHeight,
            displayW, displayH);
        return true;
    }

    void Shutdown() override
    {
        if (!m_context)
            return;
        // Recorded and in-flight work still references the context's resources.
        if (ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID))
        {
#if defined(XR_D3D12)
            xrd3d12::Flush(imm, true);
#elif defined(XR_VULKAN)
            xrvk::Flush(imm, true);
#else
            imm->Flush();
#endif
        }
        if (const DaXess* xess = da_xess())
        {
            const xess_result_t result = xess->DestroyContext(m_context);
            if (result != XESS_RESULT_SUCCESS)
                Msg("! [XeSS] context destruction failed (%d)", int(result));
        }
        m_context = nullptr;
        xr_strcpy(m_name, kXessName);
    }

    bool Dispatch(const UpscaleFrame& frame) override
    {
        const DaXess* xess = da_xess();
        if (!m_context || !xess || !frame.color || !frame.depth || !frame.output)
            return false;
        if (frame.renderW > m_displayW || frame.renderH > m_displayH || frame.displayW > m_displayW ||
            frame.displayH > m_displayH)
        {
            Msg("! [XeSS] frame %ux%u -> %ux%u exceeds the context (%ux%u)", frame.renderW, frame.renderH,
                frame.displayW, frame.displayH, m_displayW, m_displayH);
            return false;
        }
        if (!frame.velocity && !m_warnedVelocity)
        {
            Msg("! [XeSS] no motion vectors: the frame is reconstructed as if nothing moved");
            m_warnedVelocity = true;
        }
        // Stored NDC(cur) - NDC(prev), y up; XeSS wants prev - cur in render pixels, y down. The jitter is AMD's
        // convention too: render pixels, y down, the offset the projection carried.
        xess->SetVelocityScale(m_context, -0.5f * float(frame.renderW), 0.5f * float(frame.renderH));

        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
        xess_result_t result;
#if defined(XR_D3D12)
        ID3D12Resource* color = xrd3d12::NativeResource(frame.color);
        ID3D12Resource* depth = xrd3d12::NativeResource(frame.depth);
        ID3D12Resource* velocity = xrd3d12::NativeResource(frame.velocity);
        ID3D12Resource* reactive = xrd3d12::NativeResource(frame.reactive);
        ID3D12Resource* output = xrd3d12::NativeResource(frame.output);
        if (!color || !depth || !output)
            return false;
        // The layer tracks the states; XeSS leaves every resource in the state it was given.
        constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        xrd3d12::Require(imm, frame.color, read);
        xrd3d12::Require(imm, frame.depth, read);
        if (velocity)
            xrd3d12::Require(imm, frame.velocity, read);
        if (reactive)
            xrd3d12::Require(imm, frame.reactive, read);
        xrd3d12::Require(imm, frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(imm);
        if (!list)
            return false;
        xess_d3d12_execute_params_t p{};
        p.pColorTexture = color;
        p.pDepthTexture = depth;
        p.pVelocityTexture = velocity;
        p.pResponsivePixelMaskTexture = reactive;
        p.pOutputTexture = output;
#elif defined(XR_VULKAN)
        // The inputs in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL, as XeSS expects them.
        xrvk::NativeImage color, depth, velocity, reactive, output;
        if (!xrvk::PrepareNativeImage(imm, frame.color, false, color) ||
            !xrvk::PrepareNativeImage(imm, frame.depth, false, depth) ||
            !xrvk::PrepareNativeImage(imm, frame.output, true, output))
            return false;
        if (frame.velocity && !xrvk::PrepareNativeImage(imm, frame.velocity, false, velocity))
            return false;
        if (frame.reactive && !xrvk::PrepareNativeImage(imm, frame.reactive, false, reactive))
            return false;
        if (!(output.usage & VK_IMAGE_USAGE_STORAGE_BIT))
        {
            Msg("! [XeSS] the output target has no storage usage");
            return false;
        }
        const auto view = [](const xrvk::NativeImage& image, VkImageAspectFlags aspect) {
            xess_vk_image_view_info info{};
            if (!image.image)
                return info;
            info.imageView = static_cast<VkImageView>(image.view);
            info.image = static_cast<VkImage>(image.image);
            info.subresourceRange = { aspect, 0, 1, 0, 1 };
            info.format = VkFormat(image.format);
            info.width = image.width;
            info.height = image.height;
            return info;
        };
        void* command = xrvk::BeginNative(imm);
        if (!command)
            return false;
        xess_vk_execute_params_t p{};
        p.colorTexture = view(color, VK_IMAGE_ASPECT_COLOR_BIT);
        p.depthTexture = view(depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        p.velocityTexture = view(velocity, VK_IMAGE_ASPECT_COLOR_BIT);
        p.responsivePixelMaskTexture = view(reactive, VK_IMAGE_ASPECT_COLOR_BIT);
        p.outputTexture = view(output, VK_IMAGE_ASPECT_COLOR_BIT);
#else
        xess_d3d11_execute_params_t p{};
        p.pColorTexture = frame.color;
        p.pDepthTexture = frame.depth;
        p.pVelocityTexture = frame.velocity;
        p.pResponsivePixelMaskTexture = frame.reactive;
        p.pOutputTexture = frame.output;
#endif
        p.jitterOffsetX = frame.jitterX;
        p.jitterOffsetY = frame.jitterY;
        p.exposureScale = 1.f;
        p.resetHistory = frame.reset ? 1u : 0u;
        p.inputWidth = frame.renderW;
        p.inputHeight = frame.renderH;
#if defined(XR_D3D12)
        result = xess->Execute(m_context, list, &p);
        // XeSS set its own heaps, root signature and pipeline: the layer re-applies the engine's.
        xrd3d12::EndNative(imm);
#elif defined(XR_VULKAN)
        result = xess->Execute(m_context, static_cast<VkCommandBuffer>(command), &p);
        // XeSS bound its own pipelines and descriptor sets: the layer applies the engine's again.
        xrvk::EndNative(imm);
#else
        UNUSED(imm);
        result = xess->Execute(m_context, &p);
#endif
        if (result != XESS_RESULT_SUCCESS)
        {
            Msg("! [XeSS] execution failed (%d)", int(result));
            return false;
        }
        return true;
    }

    void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) override
    {
        da_upscaler_default_render_size(UpscalerKind::XeSS, quality, displayW, displayH, renderW, renderH);
    }

    pcstr Name() override { return m_name; }
};

IUpscaler* xess_create_upscaler() { return xr_new<xess_upscaler>(); }

void xess_caps(UpscalerCaps& caps)
{
    if (HW.pDevice)
        caps.xess = xess_supported();
}

const da_upscaler_registrar s_xess{ { UpscalerKind::XeSS, kXessApi, kXessName, &xess_create_upscaler, &xess_supported } };

const struct xess_caps_registrar
{
    xess_caps_registrar() { da_upscaler_register_caps_hook(&xess_caps); }
} s_xess_caps;
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
