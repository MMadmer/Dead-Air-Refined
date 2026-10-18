#include "stdafx.h"

#include "xrCore/Threading/TaskManager.hpp"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/xr_object.h"

#include "Layers/xrRender/FBasicVisual.h"
#if RENDER == R_R4
#include "Layers/xrRender/da_ground.h"
#include "Layers/xrRender/da_snow.h"
#include "Layers/xrRender/da_grass_blades.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRender/da_trees_bake.h"
#include "Layers/xrRenderPC_R4/da_framegen.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderPC_R4/da_vrs.h"
#include "Layers/xrRenderPC_R4/da_gpu_trees.h"
#include "Layers/xrRenderPC_R4/da_grass_mesh.h"
#include "Layers/xrRenderPC_R4/da_offload.h"
#include "Layers/xrRenderPC_R4/da_rt.h"
#endif
#if defined(USE_DX11)
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "Layers/xrRenderDX11/StateManager/dx11SamplerStateCache.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if RENDER == R_R4
// r4_water_field.cpp: hands the level's baked water map to the GPU when the bake changes, which
// is once per level - not once per frame.
extern void da_water_field_update();
#endif

namespace
{
#if defined(XR_NATIVE)
// DirectX 12 records command lists truly in parallel: the G-buffer grass (a fifth of the render
// thread on a grassy level) goes to contexts of its own - one per batch of hw_Render - while the
// immediate one draws the level, and the lists execute exactly where the grass always drew.
// r2_mt_render 0 keeps it inline.
struct parallel_details
{
    struct batch
    {
        u32 context = R_dsgraph_structure::INVALID_CONTEXT_ID;
        Task* task = nullptr;
    };
    batch batches[CDetailManager::dump_count];
    u32 inline_dumps = 0; // batches no context was free for: the immediate context draws them
    bool started = false;
    bool cleared = false; // finish() executed lists: the immediate context lost its G-buffer targets

    void start()
    {
        CRender& r = RImplementation;
        // Mesh grass draws the G-buffer grass itself: no vertex-path batches to record.
        if (!r.o.mt_render || !r.Details || !r.Details->HasRenderableDetails() || da_grass_mesh_frame())
            return;
        started = true;
        // Read here: RCache touches the context bookkeeping, which is this thread's alone.
        const bool velocity = RCache.xforms.m_motion_pass;
        for (u32 i = 0; i < CDetailManager::dump_count; ++i)
        {
            const u32 id = r.alloc_context();
            if (id == R_dsgraph_structure::INVALID_CONTEXT_ID)
            {
                inline_dumps |= 1u << i;
                continue;
            }
            batches[i].context = id;
            const u32 dumps = 1u << i;
            batches[i].task = &TaskScheduler->AddTask([id, velocity, dumps] {
                CRender& render = RImplementation;
                CBackend& cmd_list = render.get_context(id).cmd_list;
                render.Target->phase_scene_begin_parallel(cmd_list, velocity);
                cmd_list.set_xform_view(Device.mView);
                cmd_list.set_xform_project(da_jitter_project(Device.mProject));
                cmd_list.xforms.set_VP_motion(da_vp_nojit(), da_vp_prev());
                SSManager.UseSceneBias(cmd_list.context_id, da_upscaler_active());
                render.Details->Render(cmd_list, false, nullptr, dumps);
                SSManager.UseSceneBias(cmd_list.context_id, false);
            });
        }
    }

    // Where the immediate context drew the grass: true when the parallel batches did instead.
    bool finish(CBackend& imm)
    {
        if (!started)
            return false;
        started = false;
        CRender& r = RImplementation;
        // Before the lists run: executing one leaves the immediate context without its targets.
        if (inline_dumps)
            r.Details->Render(imm, true, nullptr, inline_dumps);
        inline_dumps = 0;
        bool executed = false;
        for (batch& b : batches)
        {
            if (b.context == R_dsgraph_structure::INVALID_CONTEXT_ID)
                continue;
            TaskScheduler->Wait(*b.task);
            b.task = nullptr;
            r.get_context(b.context).cmd_list.submit();
            r.release_context(b.context);
            b.context = R_dsgraph_structure::INVALID_CONTEXT_ID;
            executed = true;
        }
        // ExecuteCommandList(..., FALSE) cleared the immediate context: its cached state goes too,
        // and the viewport comes back - the wallmarks after the split scene set none of their own.
        if (executed)
        {
            imm.Invalidate();
            r.rmNormal(imm);
        }
        cleared = executed;
        return true;
    }

