#include "stdafx.h"

#include "xrCore/Threading/ParallelFor.hpp"
#include "Layers/xrRender/da_veg.h"
#if RENDER == R_R4
#include "Layers/xrRenderPC_R4/da_particle_light.h"
#include "Layers/xrRenderPC_R4/da_smap_cache.h"
#include "Layers/xrRenderPC_R4/da_light_volumes.h"
#include "Layers/xrRender/da_grass_blades.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
// One dynamic-object query for the whole light phase instead of one per shadowed light. Every
// query takes the spatial database lock, and the caster builds of one frame are parallel tasks - so
// with a dozen shadowed lights the phase spent its time queueing on that lock, not drawing (the
// sibling port measured 1.25 ms of waiting against 0.07 ms of shadow maps, and 1.30 -> 0.39 ms for
// the phase once the query was hoisted). A box covering the influence spheres of every shadowed
// light is queried once, and each light then filters that list with its own frustum, applying the
// same type-and-sphere test the tree would have. Any object inside a light's frustum is inside its
// sphere, and the box covers the union of the spheres, so the list is a superset of every per-light
// query and the caster set is unchanged. The bounds are taken from the spheres and not the frusta
// on purpose: a tighter volume would drop casters at the edges of lights, quietly.
static xr_vector<ISpatial*> s_light_dynamic_casters;

void CRender::render_lights(light_Package& LP)
{
    ZoneScoped;
#if RENDER == R_R4
    // The lit particles' nearest lights, before the package is consumed below.
    da_particle_light_snapshot(LP);
#endif

    //////////////////////////////////////////////////////////////////////////
    // Refactor order based on ability to pack shadow-maps
    // 1. calculate area + sort in descending order
    // const	u16		smap_unassigned		= u16(-1);
    {
        xr_vector<light*>& source = LP.v_shadowed;
        auto output = source.begin();
        for (light* L : source)
        {
            L->vis_update();
            if (!L->vis.visible)
                continue;

#if RENDER == R_R4
            // The face size on the cache's ladder: a still lamp keeps its slot while the camera walks.
            const u32 previous = L->X.S.size;
            LR.compute_xf_spot(L);
            L->X.S.size = da_smap_cache_size(L->X.S.size, previous);
#else
            LR.compute_xf_spot(L);
#endif
            *output++ = L;
        }
        source.erase(output, source.end());
    }

    // The procedural shrubs and trees in the shadowed lights' reach build their chunks now, the lights' transforms known
    // and none drawn yet (da_veg.h).
    {
        static xr_vector<DaVegLight> shrub_lights;
        shrub_lights.clear();
        for (light* L : LP.v_shadowed)
            shrub_lights.push_back({L, L->position, L->direction, L->range, L->cone, L->X.S.size, L->X.S.combine});
        da_veg_lights_prepare(get_imm_context().cmd_list, shrub_lights.data(), u32(shrub_lights.size()));
    }

    s_light_dynamic_casters.clear();
    if (ps_r__light_dyn_shared && !LP.v_shadowed.empty())
    {
        Fbox bounds;
        bounds.invalidate();
        for (light* L : LP.v_shadowed)
        {
            const auto& sphere = L->GetSpatialData().sphere;
            const Fvector radius{ sphere.R, sphere.R, sphere.R };
            Fvector corner;
            bounds.modify(corner.sub(sphere.P, radius));
            bounds.modify(corner.add(sphere.P, radius));
        }
        // get_CD yields half the extent and q_box builds its box as centre +- size (Fbox::setb), so
        // the two halves agree. Get this wrong by two and the volume would be half of what is
        // needed, with casters vanishing at the edges of lights.
        Fvector center, half_size;
        bounds.get_CD(center, half_size);
        g_pGamePersistent->SpatialSpace.q_box(s_light_dynamic_casters, 0, STYPE_RENDERABLE, center, half_size);
    }

    // 2. refactor - infact we could go from the backside and sort in ascending order
    {
        xr_vector<light*>& source = LP.v_shadowed;
        xr_vector<light*> refactored;
        refactored.reserve(source.size());
        xr_vector<light*> remaining;
        remaining.reserve(source.size());
        const size_t total = source.size();

        std::sort(source.begin(), source.end(), [](light* l1, light* l2)
        {
            // Deterministic tie-break: with quantized sizes many lights compare equal, and
            // an unstable order would hand out different atlas rects every frame.
            if (l1->X.S.size != l2->X.S.size)
                return l1->X.S.size > l2->X.S.size;
            return l1 < l2;
        });

        for (u16 smap_ID = 0; refactored.size() != total; ++smap_ID)
        {
            LP_smap_pool.initialize(RImplementation.o.smapsize);
            remaining.clear();
            for (light* L : source)
            {
                SMAP_Rect R{};
                if (LP_smap_pool.push(R, L->X.S.size))
                {
                    L->X.S.posX = R.min.x;
                    L->X.S.posY = R.min.y;
                    L->vis.smap_ID = smap_ID;
                    refactored.push_back(L);
                }
                else
                    remaining.push_back(L);
            }
            source.swap(remaining);
        }

        // save (lights are popped from back)
        std::reverse(refactored.begin(), refactored.end());
        LP.v_shadowed = std::move(refactored);
    }

#if RENDER == R_R4
    // Which faces copy their statics from the cache this frame and which fill it, before their casters are built; the
    // small faces first, as the slots go.
    {
        static xr_vector<light*> by_size;
        by_size.assign(LP.v_shadowed.begin(), LP.v_shadowed.end());
        std::sort(by_size.begin(), by_size.end(), [](const light* a, const light* b) {
            return a->X.S.size != b->X.S.size ? a->X.S.size < b->X.S.size : a < b;
        });
        for (light* L : by_size)
            da_smap_cache_mode(L);
    }
#endif

    auto& cmd_list = get_imm_context().cmd_list;

    const bool volumetricLightsEnabled = RImplementation.o.advancedpp &&
        ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS) && ps_r_lighting_quality > 1;

