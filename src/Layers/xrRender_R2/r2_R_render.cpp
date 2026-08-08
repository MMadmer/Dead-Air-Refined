#include "stdafx.h"

#include "xrCore/Threading/TaskManager.hpp"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/xr_object.h"

#include "Layers/xrRender/FBasicVisual.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Lights demoted by the shadow budget keep lighting the scene unshadowed for this frame
// only. bShadow is persistent game-visible light state, so it is restored on every exit
// path once the frame's light passes are done. Lights cannot be destroyed mid-Render(),
// which makes holding the pointers safe for the guard's lifetime.
struct demoted_shadow_guard
{
    xr_vector<light*> lights;

    ~demoted_shadow_guard()
    {
        for (light* L : lights)
            L->flags.bShadow = true;
    }
};
} // namespace

void CRender::RenderMenu()
{
#if defined(USE_DX11)
    TracyD3D11Zone(HW.profiler_ctx, "render_menu");
#endif
    PIX_EVENT(render_menu);
    //	Globals
    RCache.set_CullMode(CULL_CCW);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable();

    // Main Render
    {
        Target->u_setrt(RCache, Target->rt_Generic_0, nullptr, nullptr, Target->rt_Base_Depth); // LDR RT
        g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
    }
    // Distort
    {
        Target->u_setrt(RCache, Target->rt_Generic_1, nullptr, nullptr, Target->rt_Base_Depth); // Now RT is a distortion mask
        RCache.ClearRT(Target->rt_Generic_1, color_rgba(127, 127, 0, 127));
        g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
    }

    // Actual Display
    Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
    RCache.set_Shader(Target->s_menu);
    RCache.set_Geometry(Target->g_menu);

    Fvector2 p0, p1;
    u32 Offset;
    u32 C = color_rgba(255, 255, 255, 255);
    float _w = float(Device.dwWidth);
    float _h = float(Device.dwHeight);
    float d_Z = EPS_S;
    float d_W = 1.f;
    p0.set(.5f / _w, .5f / _h);
    p1.set((_w + .5f) / _w, (_h + .5f) / _h);

    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
#if defined(USE_DX11)
    pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
    pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    pv++;
#elif defined(USE_OGL)
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    pv++;
    pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
#else
#   error No graphics API selected or enabled!
#endif
    RImplementation.Vertex.Unlock(4, Target->g_menu->vb_stride);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

extern u32 g_r;
void CRender::Render()
{
    ZoneScoped;
#if defined(USE_DX11)
    TracyD3D11Zone(HW.profiler_ctx, "Render");
#endif
    PIX_EVENT(CRender_Render);

    g_r = 1;

    rmNormal(RCache);

    IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : 0;
    bool bMenu = pMainMenu ? pMainMenu->CanSkipSceneRendering() : false;

    // XXX: do we need to handle case when there is level, but HUD isn't loaded yet?
    // if (!(g_pGameLevel && g_hud) || bMenu)
    if (!g_pGameLevel || bMenu)
    {
        Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
        return;
    }

    if (m_bFirstFrameAfterReset)
    {
        m_bFirstFrameAfterReset = false;
        return;
    }

    //.	VERIFY					(g_pGameLevel && g_pGameLevel->pHUD);
    auto& dsgraph = get_imm_context();

    //******* Z-prefill calc - DEFERRER RENDERER
    if (ps_r2_ls_flags.test(R2FLAG_ZFILL))
    {
        PIX_EVENT(DEFER_Z_FILL);
        BasicStats.Culling.Begin();
        float z_distance = ps_r2_zfill;
        Fmatrix m_zfill, m_project;
        m_project.build_projection(deg2rad(Device.fFOV /* *Device.fASPECT*/), Device.fASPECT, VIEWPORT_NEAR,
            z_distance * g_pGamePersistent->Environment().CurrentEnv.far_plane);
        m_zfill.mul(m_project, Device.mView);

        if (last_sector_id != IRender_Sector::INVALID_SECTOR_ID)
        {
            dsgraph.o.phase = PHASE_SMAP;
            dsgraph.r_pmask(true, false); // enable priority "0"
            dsgraph.set_Recorder(nullptr);
            dsgraph.o.use_hom = true;
            dsgraph.o.is_main_pass = true;
            dsgraph.o.sector_id = last_sector_id;
            dsgraph.o.portal_traverse_flags = CPortalTraverser::VQ_HOM | CPortalTraverser::VQ_SSA | CPortalTraverser::VQ_FADE;
            dsgraph.o.spatial_traverse_flags = ISpatial_DB::O_ORDERED;
            dsgraph.o.spatial_types = STYPE_RENDERABLE | STYPE_LIGHTSOURCE;
            dsgraph.o.view_pos = Device.vCameraPosition;
            dsgraph.o.xform = m_zfill;
            dsgraph.o.view_frustum = ViewBase;
            dsgraph.o.query_box_side = VIEWPORT_NEAR + EPS_L;
            dsgraph.o.precise_portals = true;

            dsgraph.build_subspace();
        }
        BasicStats.Culling.End();
    }

    //*******
    // Sync point
    BasicStats.WaitS.Begin();
    {
        q_sync_point.Wait(ps_r2_wait_sleep, ps_r2_wait_timeout);
    }
    BasicStats.WaitS.End();
    q_sync_point.End();

    r_main.sync();

    if (ps_r2_ls_flags.test(R2FLAG_ZFILL))
    {
        // flush
        Target->phase_scene_prepare();
        dsgraph.cmd_list.set_ColorWriteEnable(FALSE);
        dsgraph.render_graph(0);
        dsgraph.cmd_list.set_ColorWriteEnable();
    }
    else
    {
        Target->phase_scene_prepare();
    }

    // Insert the first-person body only after scene building and the optional depth prepass have finished.
    if (g_pGameLevel->pHUD && ps_r__common_flags.test(RFLAG_ACTOR_BODY))
    {
        dsgraph.o.phase = PHASE_NORMAL;
        dsgraph.r_pmask(true, true, true);
        dsgraph.set_Recorder(nullptr);
        g_pGameLevel->pHUD->Render_First(dsgraph.context_id);
    }

    BOOL split_the_scene_to_minimize_wait = FALSE;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_SPLIT_SCENE))
        split_the_scene_to_minimize_wait = TRUE;

    //******* Main render :: PART-0	-- first
