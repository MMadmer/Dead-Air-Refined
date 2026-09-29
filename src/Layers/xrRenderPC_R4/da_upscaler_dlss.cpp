// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// NVIDIA DLSS Super Resolution and DLAA through Streamline (sl.dlss), on both renderer modules
// (docs/dead-air/DX12_RENDERER.md, 1 and 3): the upscale slot's backend for r__dlss. The input is
// the tonemapped LDR scene; the common constants it sets are the frame's, DLSS-G reads them too.

#include "stdafx.h"

#include "da_upscaler.h"
#include "da_streamline_sl.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
#ifdef XR_D3D12
constexpr u32 da_dlss_api = 12;
constexpr pcstr da_dlss_name = "DLSS (Streamline, DX12)";
#else
constexpr u32 da_dlss_api = 11;
constexpr pcstr da_dlss_name = "DLSS (Streamline, DX11)";
#endif

// r__dlss: 1 DLAA, 2 quality, 3 balanced, 4 performance, 5 ultra performance.
sl::DLSSMode dlss_mode(int quality)
{
    switch (quality)
    {
    case 1: return sl::DLSSMode::eDLAA;
    case 2: return sl::DLSSMode::eMaxQuality;
    case 3: return sl::DLSSMode::eBalanced;
    case 4: return sl::DLSSMode::eMaxPerformance;
    case 5: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eOff;
    }
}

// Presets stay NVIDIA's defaults per mode (they follow the DLSS model updates).
sl::DLSSOptions dlss_options(int quality, u32 displayW, u32 displayH)
{
    sl::DLSSOptions options{};
    options.mode = dlss_mode(quality);
    options.outputWidth = displayW;
    options.outputHeight = displayH;
    // Tonemapped RGBA8: no HDR; exposure 1 comes through the tagged exposure texture.
    options.colorBuffersHDR = sl::Boolean::eFalse;
    options.preExposure = 1.f;
    options.exposureScale = 1.f;
    options.useAutoExposure = sl::Boolean::eFalse;
    return options;
}

class dlss_upscaler final : public IUpscaler
{
    int m_quality{};
    u32 m_displayW{}, m_displayH{};
    bool m_ready{};
    // 1x1 R32F holding 1.0: the input is tonemapped. Without an exposure texture DLSS runs its own
    // auto-exposure, and one black first frame (a renderer reset) left it at NaN for good - the
    // output stayed black on D3D11 until the feature was created again.
    ID3D11Texture2D* m_exposure{};

    // Last frame's camera, unjittered (the constants reproject through it).
    Fmatrix m_prevView{}, m_prevProject{};
    u32 m_prevFrame{};
    bool m_prevValid{};

    bool create_exposure()
    {
        if (m_exposure)
            return true;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        static constexpr float one = 1.f;
        const D3D11_SUBRESOURCE_DATA data{ &one, sizeof(one), sizeof(one) };
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, &data, &m_exposure)))
        {
            m_exposure = nullptr;
            Msg("! [DLSS] the exposure texture could not be created");
            return false;
        }
        return true;
    }

    void release()
    {
        const StreamlineFunctions& fn = da_sl();
        if (m_ready && da_streamline_dlss() && fn.dlssSetOptions)
        {
            sl::DLSSOptions off{};
            off.mode = sl::DLSSMode::eOff;
            fn.dlssSetOptions(sl::ViewportHandle{ da_sl_viewport_id }, off);
#ifdef XR_D3D12
            // The GPU may still run the last evaluate on the feature's own resources.
            xrd3d12::Flush(HW.get_context(CHW::IMM_CTX_ID), true);
#endif
            const sl::Result result = slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle{ da_sl_viewport_id });
            if (result != sl::Result::eOk)
                da_sl_report("slFreeResources(DLSS)", result);
        }
        m_ready = false;
        _RELEASE(m_exposure);
    }

