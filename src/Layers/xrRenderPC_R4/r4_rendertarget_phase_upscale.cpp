// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderDX11/StateManager/dx11SamplerStateCache.h"
#include "da_upscaler.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// The da_fullscreen.vs quad: xy is the clip-space position, zw the texture coordinate.
void da_upscale_quad(CBackend& cmd_list, ref_geom& geom, ShaderElement* element)
{
    u32 offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, geom->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, geom->vb_stride);

    cmd_list.set_Element(element);
    cmd_list.set_Geometry(geom);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
}
} // namespace

bool CRenderTarget::upscale_ready() const
{
    return s_upscale && s_combine_display && s_postprocess_display && s_menu_display && rt_Reactive && rt_Reactive->pRT &&
        rt_Upscaled && rt_Upscaled->pRT && rt_PostDisplay && rt_PostDisplay->pRT && rt_Render_Depth &&
        rt_Render_Depth != rt_Base_Depth && rt_SSR;
}

void CRenderTarget::u_setrt_scene_base(CBackend& cmd_list)
{
    if (!da_upscaler_active())
        u_setrt(cmd_list, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
    else
        u_setrt(cmd_list, Device.dwRenderWidth, Device.dwRenderHeight, nullptr, nullptr, nullptr, rt_Render_Depth);
}

// What the upscaler cannot tell from the motion vectors: the pixels the forward passes painted
// over the opaque scene - water, glass, particles, rain, fire, volumetric light. Their colour
// moves on its own, so the history there is worth little and the current frame gets the weight.
// rt_SSR holds the opaque scene exactly as combine_1 left it (taken before the puddles and the
// forward pass); the difference to the finished scene is the mask.
void CRenderTarget::phase_reactive()
{
    PIX_EVENT(phase_reactive);

    u_setrt(RCache, rt_Reactive, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.SetViewport({ 0.f, 0.f, float(rt_Reactive->dwWidth), float(rt_Reactive->dwHeight), 0.f, 1.f });
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    da_upscale_quad(RCache, g_combine, &*s_upscale->E[2]);
}

void CRenderTarget::phase_upscale_identity()
{
    PIX_EVENT(phase_upscale_identity);

    u_setrt(RCache, rt_Upscaled, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.SetViewport({ 0.f, 0.f, float(rt_Upscaled->dwWidth), float(rt_Upscaled->dwHeight), 0.f, 1.f });
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    da_upscale_quad(RCache, g_combine, &*s_upscale->E[0]);
}

// Lens flares, bullet tracers and the UI are tested against the base depth at display size, so
// the scene depth is brought up to it: the nearest render-size sample, written as depth.
void CRenderTarget::phase_depth_upscale()
{
    PIX_EVENT(phase_depth_upscale);

    u_setrt(RCache, Device.dwWidth, Device.dwHeight, nullptr, nullptr, nullptr, get_base_zb());
    RCache.SetViewport({ 0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f });
    RCache.ClearZB(get_base_zb(), 1.f, 0);
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    RCache.set_Z(TRUE);
    da_upscale_quad(RCache, g_combine, &*s_upscale->E[1]);
}

// The upscale slot: the scene (render size, jittered) into rt_Upscaled (display size), and the
// frame switches to the display stage - screen_res, the samplers' LOD bias and the projection
// follow. Everything after it (combine_2, the flares, phase_pp, the UI) runs at display size.
void CRenderTarget::phase_upscale()
{
    PIX_EVENT(phase_upscale);

    IUpscaler* upscaler = da_upscaler_current();
    if (!upscaler)
        return;

    // The SDKs read the scene targets as resources: nothing of theirs may stay bound (D3D11
    // hands a bound target to a shader as zeros).
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, nullptr, nullptr, nullptr,
        static_cast<ID3DDepthStencilView*>(nullptr));

    // Only vectors the G-buffer wrote this frame: stale ones would drag the history astray.
    ID3DBaseTexture* velocity = velocity_ready() && t_velocity ? t_velocity->surface_get() : nullptr;

    UpscaleFrame frame;
    frame.color = rt_Generic_0->pSurface;
    frame.depth = rt_Render_Depth->pSurface;
    frame.velocity = velocity;
    frame.reactive = rt_Reactive->pSurface;
    frame.output = rt_Upscaled->pSurface;
    frame.renderW = Device.dwRenderWidth;
    frame.renderH = Device.dwRenderHeight;
    frame.displayW = Device.dwWidth;
    frame.displayH = Device.dwHeight;
    const Fvector2 jitter = da_jitter_px();
    frame.jitterX = jitter.x;
    frame.jitterY = jitter.y;
    frame.frameTimeMs = Device.fTimeDeltaUnscaled * 1000.f;
    frame.nearZ = VIEWPORT_NEAR;
    frame.farZ = g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.far_plane : 1000.f;
    frame.fovY = deg2rad(Device.fFOV);
    frame.reset = da_history_reset();
    frame.sharpness = ps_r__upscale_sharpness;

    const bool identity = !upscaler->Temporal();
    if (!upscaler->Dispatch(frame))
    {
        da_upscaler_fail("dispatch failed");
        if (IUpscaler* fallback = da_upscaler_current())
            fallback->Dispatch(frame);
    }
    // An SDK records its own compute work on the immediate context and leaves its state there.
    if (!identity)
        RCache.Invalidate();
    _RELEASE(velocity);

    phase_depth_upscale();

    // The display stage: screen_res follows the display size again, the scene's LOD bias is
    // over, and whatever draws 3D from here on (flares, tracers) is not jittered.
    da_stage_scene(false);
    SSManager.UseSceneBias(RCache.context_id, false);
    RCache.set_xform_project(Device.mProject);
}
} // namespace xray::render::RENDER_NAMESPACE
