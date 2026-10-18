#include "stdafx.h"

#include "da_async.h"

namespace xray::render::RENDER_NAMESPACE
{
// World-space radius -> screen-space radius, donor formula:
// height / (2 * tan(fov/2)) * 0.5. fFOV is the vertical FOV, like the
// pos_decompression binder assumes.
static float gtao_screen_scale()
{
    return float(Device.dwRenderHeight) / (tanf(deg2rad(Device.fFOV * 0.5f)) * 2.0f) * 0.5f;
}

// The slices of the preset's tier and the frame's turn of the noise (R2 sequence): the temporal pass gathers the turns.
static Fvector4 gtao_params(float scale)
{
    const float n = float(Device.dwFrame & 1023);
    const float a = n * 0.7548776662f, b = n * 0.5698402910f;
    return { scale, float(ps_r__gtao_slices), a - floorf(a), b - floorf(b) };
}

// Ground-truth based ambient occlusion, ported from IX-Ray. Two fullscreen passes at
// full resolution: the horizon integral writes (view-z, raw AO) into rt_gtao, the
// guided 8x8 filter turns that into the final AO in rt_ssao_temp - the same target the
// other AO modes feed, so combine_1 consumes it through the existing s_occ binding.
// Runs at the head of phase_combine, i.e. after light accumulation and right before
// the ambient term is assembled - the donor runs it at the same spot.
void CRenderTarget::phase_gtao(CBackend& cmd_list)
{
    PIX_EVENT_CTX(cmd_list, phase_gtao);

    const float p_scale = gtao_screen_scale();

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
        cmd_list.set_c("gtao_parameters", gtao_params(p_scale));
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    }

    {
        PIX_EVENT_CTX(cmd_list, gtao_filter);
        u_setrt(cmd_list, rt_gtao_cur, nullptr, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_Stencil(FALSE);

        cmd_list.set_Element(s_gtao->E[1]);
        cmd_list.set_Geometry(g_combine);
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    }
}

// The temporal accumulation (gtao_temporal.ps): the filtered AO of the frame with last frame's reprojected into
// rt_ssao_temp, which combine_1 samples, and the history for the next frame - ping-pong between the pair. A cut (a
// load, a teleport, back from the menu) or a missed frame starts it over.
void CRenderTarget::phase_gtao_temporal(CBackend& cmd_list)
{
    if (!rt_gtao_cur || !rt_gtao_hist[0] || !rt_gtao_hist[1])
        return;
    PIX_EVENT_CTX(cmd_list, gtao_temporal);
    const bool history = ps_r__gtao_temporal && m_gtao_frame + 1 == Device.dwFrame && !da_history_reset();
    const u32 read = m_gtao_hist;
    const u32 write = read ^ 1u;
    Fmatrix reproj;
    reproj.mul(m_gtao_prev_vp, Device.mInvView);

    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    u_setrt(cmd_list, rt_ssao_temp, rt_gtao_hist[write], nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
    cmd_list.SetViewport({ 0.f, 0.f, float(rt_ssao_temp->dwWidth), float(rt_ssao_temp->dwHeight), 0.f, 1.f });
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    cmd_list.set_Element(s_gtao->E[2 + read]);
    cmd_list.set_Geometry(g_combine);
    cmd_list.set_c("m_gtao_reproj", reproj);
    cmd_list.set_c("gtao_temporal", history ? 1.f : 0.f, ps_r__gtao_blend, 0.f, 0.f);
    cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    m_gtao_hist = write;
    m_gtao_frame = Device.dwFrame;
    m_gtao_prev_vp = Device.mFullTransform;
}

bool CRenderTarget::gtao_is_frame_ao() const { return ps_r_ssao_mode == ssao_mode_gtao && s_gtao; }

// The same two passes as compute shaders (gtao_render.cs, gtao_filter.cs) on the given context:
// the DirectX 12 async path, which records them into a job once the G-buffer is final. Both write
// through UAVs, 8x8 pixels a group.
bool CRenderTarget::phase_gtao_cs(CBackend& cmd_list, bool verify)
{
    if (!s_gtao_cs || !rt_gtao || !rt_gtao->pUAView || !rt_gtao_cur || !rt_gtao_cur->pUAView)
        return false;

    // Checks from the same inputs. Port: both pixel passes into the same targets first, at the
    // viewport the combine runs them at. Schedule: these compute passes inline on the immediate
    // context first, compared with the job's results after the join.
    const bool schedule = verify && da_async_verify_schedule();
    const bool port = verify && !schedule && s_gtao;
    if (port)
    {
        cmd_list.SetViewport({ 0.f, 0.f, float(Device.dwRenderWidth), float(Device.dwRenderHeight), 0.f, 1.f });
        phase_gtao(cmd_list);
        cmd_list.set_RT(nullptr, 0);
        da_async_verify_keep(cmd_list, da_async_verify_outputs[0], rt_gtao->pSurface);
        da_async_verify_keep(cmd_list, da_async_verify_outputs[1], rt_gtao_cur->pSurface);
    }
    else if (schedule && phase_gtao_cs(RCache, false))
    {
        da_async_verify_keep(RCache, da_async_verify_outputs[0], rt_gtao->pSurface);
        da_async_verify_keep(RCache, da_async_verify_outputs[1], rt_gtao_cur->pSurface);
        da_async_verify_expect(da_async_verify_outputs[0], rt_gtao->pSurface);
        da_async_verify_expect(da_async_verify_outputs[1], rt_gtao_cur->pSurface);
    }

    PIX_EVENT_CTX(cmd_list, phase_gtao_cs);

    // Neither target may stay bound for output while it is written through a UAV.
    cmd_list.set_RT(nullptr, 0);
    cmd_list.set_RT(nullptr, 1);
    cmd_list.set_RT(nullptr, 2);
    cmd_list.set_RT(nullptr, 3);

    const u32 groups_x = (Device.dwRenderWidth + 7) / 8;
    const u32 groups_y = (Device.dwRenderHeight + 7) / 8;
    auto* ctx = HW.get_context(cmd_list.context_id);
    const auto dispatch = [&](u32 element, ID3D11UnorderedAccessView* out) {
        SPass& P = *s_gtao_cs->E[element]->passes[0];
        cmd_list.set_States(P.state);
        cmd_list.set_CS(P.cs);
        cmd_list.set_Constants(P.constants);
        cmd_list.set_Textures(P.T);
        if (element == 0)
            cmd_list.set_c("gtao_parameters", gtao_params(gtao_screen_scale()));
        u32 initial = 1;
        ctx->CSSetUnorderedAccessViews(0, 1, &out, &initial);
        cmd_list.Compute(groups_x, groups_y, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
        cmd_list.clear_CS_resources();
    };
    {
        PIX_EVENT_CTX(cmd_list, gtao_render_cs);
        dispatch(0, rt_gtao->pUAView);
    }
    {
        PIX_EVENT_CTX(cmd_list, gtao_filter_cs);
        dispatch(1, rt_gtao_cur->pUAView);
    }

    if (port)
    {
        da_async_verify_compare(cmd_list, da_async_verify_outputs[0], rt_gtao->pSurface);
        da_async_verify_compare(cmd_list, da_async_verify_outputs[1], rt_gtao_cur->pSurface);
    }
    return true;
}
} // namespace xray::render::RENDER_NAMESPACE