#if RENDER != R_R4
    const auto accumulateUnshadowedSpot = [&](light* L)
    {
        // Volume-only faces exist for the unshadowed-point export case, where the parent in
        // v_point covers diffuse and the faces carry just the fog. A face demoted by the
        // shadow budget has no parent in the package and must draw its own diffuse, and its
        // owner still carries bShadow while the face's own flag is temporarily cleared.
        const bool volumeOnlyPointFace =
            L->flags.type == IRender_Light::OMNIPART && L->flags.bVolumetric && !L->flags.bShadow &&
            L->omnipart_owner && !L->omnipart_owner->flags.bShadow;
        LR.compute_xf_spot(L);
        if (!volumeOnlyPointFace)
        {
            Target->accum_spot(cmd_list, L);
            render_indirect(L);
        }
        if (volumetricLightsEnabled && L->flags.bVolumetric)
            Target->accum_volumetric(cmd_list, L);
    };
#endif

    Target->rt_smap_depth->set_slice_read(0);

#if RENDER == R_R4
    // The instanced light volumes (da_light_volumes.h): each atlas batch below lights its lamps in a draw per volume
    // type. The visible unshadowed lamps go with the last batch, the faces of a point lamp that only carry its fog with
    // nothing.
    static xr_vector<light*> lit_unshadowed, fog_spots;
    lit_unshadowed.clear();
    fog_spots.clear();
    for (light* L : LP.v_point)
    {
        L->vis_update();
        if (L->vis.visible)
            lit_unshadowed.push_back(L);
    }
    for (light* L : LP.v_spot)
    {
        L->vis_update();
        if (!L->vis.visible)
            continue;
        const bool volumeOnlyPointFace =
            L->flags.type == IRender_Light::OMNIPART && L->flags.bVolumetric && !L->flags.bShadow &&
            L->omnipart_owner && !L->omnipart_owner->flags.bShadow;
        LR.compute_xf_spot(L);
        if (!volumeOnlyPointFace)
            lit_unshadowed.push_back(L);
        if (volumetricLightsEnabled && L->flags.bVolumetric)
            fog_spots.push_back(L);
    }
    LP.v_point.clear();
    LP.v_spot.clear();
    const bool shadowed_batches = !LP.v_shadowed.empty();
