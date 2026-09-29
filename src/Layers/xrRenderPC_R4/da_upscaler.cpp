// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "da_upscaler.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderDX11/StateManager/dx11SamplerStateCache.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Plain arrays: the SDK stage registers from static initialisers, before the engine's allocator
// is guaranteed to be up.
constexpr size_t da_max_upscaler_backends = 16;
UpscalerBackend g_da_backends[da_max_upscaler_backends];
size_t g_da_backend_count = 0;

constexpr size_t da_max_caps_hooks = 8;
UpscalerCapsHook g_da_caps_hooks[da_max_caps_hooks];
size_t g_da_caps_hook_count = 0;

struct upscaler_state
{
    IUpscaler* backend{}; // the one the targets are sized for (identity included)
    UpscalerKind kind{};
    int quality{};
    bool identity{}; // no SDK backend: the stretch fills the slot
    bool initialised{};
    bool failed{}; // it could not run: native path until r__fsr / r__dlss change
};
upscaler_state g_da_upscaler;

pcstr da_kind_name(UpscalerKind kind)
{
    switch (kind)
    {
    case UpscalerKind::FSR: return "FSR";
    case UpscalerKind::DLSS: return "DLSS";
    default: return "none";
    }
}

// Until an SDK backend is registered the plumbing is proven with this: the render-size frame
// stretched to the display, nothing reconstructed - so no jitter either, it would only shake.
class identity_upscaler final : public IUpscaler
{
    UpscalerKind m_kind;

public:
    explicit identity_upscaler(UpscalerKind kind) : m_kind(kind) {}

    bool Init(u32, u32, int) override { return true; }
    void Shutdown() override {}

    bool Dispatch(const UpscaleFrame&) override
    {
        RImplementation.Target->phase_upscale_identity();
        return true;
    }

    void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) override
    {
        da_upscaler_default_render_size(m_kind, quality, displayW, displayH, renderW, renderH);
    }

    pcstr Name() override { return "identity"; }
    bool Temporal() const override { return false; }
};

const UpscalerBackend* da_find_backend(UpscalerKind kind)
{
    const u32 api = da_render_api();
    for (size_t i = 0; i < g_da_backend_count; ++i)
    {
        if (g_da_backends[i].kind == kind && g_da_backends[i].api == api)
            return &g_da_backends[i];
    }
    return nullptr;
}

void da_release_backend()
{
    if (!g_da_upscaler.backend)
        return;
    if (g_da_upscaler.initialised)
        g_da_upscaler.backend->Shutdown();
    xr_delete(g_da_upscaler.backend);
    g_da_upscaler.initialised = false;
    g_da_upscaler.identity = false;
    g_da_upscaler.kind = UpscalerKind::None;
    g_da_upscaler.quality = 0;
}

void da_render_size_native()
{
    Device.dwRenderWidth = Device.dwWidth;
    Device.dwRenderHeight = Device.dwHeight;
}
} // namespace

void da_upscaler_register(const UpscalerBackend& backend)
{
    R_ASSERT2(g_da_backend_count < da_max_upscaler_backends, "too many upscaler backends");
    g_da_backends[g_da_backend_count++] = backend;
}

void da_upscaler_register_caps_hook(UpscalerCapsHook hook)
{
    R_ASSERT2(g_da_caps_hook_count < da_max_caps_hooks, "too many upscaler caps hooks");
    g_da_caps_hooks[g_da_caps_hook_count++] = hook;
}

u32 da_render_api()
{
    return RImplementation.GetBackendAPI() == IRender::BackendAPI::D3D11 ? 11 : 12;
}

int da_upscaler_support(UpscalerKind kind)
{
    // Before the device exists nothing can be asked (user.ltx runs that early).
    if (!HW.pDevice)
        return -1;
    const UpscalerBackend* backend = da_find_backend(kind);
    if (!backend)
        return -1;
    return !backend->supported || backend->supported() ? 1 : 0;
}

UpscalerCaps da_upscaler_caps()
{
    UpscalerCaps caps;
    caps.api = da_render_api();
    caps.fsr = true;
    caps.dlss = da_upscaler_support(UpscalerKind::DLSS) == 1;
    // The DX12 renderer is offered (renderer_r5 among the renderer tokens) only on an adapter that
    // passed its probe.
    if (IConsole_Command* renderer = Console ? Console->GetCommand("renderer") : nullptr)
    {
        IConsole_Command::TInfo modes;
        renderer->Info(modes);
        caps.dx12 = caps.api == 12 || strstr(modes, "renderer_r5") != nullptr;
    }
    for (size_t i = 0; i < g_da_caps_hook_count; ++i)
        g_da_caps_hooks[i](caps);
    return caps;
}

void da_upscaler_default_render_size(UpscalerKind kind, int quality, u32 displayW, u32 displayH, u32& renderW,
    u32& renderH)
{
    // Index = the token value: off/native (or DLAA), quality, balanced, performance, ultra performance.
    static constexpr float fsr_ratio[] = { 1.f, 1.f, 1.5f, 1.7f, 2.f, 3.f };
    static constexpr float dlss_ratio[] = { 1.f, 1.f, 1.5f, 1.72f, 2.f, 3.f };
    const int q = std::clamp(quality, 0, 5);
    const float ratio = kind == UpscalerKind::DLSS ? dlss_ratio[q] : fsr_ratio[q];

    // Even: the half-resolution passes (AO, clouds) halve it.
    const auto scaled = [ratio](u32 display)
    {
        const u32 size = u32(std::lround(float(display) / ratio)) & ~1u;
        return std::min(std::max(size, 64u), display);
    };
    renderW = scaled(displayW);
    renderH = scaled(displayH);
}

