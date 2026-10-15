// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The frame generation manager (da_framegen.h): the registry of the backends (AMD FSR, NVIDIA
// DLSS), the choice from r__framegen, the swap chain the chosen one presents through, and the
// per-frame work - the hudless copy of the finished 3D frame and the backend's Prepare, or nothing
// while a menu, the loading screen or a pause is up. DirectX 12 only: the DirectX 11 module keeps
// the registry and answers "unavailable" to everything else.

#include "stdafx.h"

#include "da_framegen.h"

#ifdef XR_D3D12
#include "da_upscaler.h"
#include "da_streamline.h"
#include "Layers/xrRender/da_temporal.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrEngine/ILoadingScreen.h"
#include "xrEngine/XR_IOConsole.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Plain arrays: the backends register from static initialisers.
constexpr size_t da_max_framegen_backends = 8;
FrameGenBackend g_da_fg_backends[da_max_framegen_backends];
size_t g_da_fg_backend_count = 0;
} // namespace

void da_framegen_register(const FrameGenBackend& backend)
{
    R_ASSERT2(g_da_fg_backend_count < da_max_framegen_backends, "too many frame generation backends");
    g_da_fg_backends[g_da_fg_backend_count++] = backend;
}

#ifdef XR_D3D12
namespace
{
// The finished 3D frame without the UI: display size, the back buffer's format.
constexpr pcstr da_rt_hudless = "$user$hudless";
// Frames a started backend waits before it generates: the first ones after a reset have no
// history (and the renderer skips the first frame after a reset altogether).
constexpr u32 da_fg_settle_frames = 4;

struct framegen_state
{
    const FrameGenBackend* registered{}; // what the swap chain presents through (null = plain DXGI)
    IFrameGen* backend{};                // its instance
    int token{};                         // the r__framegen value being served
    u32 generated{};
    bool ready{};                        // `backend` serves `token`: it may start
    bool initialised{};                  // Init succeeded: frames can be generated
    bool active{};                       // SetActive's last word
    bool prepared{};                     // Prepare ran since the last Present
    bool reset_next{};                   // the first prepared frame after being off
    int failed_token = -1;               // could not run: off until r__framegen changes
    u32 settle{};
    u64 frame_index{};
    ref_rt hudless;
};
framegen_state g_da_fg;

pcstr da_fg_token_name(int token)
{
    switch (token)
    {
    case 1: return "AMD FSR frame generation";
    case 2: return "NVIDIA DLSS frame generation 2x";
    case 3: return "NVIDIA DLSS frame generation 3x";
    case 4: return "NVIDIA DLSS frame generation 4x";
    default: return "frame generation off";
    }
}

// The backend serving an r__framegen value and the frames it generates for it. DLSS may register
// once for 2x and serve 3x and 4x through maxGenerated.
const FrameGenBackend* da_fg_find(int token, u32& generated)
{
    generated = token >= 2 ? u32(token - 1) : 1u;
    const FrameGenBackend* multi = nullptr;
    for (size_t i = 0; i < g_da_fg_backend_count; ++i)
    {
        const FrameGenBackend& backend = g_da_fg_backends[i];
        if (backend.token == token)
            return &backend;
        if (token > 2 && backend.token == 2)
            multi = &backend;
    }
    return multi;
}

bool da_fg_supported(const FrameGenBackend* backend, u32 generated)
{
    if (!backend || !HW.pDevice)
        return false;
    if (backend->supported && !backend->supported())
        return false;
    return generated <= (backend->maxGenerated ? backend->maxGenerated() : 1u);
}

void da_fg_set_active(bool active)
{
    framegen_state& fg = g_da_fg;
    if (!fg.initialised)
        return;
    // On is repeated every generated frame (a backend may switch itself off on a failure); off
    // is a transition and takes effect before the next Present.
    if (active || fg.active)
        fg.backend->SetActive(active);
    if (active && !fg.active)
        fg.reset_next = true;
    fg.active = active;
    // The CPU limiter paces rendered frames: every one is shown this many times.
    Device.dwFramesPerRender = active ? fg.generated + 1 : 1;
}

void da_fg_stop()
{
    framegen_state& fg = g_da_fg;
    fg.hudless.destroy();
    if (fg.initialised)
    {
        da_fg_set_active(false);
        fg.backend->Shutdown();
        fg.initialised = false;
    }
    fg.active = false;
    fg.prepared = false;
    Device.dwFramesPerRender = 1;
}

// Nothing to interpolate between, or a screen the generated frames would smear.
bool da_fg_can_generate()
{
    if (g_da_fg.settle || Device.dwPrecacheFrame || Device.IsAnselActive || Device.Paused())
        return false;
    if (!g_pGamePersistent || g_pGamePersistent->MainMenuActiveOrLevelNotExist() ||
        g_pGamePersistent->OnRenderPPUI_query())
        return false;
    if (load_screen_renderer.IsActive() ||
        (g_pGamePersistent->m_pLoadingScreen && g_pGamePersistent->m_pLoadingScreen->IsShown()))
        return false;
    if (Console && Console->bVisible)
        return false;
    // The velocity pass keys on this: without it the vectors are last frame's or none.
    return da_motion_vectors_active();
}

// r__caps fsrfg: the FSR backend is registered and runs on this adapter.
void da_fg_caps(UpscalerCaps& caps)
{
    u32 generated = 1;
    const FrameGenBackend* fsr = da_fg_find(1, generated);
    caps.fsrfg = fsr && da_fg_supported(fsr, generated);
}

const struct da_fg_caps_registrar
{
    da_fg_caps_registrar() { da_upscaler_register_caps_hook(&da_fg_caps); }
} s_da_fg_caps;
} // namespace