#endif

    PIX_EVENT(SHADOWED_LIGHTS);

    //////////////////////////////////////////////////////////////////////////
    // sort lights by importance???
    // while (has_any_lights_that_cast_shadows) {
    //		if (has_point_shadowed)		->	generate point shadowmap
    //		if (has_spot_shadowed)		->	generate spot shadowmap
    //		switch-to-accumulator
    //		if (has_point_unshadowed)	-> 	accum point unshadowed
    //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
    //		if (was_point_shadowed)		->	accum point shadowed
    //		if (was_spot_shadowed)		->	accum spot shadowed
    //	}
    //	if (left_some_lights_that_doesn't cast shadows)
    //		accumulate them
    static xr_vector<light*> L_spot_s;
#if RENDER == R_R4
    // The frame's lamp faces are culled and recorded up front on worker threads, every worker's share of each atlas
    // batch closed into a command list of its own, and the lists run in batch order between the atlas clears and the
    // batches' lighting (the sun cascades' way, render_phase_sun.cpp): the faces' draws were a quarter of the render
    // thread with a hundred faces. A list takes kFacesPerList faces at least - on DirectX 11 every list costs its
    // execution (two faces a list measured the same on DirectX 12).
    constexpr u32 kFacesPerList = 4;
    const bool parallel = o.mt_calculate && o.mt_render;

    // One face: its caster build, then its draws into dsgraph's list. False = it had nothing to cast and lights
    // nothing (the stock rule).
    const auto record_face = [this](R_dsgraph_structure& dsgraph, light* L) -> bool
    {
        ZoneScopedN("light face");
        const auto cache = DaSmapCache(L->smap_cache.mode);
        dsgraph.o.phase = PHASE_SMAP;
        dsgraph.o.local_light = true;
        dsgraph.o.smap_cache = u8(cache);
        if (cache == DaSmapCache::fill)
            L->smap_cache.trees.clear();
        dsgraph.o.smap_trees = bool(cache) ? &L->smap_cache.trees : nullptr;
        dsgraph.r_pmask(true, RImplementation.o.Tshadows);
        dsgraph.o.sector_id = L->spatial.sector_id;
        dsgraph.o.view_pos = L->position;
        dsgraph.o.xform = L->X.S.combine;
        dsgraph.o.view_frustum.CreateFromMatrix(L->X.S.combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
        // An empty shared list means the hoist is off or found nothing; the build then queries the tree as before.
        dsgraph.o.dynamic_source =
            (ps_r__light_dyn_shared && !s_light_dynamic_casters.empty()) ? &s_light_dynamic_casters : nullptr;
        dsgraph.build_subspace();
        dsgraph.o.dynamic_source = nullptr;
        dsgraph.o.smap_trees = nullptr;

        const bool bNormal = !dsgraph.mapNormalPasses[0][0].empty() || !dsgraph.mapMatrixPasses[0][0].empty();
        const bool bSpecial = !dsgraph.mapNormalPasses[1][0].empty() || !dsgraph.mapMatrixPasses[1][0].empty() ||
            !dsgraph.mapSorted.empty();
        // Detail objects never render into local light shadow maps in the reference: CoC and Dead Air 1.0 reserve
        // them for the sun cascades. The pass is a High/Maximum preset feature (r__light_details), gated by the grass
        // shadow option itself, and only for the lamps within 40 m: beyond that their grass shadows are sub-texel.
        // With the blade grass on the level the cards left are the litter and the decals lying on the ground, and the
        // blades cast into no lamp (GRASS.md, Shadows): no detail casters then (3.7 ms of the workers' time at the
        // lamp hall).
        const bool light_near_enough = L->position.distance_to_sqr(Device.vCameraPosition) < (40.f * 40.f);
        const bool renderDetails = !da_grass_active() && ps_r__light_details && ps_r_sun_details >= detail_shadow_high &&
            Details && light_near_enough && Details->HasRenderableDetails();
        // The procedural shrubs cast into the lights near the camera as the stock bushes did.
        const bool renderShrubs = da_veg_light_casts(L);
        if (!bool(cache) && !bNormal && !bSpecial && !renderDetails && !renderShrubs)
            return false;

        PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHT);
        auto& cmd = dsgraph.cmd_list;
        // The statics into the light's slot first, with the face's bias and transforms.
        if (cache == DaSmapCache::fill)
        {
            da_smap_cache_fill_begin(cmd, L);
            cmd.StateManager.OverrideDepthBias(true, ps_r__smap_depth_bias, ps_r__smap_slope_bias);
            cmd.set_xform_world(Fidentity);
            cmd.set_xform_view(L->X.S.view);
            cmd.set_xform_project(L->X.S.project);
            dsgraph.o.smap_draw = 1;
            dsgraph.render_graph(0);
            dsgraph.o.smap_draw = 0;
            cmd.StateManager.OverrideDepthBias(false);
        }
        Target->phase_smap_spot(cmd, L);
        if (cache != DaSmapCache::off)
            da_smap_cache_copy(cmd, L);
        cmd.StateManager.OverrideDepthBias(true, ps_r__smap_depth_bias, ps_r__smap_slope_bias);
        cmd.set_xform_world(Fidentity);
        cmd.set_xform_view(L->X.S.view);
        cmd.set_xform_project(L->X.S.project);
        dsgraph.render_graph(0);
        if (renderDetails)
            Details->Render(cmd, false, &dsgraph.o.view_frustum);
        if (renderShrubs)
            da_veg_render_light(cmd, L);
        // The translucent and strictly sorted casters write their depth like the rest: the volumes read no
        // translucency from the map's colour.
        if (bSpecial)
        {
            PIX_EVENT_CTX(cmd, SHADOWED_LIGHTS_RENDER_GRAPH);
            dsgraph.render_graph(1); // normal level, secondary priority
            PIX_EVENT_CTX(cmd, SHADOWED_LIGHTS_RENDER_SORTED);
            dsgraph.render_sorted(); // strict-sorted geoms
        }
        cmd.StateManager.OverrideDepthBias(false);
        return true;
    };

    // The frame's faces, batch by batch as the atlas packing ordered them.
    static xr_vector<light*> faces;
    static xr_vector<u32> batch_first; // the batches' first faces, and the end
    static xr_vector<u8> drawn;
    faces.clear();
    batch_first.clear();
    while (!LP.v_shadowed.empty())
    {
        xr_vector<light*>& source = LP.v_shadowed;
        const u16 sid = source.back()->vis.smap_ID;
        batch_first.push_back(u32(faces.size()));
        while (!source.empty() && source.back()->vis.smap_ID == sid)
        {
            faces.push_back(source.back());
            source.pop_back();
        }
    }
    const u32 batches = u32(batch_first.size());
    batch_first.push_back(u32(faces.size()));
    drawn.assign(faces.size(), 0);

    if (batches)
    {
        u32 contexts[R__NUM_PARALLEL_CONTEXTS];
        u32 lists = 0;
        const u32 wanted = parallel ?
            std::clamp((u32(faces.size()) + kFacesPerList - 1) / kFacesPerList, 1u, u32(R__NUM_PARALLEL_CONTEXTS)) : 1u;
        while (lists < wanted)
        {
            const u32 id = alloc_context();
            if (id == R_dsgraph_structure::INVALID_CONTEXT_ID)
                break;
            contexts[lists++] = id;
        }
        R_ASSERT2(lists, "the lamps' shadow maps found no free render context");

        // The work in units: unit j of a batch cut into n is its faces j, j + n, ... closed into one command list.
        // The units go round the contexts, so every worker gets a share of every batch.
        struct Unit
        {
            u32 batch, first, step;
            ID3D11CommandList* list;
        };
        static xr_vector<Unit> units;
        units.clear();
        for (u32 b = 0; b < batches; ++b)
        {
            const u32 count = batch_first[b + 1] - batch_first[b];
            const u32 n = std::clamp((count + kFacesPerList - 1) / kFacesPerList, 1u, lists);
            for (u32 j = 0; j < n; ++j)
                units.push_back({ b, batch_first[b] + j, n, nullptr });
        }
        const auto record_context = [&](const TaskRange<u32>& range)
        {
            for (u32 k = range.begin(); k != range.end(); ++k)
            {
                auto& dsgraph = get_context(contexts[k]);
                bool fresh = true;
                for (u32 u = k; u < units.size(); u += lists)
                {
                    Unit& unit = units[u];
                    for (u32 i = unit.first; i < batch_first[unit.batch + 1]; i += unit.step)
                    {
                        if (!fresh)
                        {
                            // The context's graph emptied for the next face, its state forgotten.
                            const u32 id = dsgraph.context_id;
                            dsgraph.reset();
                            dsgraph.context_id = id;
                        }
                        fresh = false;
                        drawn[i] = record_face(dsgraph, faces[i]);
                    }
                    CHK_DX(HW.get_context(dsgraph.cmd_list.context_id)->FinishCommandList(false, &unit.list));
                }
            }
        };
        if (parallel && lists > 1)
            xr_parallel_for(TaskRange<u32>(0, lists), record_context);
        else
            record_context(TaskRange<u32>(0, lists));
        for (u32 k = 0; k < lists; ++k)
            release_context(contexts[k]);

        ID3D11DeviceContext* imm = HW.get_context(CHW::IMM_CTX_ID);
        u32 next_unit = 0;
        for (u32 b = 0; b < batches; ++b)
        {
            Stats.s_used++;
            GpuTimers.Begin(dx11GpuTimers::LightSmap);
            Target->phase_smap_spot_clear(cmd_list);
            for (; next_unit < units.size() && units[next_unit].batch == b; ++next_unit)
            {
                if (ID3D11CommandList* list = units[next_unit].list)
                {
                    imm->ExecuteCommandList(list, false);
                    list->Release();
                }
            }
            // ExecuteCommandList clears the immediate context's state: its backend cache must not survive it.
            cmd_list.Invalidate();
            for (u32 i = batch_first[b]; i < batch_first[b + 1]; ++i)
            {
                if (drawn[i])
                {
                    Stats.s_merged++;
                    L_spot_s.push_back(faces[i]);
                }
                else
                    Stats.s_finalclip++;
            }
            GpuTimers.End(dx11GpuTimers::LightSmap);

            PIX_EVENT(UNSHADOWED_LIGHTS);
            Target->phase_accumulator(cmd_list);

            PIX_EVENT(LIGHT_VOLUMES);
            da_lv_draw(cmd_list, L_spot_s, b + 1 == batches ? &lit_unshadowed : nullptr);
            for (light* p_light : L_spot_s)
                render_indirect(p_light);
            PIX_EVENT(ACCUM_VOLUMETRIC);
            if (volumetricLightsEnabled)
                for (light* p_light : L_spot_s)
                    Target->accum_volumetric(cmd_list, p_light);
            L_spot_s.clear();
        }
    }
