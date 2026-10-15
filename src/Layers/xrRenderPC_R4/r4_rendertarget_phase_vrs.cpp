// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The passes of variable rate shading (docs/dead-air/dx12-features/vrs.md 3.2, 5, 6): the luminance
// contrast and the rate image as compute passes, the debug overlay, and their targets behind
// CRenderTarget::m_da_vrs.

#include "stdafx.h"

#include <atomic>

#include "da_vrs.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "xrEngine/IGame_Persistent.h"

namespace xray::render::RENDER_NAMESPACE
{
IReader* open_shader(pcstr shader); // ResourceManager.cpp

// The scene depth under its own name: rt_Render_Depth is the base depth or the MSAA/upscale one.
static constexpr pcstr da_vrs_depth_name = "$user$vrs_depth";

// Everything VRS keeps per render-target set. Recreated with the targets, which also discards the
// history (a new set starts without a contrast of the previous frame).
struct DaVrsTargets
{
    u32 tile{};
    u32 tiles_x{}, tiles_y{};
    u32 width{}, height{}; // the render size the targets were made for

    ID3DTexture2D* image[2]{}; // R8_UINT D3D12_SHADING_RATE per tile, written by frames of that parity
    ID3D11UnorderedAccessView* image_uav[2]{};
    ref_texture alias; // r2_RT_vrs_prev: the previous rates for the rate pass, the frame's for the overlay
    ref_texture depth; // da_vrs_depth_name: the render-size scene depth (rt_Render_Depth)
    ID3DTexture2D* alias_surface{};
    ref_rt contrast; // r2_RT_vrs_contrast, one texel per 8x8 pixels

    ID3DTexture2D* stats{}; // 8x1 R32_UINT: tiles per rate (r__vrs_log)
    ID3D11UnorderedAccessView* stats_uav{};
    ID3DTexture2D* stats_staging[3]{};
    u32 stats_copied[3]{}; // the frame each staging copy holds, 0 = none
    int stats_mode[3]{}; // the mode of that frame

    ref_shader s_luma, s_luma_upscaled, s_rate, s_debug;
    ref_geom g_quad; // the overlay's full-screen quad (da_fullscreen.vs layout)

    u32 luma_frame{u32(-1)}; // the last frame phase_vrs_luma ran in
    u32 image_frame{u32(-1)}; // the last frame a rate image was written in
    // The frame mode/image below belong to, stored after them: the sun cascades may read them on
    // other threads while the rate pass runs.
    std::atomic<u32> state_frame{u32(-1)};
    int frame_mode{};
    ID3DTexture2D* frame_image{};
    int logged_mode{-1};
    bool size_logged{};
};

#if defined(XR_D3D12)
namespace
{
// The frame the contrast is measured on: the render-size frame after the anti-aliasing, or the
// upscaler's resolved output while one runs (its input is jittered and aliased).
class CBlender_da_vrs_luma : public IBlender
{
    pcstr m_source;

public:
    explicit CBlender_da_vrs_luma(pcstr source) : m_source(source) {}
    LPCSTR getComment() override { return "INTERNAL: VRS luminance contrast"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass("da_vrs_luma");
        C.r_dx11Texture("s_vrs_frame", m_source);
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
    }
};

class CBlender_da_vrs_rate : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: VRS rate image"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass("da_vrs_rate");
        C.r_dx11Texture("s_vrs_depth", da_vrs_depth_name);
        C.r_dx11Texture("s_vrs_contrast", r2_RT_vrs_contrast);
        C.r_dx11Texture("s_vrs_velocity", r2_RT_velocity);
        C.r_dx11Texture("s_vrs_prev", r2_RT_vrs_prev);
        C.r_dx11Sampler("smp_nofilter");
        C.r_End();
    }
};

// Conservative and aggressive: the contrast threshold and the silhouette's depth ratio (vrs.md 3.2,
// calibrated in 9.3). The design's motion relief is gone: coarse blocks stay fixed to the screen while
// the view turns and TAA does not hide them (vrs.md 13.3).
constexpr float vrs_s_mode[2] = {0.06f, 0.15f};
constexpr float vrs_k_jump[2] = {0.08f, 0.20f};

// D3D12_SHADING_RATE values in the order of the stats bins (da_vrs_rate.cs).
constexpr const char* vrs_bin_names[7] = {"1x1", "1x2", "2x1", "2x2", "2x4", "4x2", "4x4"};

