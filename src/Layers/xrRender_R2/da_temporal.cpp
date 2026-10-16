// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The temporal state of a frame (da_temporal.h): projection jitter, history resets, and which
// size the passes of the current stage draw at.

#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderPC_R4/da_upscaler.h"
#include "Layers/xrRenderPC_R4/da_framegen.h"
#include "Layers/xrRenderPC_R4/da_rt.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// A camera that moved further than this in one frame was put somewhere: a teleport, a level
// change, a cut. Turning does not count - the history reprojects through a rotation.
constexpr float da_camera_cut_distance = 25.f;

struct temporal_frame
{
    bool active{};
    bool motion{};
    bool reset{};
    Fvector2 jitter_px{};
    Fvector2 jitter_clip{};
};
temporal_frame g_da_frame;

// Nothing has a history before the first frame.
bool g_da_reset_pending = true;
Fvector g_da_prev_camera{};
bool g_da_prev_camera_valid = false;
bool g_da_prev_menu = true;
bool g_da_stage_scene = false;

// The unjittered view-projections of the motion vectors: the main view's, and the HUD's of the
// frame it was last drawn on.
struct motion_views
{
    Fmatrix vp_nojit{ Fidentity };
    Fmatrix vp_prev{ Fidentity };
    Fmatrix vp_last{ Fidentity }; // this frame's once the frame has ended
    bool vp_last_valid{};
    Fmatrix hud_cur{ Fidentity };
    u32 hud_cur_frame{};
    Fmatrix hud_last{ Fidentity };
    u32 hud_last_frame{};
};
motion_views g_da_views;

// Halton low-discrepancy sequence: the sample positions both AMD and NVIDIA prescribe.
float da_halton(u32 index, u32 base)
{
    float f = 1.f, result = 0.f;
    while (index > 0)
    {
        f /= float(base);
        result += f * float(index % base);
        index /= base;
    }
    return result;
}
} // namespace

bool da_temporal_active() { return g_da_frame.active; }

bool da_motion_vectors_active() { return g_da_frame.motion; }

Fvector2 da_jitter_px() { return g_da_frame.jitter_px; }

Fvector2 da_jitter_clip() { return g_da_frame.jitter_clip; }

bool da_history_reset() { return g_da_frame.reset; }

void da_history_request_reset(pcstr why)
{
    if (!g_da_reset_pending && (g_da_frame.active || da_upscaler_active() || ps_r__taa))
        Msg("* [temporal] history reset: %s", why);
    g_da_reset_pending = true;
}

void da_history_carry_reset()
{
    if (g_da_frame.reset)
        g_da_reset_pending = true;
}

void da_temporal_frame_begin()
{
    const bool menu = !g_pGamePersistent || g_pGamePersistent->MainMenuActiveOrLevelNotExist() ||
        g_pGamePersistent->OnRenderPPUI_query();
    if (g_da_prev_menu && !menu)
        da_history_request_reset("back from the menu");
    g_da_prev_menu = menu;

    const Fvector& eye = Device.vCameraPosition;
    if (g_da_prev_camera_valid && eye.distance_to(g_da_prev_camera) > da_camera_cut_distance)
        da_history_request_reset("camera cut");
    g_da_prev_camera = eye;
    g_da_prev_camera_valid = true;

    g_da_frame.reset = g_da_reset_pending;
    g_da_reset_pending = false;

    // Device.mProject is never jittered, so the engine's full transform is the unjittered one.
    g_da_views.vp_nojit = Device.mFullTransform;
    g_da_views.vp_prev = g_da_views.vp_last_valid && !g_da_frame.reset ? g_da_views.vp_last : Device.mFullTransform;

    // Exactly one temporal filter: an upscaler replaces TAA. TAA needs the single-sampled
    // G-buffer (its shader is not made under MSAA); the identity stretch reconstructs nothing.
    const IUpscaler* upscaler = da_upscaler_current();
    const bool upscale = upscaler && upscaler->Temporal();
    const CRenderTarget* target = RImplementation.Target;
    const bool taa = !upscaler && ps_r__taa && target && !RImplementation.o.msaa && target->rt_taa_history &&
        target->rt_taa_history->pRT && target->rt_taa_resolve && target->rt_taa_resolve->pRT;
    g_da_frame.active = !menu && (upscale || taa);
    // Native TAA blends ~16 jittered frames: textures take half a mip sharper (with at least 8x
    // anisotropy, as under the upscalers, so the ground does not shimmer). The upscalers set their own.
    if (!upscaler)
        SSManager.SetSceneMipBias(taa ? -0.5f : 0.f, taa ? 8u : 0u);
    // Frame generation and the ray traced effects' histories read the vectors even without a
    // temporal filter.
    g_da_frame.motion = g_da_frame.active || (!menu && (da_framegen_running() || da_rt_wants_motion()));

    g_da_frame.jitter_px = {};
    g_da_frame.jitter_clip = {};
    const u32 rw = Device.dwRenderWidth, rh = Device.dwRenderHeight;
    if (!g_da_frame.active || g_da_frame.reset || !rw || !rh)
        return;

    // AMD's phase count: enough samples to cover a display pixel at the upscale ratio, 8 at the
    // least (DLSS asks for as many). Native TAA takes 16 (DOOM Eternal 32): its slower blend sees
    // them all. Halton index 0 is (0, 0) in both bases, hence the + 1.
    const float ratio = float(Device.dwWidth) / float(rw);
    const u32 phases = upscale ? std::max(8u, u32(std::ceil(8.f * ratio * ratio))) : 16u;
    const u32 index = Device.dwFrame % phases + 1;
    g_da_frame.jitter_px.set(da_halton(index, 2) - 0.5f, da_halton(index, 3) - 0.5f);
    g_da_frame.jitter_clip.set(2.f * g_da_frame.jitter_px.x / float(rw), -2.f * g_da_frame.jitter_px.y / float(rh));
}