    // A frame left before the grass ran: its recordings are dropped, not left for the contexts'
    // next users.
    ~parallel_details()
    {
        for (batch& b : batches)
        {
            if (b.task)
                TaskScheduler->Wait(*b.task);
            if (b.context == R_dsgraph_structure::INVALID_CONTEXT_ID)
                continue;
            ID3D11CommandList* dropped = nullptr;
            if (SUCCEEDED(HW.get_context(RImplementation.get_context(b.context).cmd_list.context_id)->FinishCommandList(FALSE, &dropped)))
                _RELEASE(dropped);
            RImplementation.release_context(b.context);
        }
    }
};
#endif

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

    // While an upscaler runs the generic targets are render-sized: the menu takes the two
    // display-size targets of the upscale slot instead, so it stays sharp and pairs with the
    // base depth.
    const bool display_targets = da_upscaler_active() && Target->s_menu_display && Target->rt_PostDisplay &&
        Target->rt_Upscaled;
    ref_rt& menu_image = display_targets ? Target->rt_PostDisplay : Target->rt_Generic_0;
    ref_rt& menu_distort = display_targets ? Target->rt_Upscaled : Target->rt_Generic_1;

    // Main Render
    {
        Target->u_setrt(RCache, menu_image, nullptr, nullptr, Target->rt_Base_Depth); // LDR RT
        g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
    }
    // Distort
    {
        Target->u_setrt(RCache, menu_distort, nullptr, nullptr, Target->rt_Base_Depth); // Now RT is a distortion mask
        RCache.ClearRT(menu_distort, color_rgba(127, 127, 0, 127));
        g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
    }

    // Actual Display
    Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
    RCache.set_Shader(display_targets ? Target->s_menu_display : Target->s_menu);
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
#if RENDER == R_R4
        // The reset the renderer reset asked for would be used up by a frame that draws nothing.
        da_history_carry_reset();
#endif
        return;
    }

    // From here to the upscale in phase_combine the scene draws at the render size, with the
    // scene's own texture LOD bias while an upscaler runs (da_temporal.h). Every way out of the
    // frame returns to the display stage.
    struct scene_stage_guard
    {
        scene_stage_guard()
        {
            da_stage_scene(true);
#if defined(USE_DX11)
            if (da_upscaler_active())
                SSManager.UseSceneBias(RCache.context_id, true);
#endif
        }
        ~scene_stage_guard()
        {
#if RENDER == R_R4
            // No shading rate leaves the scene stage.
            da_vrs_reset(RCache);
#endif
            da_stage_scene(false);
#if defined(USE_DX11)
            SSManager.UseSceneBias(RCache.context_id, false);
#endif
        }
    } scene_stage;

    //.	VERIFY					(g_pGameLevel && g_pGameLevel->pHUD);
    auto& dsgraph = get_imm_context();

    //*******
    // Sync point. Ahead of the GPU timers' frame: on DirectX 12 the event submits what was recorded so
    // far, and a frame timestamp riding with it would count the GPU idling until the frame's own work
    // arrives (the offloads' auto choice compares that frame with the CPU's, da_offload.h).
    BasicStats.WaitS.Begin();
    {
#if RENDER == R_R4
        const u64 wait_start = CPU::QPC();
#endif
        q_sync_point.Wait(ps_r2_wait_sleep, ps_r2_wait_timeout);
#if RENDER == R_R4
        da_offload_sync_wait(CPU::QPC() - wait_start);
#endif
    }
    BasicStats.WaitS.End();
    q_sync_point.End();

#if defined(USE_DX11)
    GpuTimers.FrameBegin();
#endif
#if RENDER == R_R4
    // The GPU-driven trees' wind state of this frame, before any view draws them.
    da_gpu_trees_frame_begin(RCache);
#endif