bool da_vrs_shader_exists(pcstr name)
{
    IReader* file = open_shader(name);
    const bool exists = !!file;
    FS.r_close(file);
    return exists;
}

DaVrsTargets* da_vrs_state()
{
    CRenderTarget* target = RImplementation.Target;
    return target ? target->m_da_vrs : nullptr;
}

// Neither the depth nor the frame may stay bound for output while a compute pass reads it. The
// backend's cache records the unbinding, the context gets it now (a pending change would only
// reach it at the next draw).
void da_vrs_unbind_targets(CBackend& cmd_list)
{
    for (u32 i = 0; i < 4; ++i)
        cmd_list.set_RT(nullptr, i);
    cmd_list.set_ZB(nullptr);
    HW.get_context(cmd_list.context_id)->OMSetRenderTargets(0, nullptr, nullptr);
}

void da_vrs_point_alias(DaVrsTargets& s, ID3DTexture2D* surface)
{
    if (s.alias_surface == surface)
        return;
    s.alias->surface_set(surface);
    s.alias_surface = surface;
}

void da_vrs_compute(CBackend& cmd_list, ref_shader& shader, ID3D11UnorderedAccessView* u0,
    ID3D11UnorderedAccessView* u1, u32 groups_x, u32 groups_y, const std::function<void()>& constants)
{
    SPass& P = *shader->E[0]->passes[0];
    cmd_list.set_States(P.state);
    cmd_list.set_CS(P.cs);
    cmd_list.set_Constants(P.constants);
    cmd_list.set_Textures(P.T);
    constants();
    auto* ctx = HW.get_context(cmd_list.context_id);
    ID3D11UnorderedAccessView* uavs[2] = {u0, u1};
    const UINT initial[2] = {~0u, ~0u};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, initial);
    cmd_list.Compute(groups_x, groups_y, 1);
    ID3D11UnorderedAccessView* none[2] = {};
    ctx->CSSetUnorderedAccessViews(0, 2, none, initial);
    cmd_list.clear_CS_resources();
}

// The mode this frame runs at, before the history check: r__vrs_force as aggressive, an aggressive
// mode under any upscale as conservative (the upscaler magnifies block errors: aggressive at DLSS
// quality missed the worst-tile row of vrs.md 9.3).
int da_vrs_effective_mode()
{
    if (ps_r__vrs_force > 0)
        return 2;
    int mode = std::clamp(da_vrs_requested_mode(), 0, 2);
    if (mode == 2 && Device.dwRenderWidth < Device.dwWidth)
        mode = 1;
    return mode;
}

bool da_vrs_menu_pp() { return g_pGamePersistent && g_pGamePersistent->OnRenderPPUI_query(); }

void da_vrs_print_stats(DaVrsTargets& s, ID3DDeviceContext* ctx, u32 slot)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(s.stats_staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        return;
    u32 bins[8]{};
    std::memcpy(bins, mapped.pData, sizeof(bins));
    ctx->Unmap(s.stats_staging[slot], 0);
    u32 tiles = 0;
    for (u32 i = 0; i < 7; ++i)
        tiles += bins[i];
    if (!tiles)
        return;
    string256 line{};
    for (u32 i = 0; i < 7; ++i)
    {
        string32 part;
        xr_sprintf(part, " %s=%.0f%%", vrs_bin_names[i], 100.0 * double(bins[i]) / double(tiles));
        xr_strcat(line, part);
    }
    Msg("[vrs] mode=%d tiles=%u%s coarse=%.1f%%", s.stats_mode[slot], tiles, line,
        100.0 * double(tiles - bins[0]) / double(tiles));
}

// r__vrs_log N: every N frames, the tile histogram of the frame three frames back. Its staging copy
// is read before this frame's copy replaces it: Device::EndFrame has already waited for that frame
// (kFramesInFlight = 3), so the map never finds it still drawing.
void da_vrs_log_stats(DaVrsTargets& s, CBackend& cmd_list, int mode)
{
    auto* ctx = HW.get_context(cmd_list.context_id);
    const u32 frame = Device.dwFrame;
    const u32 slot = frame % 3;
    if (!(frame % u32(ps_r__vrs_log)) && s.stats_copied[slot] && s.stats_copied[slot] + 3 == frame)
        da_vrs_print_stats(s, ctx, slot);
    ctx->CopyResource(s.stats_staging[slot], s.stats);
    s.stats_copied[slot] = frame;
    s.stats_mode[slot] = mode;
}
} // namespace
#endif

