// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// AMD FSR 3.1 upscaling on Vulkan through the signed amd_fidelityfx_vk.dll of FidelityFX SDK 1.1.4 (ffx-api;
// Externals/fidelityfx-vk/README.md). Vulkan module only: the AMD FSR SDK 2.3 the DX12 module uses has no Vulkan
// runtime. The work is recorded on the immediate context's command buffer between xrvk::BeginNative and
// xrvk::EndNative; FSR transitions what it touches and returns every image to the layout it was given.

#include "stdafx.h"

#ifdef XR_VULKAN
#include "da_upscaler.h"
#include "Layers/xrRenderVK/vk_api.h"

#include "da_ffx_vk.h"

#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// r__fsr: 1 native AA, 2 quality, 3 balanced, 4 performance, 5 ultra performance (0 = off).
u32 fsr_quality_mode(int quality)
{
    static constexpr u32 modes[] = { FFX_UPSCALE_QUALITY_MODE_NATIVEAA, FFX_UPSCALE_QUALITY_MODE_NATIVEAA,
        FFX_UPSCALE_QUALITY_MODE_QUALITY, FFX_UPSCALE_QUALITY_MODE_BALANCED, FFX_UPSCALE_QUALITY_MODE_PERFORMANCE,
        FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE };
    return modes[std::clamp(quality, 0, 5)];
}

void fsr_message(uint32_t type, const wchar_t* message)
{
    string512 text;
    const int written = WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    text[written > 0 ? written : 0] = 0;
    Msg("%s [FSR] %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "!" : "~", text);
}

// An image as ffx-api describes it. The layer's typeless textures carry the mutable flag, which ffx-api takes
// for "really sRGB": the format the image was made with is the one it is read as.
FfxApiResource fsr_resource(const xrvk::NativeImage& image, uint32_t state)
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
    const VkImage handle = static_cast<VkImage>(image.image);
    return ffxApiGetResourceVK(image.image, ffxApiGetImageResourceDescriptionVK(handle, info, 0), state);
}

class fsr_vk_upscaler final : public IUpscaler
{
    ffxContext m_context{};
    u32 m_displayW{}, m_displayH{};
    bool m_warnedVelocity{};

public:
    ~fsr_vk_upscaler() override { Shutdown(); }

    bool Init(u32 displayW, u32 displayH, int /*quality*/) override
    {
        Shutdown();
        const ffxFunctions* ffx = da_ffx_vk();
        xrvk::NativeDeviceInfo device;
        const PFN_vkGetDeviceProcAddr deviceProc = da_ffx_vk_device_proc();
        if (!ffx || !deviceProc || !xrvk::GetNativeDevice(HW.pDevice, device, nullptr) || !displayW || !displayH)
            return false;

        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = static_cast<VkDevice>(device.device);
        backend.vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.physicalDevice);
        backend.vkDeviceProcAddr = deviceProc;

        // Tonemapped LDR input with standard, finite depth; the maximum render size is the display, so the quality
        // can change without a new context.
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = strstr(Core.Params, "-vkdebug") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u;
        desc.maxRenderSize = { displayW, displayH };
        desc.maxUpscaleSize = { displayW, displayH };
        desc.fpMessage = &fsr_message;

