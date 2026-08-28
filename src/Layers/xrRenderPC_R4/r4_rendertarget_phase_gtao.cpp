#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
// Ground-truth based ambient occlusion, ported from IX-Ray. Two fullscreen passes at
// full resolution: the horizon integral writes (view-z, raw AO) into rt_gtao, the
// guided 8x8 filter turns that into the final AO in rt_ssao_temp - the same target the
// other AO modes feed, so combine_1 consumes it through the existing s_occ binding.
// Runs at the head of phase_combine, i.e. after light accumulation and right before
// the ambient term is assembled - the donor runs it at the same spot.
void CRenderTarget::phase_gtao(CBackend& cmd_list)
{
    PIX_EVENT_CTX(cmd_list, phase_gtao);

    // World-space radius -> screen-space radius, donor formula:
    // height / (2 * tan(fov/2)) * 0.5. fFOV is the vertical FOV, like the
    // pos_decompression binder assumes.
    const float p_scale =
        float(Device.dwHeight) / (tanf(deg2rad(Device.fFOV * 0.5f)) * 2.0f) * 0.5f;

    // Fullscreen quad in the da_fullscreen.vs layout: xy = clip pos, zw = texcoord.
    // Same vertices as phase_da_puddle_refl - a layout that does not match the VS is
    // dropped by D3D11 without a message.
    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    {
        PIX_EVENT_CTX(cmd_list, gtao_render);
        // No clear: the pass writes every pixel, sky included (the shader parks sky at
        // "far away, unoccluded" so the filter can reject it by depth).
        u_setrt(cmd_list, rt_gtao, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);

        cmd_list.set_Element(s_gtao->E[0]);
        cmd_list.set_Geometry(g_combine);
        cmd_list.set_c("gtao_parameters", p_scale, 0.f, 0.f, 0.f);
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    }

    {
        PIX_EVENT_CTX(cmd_list, gtao_filter);
        u_setrt(cmd_list, rt_ssao_temp, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);

        cmd_list.set_Element(s_gtao->E[1]);
        cmd_list.set_Geometry(g_combine);
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    }
}
} // namespace xray::render::RENDER_NAMESPACE
