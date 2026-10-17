// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Screen-space reflections of glossy surfaces, after DOOM Eternal's computessrtrace and its temporal upscale
// (shaders/r3/da_ssr.h): wet roads, puddle-free wet ground, car bodies and polished floors mirrored what the screen
// shows of the world, not only the sky's cube, wherever the ray traced glossy reflections do not run (DirectX 11,
// or ray tracing off). The trace reads the previous frame's linear colour, which combine_1 writes into a third
// target while this runs; the result takes the cube's place in combine_1's specular term, as the traced one does.

#include "stdafx.h"

#include "Layers/xrRender/blenders/blender_da_ssr.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderPC_R4/da_rt.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "xrEngine/IGame_Persistent.h"

namespace xray::render::RENDER_NAMESPACE
{
struct DaSsr
{
    ref_rt scene, raw, dist, hist[2], result;
    ref_shader shader;
    u32 w = 0, h = 0; // the render size the targets were made for
    bool ok = false; // they all were
    u32 scene_frame = 0; // the frame combine_1 last wrote the linear colour on
    Fmatrix scene_viewproj{}; // that frame's view-projection, unjittered
    u32 hist_frame = 0; // the frame the temporal pass last wrote a history on
    u32 hist_cur = 0; // the history it wrote
    u32 ran = 0; // the frame the result is of
    u32 phase = 0;
};

namespace
{
// The ray traced reflections' thresholds (da_rt_passes.cpp): the lowest gloss traced, the gloss of the full weight, the
// spread of the mirror image by roughness.
constexpr float kGlossMin = 0.03f, kGlossFull = 0.12f, kGlossSpread = 0.35f;
// The most steps of a ray by tier: Low, High.
constexpr u32 kSteps[2] = {12u, 20u};

bool allocate(DaSsr& s)
{
    s.scene = nullptr;
    s.raw = nullptr;
    s.dist = nullptr;
    s.hist[0] = nullptr;
    s.hist[1] = nullptr;
    s.result = nullptr;
    s.w = Device.dwRenderWidth;
    s.h = Device.dwRenderHeight;
    const u32 hw = (s.w + 1) / 2, hh = (s.h + 1) / 2;
    s.scene.create(da_ssr_scene_rt, s.w, s.h, D3DFMT_A16B16G16R16F, 1);
    s.raw.create(da_ssr_raw_rt, hw, hh, D3DFMT_A16B16G16R16F, 1);
    s.dist.create(da_ssr_dist_rt, hw, hh, D3DFMT_R16F, 1);
    for (u32 i = 0; i < 2; ++i)
        s.hist[i].create(da_ssr_hist_rt[i], s.w, s.h, D3DFMT_A16B16G16R16F, 1);
    s.result.create(da_ssr_rt, s.w, s.h, D3DFMT_A16B16G16R16F, 1);
    s.scene_frame = s.hist_frame = s.ran = 0;
    s.ok = s.scene->pRT && s.raw->pRT && s.dist->pRT && s.hist[0]->pRT && s.hist[1]->pRT && s.result->pRT;
    if (!s.ok)
        Msg("! [ssr] the %ux%u targets could not be created: glossy surfaces keep the sky's cube", s.w, s.h);
    return s.ok;
}

// The targets exist for this render size (made on first use: a preset without the reflections spends no memory).
bool ready(DaSsr& s)
{
    if (s.scene && s.w == Device.dwRenderWidth && s.h == Device.dwRenderHeight)
        return s.ok;
    return allocate(s);
}

bool wanted(const DaSsr& s)
{
    return ps_r__ssr && s.shader && !(g_pGamePersistent && g_pGamePersistent->OnRenderPPUI_query());
}
} // namespace

void CRenderTarget::ssr_create()
{
    ssr_destroy();
    m_ssr = xr_new<DaSsr>();
    CBlender_da_ssr blender;
    m_ssr->shader.create(&blender, "r2" DELIMITER "da_ssr");
}

void CRenderTarget::ssr_destroy()
{
    if (m_ssr)
        xr_delete(m_ssr);
}

bool CRenderTarget::ssr_ran() const { return m_ssr && m_ssr->ran && m_ssr->ran == Device.dwFrame; }

const ref_rt* CRenderTarget::ssr_scene_target()
{
    if (!m_ssr || !wanted(*m_ssr) || !ready(*m_ssr))
        return nullptr;
    m_ssr->scene_frame = Device.dwFrame;
    m_ssr->scene_viewproj = Device.mFullTransform;
    return &m_ssr->scene;
}

void CRenderTarget::phase_ssr(CBackend& cmd_list)
{
    // The traced reflections answer instead when they ran; without last frame's colour there is nothing to show.
    if (!m_ssr || !wanted(*m_ssr) || da_rt_constants().x > 1.5f || !ready(*m_ssr) ||
        m_ssr->scene_frame + 1 != Device.dwFrame)
        return;
    DaSsr& s = *m_ssr;
    PIX_EVENT_CTX(cmd_list, phase_ssr);
    dx11GpuTimerScope gpu(dx11GpuTimers::Ssr);

    s.phase = (s.phase + 1) & 1023u;
    const bool history = s.hist_frame && s.hist_frame + 1 == Device.dwFrame && !da_history_reset();
    Fmatrix reproj;
    reproj.mul(s.scene_viewproj, Device.mInvView);
    const u32 tw = s.raw->dwWidth, th = s.raw->dwHeight;
    const auto constants = [&]
    {
        cmd_list.set_c("da_ssr_frame", float(s.phase), history ? 1.f : 0.f, float(tw), float(th));
        cmd_list.set_c("da_ssr_gloss", kGlossMin, kGlossFull, kGlossSpread, float(kSteps[ps_r__ssr > 1 ? 1 : 0]));
        cmd_list.set_c("m_ssr_reproj", reproj);
    };
    const auto pass = [&](const ref_rt& target, const ref_rt& second, u32 element)
    {
        u_setrt(cmd_list, target, second, nullptr, static_cast<ID3DDepthStencilView*>(nullptr));
        cmd_list.SetViewport({0.f, 0.f, float(target->dwWidth), float(target->dwHeight), 0.f, 1.f});
        cmd_list.set_Element(s.shader->E[element]);
        constants();
        u32 offset = 0;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
        cmd_list.set_Geometry(g_combine);
        cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
    };

    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_Stencil(FALSE);
    pass(s.raw, s.dist, 0);
    // Element 1 writes history 0 and reads history 1, element 2 the other way round.
    s.hist_cur ^= 1u;
    pass(s.hist[s.hist_cur], s.result, 1 + s.hist_cur);

    // The passes after this one draw at the render size and do not set the viewport themselves.
    cmd_list.SetViewport({0.f, 0.f, float(Device.dwRenderWidth), float(Device.dwRenderHeight), 0.f, 1.f});
    s.hist_frame = Device.dwFrame;
    s.ran = Device.dwFrame;
}
} // namespace xray::render::RENDER_NAMESPACE
