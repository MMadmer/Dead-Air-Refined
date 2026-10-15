// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// AMD FSR upscaling on DirectX 12 (docs/dead-air/DX12_RENDERER.md, 1 and 3) through the AMD FSR
// SDK 2.3 signed DLLs (ffx-api): the loader picks FSR 4 on RX 7000/9000 and FSR 3.1.5 elsewhere.
// DX12 module only; the DX11 module runs the FSR 3.1.2 port (da_upscaler_fsr_dx11.cpp). The work is
// recorded into the immediate context's native command list inside an xrD3D12 native section.

#include "stdafx.h"

#ifdef XR_D3D12
#include "da_upscaler.h"
#include "da_ffx_loader.h"
#include "Layers/xrRenderDX12/d12_api.h"

#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
ID3D12Device* fsr_device() { return HW.pDevice ? xrd3d12::NativeDevice(HW.pDevice) : nullptr; }

// r__fsr: 1 native AA, 2 quality, 3 balanced, 4 performance, 5 ultra performance (0 = off).
u32 fsr_quality_mode(int quality)
{
    static constexpr u32 modes[] = { FFX_UPSCALE_QUALITY_MODE_NATIVEAA, FFX_UPSCALE_QUALITY_MODE_NATIVEAA,
        FFX_UPSCALE_QUALITY_MODE_QUALITY, FFX_UPSCALE_QUALITY_MODE_BALANCED, FFX_UPSCALE_QUALITY_MODE_PERFORMANCE,
        FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE };
    return modes[std::clamp(quality, 0, 5)];
}