#else
    struct task_data_t
    {
        light* L{};
        Task* task{};
        u32 batch_id{};
    };
    static xr_vector<task_data_t> lights_queue{};
    lights_queue.reserve(R__NUM_PARALLEL_CONTEXTS);

    const auto& flush_lights = [&]()
    {
        ZoneScopedN("flush lights");
        for (const auto& [L, task, batch_id] : lights_queue)
        {
            if (task)
                TaskScheduler->Wait(*task);

            auto& dsgraph = get_context(batch_id);

            const bool bNormal = !dsgraph.mapNormalPasses[0][0].empty() || !dsgraph.mapMatrixPasses[0][0].empty();
            const bool bSpecial = !dsgraph.mapNormalPasses[1][0].empty() || !dsgraph.mapMatrixPasses[1][0].empty() ||
                !dsgraph.mapSorted.empty();
            // Detail objects never render into local light shadow maps in the reference:
            // CoC (R2/R3/R4) and Dead Air 1.0 reserve them for the sun cascades. The pass
            // arrived with the modern upstream base, and on grassy levels it cost more than
            // the rest of the face put together (about a quarter of the whole frame with
            // r__light_shadow_budget 0 at the Jupiter station) - so it is a High/Maximum
            // preset feature (r__light_details), gated by the grass shadow option itself.
            // Beyond ~40 m the light's grass shadows are sub-texel in its smap anyway, and this
            // pass is the single most expensive part of a grassy face - distance-gate it (donor
            // lights_render does the same).
            const bool light_near_enough = L->position.distance_to_sqr(Device.vCameraPosition) < (40.f * 40.f);
            const bool renderDetails = ps_r__light_details && ps_r_sun_details >= detail_shadow_high && Details &&
                light_near_enough && Details->HasRenderableDetails();
            // The procedural shrubs cast into the lights near the camera as the stock bushes did.
            const bool renderShrubs = da_veg_light_casts(L);
            if (bNormal || bSpecial || renderDetails || renderShrubs)
            {
                PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHT);

                Stats.s_merged++;
                L_spot_s.push_back(L);
                Target->phase_smap_spot(dsgraph.cmd_list, L);
                dsgraph.cmd_list.StateManager.OverrideDepthBias(true, ps_r__smap_depth_bias, ps_r__smap_slope_bias);
                dsgraph.cmd_list.set_xform_world(Fidentity);
                dsgraph.cmd_list.set_xform_view(L->X.S.view);
                dsgraph.cmd_list.set_xform_project(L->X.S.project);
                dsgraph.render_graph(0);
                if (renderDetails)
                    Details->Render(dsgraph.cmd_list, false, &dsgraph.o.view_frustum);
                if (renderShrubs)
                    da_veg_render_light(dsgraph.cmd_list, L);
                L->X.S.transluent = FALSE;
                if (bSpecial)
                {
                    L->X.S.transluent = TRUE;
                    Target->phase_smap_spot_tsh(dsgraph.cmd_list, L);
                    PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHTS_RENDER_GRAPH);
                    dsgraph.render_graph(1); // normal level, secondary priority
                    PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHTS_RENDER_SORTED);
                    dsgraph.render_sorted(); // strict-sorted geoms
                }
                dsgraph.cmd_list.StateManager.OverrideDepthBias(false);
            }
            else
            {
                Stats.s_finalclip++;
            }

            L->svis[batch_id].end(); // NOTE(DX11): occqs are fetched here, this should be done on the imm context only
            RImplementation.release_context(batch_id);
        }

        lights_queue.clear();
    };

    while (!LP.v_shadowed.empty())
    {
        // if (has_spot_shadowed)
        Stats.s_used++;

        // generate spot shadowmap
        Target->phase_smap_spot_clear(cmd_list);
        xr_vector<light*>& source = LP.v_shadowed;
        light* L = source.back();
        const u16 sid = L->vis.smap_ID;
        while (true)
        {
            if (source.empty())
                break;
            L = source.back();
            if (L->vis.smap_ID != sid)
                break;

            const auto batch_id = alloc_context(false);
            if (batch_id == R_dsgraph_structure::INVALID_CONTEXT_ID)
            {
                VERIFY(!lights_queue.empty());
                flush_lights();
                continue;
            }

            source.pop_back();
            Lights_LastFrame.push_back(L);

            task_data_t data{};
            data.batch_id = batch_id;
            data.L = L;

            // The svis slot is the context id, fixed at light creation. The old scheme kept
            // one slot and retargeted it per frame (begin(graph_id)), which let caster state
            // collected for one graph leak into another and reshadow the light differently
            // between frames.
            const auto& calc_lights = [data]
            {
                ZoneScopedN("calc lights");
                auto& dsgraph = RImplementation.get_context(data.batch_id);
                light* L = data.L;

                L->svis[data.batch_id].begin();

                dsgraph.o.phase = PHASE_SMAP;
                dsgraph.o.local_light = true;
                dsgraph.r_pmask(true, RImplementation.o.Tshadows);
                dsgraph.o.sector_id = L->spatial.sector_id;
                dsgraph.o.view_pos = L->position;
                dsgraph.o.xform = L->X.S.combine;
                dsgraph.o.view_frustum.CreateFromMatrix(L->X.S.combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
                // An empty shared list means the hoist is off or found nothing; the build then
                // queries the tree as before. The context goes back to the pool, so the pointer is
                // cleared right after.
                dsgraph.o.dynamic_source =
                    (ps_r__light_dyn_shared && !s_light_dynamic_casters.empty()) ? &s_light_dynamic_casters : nullptr;

                dsgraph.build_subspace();

                dsgraph.o.dynamic_source = nullptr;
                dsgraph.o.smap_trees = nullptr;
            };

            if (o.mt_calculate)
                data.task = &TaskScheduler->AddTask(calc_lights);
            else
                calc_lights();

            lights_queue.emplace_back(data);
        }
        flush_lights(); // in case if something left

        cmd_list.Invalidate();

        PIX_EVENT(UNSHADOWED_LIGHTS);

        //		switch-to-accumulator
        Target->phase_accumulator(cmd_list);

        PIX_EVENT(POINT_LIGHTS);

        //		if (has_point_unshadowed)	-> 	accum point unshadowed
        if (!LP.v_point.empty())
        {
            light* L2 = LP.v_point.back();
            LP.v_point.pop_back();
            L2->vis_update();
            if (L2->vis.visible)
            {
                Target->accum_point(cmd_list, L2);
                render_indirect(L2);
            }
        }

        PIX_EVENT(SPOT_LIGHTS);

        //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
        if (!LP.v_spot.empty())
        {
            light* L2 = LP.v_spot.back();
            LP.v_spot.pop_back();
            L2->vis_update();
            if (L2->vis.visible)
                accumulateUnshadowedSpot(L2);
        }

        PIX_EVENT(SPOT_LIGHTS_ACCUM_VOLUMETRIC);

        //		if (was_spot_shadowed)		->	accum spot shadowed
        if (!L_spot_s.empty())
        {
            PIX_EVENT(ACCUM_SPOT);
            for (light* p_light : L_spot_s)
            {
                Target->accum_spot(cmd_list, p_light);
                render_indirect(p_light);
            }

            PIX_EVENT(ACCUM_VOLUMETRIC);
            if (volumetricLightsEnabled)
                for (light* p_light : L_spot_s)
                    Target->accum_volumetric(cmd_list, p_light);

            L_spot_s.clear();
        }
    }
#endif

#if RENDER == R_R4
    // No atlas batch this frame: the unshadowed lamps light on their own.
    if (!shadowed_batches)
    {
        Target->phase_accumulator(cmd_list);
        PIX_EVENT(LIGHT_VOLUMES);
        da_lv_draw(cmd_list, L_spot_s, &lit_unshadowed);
    }
    for (light* p_light : lit_unshadowed)
        render_indirect(p_light);
    PIX_EVENT(ACCUM_VOLUMETRIC);
    for (light* p_light : fog_spots)
        Target->accum_volumetric(cmd_list, p_light);
#else
    PIX_EVENT(POINT_LIGHTS_ACCUM);
    // Point lighting (unshadowed, if left)
    if (!LP.v_point.empty())
    {
        xr_vector<light*>& Lvec = LP.v_point;
        for (light* p_light : Lvec)
        {
            p_light->vis_update();
            if (p_light->vis.visible)
            {
                render_indirect(p_light);
                Target->accum_point(cmd_list, p_light);
            }
        }
        Lvec.clear();
    }

    PIX_EVENT(SPOT_LIGHTS_ACCUM);
    // Spot lighting (unshadowed, if left)
    if (!LP.v_spot.empty())
    {
        xr_vector<light*>& Lvec = LP.v_spot;
        for (light* p_light : Lvec)
        {
            p_light->vis_update();
            if (p_light->vis.visible)
                accumulateUnshadowedSpot(p_light);
        }
        Lvec.clear();
    }
#endif
}

