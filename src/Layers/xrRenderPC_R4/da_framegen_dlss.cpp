// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// NVIDIA DLSS frame generation through Streamline (sl.dlss_g), DirectX 12 only: the IFrameGen
// backend for r__framegen st_fg_dlss (2x), st_fg_dlss_x3 and st_fg_dlss_x4. sl.dlss_g takes the
// swap chain over (Provider) and generates at every Present from the tagged depth, motion vectors
// and the hudless frame; Reflex low latency runs with it (DLSS-G requires it).

#include "stdafx.h"

#ifdef XR_D3D12
#include "da_framegen.h"
#include "da_streamline_sl.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// How often the runtime status is read (cheap, but not needed every frame).
constexpr u32 da_dlssg_status_interval = 60;

class dlssg_framegen final : public IFrameGen
{
    u32 m_generated{ 1 };
    bool m_initialised{};
    bool m_active{};

    // What DLSS-G was last told: options are sent only when they change.
    bool m_applied{};
    bool m_appliedOn{};
    u32 m_appliedFrames{};

    u32 m_statusCountdown{};
    u32 m_lastStatus{};

    void apply(bool on)
    {
        const StreamlineFunctions& fn = da_sl();
        if (!fn.dlssgSetOptions)
            return;
        if (m_applied && m_appliedOn == on && m_appliedFrames == m_generated)
            return;
        sl::DLSSGOptions options{};
        options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = m_generated;
        // Menus switch it off and on again: its resources stay meanwhile, coming back is smooth.
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        const sl::Result result = fn.dlssgSetOptions(sl::ViewportHandle{ da_sl_viewport_id }, options);
        if (result != sl::Result::eOk)
            da_sl_report("slDLSSGSetOptions", result);
        m_applied = true;
        m_appliedOn = on;
        m_appliedFrames = m_generated;
    }

    // Nothing valid to generate from (menus, loading): the tags must not point at stale frames.
    void clear_tags()
    {
        const sl::FrameToken* token = da_sl_frame();
        if (!token)
            return;
        constexpr auto lifecycle = sl::ResourceLifecycle::eValidUntilPresent;
        const sl::ResourceTag tags[] = {
            { nullptr, sl::kBufferTypeDepth, lifecycle },
            { nullptr, sl::kBufferTypeMotionVectors, lifecycle },
            { nullptr, sl::kBufferTypeHUDLessColor, lifecycle },
        };
        const sl::Result result = slSetTagForFrame(*token, sl::ViewportHandle{ da_sl_viewport_id }, tags,
            u32(std::size(tags)), nullptr);
        if (result != sl::Result::eOk)
            da_sl_report("slSetTagForFrame(DLSS-G, none)", result);
    }

    // A failing DLSS-G draws a pink overlay: the reason goes to the log once per change.
    void check_status()
    {
        if (m_statusCountdown--)
            return;
        m_statusCountdown = da_dlssg_status_interval;
        const StreamlineFunctions& fn = da_sl();
        sl::DLSSGState state{};
        if (!fn.dlssgGetState || fn.dlssgGetState(sl::ViewportHandle{ da_sl_viewport_id }, state, nullptr) != sl::Result::eOk)
            return;
        const u32 status = u32(state.status);
        if (status == m_lastStatus)
            return;
        m_lastStatus = status;
        if (status)
        {
            Msg("! [DLSS-G] runtime status 0x%x:%s%s%s%s%s", status,
                status & u32(sl::DLSSGStatus::eFailResolutionTooLow) ? " resolution too low" : "",
                status & u32(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) ? " Reflex not detected" : "",
                status & u32(sl::DLSSGStatus::eFailHDRFormatNotSupported) ? " HDR format" : "",
                status & u32(sl::DLSSGStatus::eFailCommonConstantsInvalid) ? " constants invalid" : "",
                status & u32(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) ? " no GetCurrentBackBufferIndex" : "");
        }
        else
            Msg("* [DLSS-G] runtime status ok");
    }

public:
    ~dlssg_framegen() override { Shutdown(); }

    pcstr Name() const override { return "DLSS frame generation (Streamline)"; }

    xrd3d12::SwapChainProvider* Provider() override { return da_streamline_dlssg_provider(); }

    bool Init(u32 displayW, u32 displayH, u32 generated) override
    {
        Shutdown();
        if (!da_streamline_dlssg() || !da_sl().dlssgSetOptions)
            return false;
        // The swap chain came through Provider(), which loaded sl.dlss_g already.
        if (!da_streamline_dlssg_load(true))
            return false;
        m_generated = std::clamp(generated, 1u, std::max(da_streamline_dlssg_max(), 1u));
        m_initialised = true;
        m_active = false;
        m_applied = false;
        m_statusCountdown = da_dlssg_status_interval;
        m_lastStatus = 0;
        // DLSS-G needs Reflex; its limiter replaces the engine's while frames are generated.
        da_streamline_low_latency(true);
        apply(false);
        Msg("* [DLSS-G] ready: %ux%u, %ux", displayW, displayH, m_generated + 1);
        return true;
    }