void da_framegen_stop() { da_fg_stop(); }

void da_framegen_switch()
{
    framegen_state& fg = g_da_fg;
    da_fg_stop();
    fg.ready = false;

    int token = int(ps_r__framegen);
    u32 generated = 1;
    const FrameGenBackend* wanted = nullptr;
    // A failure holds until the setting changes; another value gets its own try.
    if (fg.failed_token >= 0 && fg.failed_token != token)
        fg.failed_token = -1;
    if (token)
    {
        wanted = da_fg_find(token, generated);
        if (!da_fg_supported(wanted, generated))
        {
            Msg("! [framegen] %s does not run on this adapter: r__framegen is switched off", da_fg_token_name(token));
            ps_r__framegen = 0;
            token = 0;
            wanted = nullptr;
        }
        else if (token == fg.failed_token)
            wanted = nullptr;
        else if (RImplementation.o.msaa)
        {
            // The scene depth is multisampled: nothing can read it.
            Msg("! [framegen] %s does not run with MSAA", da_fg_token_name(token));
            wanted = nullptr;
        }
    }
    fg.token = token;
    fg.generated = generated;
    if (wanted == fg.registered)
    {
        fg.ready = wanted != nullptr;
        return;
    }

    IFrameGen* next = wanted ? wanted->create() : nullptr;
    if (wanted && !next)
    {
        Msg("! [framegen] [%s] could not be created", wanted->name);
        fg.failed_token = token;
        if (!fg.registered)
            return;
    }

    // Nothing holds a back buffer view here: the target is gone and its surfaces are released.
    // Without a backend the chain goes through Streamline's upgraded factory where it runs (its
    // bookkeeping sees every present), else it is a plain DXGI one.
    IDXGISwapChain4* before = xrd3d12::NativeSwapChain(HW.m_pSwapChain);
    const HRESULT hr =
        xrd3d12::RecreateSwapChain(HW.m_pSwapChain, next ? next->Provider() : da_streamline_swapchain_provider());
    if (FAILED(hr))
    {
        // Refused = nothing changed and the old swap chain (and its backend) stays; otherwise the
        // layer fell back to a plain one and the old backend's provider is gone with its chain.
        const bool refused = hr == DXGI_ERROR_INVALID_CALL && xrd3d12::NativeSwapChain(HW.m_pSwapChain) == before;
        Msg("! [framegen] the swap chain for [%s] could not be made (0x%08X)%s", next ? next->Name() : "plain",
            u32(hr), refused ? ", the back buffer is still referenced" : ", presenting through a plain one");
        xr_delete(next);
        if (token)
            fg.failed_token = token;
        if (!refused)
        {
            xr_delete(fg.backend);
            fg.registered = nullptr;
        }
        return;
    }

    xr_delete(fg.backend);
    fg.backend = next;
    fg.registered = next ? wanted : nullptr;
    fg.ready = next != nullptr;
    Msg("* [framegen] swap chain through [%s]", next ? next->Name() : "no frame generation");
}

void da_framegen_start()
{
    framegen_state& fg = g_da_fg;
    if (!fg.ready || fg.initialised)
        return;

    const u32 w = Device.dwWidth, h = Device.dwHeight;
    if (!fg.backend->Init(w, h, fg.generated))
    {
        // The swap chain stays until the next reset, which then goes back to a plain one.
        Msg("! [framegen] [%s] did not initialise: off until r__framegen changes", fg.backend->Name());
        fg.failed_token = fg.token;
        fg.ready = false;
        return;
    }
    fg.hudless.create(da_rt_hudless, w, h, HW.Caps.fTarget, 1);
    if (!fg.hudless || !fg.hudless->pSurface)
    {
        Msg("! [framegen] the hudless target cannot be created: off until r__framegen changes");
        fg.hudless.destroy();
        fg.backend->Shutdown();
        fg.failed_token = fg.token;
        fg.ready = false;
        return;
    }
    fg.initialised = true;
    fg.active = false;
    fg.prepared = false;
    fg.settle = da_fg_settle_frames;
    Msg("* [framegen] %s through [%s] at %ux%u", da_fg_token_name(fg.token), fg.backend->Name(), w, h);
}