void da_temporal_frame_end()
{
    g_da_views.vp_last = g_da_views.vp_nojit;
    g_da_views.vp_last_valid = true;
    if (g_da_views.hud_cur_frame == Device.dwFrame)
    {
        g_da_views.hud_last = g_da_views.hud_cur;
        g_da_views.hud_last_frame = Device.dwFrame;
    }
}

const Fmatrix& da_vp_nojit() { return g_da_views.vp_nojit; }

const Fmatrix& da_vp_prev() { return g_da_views.vp_prev; }

void da_hud_vp(const Fmatrix& hud_project, Fmatrix& vp_nojit, Fmatrix& vp_prev)
{
    vp_nojit.mul(hud_project, Device.mView);
    // A HUD that was not drawn on the previous frame has no previous view to speak of.
    const bool continuous = !g_da_frame.reset && g_da_views.hud_last_frame &&
        g_da_views.hud_last_frame + 1 == Device.dwFrame;
    vp_prev = continuous ? g_da_views.hud_last : vp_nojit;
    g_da_views.hud_cur = vp_nojit;
    g_da_views.hud_cur_frame = Device.dwFrame;
}

Fmatrix da_jitter_project(const Fmatrix& P)
{
    const Fvector2& j = g_da_frame.jitter_clip;
    if (j.x == 0.f && j.y == 0.f)
        return P;

    // clip.xy += j * clip.w: the w column (_14/_24/_34/_44) scaled into the x and y columns.
    // For a perspective projection that is _31/_32 alone.
    Fmatrix R = P;
    R._11 += j.x * P._14;
    R._21 += j.x * P._24;
    R._31 += j.x * P._34;
    R._41 += j.x * P._44;
    R._12 += j.y * P._14;
    R._22 += j.y * P._24;
    R._32 += j.y * P._34;
    R._42 += j.y * P._44;
    return R;
}

Fvector4 da_pos_decompress(float fov_deg)
{
    const float VertTan = -1.0f * tanf(deg2rad(fov_deg / 2.0f));
    const float HorzTan = -VertTan / Device.fASPECT;
    Fvector4 params;
    params.set(HorzTan, VertTan, (2.0f * HorzTan) / (float)Device.dwRenderWidth,
        (2.0f * VertTan) / (float)Device.dwRenderHeight);
    // A pixel of the jittered frame shows the scene jitter_px away from its own centre.
    const Fvector2& j = g_da_frame.jitter_px;
    params.x += j.x * params.z;
    params.y += j.y * params.w;
    return params;
}

void da_stage_scene(bool scene) { g_da_stage_scene = scene; }

u32 da_stage_width() { return g_da_stage_scene ? Device.dwRenderWidth : Device.dwWidth; }

u32 da_stage_height() { return g_da_stage_scene ? Device.dwRenderHeight : Device.dwHeight; }
} // namespace xray::render::RENDER_NAMESPACE