    void Shutdown() override
    {
        if (!m_initialised)
            return;
        apply(false);
        clear_tags();
        const sl::Result result = slFreeResources(sl::kFeatureDLSS_G, sl::ViewportHandle{ da_sl_viewport_id });
        if (result != sl::Result::eOk)
            da_sl_report("slFreeResources(DLSS-G)", result);
        da_streamline_low_latency(false);
        // Out of every later swap chain; waits until this backend's chain is gone.
        da_streamline_dlssg_load(false);
        m_initialised = false;
        m_active = false;
    }

    void Prepare(const FrameGenFrame& frame) override
    {
        const sl::FrameToken* token = da_sl_frame();
        if (!m_initialised || !m_active || !token || !frame.depth || !frame.velocity || !frame.hudless)
            return;

        // The constants of the frame: DLSS Super Resolution may have set them already (the same).
        StreamlineCamera camera;
        camera.view = frame.view;
        camera.project = frame.project;
        camera.prevView = frame.prevView;
        camera.prevProject = frame.prevProject;
        camera.nearZ = frame.nearZ;
        camera.farZ = frame.farZ;
        camera.fovY = frame.fovY;
        camera.renderW = frame.renderW;
        camera.renderH = frame.renderH;
        camera.jitterX = frame.jitterX;
        camera.jitterY = frame.jitterY;
        camera.reset = frame.reset;
        if (!da_sl_constants(camera))
            return;

        // DLSS-G reads its inputs at Present, after the UI changed the targets around them:
        // tagged as valid only now, Streamline copies them on this list. The copies read from the
        // layer's shader-read state, which the tags state exactly.
        constexpr u32 read = u32(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D11DeviceContext* immediate = HW.get_context(CHW::IMM_CTX_ID);
        ID3D12GraphicsCommandList* list = xrd3d12::BeginNative(immediate);
        if (!list)
            return;
        for (ID3D11Resource* input : { frame.depth, frame.velocity, frame.hudless })
            xrd3d12::Require(immediate, input, D3D12_RESOURCE_STATES(read));

        sl::Resource depth = da_sl_texture(frame.depth, read);
        sl::Resource motion = da_sl_texture(frame.velocity, read);
        sl::Resource hudless = da_sl_texture(frame.hudless, read);
        const sl::Extent renderExtent{ 0, 0, frame.renderW, frame.renderH };
        const sl::Extent displayExtent{ 0, 0, frame.displayW, frame.displayH };
        constexpr auto lifecycle = sl::ResourceLifecycle::eOnlyValidNow;
        const sl::ResourceTag tags[] = {
            { &depth, sl::kBufferTypeDepth, lifecycle, &renderExtent },
            { &motion, sl::kBufferTypeMotionVectors, lifecycle, &renderExtent },
            { &hudless, sl::kBufferTypeHUDLessColor, lifecycle, &displayExtent },
        };
        const sl::Result result = slSetTagForFrame(*token, sl::ViewportHandle{ da_sl_viewport_id }, tags,
            u32(std::size(tags)), list);
        // Streamline recorded its copies; the layer re-applies its bindings.
        xrd3d12::EndNative(immediate);
        if (result != sl::Result::eOk)
            da_sl_report("slSetTagForFrame(DLSS-G)", result);

        check_status();
    }

    void SetActive(bool active) override
    {
        if (!m_initialised)
            return;
        // Off takes effect at the next Present, which then shows the rendered frame alone.
        if (!active && m_active)
            clear_tags();
        m_active = active;
        apply(active);
    }
};

IFrameGen* dlssg_create() { return xr_new<dlssg_framegen>(); }

// Known once the device is: RTX 40 or newer, HAGS on, a recent driver, Windows 10 20H1+.
bool dlssg_supported() { return da_streamline_dlssg(); }

// 1 = 2x (RTX 40), 3 = 4x (RTX 50): what r__framegen may ask for.
u32 dlssg_max_generated() { return da_streamline_dlssg_max(); }

const da_framegen_registrar s_dlssg_2x{ { 2, "DLSS-G 2x", &dlssg_create, &dlssg_supported, &dlssg_max_generated } };
const da_framegen_registrar s_dlssg_3x{ { 3, "DLSS-G 3x", &dlssg_create, &dlssg_supported, &dlssg_max_generated } };
const da_framegen_registrar s_dlssg_4x{ { 4, "DLSS-G 4x", &dlssg_create, &dlssg_supported, &dlssg_max_generated } };
} // namespace
} // namespace xray::render::RENDER_NAMESPACE
#endif // XR_D3D12