        const ffxReturnCode_t code = ffx->CreateContext(&m_context, &desc.header, nullptr);
        if (code != FFX_API_RETURN_OK || !m_context)
        {
            Msg("! [FSR] Vulkan context creation failed (%u)", u32(code));
            m_context = nullptr;
            return false;
        }
        m_displayW = displayW;
        m_displayH = displayH;
        m_warnedVelocity = false;
        Msg("* [FSR] ready through [FSR 3.1 (Vulkan)]: %ux%u -> %ux%u", Device.dwRenderWidth, Device.dwRenderHeight,
            displayW, displayH);
        return true;
    }

    void Shutdown() override
    {
        if (!m_context)
            return;
        // Recorded and in-flight work still references the context's resources.
        if (ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID))
            xrvk::Flush(imm, true);
        if (const ffxFunctions* ffx = da_ffx_vk())
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR] Vulkan context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
    }

    bool Dispatch(const UpscaleFrame& frame) override
    {
        const ffxFunctions* ffx = da_ffx_vk();
        if (!m_context || !ffx || !frame.color || !frame.depth || !frame.output)
            return false;
        if (frame.renderW > m_displayW || frame.renderH > m_displayH || frame.displayW > m_displayW ||
            frame.displayH > m_displayH)
        {
            Msg("! [FSR] frame %ux%u -> %ux%u exceeds the context (%ux%u)", frame.renderW, frame.renderH,
                frame.displayW, frame.displayH, m_displayW, m_displayH);
            return false;
        }
        if (!frame.velocity && !m_warnedVelocity)
        {
            Msg("! [FSR] no motion vectors: the frame is reconstructed as if nothing moved");
            m_warnedVelocity = true;
        }

        // The inputs in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL: FSR is told those layouts and gives
        // every image back in it.
        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
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
            Msg("! [FSR] the output target has no storage usage");
            return false;
        }
        void* command = xrvk::BeginNative(imm);
        if (!command)
            return false;

        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = command;
        d.color = fsr_resource(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = fsr_resource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = fsr_resource(velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.reactive = fsr_resource(reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = fsr_resource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

        // Jitter: render pixels, y down, the offset the projection carried (AMD's convention).
        d.jitterOffset = { frame.jitterX, frame.jitterY };
        // Stored NDC(cur) - NDC(prev), y up; FSR wants prev - cur in render pixels, y down.
        d.motionVectorScale = { -0.5f * float(frame.renderW), 0.5f * float(frame.renderH) };
        d.renderSize = { frame.renderW, frame.renderH };
        d.upscaleSize = { frame.displayW, frame.displayH };
        d.enableSharpening = frame.sharpness > 0.f;
        d.sharpness = std::clamp(frame.sharpness, 0.f, 1.f);
        d.frameTimeDelta = std::max(frame.frameTimeMs, 1.f);
        d.preExposure = 1.f;
        d.reset = frame.reset;
        d.cameraNear = frame.nearZ;
        d.cameraFar = frame.farZ;
        d.cameraFovAngleVertical = frame.fovY;
        d.viewSpaceToMetersFactor = 1.f;
        d.flags = 0;

        const ffxReturnCode_t code = ffx->Dispatch(&m_context, &d.header);
        // FSR bound its own pipelines and descriptor sets: the layer applies the engine's again.
        xrvk::EndNative(imm);
        if (code != FFX_API_RETURN_OK)
        {
            Msg("! [FSR] Vulkan dispatch failed (%u)", u32(code));
            return false;
        }
        return true;
    }

    void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) override
    {
        if (quality < 1)
        {
            renderW = displayW;
            renderH = displayH;
            return;
        }
        if (const ffxFunctions* ffx = da_ffx_vk())
        {
            uint32_t w = 0, h = 0;
            ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
            query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
            query.displayWidth = displayW;
            query.displayHeight = displayH;
            query.qualityMode = fsr_quality_mode(quality);
            query.pOutRenderWidth = &w;
            query.pOutRenderHeight = &h;
            if (ffx->Query(nullptr, &query.header) == FFX_API_RETURN_OK && w && h)
            {
                renderW = w;
                renderH = h;
                return;
            }
        }
        da_upscaler_default_render_size(UpscalerKind::FSR, quality, displayW, displayH, renderW, renderH);
    }

    pcstr Name() override { return "FSR 3.1 (Vulkan)"; }
};

IUpscaler* fsr_vk_create() { return xr_new<fsr_vk_upscaler>(); }

// FSR runs on any Vulkan device the renderer runs on; only its runtime may be missing.
bool fsr_vk_supported() { return da_ffx_vk() != nullptr; }

void fsr_vk_caps(UpscalerCaps& caps)
{
    if (HW.pDevice)
        caps.fsr = fsr_vk_supported();
}

const da_upscaler_registrar s_fsr_vk{ { UpscalerKind::FSR, 13, "FSR (Vulkan)", &fsr_vk_create, &fsr_vk_supported } };

const struct fsr_vk_caps_registrar
{
    fsr_vk_caps_registrar() { da_upscaler_register_caps_hook(&fsr_vk_caps); }
} s_fsr_vk_caps;
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_VULKAN