#if RENDER == R_R4
    // The grass rows the calc task produced go to the GPU.
    da_grass_mesh_frame_prepare(RCache);
    // The season's ground window, snow depth and blade generation, in this order: the snow sits on
    // the window, the blades read both (da_grass_frame_prepare waits for the detail calc task).
    {
        dx11GpuTimerScope gpu_snow(dx11GpuTimers::SnowPrep);
        da_ground_frame_prepare(RCache);
        da_snow_frame_prepare(RCache);
    }
    {
        dx11GpuTimerScope gpu_grass_gen(dx11GpuTimers::GrassGen);
        da_grass_frame_prepare(RCache);
    }
    // The vegetation's frame (the shrubs, the trees) is built in Calculate, before the sun and rain passes that draw it;
    // its bends here, on the immediate context, ahead of every list that draws it (each population under its timer).
    da_veg_bend(RCache);
    // The billboards' bakes (the trees' atlas, the stock path's level_lods) before any list draws them.
    da_trees_bake_frame(RCache);
#endif

    r_main.sync();

#if RENDER == R_R4
    // Fixed order (PLAN.md 2.5): the async path of the frame, the RT scene build (it may wait for
    // the GPU, so no job may be open yet), then fork A. async_early: the ripple steps and the cloud
    // march went into jobs and the frame skips its own below.
    da_async_frame_begin();
    da_rt_frame_scene(RCache);
    const bool async_early = da_async_fork_early(RCache);
#endif

    Target->phase_scene_prepare();

    // Insert the first-person body only after scene building has
    // finished. It is not optional: the player always has legs, and Render_First gates itself on
    // the camera mode anyway.
    if (g_pGameLevel->pHUD)
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
#if defined(USE_DX11)
    GpuTimers.Begin(dx11GpuTimers::Scene);
#endif
    // The G-buffer grass, once per frame, the mesh/vertex A/B tool around it: the stock detail
    // cards, then the blades, then the near snow grid over both (contract order, before
    // phase_scene_end).
#if defined(XR_NATIVE)
    parallel_details details_job;
    const auto render_details = [&] {
        // The procedural trees, their far billboards and the shrubs first (each under its own GPU timer), the grass after
        // them: their trunks and crowns hide the grass behind them from its pixel shaders, as the stock trees drawn with
        // the level did (drawn after the grass, the trees cost it and the shrubs a millisecond).
        da_veg_render_gbuffer(dsgraph.cmd_list);
        {
            dx11GpuTimerScope gpu_grass(dx11GpuTimers::Grass);
            da_grass_ab_gbuffer_begin();
            if (!details_job.finish(dsgraph.cmd_list) && Details)
                Details->Render(dsgraph.cmd_list, true);
            // The executed lists left the immediate context without the G-buffer, and the blades and
            // the snow grid draw on that context: the targets come back only when one of them draws.
            if (details_job.cleared && (da_grass_active() || da_snow_active()))
                Target->phase_scene_begin_parallel(dsgraph.cmd_list, dsgraph.cmd_list.xforms.m_motion_pass);
            da_grass_render_gbuffer(dsgraph.cmd_list);
            da_grass_ab_gbuffer_end();
        }
        dx11GpuTimerScope gpu_snow(dx11GpuTimers::SnowGrid);
        da_snow_render_grid(dsgraph.cmd_list);
    };
#elif RENDER == R_R4
    const auto render_details = [&] {
        // The procedural trees, their far billboards and the shrubs first, the grass after them (see above).
        da_veg_render_gbuffer(dsgraph.cmd_list);
        {
            dx11GpuTimerScope gpu_grass(dx11GpuTimers::Grass);
            da_grass_ab_gbuffer_begin();
            if (Details)
                Details->Render(dsgraph.cmd_list, true);
            da_grass_render_gbuffer(dsgraph.cmd_list);
            da_grass_ab_gbuffer_end();
        }
        dx11GpuTimerScope gpu_snow(dx11GpuTimers::SnowGrid);
        da_snow_render_grid(dsgraph.cmd_list);
    };
#else
    const auto render_details = [&] {
        if (Details)
            Details->Render(dsgraph.cmd_list, true);
    };
#endif
    if (!split_the_scene_to_minimize_wait)
    {
        PIX_EVENT(DEFER_PART0_NO_SPLIT);
        // level, DO NOT SPLIT
        Target->phase_scene_begin();
#if defined(XR_NATIVE)
        details_job.start();
#endif
        dsgraph.render_hud();
        dsgraph.render_graph(0);
        dsgraph.render_lods(true, true);
        render_details();
        Target->phase_scene_end();
    }
    else
    {
        PIX_EVENT(DEFER_PART0_SPLIT);
        // level, SPLIT
        Target->phase_scene_begin();
#if defined(XR_NATIVE)
        details_job.start();
#endif
        dsgraph.render_graph(0);
        Target->disable_aniso();
    }
