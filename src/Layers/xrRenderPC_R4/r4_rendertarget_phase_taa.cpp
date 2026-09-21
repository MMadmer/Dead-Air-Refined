// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
// Camera-reprojection temporal AA. One resolve pass: the current LDR frame (generic0)
// is blended with the previous resolved frame reprojected through the mblur matrix,
// under a 3x3 neighbourhood clamp (see da_taa.ps for the ghosting bounds). The result
// goes back into generic0 (RGBA8, target 0) and, through the 10-bit second target,
// becomes the next frame's history.
// Runs right after the spatial AA slot of phase_combine - SMAA removes geometric
// staircases, this pass removes the temporal shimmer SMAA cannot see.
void CRenderTarget::phase_taa(const Fmatrix& reproject)
{
    PIX_EVENT(phase_taa);

    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);

    u_setrt(RCache, rt_Generic, rt_taa_resolve, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.set_Element(s_taa->E[0]);
    RCache.set_Geometry(g_combine);
    RCache.set_c("m_taa_previous", reproject);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // Resolve back into the frame; the 10-bit copy becomes the next frame's history.
    HW.get_context(RCache.context_id)->CopyResource(rt_Generic_0->pSurface, rt_Generic->pSurface);
    HW.get_context(RCache.context_id)->CopyResource(rt_taa_history->pSurface, rt_taa_resolve->pSurface);
}
} // namespace xray::render::RENDER_NAMESPACE
