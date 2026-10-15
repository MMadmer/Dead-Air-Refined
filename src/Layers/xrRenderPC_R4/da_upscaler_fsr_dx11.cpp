// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// AMD FSR 3.1.2 upscaler on DirectX 11 (docs/dead-air/DX12_RENDERER.md, 1 and 3): the MSVC build
// of the optiscaler/FidelityFX-SDK-DX11 port, static libraries (Externals/fidelityfx-dx11). DX11
// module only; the DX12 module runs FSR through AMD's signed DLLs. Kept in its own translation
// unit: these headers and the FSR SDK 2.3.0 ones both define FfxFrameGenerationConfig.

#include "stdafx.h"

#ifndef XR_D3D12
#include "da_upscaler.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// MSVC's exception code for a thrown C++ object.
constexpr DWORD da_cpp_exception_code = 0xE06D7363;

// The backend reports a failed D3D11 call by throwing (TIF in ffx_dx11.cpp) through a C interface:
// /EHsc assumes extern "C" never throws and ReleaseMasterGold builds without C++ exceptions, so the
// throw is caught as a structured exception and becomes an error code (the native path takes over).
template <typename Call>
FfxErrorCode fsr_guarded(const Call& call)
{
    __try
    {
        return call();
    }
    __except (GetExceptionCode() == da_cpp_exception_code ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return FFX_ERROR_BACKEND_API_ERROR;
    }
}

u32 fsr_code(FfxErrorCode code) { return u32(code); }

// Only called with FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING (debug builds): the library validates
// sizes, depth convention and jitter range far better than a wrong picture tells.
void fsr_message(FfxMsgType type, const wchar_t* message)
{
    string512 text;
    const int written = WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text) - 1, nullptr, nullptr);
    text[written > 0 ? written : 0] = 0;
    Msg("%s [FSR3] %s", type == FFX_MESSAGE_TYPE_ERROR ? "!" : "~", text);
}

// The backend leaves its compute shader and constant buffers bound on the context it was given,
// and restores the game's compute samplers; nothing of it may survive into the next pass. Before
// the dispatch the same reset makes an input FSR binds as null read zeros instead of a stale view.
void fsr_unbind_compute(ID3D11DeviceContext* ctx)
{
    ID3D11UnorderedAccessView* uavs[FFX_MAX_NUM_UAVS]{};
    ID3D11ShaderResourceView* srvs[FFX_MAX_NUM_SRVS]{};
    ID3D11Buffer* cbs[FFX_MAX_NUM_CONST_BUFFERS]{};
    ID3D11SamplerState* samplers[FFX_MAX_SAMPLERS]{};
    // Feature level 11_0 has 8 compute UAV slots; a wider range is rejected there.
    const UINT uav_slots = HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_1 ? FFX_MAX_NUM_UAVS : D3D11_PS_CS_UAV_REGISTER_COUNT;

    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, uav_slots, uavs, nullptr);
    ctx->CSSetShaderResources(0, FFX_MAX_NUM_SRVS, srvs);
    ctx->CSSetConstantBuffers(0, FFX_MAX_NUM_CONST_BUFFERS, cbs);
    ctx->CSSetSamplers(0, FFX_MAX_SAMPLERS, samplers);
}

FfxResource fsr_resource(ID3D11Resource* resource, FfxResourceStates state = FFX_RESOURCE_STATE_COMPUTE_READ)
{
    if (!resource)
        return ffxGetResourceDX11_Fsr31(nullptr, FfxResourceDescription{}, nullptr, state);
    return ffxGetResourceDX11_Fsr31(resource, GetFfxResourceDescriptionDX11(resource), nullptr, state);
}

class fsr_dx11_upscaler final : public IUpscaler
{
    FfxFsr3UpscalerContext m_context{}; // 512 KB: the object lives on the heap (xr_new)
    FfxInterface m_backend{};
    void* m_scratch{};
    bool m_created{};
    u32 m_displayW{}, m_displayH{};

    // The three buffers FSR 3.1 shares with the effects after it (frame interpolation, unused
    // here) are the application's. Exactly the render size: the passes Load() them by render
    // pixel, and the sibling project saw dispatch throw with them at the maximum size.
    ID3D11Texture2D* m_dilatedDepth{};
    ID3D11Texture2D* m_dilatedMotion{};
    ID3D11Texture2D* m_prevNearestDepth{};
    u32 m_sharedW{}, m_sharedH{};