#ifdef USE_OGL
    if (psDeviceFlags.test(rsWireframe))
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

#if defined(USE_DX11)
    GpuTimers.End(dx11GpuTimers::Scene);
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
        render_details();
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

#if RENDER != R_R4
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
                for (int id = 0; id < R__NUM_PARALLEL_CONTEXTS; ++id)
                    Lights_LastFrame[it]->svis[id].flushoccq();
            }
            catch (...)
            {
                Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
            }
        }
        Lights_LastFrame.clear();
    }
#endif

    // full screen pass to mark msaa-edge pixels in highest stencil bit
    if (o.msaa)
    {
        PIX_EVENT(MARK_MSAA_EDGES);
        Target->mark_msaa_edges();
    }

    r_rain.sync();

#if RENDER == R_R4
    // The snow layer rewrites the G-buffer over every opaque surface, the rain's wet look included.
    {
        dx11GpuTimerScope gpu_snow(dx11GpuTimers::SnowLayer);
        da_snow_apply_layer(RCache);
    }
    // Fork B: the G-buffer is final, GTAO may run as a job.
    da_async_fork_gtao(RCache);
#endif

    // The water field and the ripple sim come first: the sun accumulation reads the field for
    // its caustics and the forward water surface reads both, so both have to hold this frame's
    // state before either of those runs. The field call is a handful of compares once the
    // level's bake has landed.
#if RENDER == R_R4
    if (!async_early)
    {
        da_water_field_update();
        dx11GpuTimerScope gpu_ripple(dx11GpuTimers::Ripple);
        Target->phase_water_ripple();
    }
    // The visor is read at the very end of the frame, in the combine, but its field is stepped
    // here with the other fixed-step simulations: one place in the frame owns the clock.
    Target->phase_visor_drops();
#endif

    // The cloud deck field for this frame: the sun passes below shade the ground with it, the
    // shafts march through it, and the deck itself is drawn from it in combine.
#if RENDER == R_R4
    if (!async_early)
    {
        Target->phase_cloud_map();
        dx11GpuTimerScope gpu_march(dx11GpuTimers::CloudsMarch);
        Target->phase_clouds_march();
    }

    // Before the sun accumulation: the traced sun shadow it reads, then the frame's shading rates.
    da_rt_sun_pass(RCache);
    da_vrs_rate_pass(RCache);