bool da_vrs_supported()
{
#if defined(XR_D3D12)
    return !!da_vrs_state();
#else
    return false;
#endif
}

int da_vrs_frame_mode()
{
#if defined(XR_D3D12)
    const DaVrsTargets* s = da_vrs_state();
    return s && s->state_frame.load(std::memory_order_acquire) == Device.dwFrame ? s->frame_mode : 0;
#else
    return 0;
#endif
}

ID3DTexture2D* da_vrs_frame_image()
{
#if defined(XR_D3D12)
    const DaVrsTargets* s = da_vrs_state();
    return s && s->state_frame.load(std::memory_order_acquire) == Device.dwFrame ? s->frame_image : nullptr;
#else
    return nullptr;
#endif
}

void da_vrs_targets_create(CRenderTarget& target)
{
#if defined(XR_D3D12)
    const CHWNativeCaps& caps = HW.native;
    pcstr reason = nullptr;
    if (caps.vrsTier < 2)
        reason = "tier";
    else if (caps.vrsTileSize != 8 && caps.vrsTileSize != 16 && caps.vrsTileSize != 32)
        reason = "tile size";
    else if (RImplementation.o.msaa)
        reason = "MSAA";
    else if (!target.rt_Render_Depth || !target.rt_Render_Depth->pSurface)
        reason = "no scene depth";
    else if (!da_vrs_shader_exists("da_vrs_luma.cs") || !da_vrs_shader_exists("da_vrs_rate.cs"))
        reason = "shaders missing";
    if (reason)
    {
        if (caps.vrsTier < 2)
            Msg("* [vrs] off: tier %u", caps.vrsTier);
        else
            Msg("* [vrs] off: %s", reason);
        return;
    }

    auto* s = xr_new<DaVrsTargets>();
    s->tile = caps.vrsTileSize;
    s->width = Device.dwRenderWidth;
    s->height = Device.dwRenderHeight;
    s->tiles_x = (s->width + s->tile - 1) / s->tile;
    s->tiles_y = (s->height + s->tile - 1) / s->tile;

    bool ok = true;
    const auto texture = [&](u32 w, u32 h, DXGI_FORMAT format, UINT bind, D3D11_USAGE usage, UINT cpu, ID3DTexture2D** out) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = cpu;
        ok = ok && SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, out));
    };
    const auto uav = [&](ID3DTexture2D* resource, ID3D11UnorderedAccessView** out) {
        ok = ok && SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(resource, nullptr, out));
    };
    constexpr UINT srv_uav = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (u32 i = 0; i < 2; ++i)
    {
        texture(s->tiles_x, s->tiles_y, DXGI_FORMAT_R8_UINT, srv_uav, D3D11_USAGE_DEFAULT, 0, &s->image[i]);
        uav(s->image[i], &s->image_uav[i]);
    }
    texture(8, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &s->stats);
    uav(s->stats, &s->stats_uav);
    for (auto& staging : s->stats_staging)
        texture(8, 1, DXGI_FORMAT_R32_UINT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &staging);
    if (ok)
    {
        s->contrast.create(r2_RT_vrs_contrast, (s->width + 7) / 8, (s->height + 7) / 8, D3DFMT_A16B16G16R16F, 1, { CRT::CreateUAV });
        ok = s->contrast && s->contrast->pUAView;
    }
    if (ok)
    {
        s->alias.create(r2_RT_vrs_prev);
        s->depth.create(da_vrs_depth_name);
        s->depth->surface_set(target.rt_Render_Depth->pSurface);
        static CBlender_da_vrs_luma b_luma(r2_RT_generic0);
        static CBlender_da_vrs_luma b_luma_upscaled(r2_RT_upscaled);
        static CBlender_da_vrs_rate b_rate;
        s->s_luma.create(&b_luma, "r2" DELIMITER "da_vrs_luma");
        s->s_luma_upscaled.create(&b_luma_upscaled, "r2" DELIMITER "da_vrs_luma_upscaled");
        s->s_rate.create(&b_rate, "r2" DELIMITER "da_vrs_rate");
        ok = s->s_luma && s->s_luma_upscaled && s->s_rate && s->s_luma->E[0] && s->s_luma_upscaled->E[0] &&
            s->s_rate->E[0];
        if (ok && da_vrs_shader_exists("da_vrs_debug.s") && da_vrs_shader_exists("da_vrs_debug.ps"))
        {
            s->s_debug.create("da_vrs_debug");
            s->g_quad.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        }
    }
    target.m_da_vrs = s;
    if (!ok)
    {
        Msg("* [vrs] off: target creation failed");
        da_vrs_targets_destroy(target);
        return;
    }
    Msg("* [vrs] tier %u, tile %u, additional rates %d: mode %d (preset %d, override %d)", caps.vrsTier, s->tile,
        caps.vrsAdditionalRates ? 1 : 0, da_vrs_effective_mode(), ps_r__vrs, ps_r__vrs_override);