    ID3D11Resource* m_checkedOutput{};
    bool m_warnedVelocity{};

    // Formats and usage as ffxFsr3UpscalerGetSharedResourceDescriptions describes them.
    bool create_shared(u32 w, u32 h)
    {
        release_shared();
        const auto make = [w, h](DXGI_FORMAT format, bool render_target, ID3D11Texture2D** out)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
                (render_target ? D3D11_BIND_RENDER_TARGET : 0u);
            return SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, out));
        };
        if (!make(DXGI_FORMAT_R32_FLOAT, true, &m_dilatedDepth) ||
            !make(DXGI_FORMAT_R16G16_FLOAT, true, &m_dilatedMotion) ||
            !make(DXGI_FORMAT_R32_UINT, false, &m_prevNearestDepth))
        {
            Msg("! [FSR3] cannot create the shared buffers (%ux%u)", w, h);
            release_shared();
            return false;
        }
        m_sharedW = w;
        m_sharedH = h;
        return true;
    }

    void release_shared()
    {
        _RELEASE(m_dilatedDepth);
        _RELEASE(m_dilatedMotion);
        _RELEASE(m_prevNearestDepth);
        m_sharedW = m_sharedH = 0;
    }

    void release()
    {
        if (m_created)
        {
            const FfxErrorCode code = fsr_guarded([this] { return ffxFsr3UpscalerContextDestroy(&m_context); });
            if (code != FFX_OK)
                Msg("! [FSR3] context destruction failed (0x%08x)", fsr_code(code));
            m_created = false;
        }
        release_shared();
        if (m_scratch)
        {
            xr_free(m_scratch);
            m_scratch = nullptr;
        }
        m_backend = {};
        m_checkedOutput = nullptr;
    }

    // rt_Upscaled asks for a UAV, but the render target only grants it where the format allows
    // typed UAV stores; without one the accumulate pass would write nowhere.
    bool output_writable(ID3D11Resource* output)
    {
        if (output == m_checkedOutput)
            return true;
        D3D11_RESOURCE_DIMENSION dimension{};
        output->GetType(&dimension);
        D3D11_TEXTURE2D_DESC desc{};
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            static_cast<ID3D11Texture2D*>(output)->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
        {
            Msg("! [FSR3] the output target has no unordered access view");
            return false;
        }
        m_checkedOutput = output;
        return true;
    }

