#include "stdafx.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

namespace xray::render::RENDER_NAMESPACE
{
// Where the map sits this frame: world XZ of its centre and its edge length in metres. The
// da_cloud_map binder hands these to every shader that samples the map.
float g_da_cloud_map_center_x = 0.f;
float g_da_cloud_map_center_z = 0.f;
float g_da_cloud_map_extent = 16000.f;

// The cloud deck field, once per frame: for every texel of a square of the deck plane around
// the camera, the field's density, its coverage and its detail. The sun passes read it per lit
// pixel, the shafts per march sample, the deck per sky pixel - each as one texture fetch where
// they used to evaluate six octaves of noise. 1024x1024 texels of six noise reads is nothing;
// the same reads per screen pixel per pass were fourteen milliseconds.
void CRenderTarget::phase_cloud_map()
{
    if (!s_cloud_map || !rt_cloud_map || !g_pGamePersistent)
        return;

    PIX_EVENT(DA_phase_cloud_map);

    // Snap the centre to the texel grid so the map's contents do not swim under the camera:
    // the field is evaluated in world space, so a snapped centre only moves which texels
    // exist, never what they hold.
    const float texel = g_da_cloud_map_extent / 1024.f;
    const Fvector& eye = Device.vCameraPosition;
    g_da_cloud_map_center_x = floorf(eye.x / texel) * texel;
    g_da_cloud_map_center_z = floorf(eye.z / texel) * texel;

    u_setrt(RCache, rt_cloud_map, nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
    RCache.set_Stencil(FALSE);
    RCache.set_Z(FALSE);
    RCache.set_CullMode(CULL_NONE);
    RCache.set_ColorWriteEnable();

    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    RCache.set_Element(s_cloud_map->E[0]);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // Back to the device-sized viewport for whatever renders next.
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
}
} // namespace xray::render::RENDER_NAMESPACE
