#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
// Accumulated ground wetness, published by the rain_params binder (r2.cpp).
extern float g_da_rain_wetness;

// World reflections in rain puddles: one fullscreen pass over the lit frame.
//
// The spot in the frame is chosen, not incidental. The pass sits RIGHT AFTER the frame copy
// taken for the water SSR and BEFORE the forward pass that draws water:
//   * after the copy - the thing to reflect is the lit picture, and the copy is it;
//   * before water - otherwise puddles would reflect water that then gets drawn on top,
//     giving a reflection of a reflection on every lake shore.
//
// Reads the copy (rt_SSR), writes into the scene (rt_Generic_0_r). Reading and writing one
// target is not allowed - DirectX returns garbage at best, silently; here source and
// destination differ by construction.
void CRenderTarget::phase_da_puddle_refl()
{
    if (!ps_r__puddles || !ps_r__puddles_refl || ps_r__puddles_refl_power <= 0.f)
        return;
    // Dry ground: the shader's mask is zero everywhere, the whole pass would only pay a
    // G-buffer load per pixel to output nothing. The same threshold the mask math uses.
    if (g_da_rain_wetness < 0.01f)
        return;
    if (!s_puddle_refl || !rt_SSR)
        return;

    PIX_EVENT(DA_phase_puddle_reflections);

    u_setrt(RCache, rt_Generic_0_r, nullptr, nullptr, rt_MSAADepth);
    RCache.set_Stencil(FALSE);
    RCache.set_Z(FALSE);
    RCache.set_CullMode(CULL_NONE);

    // FVF::TL and the same geometry as our other fullscreen passes: a vertex layout that
    // does not match the vertex shader is dropped by DirectX without a single message.
    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    RCache.set_Element(s_puddle_refl->E[0]);
    RCache.set_Geometry(g_combine);
    // y = the reflection level: 1 = sky only (no depth march), 2 = the world ray-march.
    // z = fresnel floor (r__puddles_facing), w = sky share on a ray miss.
    RCache.set_c("da_puddle_refl", ps_r__puddles_refl_power, float(ps_r__puddles_refl), ps_r__puddles_facing,
        ps_r__puddles_sky);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}
} // namespace xray::render::RENDER_NAMESPACE