public:
    ~fsr_dx11_upscaler() override { release(); }

    bool Init(u32 displayW, u32 displayH, int /*quality*/) override
    {
        release();
        if (!HW.pDevice || !displayW || !displayH)
            return false;

        // The backend refuses a scratch block whose context header is not zero.
        const size_t scratch_size = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
        m_scratch = xr_malloc(scratch_size);
        std::memset(m_scratch, 0, scratch_size);

        FfxErrorCode code = ffxGetInterfaceDX11(&m_backend, ffxGetDeviceDX11_Fsr31(HW.pDevice), m_scratch,
            scratch_size, FFX_FSR3UPSCALER_CONTEXT_COUNT);
        if (code != FFX_OK)
        {
            Msg("! [FSR3] cannot set up the DX11 backend (0x%08x)", fsr_code(code));
            release();
            return false;
        }

        // The input is tonemapped LDR with standard depth: no HDR, no exposure, no inverted or
        // infinite depth. The maximum render size is the display, so the quality can change
        // without a new context.
        FfxFsr3UpscalerContextDescription desc{};
        desc.flags = 0;
#ifdef DEBUG
        desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
#endif
        desc.maxRenderSize = { displayW, displayH };
        desc.maxUpscaleSize = { displayW, displayH };
        desc.fpMessage = &fsr_message;
        desc.backendInterface = m_backend;

        code = fsr_guarded([this, &desc] { return ffxFsr3UpscalerContextCreate(&m_context, &desc); });
        if (code != FFX_OK)
        {
            Msg("! [FSR3] context creation failed (0x%08x)", fsr_code(code));
            release();
            return false;
        }
        m_created = true;
        m_displayW = displayW;
        m_displayH = displayH;
        m_warnedVelocity = false;

        if (!create_shared(Device.dwRenderWidth, Device.dwRenderHeight))
        {
            release();
            return false;
        }
        Msg("* [FSR3] ready: %ux%u -> %ux%u", Device.dwRenderWidth, Device.dwRenderHeight, displayW, displayH);
        return true;
    }

    void Shutdown() override { release(); }

    bool Dispatch(const UpscaleFrame& frame) override
    {
        if (!m_created || !frame.color || !frame.depth || !frame.output)
            return false;
        if (frame.renderW > m_displayW || frame.renderH > m_displayH || frame.displayW > m_displayW ||
            frame.displayH > m_displayH)
        {
            Msg("! [FSR3] frame %ux%u -> %ux%u exceeds the context (%ux%u)", frame.renderW, frame.renderH,
                frame.displayW, frame.displayH, m_displayW, m_displayH);
            return false;
        }

        // New shared buffers carry no history.
        bool reset = frame.reset;
        if (frame.renderW != m_sharedW || frame.renderH != m_sharedH)
        {
            if (!create_shared(frame.renderW, frame.renderH))
                return false;
            reset = true;
        }
        if (!output_writable(frame.output))
            return false;
        if (!frame.velocity && !m_warnedVelocity)
        {
            Msg("! [FSR3] no motion vectors: the frame is reconstructed as if nothing moved");
            m_warnedVelocity = true;
        }

        ID3D11DeviceContext* ctx = HW.get_context(CHW::IMM_CTX_ID);
        fsr_unbind_compute(ctx);

        FfxFsr3UpscalerDispatchDescription d{};
        d.commandList = ffxGetCommandListDX11(ctx);
        d.color = fsr_resource(frame.color);
        d.depth = fsr_resource(frame.depth);
        d.motionVectors = fsr_resource(frame.velocity);
        d.exposure = fsr_resource(nullptr);
        d.reactive = fsr_resource(frame.reactive);
        d.transparencyAndComposition = fsr_resource(nullptr);
        d.dilatedDepth = fsr_resource(m_dilatedDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.dilatedMotionVectors = fsr_resource(m_dilatedMotion, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.reconstructedPrevNearestDepth = fsr_resource(m_prevNearestDepth, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        d.output = fsr_resource(frame.output, FFX_RESOURCE_STATE_UNORDERED_ACCESS);

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
        d.reset = reset;
        d.cameraNear = frame.nearZ;
        d.cameraFar = frame.farZ;
        d.cameraFovAngleVertical = frame.fovY;
        d.viewSpaceToMetersFactor = 1.f;
        d.flags = 0;

        const FfxErrorCode code = fsr_guarded([this, &d] { return ffxFsr3UpscalerContextDispatch(&m_context, &d); });
        // The engine's cached state is invalidated by the caller (phase_upscale).
        fsr_unbind_compute(ctx);
        if (code != FFX_OK)
        {
            Msg("! [FSR3] dispatch failed (0x%08x)", fsr_code(code));
            return false;
        }
        return true;
    }

    void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) override
    {
        // r__fsr: 1 native AA, 2 quality, 3 balanced, 4 performance, 5 ultra performance.
        if (quality < 1)
        {
            renderW = displayW;
            renderH = displayH;
            return;
        }
        const auto mode = FfxFsr3UpscalerQualityMode(std::min(quality - 1, int(FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE)));
        if (ffxFsr3UpscalerGetRenderResolutionFromQualityMode(&renderW, &renderH, displayW, displayH, mode) != FFX_OK)
            da_upscaler_default_render_size(UpscalerKind::FSR, quality, displayW, displayH, renderW, renderH);
    }

    pcstr Name() override { return "FSR 3.1.2 (DX11)"; }
};

IUpscaler* fsr_dx11_create() { return xr_new<fsr_dx11_upscaler>(); }

// Compute shader model 5.0: every feature level 11 adapter.
bool fsr_dx11_supported() { return HW.pDevice && HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0; }

// r__caps fsr: off on a feature level 10 adapter, where FSR cannot run on DX11.
void fsr_dx11_caps(UpscalerCaps& caps) { caps.fsr = da_upscaler_support(UpscalerKind::FSR) != 0; }

const da_upscaler_registrar s_fsr_dx11{ { UpscalerKind::FSR, 11, "FSR 3.1 (DX11)", &fsr_dx11_create,
    &fsr_dx11_supported } };

const struct fsr_dx11_caps_registrar
{
    fsr_dx11_caps_registrar() { da_upscaler_register_caps_hook(&fsr_dx11_caps); }
} s_fsr_dx11_caps;
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // !XR_D3D12