void CRender::render_indirect(light* L) const
{
    if (!ps_r2_ls_flags.test(R2FLAG_GI))
        return;

    auto& cmd_list = RImplementation.get_imm_context().cmd_list;

    light LIGEN;
    LIGEN.set_type(IRender_Light::REFLECTED);
    LIGEN.set_shadow(false);
    LIGEN.set_cone(PI_DIV_2 * 2.f);

    const xr_vector<light_indirect>& Lvec = L->indirect;
    if (Lvec.empty())
        return;
    const float LE = L->color.intensity();
    for (auto& LI : Lvec)
    {
        // energy and color
        const float LIE = LE * LI.E;
        if (LIE < ps_r2_GI_clip)
            continue;

        Fvector T{ L->color.r, L->color.g, L->color.b };
        T.mul(LI.E);
        LIGEN.set_color(T.x, T.y, T.z);

        // geometric
        Fvector L_up{ 0, 1, 0 }, L_right;
        if (_abs(L_up.dotproduct(LI.D)) > .99f)
            L_up = { 0, 0, 1 };

        L_right.crossproduct(L_up, LI.D).normalize();
        LIGEN.spatial.sector_id = LI.S;
        LIGEN.set_position(LI.P);
        LIGEN.set_rotation(LI.D, L_right);

        // range
        // dist^2 / range^2 = A - has infinity number of solutions
        // approximate energy by linear fallof Emax / (1 + x) = Emin
        const float Emax = LIE;
        const float Emin = 1.f / 255.f;
        const float x = (Emax - Emin) / Emin;
        if (x < 0.1f)
            continue;
        LIGEN.set_range(x);

        Target->accum_reflected(cmd_list, &LIGEN);
    }
}
} // namespace xray::render::RENDER_NAMESPACE