#else
    (void)target;
    Msg("* [vrs] off: no DirectX 12");
#endif
}

void da_vrs_targets_destroy(CRenderTarget& target)
{
    DaVrsTargets* s = target.m_da_vrs;
    if (!s)
        return;
    target.m_da_vrs = nullptr;
    // The alias may outlive the targets in a shader's texture list: it must not keep an image.
    if (s->alias)
        s->alias->surface_set(nullptr);
    if (s->depth)
        s->depth->surface_set(nullptr);
    for (u32 i = 0; i < 2; ++i)
    {
        _RELEASE(s->image_uav[i]);
        _RELEASE(s->image[i]);
    }
    _RELEASE(s->stats_uav);
    _RELEASE(s->stats);
    for (auto& staging : s->stats_staging)
        _RELEASE(staging);
    xr_delete(s);
}

#if defined(XR_D3D12)
namespace
{
// The rate image of this frame in the given mode, or null: without the previous frame's contrast
// (first frame, a reset, a gap) every image pass shades 1x1 (r__vrs_force needs no history).
ID3DTexture2D* da_vrs_build_image(DaVrsTargets& s, CBackend& cmd_list, int mode)
{
    const u32 frame = Device.dwFrame;
    const int force = ps_r__vrs_force;
    const bool history = s.luma_frame + 1 == frame && !da_history_reset();
    if (!history && !force)
        return nullptr;

    PIX_EVENT(phase_vrs_rate);
    dx11GpuTimerScope gpu(dx11GpuTimers::VrsRate);
    CRenderTarget& target = *RImplementation.Target;
    const u32 current = frame & 1;
    const bool prev_valid = s.image_frame + 1 == frame;
    da_vrs_unbind_targets(cmd_list);
    // The previous frame's rates are read, this frame's are written (the alias names the former).
    da_vrs_point_alias(s, s.image[current ^ 1]);
    const bool log = ps_r__vrs_log > 0;
    if (log)
    {
        const UINT zero[4]{};
        HW.get_context(cmd_list.context_id)->ClearUnorderedAccessViewUint(s.stats_uav, zero);
    }

    const size_t m = size_t(mode - 1);
    const float render_scale = float(Device.dwRenderWidth) / float(std::max(Device.dwWidth, 1u));
    Fvector3 dof{};
    if (g_pGamePersistent)
        g_pGamePersistent->GetCurrentDof(dof);
    const bool dof_on = ps_r2_ls_flags.test(R2FLAG_DOF) && ps_r2_dof_kernel_size >= 2.f && dof.z > dof.y;
    Fmatrix reproject;
    reproject.mul(da_vp_prev(), Device.mInvView);
    // A group covers two by two tiles of 8 or 16, or one tile of 32 (da_vrs_rate.cs).
    const u32 region = std::min(s.tile * 2, 32u);
    const u32 groups_x = (s.width + region - 1) / region;
    const u32 groups_y = (s.height + region - 1) / region;
    const float mode_code = force ? (force == 1 ? 3.f : 4.f) : float(mode);
    da_vrs_compute(cmd_list, s.s_rate, s.image_uav[current], log ? s.stats_uav : nullptr, groups_x, groups_y, [&] {
        cmd_list.set_c("da_vrs_params", float(s.tile), mode_code, vrs_s_mode[m] * ps_r__vrs_threshold * render_scale,
            vrs_k_jump[m] * ps_r__vrs_edge);
        cmd_list.set_c("da_vrs_params2", Device.mProject._33, Device.mProject._43, r2_hud_depth_limit, 0.f);
        cmd_list.set_c("da_vrs_params3", float(s.width), float(s.height), prev_valid ? 1.f : 0.f,
            target.velocity_ready() ? 1.f : 0.f);
        cmd_list.set_c("da_vrs_params4", HW.native.vrsAdditionalRates ? 1.f : 0.f, log ? 1.f : 0.f, dof_on ? dof.z : 0.f, 0.f);
        cmd_list.set_c("da_vrs_reproj", reproject);
    });
    s.image_frame = frame;
    if (log)
        da_vrs_log_stats(s, cmd_list, mode);
    return s.image[current];
}
} // namespace
#endif

