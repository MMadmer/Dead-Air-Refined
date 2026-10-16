// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The render-target side of ray tracing (docs/dead-air/dx12-features/dxr.md 5): its targets behind
// CRenderTarget::m_da_rt, the mirror requests of the water and the puddles, and the debug view.

#include "stdafx.h"

#include "da_rt.h"
#include "da_rt_device.h"
#include "Layers/xrRender/r__dsgraph_types.h"

namespace xray::render::RENDER_NAMESPACE
{
#if defined(DA_RT_LANE)
// Accumulated ground wetness, published by the rain_params binder (r2.cpp).
extern float g_da_rain_wetness;
// The forward water surfaces of this frame (r4_rendertarget_phase_sky_water_velocity.cpp).
bool da_water_forward_items_empty();
void da_water_forward_draw(CBackend& cmd_list, ShaderElement* element, ShaderElement* element_b);

namespace
{
// The da_fullscreen.vs quad: xy is the clip-space position, zw the texture coordinate.
void da_rt_fullscreen(CBackend& cmd_list, CRenderTarget& target)
{
    u32 offset = 0;
    ref_geom& quad = DaRtAccess::quad(target);
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, quad->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    RImplementation.Vertex.Unlock(4, quad->vb_stride);
    cmd_list.set_Geometry(quad);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
}
} // namespace

DaRtTargets* da_rt_targets() { return RImplementation.Target ? RImplementation.Target->m_da_rt : nullptr; }

bool da_rt_targets_ready(RtEffect effect)
{
    const DaRtTargets* rt = da_rt_targets();
    if (!rt)
        return false;
    switch (effect)
    {
    case RtEffect::Shadows:
        return rt->sun && rt->sun->pUAView && rt->sun_raw && rt->sun_raw->pUAView && rt->sun_hist[0] &&
            rt->sun_hist[0]->pUAView && rt->sun_hist[1] && rt->sun_hist[1]->pUAView;
    case RtEffect::AO:
    {
        CRenderTarget& t = *RImplementation.Target;
        const ref_rt& gtao = DaRtAccess::gtao_target(t);
        return rt->ao_raw && rt->ao_raw->pUAView && rt->ao_hist[0] && rt->ao_hist[0]->pUAView && rt->ao_hist[1] &&
            rt->ao_hist[1]->pUAView && DaRtAccess::gtao(t) && gtao && gtao->pUAView && DaRtAccess::ao_target(t);
    }
    case RtEffect::Reflections:
        return rt->refl_req && rt->refl_req->pRT && rt->refl && rt->refl->pUAView && RImplementation.Target->rt_SSR;
    default: return false;
    }
}

bool da_rt_reflection_requests(CBackend& cmd_list, CRenderTarget& target, DaRtTargets& rt)
{
    const bool puddles = ps_r__puddles && ps_r__puddles_refl && ps_r__puddles_refl_power > 0.f && g_da_rain_wetness >= 0.01f &&
        DaRtAccess::puddle_pass(target);
    const bool water = !da_water_forward_items_empty();
    if (!puddles && !water)
        return false;

    PIX_EVENT(DA_rt_reflection_requests);
    cmd_list.ClearRT(rt.refl_req, {});
    target.u_setrt(cmd_list, rt.refl_req, nullptr, nullptr, target.rt_MSAADepth);
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    cmd_list.set_ColorWriteEnable();

    // Puddles first: water drawn over them covers the ground under it.
    if (puddles)
    {
        if (!rt.s_puddle_req)
            rt.s_puddle_req.create("da_puddle_refl_req");
        if (rt.s_puddle_req)
        {
            cmd_list.set_Z(FALSE);
            cmd_list.set_Element(rt.s_puddle_req->E[0]);
            // The same gates as the puddle pass (phase_da_puddle_refl).
            cmd_list.set_c("da_puddle_refl", ps_r__puddles_refl_power, float(ps_r__puddles_refl), ps_r__puddles_facing,
                ps_r__puddles_sky);
            da_rt_fullscreen(cmd_list, target);
        }
    }
    if (water)
    {
        if (!rt.s_water_req)
            rt.s_water_req.create("da_water_rtreq");
        if (!rt.s_water_req_b)
            rt.s_water_req_b.create("da_water_green_rtreq");
        if (rt.s_water_req)
        {
            cmd_list.set_xform_world(Fidentity);
            da_water_forward_draw(cmd_list, rt.s_water_req->E[0]._get(), rt.s_water_req_b ? rt.s_water_req_b->E[0]._get() : nullptr);
        }
    }
    cmd_list.set_RT(nullptr, 0);
    return true;
}

void da_rt_debug_blit(CBackend& cmd_list, CRenderTarget& target, DaRtTargets& rt)
{
    if (!rt.debug)
        return;
    if (!rt.s_debug)
        rt.s_debug.create("da_rt_debug");
    if (!rt.s_debug)
        return;
    PIX_EVENT(DA_rt_debug);
    target.u_setrt(cmd_list, target.rt_Generic_0_r, nullptr, nullptr, target.rt_MSAADepth);
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    cmd_list.set_Z(FALSE);
    cmd_list.set_ColorWriteEnable();
    cmd_list.set_Element(rt.s_debug->E[0]);
    da_rt_fullscreen(cmd_list, target);
}
#endif

void da_rt_targets_create(CRenderTarget& target)
{
#if defined(DA_RT_LANE)
    da_rt_targets_destroy(target);
    if (!da_rt_available() || !(ps_r__rt_reflections || ps_r__rt_shadows || ps_r__rtao))
        return;
    // The first effect switched on on this device brings the pass pipelines.
    da_rt_passes_create();
    auto* rt = xr_new<DaRtTargets>();
    target.m_da_rt = rt;
    const u32 w = Device.dwRenderWidth, h = Device.dwRenderHeight;
    rt->width = w;
    rt->height = h;
    const Flags32 uav{CRT::CreateUAV};
    if (ps_r__rt_shadows)
    {
        rt->sun_raw.create(r2_RT_rt_sun_raw, w, h, D3DFMT_G16R16F, 1, uav);
        rt->sun_hist[0].create(r2_RT_rt_sun_hist0, w, h, D3DFMT_A16B16G16R16F, 1, uav);
        rt->sun_hist[1].create(r2_RT_rt_sun_hist1, w, h, D3DFMT_A16B16G16R16F, 1, uav);
        rt->sun.create(r2_RT_rt_sun, w, h, D3DFMT_L8, 1, uav);
    }
    if (ps_r__rtao)
    {
        // The trace resolution follows the quality the renderer started with.
        rt->ao_half = da_rt_tier_of(ps_r__rt_quality).ao_half;
        rt->ao_width = rt->ao_half ? (w + 1) / 2 : w;
        rt->ao_height = rt->ao_half ? (h + 1) / 2 : h;
        rt->ao_raw.create(r2_RT_rt_ao_half, rt->ao_width, rt->ao_height, D3DFMT_G16R16F, 1, uav);
        rt->ao_hist[0].create(r2_RT_rt_ao_hist0, rt->ao_width, rt->ao_height, D3DFMT_A16B16G16R16F, 1, uav);
        rt->ao_hist[1].create(r2_RT_rt_ao_hist1, rt->ao_width, rt->ao_height, D3DFMT_A16B16G16R16F, 1, uav);
    }
    if (ps_r__rt_reflections)
    {
        rt->refl_req.create(r2_RT_rt_refl_req, w, h, D3DFMT_A16B16G16R16F, 1);
        rt->refl.create(r2_RT_rt_refl, w, h, D3DFMT_A16B16G16R16F, 1, uav);
        // The glossy surfaces' (dxr.md 3.9), for the quality the renderer started with.
        if (da_rt_tier_of(ps_r__rt_quality).gloss)
        {
            rt->gloss_width = (w + 1) / 2;
            rt->gloss_height = (h + 1) / 2;
            rt->gloss_raw.create(r2_RT_rt_gloss_raw, rt->gloss_width, rt->gloss_height, D3DFMT_A16B16G16R16F, 1, uav);
            rt->gloss_dist.create(r2_RT_rt_gloss_dist, rt->gloss_width, rt->gloss_height, D3DFMT_R16F, 1, uav);
            rt->gloss_hist[0].create(r2_RT_rt_gloss_hist0, w, h, D3DFMT_A16B16G16R16F, 1, uav);
            rt->gloss_hist[1].create(r2_RT_rt_gloss_hist1, w, h, D3DFMT_A16B16G16R16F, 1, uav);
            rt->gloss.create(r2_RT_rt_gloss, w, h, D3DFMT_A16B16G16R16F, 1, uav);
            rt->gloss_scene.create(r2_RT_rt_gloss_scene, w, h, D3DFMT_A16B16G16R16F, 1);
        }
    }
    rt->t_env0.create(r2_T_envs0);
    rt->t_env1.create(r2_T_envs1);
    rt->t_sky0.create(r2_T_sky0);
    rt->t_sky1.create(r2_T_sky1);
    rt->t_tonemap.create(r2_RT_luminance_cur);
#else
    UNUSED(target);
#endif
}

void da_rt_targets_destroy(CRenderTarget& target)
{
#if defined(DA_RT_LANE)
    xr_delete(target.m_da_rt);
#else
    UNUSED(target);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