// The runtime's messages; with FFX_UPSCALE_ENABLE_DEBUG_CHECKING (-dxdebug) it also validates
// sizes, depth convention and jitter range.
void fsr_message(uint32_t type, const wchar_t* message)
{
    string512 text;
    const int written = WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    text[written > 0 ? written : 0] = 0;
    Msg("%s [FSR] %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "!" : "~", text);
}

bool fsr_dx12_supported()
{
    return da_ffx_providers(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, fsr_device(), "upscaling") > 0;
}

class fsr_dx12_upscaler final : public IUpscaler
{
    ffxContext m_context{};
    u32 m_displayW{}, m_displayH{};
    bool m_warnedVelocity{};
    string64 m_name{ "FSR (DX12)" };

public:
    ~fsr_dx12_upscaler() override { Shutdown(); }

    bool Init(u32 displayW, u32 displayH, int /*quality*/) override
    {
        Shutdown();
        const ffxFunctions* ffx = da_ffx();
        ID3D12Device* device = fsr_device();
        if (!ffx || !device || !displayW || !displayH)
            return false;

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;

        // Mandatory since SDK 2.1: the API version this code was written against.
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_UPSCALER_VERSION;

        // The input is tonemapped LDR with standard, finite depth: no HDR, no auto exposure, no
        // inverted or infinite depth. The maximum render size is the display, so the quality can
        // change without a new context.
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &version.header;
        desc.flags = strstr(Core.Params, "-dxdebug") ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0u;
        desc.maxRenderSize = { displayW, displayH };
        desc.maxUpscaleSize = { displayW, displayH };
        desc.fpMessage = &fsr_message;

        const ffxReturnCode_t code = ffx->CreateContext(&m_context, &desc.header, nullptr);
        if (code != FFX_API_RETURN_OK || !m_context)
        {
            Msg("! [FSR] context creation failed (%u)", u32(code));
            m_context = nullptr;
            return false;
        }
        m_displayW = displayW;
        m_displayH = displayH;
        m_warnedVelocity = false;

        // Which provider the loader chose: FSR 4 or FSR 3.1 is only known from here.
        ffxQueryGetProviderVersion provider{};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (ffx->Query(&m_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
            xr_sprintf(m_name, "FSR %s (DX12)", provider.versionName);
        Msg("* [FSR] ready through [%s]: %ux%u -> %ux%u", m_name, Device.dwRenderWidth, Device.dwRenderHeight,
            displayW, displayH);
        return true;
    }

    void Shutdown() override
    {
        if (!m_context)
            return;
        // Recorded and in-flight work still references the context's resources.
        if (ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID))
            xrd3d12::Flush(imm, true);
        if (const ffxFunctions* ffx = da_ffx())
        {
            const ffxReturnCode_t code = ffx->DestroyContext(&m_context, nullptr);
            if (code != FFX_API_RETURN_OK)
                Msg("! [FSR] context destruction failed (%u)", u32(code));
        }
        m_context = nullptr;
        xr_strcpy(m_name, "FSR (DX12)");
    }

    bool Dispatch(const UpscaleFrame& frame) override
    {
        const ffxFunctions* ffx = da_ffx();
        if (!m_context || !ffx)
            return false;
        if (frame.renderW > m_displayW || frame.renderH > m_displayH || frame.displayW > m_displayW ||
            frame.displayH > m_displayH)
        {
            Msg("! [FSR] frame %ux%u -> %ux%u exceeds the context (%ux%u)", frame.renderW, frame.renderH,
                frame.displayW, frame.displayH, m_displayW, m_displayH);
            return false;
        }

        ID3D12Resource* color = xrd3d12::NativeResource(frame.color);
        ID3D12Resource* depth = xrd3d12::NativeResource(frame.depth);
        ID3D12Resource* velocity = xrd3d12::NativeResource(frame.velocity);
        ID3D12Resource* reactive = xrd3d12::NativeResource(frame.reactive);
        ID3D12Resource* output = xrd3d12::NativeResource(frame.output);
        if (!color || !depth || !output)
            return false;
        if (!(output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        {
            Msg("! [FSR] the output target has no unordered access");
            return false;
        }
        if (!velocity && !m_warnedVelocity)
        {
            Msg("! [FSR] no motion vectors: the frame is reconstructed as if nothing moved");
            m_warnedVelocity = true;
        }

        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(imm);
        if (!list)
            return false;

        // The layer tracks the states; FSR returns every resource to the state it was given.
        constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        xrd3d12::Require(imm, frame.color, read);
        xrd3d12::Require(imm, frame.depth, read);
        if (velocity)
            xrd3d12::Require(imm, frame.velocity, read);
        if (reactive)
            xrd3d12::Require(imm, frame.reactive, read);
        xrd3d12::Require(imm, frame.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = list;
        d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.reactive = ffxApiGetResourceDX12(reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

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
        // FSR set its own heaps, root signature and pipeline: the layer re-applies the engine's.
        // The engine's cached state is invalidated by the caller (phase_upscale).
        xrd3d12::EndNative(imm);
        if (code != FFX_API_RETURN_OK)
        {
            Msg("! [FSR] dispatch failed (%u)", u32(code));
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
        // Without a context the loader answers with its default provider; FSR 3.1 and 4 share the ratios.
        if (const ffxFunctions* ffx = da_ffx())
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

    pcstr Name() override { return m_name; }
};

IUpscaler* fsr_dx12_create() { return xr_new<fsr_dx12_upscaler>(); }

// r__caps fsr on DX12: the loader is there and has an upscaling provider for this adapter. Before
// the device exists nothing can be asked (the menu reads the caps after it). fsrfg belongs to the
// frame generation stage.
void fsr_dx12_caps(UpscalerCaps& caps)
{
    if (fsr_device())
        caps.fsr = fsr_dx12_supported();
}

const da_upscaler_registrar s_fsr_dx12{ { UpscalerKind::FSR, 12, "FSR (DX12)", &fsr_dx12_create,
    &fsr_dx12_supported } };

const struct fsr_dx12_caps_registrar
{
    fsr_dx12_caps_registrar() { da_upscaler_register_caps_hook(&fsr_dx12_caps); }
} s_fsr_dx12_caps;
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_D3D12