void da_vrs_rate_pass(CBackend& cmd_list)
{
#if defined(XR_D3D12)
    DaVrsTargets* s = da_vrs_state();
    if (!s)
        return;
    int mode = 0;
    ID3DTexture2D* image = nullptr;
    if (!da_vrs_menu_pp())
    {
        mode = da_vrs_effective_mode();
        if (mode != s->logged_mode)
        {
            Msg("* [vrs] mode %d (preset %d, override %d, force %d)", mode, ps_r__vrs, ps_r__vrs_override, ps_r__vrs_force);
            s->logged_mode = mode;
        }
        // Targets of another render size than the frame's (a size change the targets have not
        // followed yet) must not reach a pass: the frame shades 1x1 until they are recreated.
        if (mode && (s->width != Device.dwRenderWidth || s->height != Device.dwRenderHeight))
        {
            if (!s->size_logged)
            {
                Msg("~ [vrs] render size %ux%u, targets %ux%u: off until they are recreated", Device.dwRenderWidth,
                    Device.dwRenderHeight, s->width, s->height);
                s->size_logged = true;
            }
            mode = 0;
        }
        if (mode)
            image = da_vrs_build_image(*s, cmd_list, mode);
    }
    s->frame_mode = mode;
    s->frame_image = image;
    s->state_frame.store(Device.dwFrame, std::memory_order_release);
#else
    (void)cmd_list;
#endif
}

#if defined(XR_D3D12)
namespace
{
void da_vrs_luma(CBackend& cmd_list, bool upscaled)
{
    DaVrsTargets* s = da_vrs_state();
    if (!s || da_upscaler_active() != upscaled || da_vrs_menu_pp() || !da_vrs_effective_mode())
        return;
    if (s->width != Device.dwRenderWidth || s->height != Device.dwRenderHeight)
        return;
    PIX_EVENT(phase_vrs_luma);
    dx11GpuTimerScope gpu(dx11GpuTimers::VrsLuma);
    da_vrs_unbind_targets(cmd_list);
    // A group covers 32x32 pixels (da_vrs_luma.cs).
    da_vrs_compute(cmd_list, upscaled ? s->s_luma_upscaled : s->s_luma, s->contrast->pUAView, nullptr,
        (s->width + 31) / 32, (s->height + 31) / 32,
        [&] { cmd_list.set_c("da_vrs_size", float(s->width), float(s->height), 0.f, 0.f); });
    s->luma_frame = Device.dwFrame;
}
} // namespace
#endif

void da_vrs_luma_pass(CBackend& cmd_list)
{
#if defined(XR_D3D12)
    da_vrs_luma(cmd_list, false);
#else
    (void)cmd_list;
#endif
}

void da_vrs_luma_upscaled_pass(CBackend& cmd_list)
{
#if defined(XR_D3D12)
    da_vrs_luma(cmd_list, true);
#else
    (void)cmd_list;
#endif
}

void da_vrs_debug_pass(CBackend& cmd_list)
{
#if defined(XR_D3D12)
    if (!ps_r__vrs_debug)
        return;
    DaVrsTargets* s = da_vrs_state();
    ID3DTexture2D* image = da_vrs_frame_image();
    if (!s || !s->s_debug || !image || da_vrs_menu_pp())
        return;
    PIX_EVENT(phase_vrs_debug);
    // The frame's rates; the next rate pass reads the same image as its previous one.
    da_vrs_point_alias(*s, image);

    CRenderTarget& target = *RImplementation.Target;
    u32 offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, s->g_quad->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, s->g_quad->vb_stride);

    // The binding phase_pp leaves: the back buffer at display size.
    target.u_setrt(cmd_list, Device.dwWidth, Device.dwHeight, target.get_base_rt(), nullptr, nullptr, target.get_base_zb());
    cmd_list.SetViewport({0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f});
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    cmd_list.set_Shader(s->s_debug);
    cmd_list.set_c("da_vrs_debug", float(s->width), float(s->height), float(s->tile), 0.f);
    cmd_list.set_Geometry(s->g_quad);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
#else
    (void)cmd_list;
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
