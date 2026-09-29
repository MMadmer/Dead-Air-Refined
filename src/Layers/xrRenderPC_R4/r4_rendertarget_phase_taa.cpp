// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
IReader* open_shader(pcstr shader); // ResourceManager.cpp

// Temporal AA. One resolve pass: the current LDR frame, rendered through the jittered projection
// (da_temporal.h), is blended with the previous resolved frame reprojected through the motion
// vectors when the velocity pass made them this frame, through the unjittered camera matrices of
// the mblur block otherwise - under a 3x3 neighbourhood clamp (see da_taa.ps for the ghosting
// bounds). The result lands in generic0 (RGBA8, target 0); the 10-bit second target is the next
// frame's history.
// Runs right after the spatial AA slot of phase_combine - SMAA removes geometric staircases,
// this pass resolves the jitter and the temporal shimmer SMAA cannot see. SMAA leaves its frame in
// rt_Generic, so the resolve reads it there and writes generic0 directly; alone, it reads generic0
// and has to go through rt_Generic and a copy. The history targets swap roles every frame.
void CRenderTarget::phase_taa(const Fmatrix& reproject, bool smaa_in_generic)
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

    // The motion vectors of the G-buffer (MRT 2), when it wrote them this frame: a target left
    // from an earlier frame, or never made, would say "nothing moved" or worse.
    const bool velocity = velocity_ready() && da_temporal_active();
    // The jitter in uv, y down: the scene at a pixel was sampled that far from its centre.
    const Fvector2 jitter = da_jitter_px();
    const float jitter_u = jitter.x / float(Device.dwRenderWidth);
    const float jitter_v = jitter.y / float(Device.dwRenderHeight);

    const ref_rt& history_out = m_taa_history_in_resolve ? rt_taa_history : rt_taa_resolve;
    u_setrt(RCache, smaa_in_generic ? rt_Generic_0 : rt_Generic, history_out, static_cast<ID3DDepthStencilView*>(nullptr));
    RCache.set_Element(s_taa->E[(smaa_in_generic ? 2 : 0) + (m_taa_history_in_resolve ? 1 : 0)]);
    RCache.set_Geometry(g_combine);
    RCache.set_c("m_taa_previous", reproject);
    RCache.set_c("da_taa_params", velocity ? 1.f : 0.f, da_history_reset() ? 1.f : 0.f, jitter_u, jitter_v);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    m_taa_history_in_resolve = !m_taa_history_in_resolve;

    if (!smaa_in_generic)
        HW.get_context(RCache.context_id)->CopyResource(rt_Generic_0->pSurface, rt_Generic->pSurface);
}

// r__motion_vectors 1: the G-buffer's motion vectors laid over the finished frame at display
// size - hue is the direction, strength the length (da_velocity_debug.ps). The shader comes
// with the content; without it the view stays off instead of failing the frame.
void CRenderTarget::phase_velocity_debug()
{
    if (!velocity_ready())
        return;
    if (!s_velocity_debug && !m_velocity_debug_tried)
    {
        m_velocity_debug_tried = true;
        IReader* script = open_shader("da_velocity_debug.s");
        IReader* code = open_shader("da_velocity_debug.ps");
        if (script && code)
            s_velocity_debug.create("da_velocity_debug");
        else
            Msg("! [temporal] r__motion_vectors: da_velocity_debug is not in the shaders");
        FS.r_close(script);
        FS.r_close(code);
    }
    if (!s_velocity_debug)
        return;

    PIX_EVENT(phase_velocity_debug);

    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    // The same binding phase_pp leaves: the back buffer and its depth (the shader tests none).
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
    RCache.SetViewport({ 0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f });
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(FALSE);
    RCache.set_Shader(s_velocity_debug);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}
} // namespace xray::render::RENDER_NAMESPACE