#endif

    // Directional light - fucking sun
    {
        PIX_EVENT(DEFER_SUN);
#if defined(USE_DX11)
        dx11GpuTimerScope gpu_sun(dx11GpuTimers::Sun);
#endif
        Stats.l_visible++;
        if (!RImplementation.o.oldshadowcascades)
            r_sun.sync();
        else
            r_sun_old.sync();
#if defined(USE_DX11)
        // Between the cascades and the ambient blend the accumulator holds the sun term and
        // nothing else, which is the only point where the first-person self-shadow can be
        // applied to sunlight alone.
        render_hud_shadow();
#endif
        Target->accum_direct_blend(dsgraph.cmd_list);
#if RENDER == R_R4
        // A frame without the sun's pass (a moonless night): the froxel volume still carries the player's torch.
        if (!Target->froxels_ran())
            Target->phase_froxels(dsgraph.cmd_list);
#endif
    }

    {
        PIX_EVENT(DEFER_SELF_ILLUM);
        Target->phase_accumulator(dsgraph.cmd_list);
        // Render emissive geometry, stencil - write 0x0 at pixel pos
        dsgraph.cmd_list.set_xform_project(da_jitter_project(Device.mProject));
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

    // Diagnostics (-dxdebug): messages accumulated up to the sun belong to earlier phases.
    static const bool drainDebugMessages = !!strstr(Core.Params, "-dxdebug");
    if (drainDebugMessages)
    {
        extern void dx11_debug_drain_messages(pcstr tag);
        dx11_debug_drain_messages("pre_lights");
    }

#if defined(USE_DX11)
    GpuTimers.Begin(dx11GpuTimers::Lights);
#endif
    // Lighting, non dependant on OCCQ
    {
        PIX_EVENT(DEFER_LIGHT_NO_OCCQ);
#if RENDER == R_R4
        Target->copy_depth_for_hud();
#endif
        render_lights(LP_normal);
    }

    // Lighting, dependant on OCCQ
    {
        PIX_EVENT(DEFER_LIGHT_OCCQ);
        render_lights(LP_pending);
    }
#if defined(USE_DX11)
    GpuTimers.End(dx11GpuTimers::Lights);
#endif

    // Diagnostics (-dxdebug): drain the D3D11 debug-layer validator messages into the log.
    if (drainDebugMessages)
    {
        extern void dx11_debug_drain_messages(pcstr tag);
        dx11_debug_drain_messages("lights");
    }

    // Postprocess
    {
        PIX_EVENT(DEFER_LIGHT_COMBINE);
#if defined(USE_DX11)
        GpuTimers.Begin(dx11GpuTimers::Combine);
#endif
        Target->phase_combine();
#if defined(USE_DX11)
        GpuTimers.End(dx11GpuTimers::Combine);
#endif
    }

    // r__screenshot_every N: the finished 3D frame, before the HUD and the stats text, so a
    // hidden-desktop QA run can hand back what it rendered.
    if (ps_r__screenshot_every > 0 && (Device.dwFrame % u32(ps_r__screenshot_every)) == 0)
        Screenshot(SM_NORMAL);

#if RENDER == R_R4
    // Frame generation: the hudless copy of this finished 3D frame and the backend's Prepare,
    // before the UI goes on the back buffer.
    da_framegen_frame();
#endif

#if defined(USE_DX11)
    GpuTimers.FrameEnd();
    // r__gpu_log N: write the pass timings to the log every N frames. This is how a QA probe
    // reads GPU cost back - the stats HUD never reaches the log.
    if (ps_r__gpu_log > 0 && GpuTimers.valid() && (Device.dwFrame % u32(ps_r__gpu_log)) == 0)
        GpuTimers.LogLine();
#endif

#if RENDER == R_R4
    // This frame's view-projections become the previous ones of the next frame's motion vectors.
    da_temporal_frame_end();
#endif

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
void CRender::BeforeWorldRender()
{
    // 3D PDA: rasterize the PDA dialog into $user$ui before any world pass runs. This spot is
    // deliberate (see docs/dead-air/pda-3d-port-plan.md §4.2): it is outside the parallel
    // context window (the UI locks the shared dynamic VB that r_sun/r_rain contexts also
    // lock), it does not freeze under the pause menu the way a pass inside Render() would,
    // and no scene target is bound yet.
    if (!g_pGameLevel || !g_pGameLevel->pHUD || !Target)
        return;

    // The HUD side decides whether anything wants the texture this frame (PDA shown, 3D
    // presenter active or debug forced) - the early-out keeps the pass free when idle.
    if (!g_pGameLevel->pHUD->RenderPdaScreenUIQuery())
        return;

    // Lazy target: allocated on the first frame anything actually wants it, so installs
    // without the 3D PDA data never pay the VRAM. Survives until device reset like every
    // other member target; recreated here afterwards the same way.
    if (!Target->rt_ui)
    {
        Target->rt_ui.create(r2_RT_ui, Device.dwWidth, Device.dwHeight, D3DFMT_A8R8G8B8, 1);
        if (!Target->rt_ui)
            return;
    }

    PIX_EVENT(render_pda_screen_ui);
    Target->u_setrt(RCache, Target->rt_ui, nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
    // u_setrt does NOT reset the viewport state helpers - rmNormal reads the size the last
    // u_setrt wrote (the same two lines IX-Ray had to add to their pass after the fact).
    rmNormal(RCache);
    RCache.ClearRT(Target->rt_ui, color_rgba(0, 0, 0, 255));
    g_pGameLevel->pHUD->RenderPdaScreenUI();
    // Hand the backbuffer back so whatever runs before the scene phases sees the same state
    // RenderMenu leaves behind.
    Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
    rmNormal(RCache);
}

// После рендера мира и пост-эффектов --#SM+#--
void CRender::AfterWorldRender() {}
} // namespace xray::render::RENDER_NAMESPACE
