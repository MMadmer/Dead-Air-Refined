#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"

namespace xray::render::RENDER_NAMESPACE
{
// Previous frame's camera basis (the same model the march builds its rays from - not the
// engine's full transform, whose conventions the reprojection must not have to match) and the
// frame counter for the temporal blend of the march.
static Fvector4 g_da_cloud_prev_r{}, g_da_cloud_prev_u{}, g_da_cloud_prev_d{};
static bool g_da_cloud_hist_valid = false;
static u32 g_da_cloud_frame = 0;

static int da_cloud_tier()
{
    return ps_r__clouds_quality_override >= 0 ? ps_r__clouds_quality_override : ps_r__clouds_quality;
}

// The camera for the full-screen passes: basis vectors and the half-angle tangents, so a
// pixel becomes a world ray without the projection matrix.
static void da_cloud_set_camera(CBackend& cmd_list)
{
    const float tan_y = tanf(deg2rad(Device.fFOV * 0.5f));
    const float tan_x = tan_y * Device.fASPECT;
    const Fvector& r = Device.vCameraRight;
    const Fvector& u = Device.vCameraTop;
    const Fvector& d = Device.vCameraDirection;
    cmd_list.set_c("da_cloud_cam_r", r.x, r.y, r.z, tan_x);
    cmd_list.set_c("da_cloud_cam_u", u.x, u.y, u.z, tan_y);
    cmd_list.set_c("da_cloud_cam_d", d.x, d.y, d.z, 0.f);
}

static void da_cloud_remember_camera()
{
    const float tan_y = tanf(deg2rad(Device.fFOV * 0.5f));
    const float tan_x = tan_y * Device.fASPECT;
    g_da_cloud_prev_r.set(Device.vCameraRight.x, Device.vCameraRight.y, Device.vCameraRight.z, tan_x);
    g_da_cloud_prev_u.set(Device.vCameraTop.x, Device.vCameraTop.y, Device.vCameraTop.z, tan_y);
    g_da_cloud_prev_d.set(Device.vCameraDirection.x, Device.vCameraDirection.y, Device.vCameraDirection.z, 0.f);
}

// One clip-space quad through the da_fullscreen vertex shader (see da_fullscreen.vs for the
// vertex layout it expects).
static void da_cloud_draw_quad(CBackend& cmd_list, ref_geom& g_combine)
{
    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
    cmd_list.set_Geometry(g_combine);
    cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

// The volumetric march (da_clouds_march.ps) at half resolution, every direction, blended
// with the previous frame's result. Runs before the sun so the composite finds it ready.
void CRenderTarget::phase_clouds_march()
{
    if (!s_clouds_march || !rt_clouds[0] || !rt_clouds[1] || !g_pGamePersistent)
        return;
    const int tier = da_cloud_tier();
    if (tier < 2)
    {
        g_da_cloud_hist_valid = false;
        return;
    }

    PIX_EVENT(DA_phase_clouds_march);

    u_setrt(RCache, rt_clouds[0], nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
    // u_setrt binds the target but leaves the viewport alone (the stock passes set their own);
    // without this the quad rasterizes at screen size and only its top-left lands in the target.
    RCache.SetViewport({ 0.f, 0.f, float(rt_clouds[0]->dwWidth), float(rt_clouds[0]->dwHeight), 0.f, 1.f });
    RCache.set_Stencil(FALSE);
    RCache.set_Z(FALSE);
    RCache.set_CullMode(CULL_NONE);
    RCache.set_ColorWriteEnable();

    RCache.set_Element(s_clouds_march->E[0]);
    da_cloud_set_camera(RCache);
    // Tier 2 marches 16 steps, tier 3 marches 28; the temporal blend gives both the rest.
    const float steps = tier >= 3 ? 28.f : 16.f;
    const float blend = g_da_cloud_hist_valid ? 0.88f : 0.f;
    RCache.set_c("da_cloud_temporal", blend, float(g_da_cloud_frame % 4096), g_da_cloud_hist_valid ? 1.f : 0.f, steps);
    RCache.set_c("da_cloud_prev_r", g_da_cloud_prev_r);
    RCache.set_c("da_cloud_prev_u", g_da_cloud_prev_u);
    RCache.set_c("da_cloud_prev_d", g_da_cloud_prev_d);
    da_cloud_draw_quad(RCache, g_combine);

    // This frame becomes the history of the next.
    HW.get_context(RCache.context_id)->CopyResource(rt_clouds[1]->pSurface, rt_clouds[0]->pSurface);
    da_cloud_remember_camera();
    g_da_cloud_hist_valid = true;
    ++g_da_cloud_frame;

    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
}

// The deck over the sky (da_clouds.ps): the sky pixels are the ones the g-buffer never marked
// (geometry writes a stencil of 1 or more), so the quad is drawn where 1 > stencil.
void CRenderTarget::phase_clouds_composite()
{
    if (!s_clouds_composite || !g_pGamePersistent)
        return;

    PIX_EVENT(DA_phase_clouds_composite);

    RCache.set_Stencil(TRUE, D3DCMP_GREATER, 0x01, 0xff, 0x00);
    RCache.set_Z(FALSE);
    RCache.set_CullMode(CULL_NONE);
    RCache.set_ColorWriteEnable();
    RCache.set_Element(s_clouds_composite->E[0]);
    da_cloud_set_camera(RCache);
    da_cloud_draw_quad(RCache, g_combine);
    RCache.set_Stencil(FALSE);
}
} // namespace xray::render::RENDER_NAMESPACE
