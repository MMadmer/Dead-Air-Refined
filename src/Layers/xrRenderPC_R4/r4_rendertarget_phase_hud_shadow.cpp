#include "stdafx.h"

#include "xrEngine/CustomHUD.h"
#include "xrEngine/IGame_Level.h"

namespace xray::render::RENDER_NAMESPACE
{
void CRender::render_hud_shadow()
{
    if (!ps_r__hud_shadow)
        return;
    if (!Target || !Target->hud_shadow_available())
        return;
    // The pass reads the deferred position as a plain 2D surface; under MSAA that target is
    // multisampled, so the feature stays off there instead of guessing a sample. Say so once:
    // a player who enabled the Maximum preset with MSAA on reports "weapon shadows stopped
    // working" otherwise, and the session log carries no clue.
    if (o.msaa)
    {
        static bool reported = false;
        if (!reported)
        {
            Msg("! [hud_shadow] r__hud_shadow is on, but MSAA is active - the first-person self-shadow stays off until MSAA is disabled");
            reported = true;
        }
        return;
    }
    if (!g_pGameLevel || !g_pGameLevel->pHUD)
        return;
    if (o.sunstatic || !ps_r2_ls_flags.test(R2FLAG_SUN))
        return;

    light* sun = (light*)Lights.sun._get();
    if (!sun)
        return;
    // nofloor - see render_phase_sun.cpp.
    if (u_diffuse2s_nofloor(sun->color.r, sun->color.g, sun->color.b) <= EPS)
        return;

    const u32 context_id = alloc_context();
    if (context_id == R_dsgraph_structure::INVALID_CONTEXT_ID)
        return;

    // A tight box around the eye. The hands start at the camera and the longest item reaches
    // about a metre ahead, so this covers every weapon and every aim pose while keeping the
    // 1024-texel map at roughly three millimetres per texel - the resolution self-shadowing
    // at arm's length actually needs, and one a 20 m world cascade could never give.
    constexpr float hud_extent = 1.5f;
    constexpr float light_offset = 3.f;

    Fvector center;
    center.mad(Device.vCameraPosition, Device.vCameraDirection, 0.5f);

    Fvector L_dir = sun->direction;
    L_dir.normalize();

    Fvector L_up, L_right;
    L_up.set(0.f, 1.f, 0.f);
    if (_abs(L_up.dotproduct(L_dir)) > .99f)
        L_up.set(0.f, 0.f, 1.f);
    L_right.crossproduct(L_up, L_dir).normalize();
    L_up.crossproduct(L_dir, L_right).normalize();

    Fvector L_pos;
    L_pos.mad(center, L_dir, -light_offset);

    Fmatrix mdir_View, mdir_Project, combine;
    mdir_View.build_camera_dir(L_pos, L_dir, L_up);
    mdir_Project.build_projection_ortho(2.f * hud_extent, 2.f * hud_extent, 0.1f, light_offset + 2.f * hud_extent);
    combine.mul(mdir_Project, mdir_View);

    // Same texel adjust the sun accumulation uses, so the shader can transform an eye-space
    // point straight into map texels. The bias is far smaller than the sun's: this map spans
    // three metres instead of twenty, and the sun's slope bias would peter-pan the contact
    // shadow of a trigger guard clean off the receiver.
    constexpr float fBias = -0.0002f;
    Fmatrix m_TexelAdjust =
    {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, fBias, 1.0f
    };

    Fmatrix xf_project, hud_shadow_xform;
    xf_project.mul(m_TexelAdjust, combine);
    hud_shadow_xform.mul(xf_project, Device.mInvView);

    // The HUD is rasterized through its own narrow projection, so its pixels have to be
    // rebuilt with the HUD field of view. Same layout as the deferred decompression params.
    const float hud_fov = psHUD_FOV * Device.fFOV;
    const float VertTan = -1.0f * tanf(deg2rad(hud_fov / 2.0f));
    const float HorzTan = -VertTan / Device.fASPECT;
    Fvector4 hud_pos_decompress;
    hud_pos_decompress.set(HorzTan, VertTan,
        (2.0f * HorzTan) / float(Device.dwWidth), (2.0f * VertTan) / float(Device.dwHeight));

    Target->set_hud_shadow_params(hud_shadow_xform, hud_pos_decompress);

    // Collect the casters. The HUD never enters a shadow graph on its own: it is submitted
    // by the main pass only, so it has to be handed to this context explicitly.
    auto& dsgraph = get_context(context_id);
    dsgraph.o.phase = PHASE_SMAP;
    dsgraph.r_pmask(true, false);
    dsgraph.o.sector_id = get_largest_sector();
    dsgraph.o.view_pos = Device.vCameraPosition;
    dsgraph.o.xform = combine;
    dsgraph.o.view_frustum.CreateFromMatrix(combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));

