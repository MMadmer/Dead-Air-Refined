#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
// SMAA 1x (Enhanced Subpixel Morphological Antialiasing), ported from IX-Ray 1.6 stcop.
// Three fullscreen passes over the tonemapped LDR frame in $user$generic0:
//   1) edge detect   -> rt_smaa_edges (RG mask; discard keeps non-edges at the cleared 0)
//   2) blend weights -> rt_smaa_blend (edge mask + two baked LUTs)
//   3) neighbour blend -> rt_Generic, then copied back over generic0 (phase_fxaa pattern)
// Runs in the FXAA slot of phase_combine and supersedes it (see the call site).
// Donor deviations, on purpose: no stencil "optimization" (theirs never ran - stencil ops
// with no DSV bound), and rt_Generic instead of their rt_Generic_2 (ours is the live light
// accumulator, FP16 and MSAA-sampled under MSAA).
void CRenderTarget::phase_smaa()
{
    PIX_EVENT(phase_smaa);

    u32 Offset = 0;

    // Same clip-space quad as phase_da_puddle_refl: da_fullscreen.vs takes xy as position
    // and zw as texcoord. Layout must stay FVF::TL - a mismatch is dropped silently.
    const auto fullscreen_quad = [&]()
    {
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
    };

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);

    // Pass 1: edge detection. Both working targets are cleared every frame - the shaders
    // discard on "nothing to do", which would otherwise keep last frame's bytes.
    u_setrt(RCache, rt_smaa_edges, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.ClearRT(rt_smaa_edges, {});
    fullscreen_quad();
    RCache.set_Element(s_smaa->E[0]);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // Pass 2: blending weight calculation
    u_setrt(RCache, rt_smaa_blend, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.ClearRT(rt_smaa_blend, {});
    fullscreen_quad();
    RCache.set_Element(s_smaa->E[1]);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // Pass 3: neighbourhood blending, resolved back into the frame like phase_fxaa
    u_setrt(RCache, rt_Generic, nullptr, nullptr, rt_Base_Depth);
    fullscreen_quad();
    RCache.set_Element(s_smaa->E[2]);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    HW.get_context(RCache.context_id)->CopyResource(rt_Generic_0->pSurface, rt_Generic->pSurface);
}
} // namespace xray::render::RENDER_NAMESPACE