void da_update_render_size()
{
    da_release_backend();
    da_render_size_native();

    // A DLSS choice on an adapter known not to run it is switched off, once, in the open.
    if (ps_r__dlss && da_upscaler_support(UpscalerKind::DLSS) == 0)
    {
        Msg("! [upscaler] DLSS is not supported by this adapter: r__dlss is switched off");
        ps_r__dlss = 0;
    }

    UpscalerKind kind = UpscalerKind::None;
    int quality = 0;
    if (ps_r__dlss)
    {
        kind = UpscalerKind::DLSS;
        quality = int(ps_r__dlss);
    }
    else if (ps_r__fsr)
    {
        kind = UpscalerKind::FSR;
        quality = int(ps_r__fsr);
    }

    // MSAA is a different deferred path (per-sample G-buffer): nothing temporal runs under it.
    if (kind == UpscalerKind::None || g_da_upscaler.failed || RImplementation.o.msaa)
        return;

    IUpscaler* backend = nullptr;
    bool identity = false;
    if (const UpscalerBackend* registered = da_find_backend(kind))
    {
        if (registered->supported && !registered->supported())
        {
            Msg("! [upscaler] %s: [%s] does not run on this adapter - native rendering", da_kind_name(kind),
                registered->name);
            g_da_upscaler.failed = true;
            return;
        }
        backend = registered->create();
    }
    if (!backend)
    {
        backend = xr_new<identity_upscaler>(kind);
        identity = true;
    }

    const u32 dw = Device.dwWidth, dh = Device.dwHeight;
    u32 rw = dw, rh = dh;
    backend->RenderSize(quality, dw, dh, rw, rh);
    // Whatever the backend answered: even, at least 64, never above the display.
    rw = std::min(std::max(rw & ~1u, 64u), dw);
    rh = std::min(std::max(rh & ~1u, 64u), dh);

    g_da_upscaler.backend = backend;
    g_da_upscaler.kind = kind;
    g_da_upscaler.quality = quality;
    g_da_upscaler.identity = identity;
    Device.dwRenderWidth = rw;
    Device.dwRenderHeight = rh;
    Msg("* [upscaler] %s quality %d through [%s]: %ux%u -> %ux%u", da_kind_name(kind), quality, backend->Name(), rw,
        rh, dw, dh);
}

bool da_upscaler_create()
{
    upscaler_state& up = g_da_upscaler;
    if (!up.backend)
    {
        SSManager.SetSceneMipBias(0.f, 0);
        return true;
    }

    pcstr why = nullptr;
    if (!RImplementation.Target || !RImplementation.Target->upscale_ready())
        why = "its targets or shaders are missing (a content archive older than the binaries?)";
    else if (up.kind == UpscalerKind::DLSS && !up.identity && !RImplementation.Target->rt_Velocity)
        why = "DLSS needs motion vectors, which only the packed G-buffer draws (r3_gbuffer_opt on)";
    else if (!up.backend->Init(Device.dwWidth, Device.dwHeight, up.quality))
        why = "the backend did not initialise";

    if (why)
    {
        Msg("! [upscaler] %s through [%s] cannot run: %s - native rendering until the setting changes",
            da_kind_name(up.kind), up.backend->Name(), why);
        da_release_backend();
        up.failed = true;
        da_render_size_native();
        SSManager.SetSceneMipBias(0.f, 0);
        return false;
    }

    up.initialised = true;
    // The scene is sampled at the render size and resolved at the display size: textures take
    // the mip the display would pick (AMD's log2(render/display) - 1). A negative bias shimmers
    // on the ground without anisotropy, so the scene samplers get at least 8x while it runs.
    const float bias = da_upscaler_mip_bias();
    SSManager.SetSceneMipBias(bias, bias < 0.f ? 8u : 0u);
    da_history_request_reset("upscaler started");
    return true;
}

void da_upscaler_destroy()
{
    da_release_backend();
    SSManager.SetSceneMipBias(0.f, 0);
}

IUpscaler* da_upscaler_current() { return g_da_upscaler.backend; }

bool da_upscaler_active() { return g_da_upscaler.backend != nullptr; }

void da_upscaler_fail(pcstr why)
{
    upscaler_state& up = g_da_upscaler;
    if (!up.backend || up.identity)
        return;

    Msg("! [upscaler] %s through [%s]: %s - stretched without reconstruction until the renderer resets",
        da_kind_name(up.kind), up.backend->Name(), why);
    const UpscalerKind kind = up.kind;
    const int quality = up.quality;
    da_release_backend();
    // The targets stay split until the next reset; the stretch fills the slot meanwhile, and the
    // reset itself brings the native path (failed).
    up.backend = xr_new<identity_upscaler>(kind);
    up.kind = kind;
    up.quality = quality;
    up.identity = true;
    up.initialised = true;
    up.failed = true;
    SSManager.SetSceneMipBias(0.f, 0);
}

void da_upscaler_settings_changed()
{
    g_da_upscaler.failed = false;
    da_history_request_reset("upscaler changed");
}

float da_upscaler_mip_bias()
{
    const upscaler_state& up = g_da_upscaler;
    if (!up.backend || up.identity || !Device.dwWidth)
        return 0.f;
    return std::log2(float(Device.dwRenderWidth) / float(Device.dwWidth)) - 1.f;
}
} // namespace xray::render::RENDER_NAMESPACE