public:
    ~dlss_upscaler() override { release(); }

    bool Init(u32 displayW, u32 displayH, int quality) override
    {
        release();
        const StreamlineFunctions& fn = da_sl();
        if (!da_streamline_dlss() || !fn.dlssSetOptions || !displayW || !displayH || !create_exposure())
            return false;
        const sl::Result result = fn.dlssSetOptions(sl::ViewportHandle{ da_sl_viewport_id },
            dlss_options(quality, displayW, displayH));
        if (result != sl::Result::eOk)
        {
            da_sl_report("slDLSSSetOptions", result);
            return false;
        }
        m_quality = quality;
        m_displayW = displayW;
        m_displayH = displayH;
        m_prevValid = false;
        m_ready = true;
        Msg("* [DLSS] ready: %ux%u -> %ux%u", Device.dwRenderWidth, Device.dwRenderHeight, displayW, displayH);
        return true;
    }

    void Shutdown() override { release(); }

    bool Dispatch(const UpscaleFrame& frame) override
    {
        const StreamlineFunctions& fn = da_sl();
        const sl::FrameToken* token = da_sl_frame();
        if (!m_ready || !m_exposure || !token || !fn.dlssSetOptions || !frame.color || !frame.depth || !frame.output)
            return false;
        // A frame drawn without motion vectors (the save screenshot under the pause menu) has
        // nothing to reconstruct from: it is stretched, and the history starts over on the next
        // frame that has them. Not a failure - that would end DLSS for the session.
        if (!frame.velocity)
        {
            RImplementation.Target->phase_upscale_identity();
            m_prevValid = false;
            return true;
        }

        // The camera of this frame and the last one, both unjittered (Device.mProject never is).
        const bool contiguous = m_prevValid && m_prevFrame + 1 == Device.dwFrame;
        StreamlineCamera camera;
        camera.view = Device.mView;
        camera.project = Device.mProject;
        camera.prevView = contiguous ? m_prevView : Device.mView;
        camera.prevProject = contiguous ? m_prevProject : Device.mProject;
        camera.nearZ = frame.nearZ;
        camera.farZ = frame.farZ;
        camera.fovY = frame.fovY;
        camera.renderW = frame.renderW;
        camera.renderH = frame.renderH;
        camera.jitterX = frame.jitterX;
        camera.jitterY = frame.jitterY;
        camera.reset = frame.reset || !contiguous;
        m_prevView = Device.mView;
        m_prevProject = Device.mProject;
        m_prevFrame = Device.dwFrame;
        m_prevValid = true;
        if (!da_sl_constants(camera))
            return false;

        const sl::ViewportHandle viewport{ da_sl_viewport_id };
        sl::Result result = fn.dlssSetOptions(viewport, dlss_options(m_quality, frame.displayW, frame.displayH));
        if (result != sl::Result::eOk)
        {
            da_sl_report("slDLSSSetOptions", result);
            return false;
        }

        ID3D11DeviceContext* immediate = HW.get_context(CHW::IMM_CTX_ID);
#ifdef XR_D3D12
        // The layer puts the inputs into its shader-read state (pixel | non-pixel: a lone
        // non-pixel request merges into it) and the output into UAV; Streamline gets exactly those.
        constexpr u32 read = u32(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        constexpr u32 write = u32(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(immediate);
        if (!list)
            return false;
        for (ID3D11Resource* input : { frame.color, frame.depth, frame.velocity, frame.reactive, static_cast<ID3D11Resource*>(m_exposure) })
            xrd3d12::Require(immediate, input, D3D12_RESOURCE_STATES(read));
        xrd3d12::Require(immediate, frame.output, D3D12_RESOURCE_STATES(write));
        sl::CommandBuffer* command = list;
#else
        // D3D11 resources carry no states.
        constexpr u32 read = 0, write = 0;
        sl::CommandBuffer* command = immediate;
#endif

        sl::Resource color = da_sl_texture(frame.color, read);
        sl::Resource depth = da_sl_texture(frame.depth, read);
        sl::Resource motion = da_sl_texture(frame.velocity, read);
        sl::Resource output = da_sl_texture(frame.output, write);
        sl::Resource exposure = da_sl_texture(m_exposure, read);
        sl::Resource reactive = da_sl_texture(frame.reactive, read);
        const sl::Extent renderExtent{ 0, 0, frame.renderW, frame.renderH };
        const sl::Extent displayExtent{ 0, 0, frame.displayW, frame.displayH };
#ifdef XR_D3D12
        // Local tags: valid for this evaluate only, nothing is copied.
        constexpr auto lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate;
#else
        // D3D11: DLSS reads nothing out of the engine's own targets (a black frame, no error at
        // all); it works on the copies Streamline takes of frame tags that are only valid now.
        constexpr auto lifecycle = sl::ResourceLifecycle::eOnlyValidNow;
#endif
        sl::ResourceTag tags[] = {
            { &color, sl::kBufferTypeScalingInputColor, lifecycle, &renderExtent },
            { &depth, sl::kBufferTypeDepth, lifecycle, &renderExtent },
            { &motion, sl::kBufferTypeMotionVectors, lifecycle, &renderExtent },
            { &output, sl::kBufferTypeScalingOutputColor, lifecycle, &displayExtent },
            { &exposure, sl::kBufferTypeExposure, lifecycle, nullptr },
            // The reactive mask: the pixels whose history is worth little (lerp towards current).
            { &reactive, sl::kBufferTypeBiasCurrentColorHint, lifecycle, &renderExtent },
        };
        const u32 tagCount = frame.reactive ? u32(std::size(tags)) : u32(std::size(tags)) - 1;
        const sl::BaseStructure* inputs[std::size(tags) + 1] = { &viewport };
        u32 inputCount = 1;
#ifdef XR_D3D12
        for (u32 i = 0; i < tagCount; ++i)
            inputs[inputCount++] = &tags[i];
#else
        // Local tags are never copied: the copies need the frame's tags.
        result = slSetTagForFrame(*token, viewport, tags, tagCount, command);
        if (result != sl::Result::eOk)
        {
            da_sl_report("slSetTagForFrame(DLSS)", result);
            return false;
        }
#endif

        result = slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, inputCount, command);
#ifdef XR_D3D12
        // Streamline changed the list's bindings; the layer applies its own again.
        xrd3d12::EndNative(immediate);
#else
        // D3D11: the output stays bound as a compute UAV after the evaluate, and D3D11 hands a
        // resource bound for writing to every later reader as zeros (the frame came out black).
        ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
        ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
        ID3D11Buffer* cbs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT]{};
        ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
        const u32 uavSlots = HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
        immediate->CSSetShader(nullptr, nullptr, 0);
        immediate->CSSetUnorderedAccessViews(0, uavSlots, uavs, nullptr);
        immediate->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);
        immediate->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cbs);
        immediate->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