void da_framegen_destroy()
{
    framegen_state& fg = g_da_fg;
    da_fg_stop();
    // The swap chain keeps the provider (a static object) until the device goes.
    xr_delete(fg.backend);
    fg.registered = nullptr;
    fg.ready = false;
}

void da_framegen_frame()
{
    framegen_state& fg = g_da_fg;
    CRenderTarget* target = RImplementation.Target;
    if (!fg.initialised || !target)
        return;

    ID3DBaseTexture* velocity = target->t_velocity ? target->t_velocity->surface_get() : nullptr;
    if (!velocity || !da_fg_can_generate() || !target->rt_Render_Depth || !target->rt_Render_Depth->pSurface)
    {
        _RELEASE(velocity);
        da_fg_set_active(false);
        return;
    }

    PIX_EVENT(da_framegen_prepare);

    // The hudless frame: the back buffer before the UI goes on it. The back buffer may not stay
    // bound while it is the source of the copy.
    target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, nullptr, nullptr, nullptr,
        static_cast<ID3DDepthStencilView*>(nullptr));
    HW.get_context(CHW::IMM_CTX_ID)->CopyResource(fg.hudless->pSurface, target->rt_Base[HW.CurrentBackBuffer]->pSurface);

    FrameGenFrame frame;
    frame.depth = target->rt_Render_Depth->pSurface;
    frame.velocity = velocity;
    frame.hudless = fg.hudless->pSurface;
    frame.renderW = Device.dwRenderWidth;
    frame.renderH = Device.dwRenderHeight;
    frame.displayW = Device.dwWidth;
    frame.displayH = Device.dwHeight;
    const Fvector2 jitter = da_jitter_px();
    frame.jitterX = jitter.x;
    frame.jitterY = jitter.y;
    frame.frameTimeMs = Device.fTimeDeltaUnscaled * 1000.f;
    frame.nearZ = VIEWPORT_NEAR;
    frame.farZ = g_pGamePersistent->Environment().CurrentEnv.far_plane;
    frame.fovY = deg2rad(Device.fFOV);
    frame.reset = da_history_reset();
    // Device keeps the unjittered matrices; the saved ones are last frame's (RenderEnd).
    frame.view = Device.mView;
    frame.project = Device.mProject;
    frame.prevView = Device.mViewSaved;
    frame.prevProject = Device.mProjectSaved;
    frame.cameraPosition = Device.vCameraPosition;
    frame.cameraUp = Device.vCameraTop;
    frame.cameraRight = Device.vCameraRight;
    frame.cameraForward = Device.vCameraDirection;
    // FSR resets its history on any step other than exactly one.
    frame.frameIndex = ++fg.frame_index;

    // Back from off: nothing generated before this frame belongs to it.
    da_fg_set_active(true);
    frame.reset = frame.reset || fg.reset_next;
    fg.backend->Prepare(frame);
    fg.reset_next = false;
    fg.prepared = true;
    _RELEASE(velocity);

    // The UI, the tracers and the stats draw on the back buffer next.
    target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, target->get_base_rt(), nullptr, nullptr,
        target->get_base_zb());
}

void da_framegen_present()
{
    framegen_state& fg = g_da_fg;
    if (!fg.initialised)
        return;
    // Menus, the loading screen and the early outs of Render() prepare nothing: such a frame is
    // shown alone. Every Present consumes the preparation (the loading screen presents without
    // a new frame).
    if (fg.active && !fg.prepared)
        da_fg_set_active(false);
    fg.prepared = false;
    if (fg.settle)
        --fg.settle;
}

bool da_framegen_running() { return g_da_fg.initialised; }
#else
// DirectX 11 has no frame generation. r__framegen keeps its value for the DirectX 12 module.
void da_framegen_switch()
{
    static bool reported = false;
    if (ps_r__framegen && !reported)
    {
        Msg("~ [framegen] frame generation needs DirectX 12: off in this renderer");
        reported = true;
    }
}
void da_framegen_stop() {}
void da_framegen_start() {}
void da_framegen_destroy() {}
void da_framegen_frame() {}
void da_framegen_present() {}
bool da_framegen_running() { return false; }
#endif
} // namespace xray::render::RENDER_NAMESPACE