    g_pGameLevel->pHUD->Render_Shadow(context_id);

    const bool has_casters = !dsgraph.mapNormalPasses[0][0].empty() || !dsgraph.mapMatrixPasses[0][0].empty();
    if (has_casters)
    {
        Target->phase_smap_hud(dsgraph.cmd_list);
        dsgraph.cmd_list.set_xform_world(Fidentity);
        dsgraph.cmd_list.set_xform_view(Fidentity);
        dsgraph.cmd_list.set_xform_project(combine);
        dsgraph.render_graph(0);
    }

    dsgraph.cmd_list.submit();

    // ExecuteCommandList clears the immediate context to its default state; drop the cached
    // backend state so the accumulator's targets are re-applied for real.
    RCache.Invalidate();
    release_context(context_id);

    if (!has_casters)
        return;

    // Back to the accumulator the sun left bound, then modulate.
    Target->phase_accumulator(RCache);
    Target->phase_hud_shadow(RCache);
}

void CRenderTarget::phase_smap_hud(CBackend& cmd_list)
{
    // Depth only, like the rain map: the caster set is the first-person hands and item, and
    // nothing samples a colour channel of it.
    u_setrt(cmd_list, nullptr, nullptr, nullptr, rt_smap_hud);
    cmd_list.ClearZB(rt_smap_hud, 1.0f);
    cmd_list.SetViewport({ 0, 0, float(rt_smap_hud->dwWidth), float(rt_smap_hud->dwHeight), 0.f, 1.f });
    cmd_list.set_Stencil(FALSE);
}

void CRenderTarget::phase_hud_shadow(CBackend& cmd_list)
{
    PIX_EVENT_CTX(cmd_list, hud_self_shadow);

    // Runs straight after the sun cascades accumulated and before the ambient blend, so the
    // accumulator carries the sun term alone. Modulating it here darkens exactly the sunlight
    // on the weapon and leaves ambient, emissive and every local light untouched.
    u32 Offset;
    const u32 C = color_rgba(255, 255, 255, 255);
    const float _w = float(Device.dwWidth);
    const float _h = float(Device.dwHeight);
    Fvector2 p0, p1;
    p0.set(.5f / _w, .5f / _h);
    p1.set((_w + .5f) / _w, (_h + .5f) / _h);

    // The quad is issued exactly at the HUD depth limit and the compare is inverted, so the
    // depth buffer itself selects the pixels: only what rmNear() wrote survives. No stencil
    // bit is spent and no marker has to be carried through the g-buffer.
    const float d_Z = r2_hud_depth_limit, d_W = 1.f;

    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
    pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    const float smap_size = float(hud_smap_size());

    cmd_list.set_Geometry(g_combine);
    cmd_list.set_Element(s_hud_shadow->E[0]);
    cmd_list.set_c("m_hud_shadow", m_hud_shadow_xform);
    cmd_list.set_c("hud_pos_decompress",
        m_hud_pos_decompress.x, m_hud_pos_decompress.y, m_hud_pos_decompress.z, m_hud_pos_decompress.w);
    cmd_list.set_c("hud_smap_size", smap_size, 1.f / smap_size,
        ps_r__hud_shadow_normal_offset, ps_r__hud_shadow_slope_bias);

    cmd_list.set_Stencil(FALSE);
    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_ColorWriteEnable();
    cmd_list.set_ZFunc(D3DCMP_GREATEREQUAL);
    cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    cmd_list.set_ZFunc(D3DCMP_LESSEQUAL);
}
} // namespace xray::render::RENDER_NAMESPACE