#endif
        // The engine's cached state is invalidated by the caller (phase_upscale).
        if (result != sl::Result::eOk)
        {
            da_sl_report("slEvaluateFeature(DLSS)", result);
            return false;
        }
        return true;
    }

    void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) override
    {
        const StreamlineFunctions& fn = da_sl();
        sl::DLSSOptimalSettings settings{};
        if (fn.dlssGetOptimalSettings &&
            fn.dlssGetOptimalSettings(dlss_options(quality, displayW, displayH), settings) == sl::Result::eOk &&
            settings.optimalRenderWidth && settings.optimalRenderHeight)
        {
            renderW = settings.optimalRenderWidth;
            renderH = settings.optimalRenderHeight;
            return;
        }
        da_upscaler_default_render_size(UpscalerKind::DLSS, quality, displayW, displayH, renderW, renderH);
    }

    pcstr Name() override { return da_dlss_name; }
};

IUpscaler* dlss_create() { return xr_new<dlss_upscaler>(); }

// Known once the device is (slSetD3DDevice); a saved r__dlss is switched off where it is false.
bool dlss_supported() { return da_streamline_dlss(); }

const da_upscaler_registrar s_dlss{ { UpscalerKind::DLSS, da_dlss_api, "DLSS (Streamline)", &dlss_create,
    &dlss_supported } };
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