#ifdef USE_OGL
    if (psDeviceFlags.test(rsWireframe))
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
#endif
    if (!split_the_scene_to_minimize_wait)
    {
        PIX_EVENT(DEFER_PART0_NO_SPLIT);
        // level, DO NOT SPLIT
        Target->phase_scene_begin();
        dsgraph.render_hud();
        dsgraph.render_graph(0);
        dsgraph.render_lods(true, true);
        if (Details)
            Details->Render(dsgraph.cmd_list, true);
        Target->phase_scene_end();
    }
    else
    {
        PIX_EVENT(DEFER_PART0_SPLIT);
        // level, SPLIT
        Target->phase_scene_begin();
        dsgraph.render_graph(0);
        Target->disable_aniso();
    }
#ifdef USE_OGL
    if (psDeviceFlags.test(rsWireframe))
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

    //******* Occlusion testing of volume-limited light-sources
    Target->phase_occq();
    LP_normal.clear();
    LP_pending.clear();
    if (o.msaa)
    {
#if defined(USE_DX11)
        dsgraph.cmd_list.set_ZB(Target->rt_MSAADepth->pZRT[dsgraph.cmd_list.context_id]);
#elif defined(USE_OGL)
        dsgraph.cmd_list.set_ZB(Target->rt_MSAADepth->pZRT);
#endif
    }
    demoted_shadow_guard demoted;
    {
        PIX_EVENT(DEFER_TEST_LIGHT_VIS);
        light_Package& LP = Lights.package;

        // Keep only the most prominent shadow-map faces and demote the rest to unshadowed
        // lighting for this frame. Per face, not per lamp group - exact reference behavior:
        // the unit is scene passes, and a demoted face keeps lighting through the cheap
        // path. 0 keeps the unlimited behavior.
        if (ps_r__light_shadow_budget > 0 && LP.v_shadowed.size() > size_t(ps_r__light_shadow_budget))
        {
            const Fvector eye = Device.vCameraPosition;

            // Privileged sources (the actor's torch, see set_never_demote) go first and do
            // not spend the budget. stable_partition keeps the order of the rest, otherwise
            // the hysteresis below would compare a different set every frame.
            const auto first_ordinary = std::stable_partition(LP.v_shadowed.begin(), LP.v_shadowed.end(),
                [](const light* L) { return !!L->flags.bNeverDemote; });
            const size_t privileged = size_t(first_ordinary - LP.v_shadowed.begin());
            const size_t budget = privileged + size_t(ps_r__light_shadow_budget);

            // The budget goes to the nearest sources, measured to the edge of the lit area
            // (distance - radius): a lamp the camera stands inside scores negative and wins.
            // One-sided hysteresis in the same units - a face that held a slot last frame
            // keeps a 1.5 m (or 15% of range) advantage - so near-equal lamps do not swap
            // shadows on every small camera move. Pointers are compared, never dereferenced,
            // so a source deleted between frames is safe.
            static xr_vector<const light*> s_prev_shadowed;

            const auto kept_last_frame = [](const light* L)
            {
                return std::find(s_prev_shadowed.begin(), s_prev_shadowed.end(), L) != s_prev_shadowed.end();
            };
            const auto score = [&](const light* L) // lower is more important
            {
                const float base = L->position.distance_to(eye) - L->range;
                return kept_last_frame(L) ? base - _max(1.5f, L->range * 0.15f) : base;
            };

            const bool over_budget = LP.v_shadowed.size() > budget;
            if (over_budget)
            {
                std::partial_sort(first_ordinary, LP.v_shadowed.begin() + budget, LP.v_shadowed.end(),
                    [&score](const light* a, const light* b) { return score(a) < score(b); });
            }

            for (size_t i = over_budget ? budget : LP.v_shadowed.size(); i < LP.v_shadowed.size(); ++i)
            {
                light* L = LP.v_shadowed[i];

                // bShadow MUST clear: accum_spot reads it and would sample shadow-atlas
                // coordinates nobody computed for a demoted light. Restored by the guard.
                L->flags.bShadow = false;
                demoted.lights.push_back(L);

                // Everything demoted goes to v_spot, OMNIPART faces included: accum_spot has
                // an explicit OMNIPART branch, and the engine calls compute_xf_spot for
                // v_spot entries, while accum_point would treat the open wedge as a closed
                // sphere and smear light across the screen. Real POINTs never appear in
                // v_shadowed; the branch is a guard only.
                if (L->flags.type == IRender_Light::POINT)
                    LP.v_point.push_back(L);
                else
                    LP.v_spot.push_back(L);
            }
            if (over_budget)
                LP.v_shadowed.resize(budget);

            s_prev_shadowed.assign(LP.v_shadowed.begin(), LP.v_shadowed.end());
        }

        // stats
        Stats.l_shadowed = LP.v_shadowed.size();
        Stats.l_unshadowed = LP.v_point.size() + LP.v_spot.size();
        Stats.l_total = Stats.l_shadowed + Stats.l_unshadowed;

        // perform tests
        size_t count = 0;
        count = _max(count, LP.v_point.size());
        count = _max(count, LP.v_spot.size());
        count = _max(count, LP.v_shadowed.size());
        for (size_t it = 0; it < count; it++)
        {
            if (it < LP.v_point.size())
            {
                light* L = LP.v_point[it];
                L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_point.push_back(L);
                else
                    LP_normal.v_point.push_back(L);
            }
            if (it < LP.v_spot.size())
            {
                light* L = LP.v_spot[it];
                L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_spot.push_back(L);
                else
                    LP_normal.v_spot.push_back(L);
            }
            if (it < LP.v_shadowed.size())
            {
                light* L = LP.v_shadowed[it];
                L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_shadowed.push_back(L);
                else
                    LP_normal.v_shadowed.push_back(L);
            }
        }
    }
    LP_normal.sort();
    LP_pending.sort();

    //******* Main render :: PART-1 (second)
    if (split_the_scene_to_minimize_wait)
    {
        PIX_EVENT(DEFER_PART1_SPLIT);
        // skybox can be drawn here
        if (false)
        {
            Target->u_setrt(dsgraph.cmd_list, Target->rt_Generic_0_r, Target->rt_Generic_1_r, nullptr, Target->rt_MSAADepth);
            dsgraph.cmd_list.set_CullMode(CULL_NONE);
            dsgraph.cmd_list.set_Stencil(FALSE);

            // draw skybox
            dsgraph.cmd_list.set_ColorWriteEnable();
            dsgraph.cmd_list.set_Z(false);
            g_pGamePersistent->Environment().RenderSky();
            dsgraph.cmd_list.set_Z(true);
        }

        // level
        Target->phase_scene_begin();
        dsgraph.render_hud();
        dsgraph.render_lods(true, true);
        if (Details)
            Details->Render(dsgraph.cmd_list, true);
        Target->phase_scene_end();
    }

    if (g_pGameLevel->pHUD && g_pGameLevel->pHUD->RenderActiveItemUIQuery())
    {
        Target->phase_wallmarks();
        dsgraph.render_hud_ui();
    }

    // Wall marks
    if (Wallmarks)
    {
        PIX_EVENT(DEFER_WALLMARKS);
        Target->phase_wallmarks();
        g_r = 0;
        Wallmarks->Render(); // wallmarks has priority as normal geometry
    }

    // Update incremental shadowmap-visibility solver
    {
        PIX_EVENT(DEFER_FLUSH_OCCLUSION);
        u32 it = 0;
        for (it = 0; it < Lights_LastFrame.size(); it++)
        {
            if (0 == Lights_LastFrame[it])
                continue;
            try
            {
                for (int id = 0; id < 3; ++id)
                    Lights_LastFrame[it]->svis[id].flushoccq();
            }
            catch (...)
            {
                Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
            }
        }
        Lights_LastFrame.clear();
    }

    // full screen pass to mark msaa-edge pixels in highest stencil bit
    if (o.msaa)
    {
        PIX_EVENT(MARK_MSAA_EDGES);
        Target->mark_msaa_edges();
    }

    r_rain.sync();

    // Directional light - fucking sun
    {
        PIX_EVENT(DEFER_SUN);
        Stats.l_visible++;
        if (!RImplementation.o.oldshadowcascades)
            r_sun.sync();
        else
            r_sun_old.sync();
        Target->accum_direct_blend(dsgraph.cmd_list);
    }

    {
        PIX_EVENT(DEFER_SELF_ILLUM);
        Target->phase_accumulator(dsgraph.cmd_list);
        // Render emissive geometry, stencil - write 0x0 at pixel pos
        dsgraph.cmd_list.set_xform_project(Device.mProject);
        dsgraph.cmd_list.set_xform_view(Device.mView);
        // Stencil - write 0x1 at pixel pos -
        if (!o.msaa)
        {
            dsgraph.cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff,
                D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        }
        else
        {
            dsgraph.cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f,
                D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        }
        dsgraph.cmd_list.set_CullMode(CULL_CCW);
        dsgraph.cmd_list.set_ColorWriteEnable();
        dsgraph.render_emissive();
    }

    // Lighting, non dependant on OCCQ
    {
        PIX_EVENT(DEFER_LIGHT_NO_OCCQ);
        render_lights(LP_normal);
    }

    // Lighting, dependant on OCCQ
    {
        PIX_EVENT(DEFER_LIGHT_OCCQ);
        render_lights(LP_pending);
    }

    // Postprocess
    {
        PIX_EVENT(DEFER_LIGHT_COMBINE);
        Target->phase_combine();
    }

    VERIFY(dsgraph.mapDistort.empty());
}

void CRender::render_forward()
{
    ZoneScoped;
    auto& dsgraph = get_imm_context();

    //******* Main render - second order geometry (the one, that doesn't support deffering)
    //.todo: should be done inside "combine" with estimation of of luminance, tone-mapping, etc.
    {
        //	Igor: we don't want to render old lods on next frame.
        dsgraph.mapLOD.clear();
        dsgraph.render_graph(1); // normal level, secondary priority
        dsgraph.PortalTraverser.fade_render(); // faded-portals
        dsgraph.render_sorted(); // strict-sorted geoms
        g_pGamePersistent->Environment().RenderLast(); // rain/thunder-bolts
    }
}

// Перед началом рендера мира --#SM+#--
void CRender::BeforeWorldRender() {}

// После рендера мира и пост-эффектов --#SM+#--
void CRender::AfterWorldRender() {}
} // namespace xray::render::RENDER_NAMESPACE
