#pragma once

#include "r__sector.h"

namespace xray::render::RENDER_NAMESPACE
{
class FTreeVisual;

// is_static_geometry_valuable's thresholds at the current quality, for the GPU-driven trees' cull;
// false while the static value filter is off.
bool dsgraph_static_value_thresholds(float (&volume)[5], float (&distance)[5]);
// feedback	for receiving visuals
class R_feedback
{
public:
    virtual void rfeedback_static(dxRender_Visual* V) = 0;
};

struct R_dsgraph_structure
{
    static constexpr auto INVALID_CONTEXT_ID = static_cast<u32>(-1);
#if RENDER == R_R1
    static constexpr auto IMM_CTX_ID = 0; // TODO: to remove this ugly #ifdef we need to introduce per-render configuration
#else
    static constexpr auto IMM_CTX_ID = R__NUM_PARALLEL_CONTEXTS; // the next after pooled
#endif

    R_feedback* val_feedback{}; // feedback for geometry being rendered
    u32 val_feedback_breakp{}; // breakpoint
    xr_vector<Fbox3>* val_recorder; // coarse structure recorder
    u32 marker{};
    u32 context_id{ INVALID_CONTEXT_ID };

    struct options_t
    {
        u32 phase{};
        u32 portal_traverse_flags{};
        u32 spatial_traverse_flags{};
        u32 spatial_types{ STYPE_RENDERABLE };
        float query_box_side{ EPS_L * 20.0f };
        Fvector view_pos{};
        Fmatrix xform{};
        CFrustum view_frustum{};
        IRender_Sector::sector_id_t sector_id;
        bool pmask[2];
        bool pmask_wmark;
        bool use_hom{ false };
        bool precise_portals{ false };
        bool is_main_pass{ false };
        bool mt_calculate{ false };
        // Static casters marked dxRender_Visual::rt_static stay out of this view: ray traced sun
        // shadows cover them in the near and middle cascades (dx12-features/dxr.md 3.5).
        bool rt_skip_static{ false };
        // A local light's shadow face: the GPU-driven trees stay on the CPU walk there (gpudriven.md 3.13).
        bool local_light{ false };
        // A light the actor carries (his torch, a glowstick or a detector in hand: set_never_demote): his own shadow
        // stays out of its faces.
        bool carried_light{ false };
        // The lamp shadow cache (da_smap_cache.h) for the face this view builds: 1 fill - every static caster, none
        // dropped for its distance to the camera; 2 use - the static casters are in the slot, the build skips them.
        u8 smap_cache{ 0 };
        // render_graph's share of the fill: 1 the statics alone, the trees and the moving casters kept for a second call.
        u8 smap_draw{ 0 };
        // The face's tree list: the fill records into it, a cached face inserts it in place of the static walk.
        xr_vector<dxRender_Visual*>* smap_trees{ nullptr };
        // A dynamic-object list collected ahead of time. When set, build_subspace_dynamic filters it
        // with the view frustum instead of querying the spatial database - the database takes one
        // lock for every query, and the shadow-map passes of one frame would otherwise queue up on
        // it, one query per light.
        const xr_vector<ISpatial*>* dynamic_source{ nullptr };
    } o;

    // Dynamic scene graph
    // R_dsgraph::mapNormal_T										mapNormal	[2]		;	// 2==(priority/2)
    R_dsgraph::mapNormalPasses_T mapNormalPasses[2]; // 2==(priority/2)
    // R_dsgraph::mapMatrix_T										mapMatrix	[2]		;
    R_dsgraph::mapMatrixPasses_T mapMatrixPasses[2];
    SPass* cachedNormalPasses[2][SHADER_PASSES_MAX]{};
    R_dsgraph::mapNormalItems* cachedNormalItems[2][SHADER_PASSES_MAX]{};
    SPass* cachedMatrixPasses[2][SHADER_PASSES_MAX]{};
    R_dsgraph::mapMatrixItems* cachedMatrixItems[2][SHADER_PASSES_MAX]{};
    R_dsgraph::mapSorted_T mapSorted;
    R_dsgraph::mapHUD_T mapHUD;
    R_dsgraph::mapLOD_T mapLOD;
    R_dsgraph::mapSorted_T mapDistort;
    R_dsgraph::mapHUD_T    mapHUDSorted;

#if RENDER != R_R1
    R_dsgraph::mapSorted_T mapWmark; // sorted
    R_dsgraph::mapSorted_T mapEmissive;
    R_dsgraph::mapSorted_T mapHUDEmissive;
#endif

    xr_vector<CSector*> Sectors;
    xr_vector<CPortal*> Portals;
    CPortalTraverser PortalTraverser;
    xrXRC Sectors_xrc;

    // Runtime structures
    xr_vector<R_dsgraph::mapNormal_T::value_type*> nrmPasses;
    xr_vector<FTreeVisual*> windVisuals;
    xr_vector<R_dsgraph::mapMatrix_T::value_type*> matPasses;
    xr_vector<R_dsgraph::_LodItem> lstLODs;
    xr_vector<int> lstLODgroups;
    xr_vector<ISpatial*> lstRenderables;
    xr_vector<std::pair<float, ISpatial*>> lstRenderablesOrdered;
    xr_vector<ISpatial*> lstSpatial;
    xr_vector<dxRender_Visual*> lstVisuals;

    struct deferred_static_visual_t
    {
        dxRender_Visual* visual{};
        CFrustum view{};
        u32 planes{};
        bool partial{};
        bool lod_managed{};
    };
    xr_vector<deferred_static_visual_t> deferred_static_visuals;
    bool defer_static_main_thread_work{};

    CBackend cmd_list{};

    // GPU-driven trees (da_gpu_trees.h): the view the last build set up (0: none), consumed by the first
    // render_graph(0), and how the build walks the nodes the registry took out of the hierarchies.
    u32 gd_view{};
    u8 gd_walk{};

    u32 counter_S{};
    u32 counter_D{};

    void set_Feedback(R_feedback* V, u32 id)
    {
        val_feedback_breakp = id;
        val_feedback = V;
    }
    void set_Recorder(xr_vector<Fbox3>* dest)
    {
        val_recorder = dest;
        if (dest)
            dest->clear();
    }
    void get_Counters(u32& s, u32& d)
    {
        s = counter_S;
        d = counter_D;
    }
    void clear_Counters() { counter_S = counter_D = 0; }

    R_dsgraph::mapNormalItems& get_normal_pass_items(u32 priority, u32 passIndex, SPass* pass)
    {
        if (cachedNormalPasses[priority][passIndex] != pass)
        {
            cachedNormalPasses[priority][passIndex] = pass;
            cachedNormalItems[priority][passIndex] = &mapNormalPasses[priority][passIndex][pass];
        }
        return *cachedNormalItems[priority][passIndex];
    }

    R_dsgraph::mapMatrixItems& get_matrix_pass_items(u32 priority, u32 passIndex, SPass* pass)
    {
        if (cachedMatrixPasses[priority][passIndex] != pass)
        {
            cachedMatrixPasses[priority][passIndex] = pass;
            cachedMatrixItems[priority][passIndex] = &mapMatrixPasses[priority][passIndex][pass];
        }
        return *cachedMatrixItems[priority][passIndex];
    }

    void invalidate_pass_item_cache(u32 priority, u32 passIndex)
    {
        cachedNormalPasses[priority][passIndex] = nullptr;
        cachedNormalItems[priority][passIndex] = nullptr;
        cachedMatrixPasses[priority][passIndex] = nullptr;
        cachedMatrixItems[priority][passIndex] = nullptr;
    }

    R_dsgraph_structure() : Sectors_xrc("dsgraph")
    {
        r_pmask(true, true);
    };

    void reset()
    {
        //marker = 0;
        context_id = INVALID_CONTEXT_ID;

        o.query_box_side = EPS_L * 20;
        o.use_hom = false;
        o.precise_portals = false;
        o.is_main_pass = false;
        o.spatial_traverse_flags = 0;
        o.portal_traverse_flags = 0;
        o.spatial_types = STYPE_RENDERABLE;
        o.dynamic_source = nullptr;
        o.rt_skip_static = false;
        o.local_light = false;
        o.carried_light = false;
        o.smap_cache = 0;
        o.smap_draw = 0;
        o.smap_trees = nullptr;
        gd_view = 0;
        gd_walk = 0;

        val_recorder = nullptr;
        val_feedback = nullptr;

        nrmPasses.clear();
        windVisuals.clear();
        matPasses.clear();

        lstLODs.clear();
        lstLODgroups.clear();
        lstRenderables.clear();
        lstRenderablesOrdered.clear();
        lstSpatial.clear();
        lstVisuals.clear();
        deferred_static_visuals.clear();
        defer_static_main_thread_work = false;

        for (int i = 0; i < SHADER_PASSES_MAX; ++i)
        {
            for (int priority = 0; priority < 2; ++priority)
            {
                invalidate_pass_item_cache(priority, i);

                auto& normal = mapNormalPasses[priority][i];
                if (!normal.empty())
                {
                    for (auto node = normal.begin(), end = normal.end(); node != end; ++node)
                    {
                        node->second.clear();
                        node->second.ssa = 0.0f;
                    }
                }
                normal.clear();

                auto& matrix = mapMatrixPasses[priority][i];
                if (!matrix.empty())
                {
                    for (auto node = matrix.begin(), end = matrix.end(); node != end; ++node)
                    {
                        node->second.clear();
                        node->second.ssa = 0.0f;
                    }
                }
                matrix.clear();
            }
        }
        mapSorted.clear();
        mapHUD.clear();
        mapLOD.clear();
        mapDistort.clear();
        mapHUDSorted.clear();

#if RENDER != R_R1
        mapWmark.clear();
        mapEmissive.clear();
        mapHUDEmissive.clear();
#endif
        cmd_list.Invalidate();
    }

    void r_pmask(bool _1, bool _2, bool _wm = false)
    {
        o.pmask[0] = _1;
        o.pmask[1] = _2;
        o.pmask_wmark = _wm;
    }

    void load(const xr_vector<CSector::level_sector_data_t> &sectors, const xr_vector<CPortal::level_portal_data_t> &portals);
    void unload();

    ICF IRender_Portal* get_portal(size_t id) const
    {
        VERIFY(id < Portals.size());
        return Portals[id];
    }
    ICF IRender_Sector* get_sector(size_t id) const
    {
        VERIFY(id < Sectors.size());
        return Sectors[id];
    }
    IRender_Sector::sector_id_t detect_sector(const Fvector& P);
    IRender_Sector::sector_id_t detect_sector(const Fvector& P, Fvector& D);

    // lod_managed marks a subtree hanging off an MT_LOD node: its visibility is already
    // governed by r_ssaLOD_A/B against the imposter, so the static value filter must not
    // cull it a second time.
    void add_static(dxRender_Visual* pVisual, const CFrustum& view, u32 planes, bool lod_managed = false);
    // A sector's static geometry through one of its frustums: its root, and the vegetation nodes the
    // GPU-driven registry moved out of it, under the root's own tests as before.
    void add_static_sector(CSector* sector, const CFrustum& view);
    // The main view of phase 1 (gpudriven.md 3.5): add_static's tests of a GPU-driven node inside the
    // frustum (HOM, value filter, the FLOD switch with its impostor); its instances go to the GPU cull.
    void gd_node_main(u32 node, dxRender_Visual* visual);
    void add_leafs_dynamic(IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform); // if detected node's full visibility
    void add_leafs_static(dxRender_Visual* pVisual, bool lod_managed = false); // if detected node's full visibility

    void insert_dynamic(IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, Fvector& Center);
    // cover: the share of its pixels the main view's walk gave it (static_cover), -1 = the stock screen-size test.
    void insert_static(dxRender_Visual* pVisual, float cover = -1.f);

    // The fade across the draw distance (r__dsgraph_build.cpp): only the main view has one.
    bool fade_view() const;
    float fade_gate(dxRender_Visual* visual, bool in_range);
    float static_cover(dxRender_Visual* visual, bool lod_managed);
    // The product of the shares down the static walk to the current node.
    float walk_cover{ 1.f };
    // r__fade_stats: the gate's verdicts this frame - whole, fading, dropped.
    u32 fade_counts[3]{};

    // render primitives
    void render_graph(u32 _priority);
    void render_hud();
    void render_hud_ui();
    void render_lods(bool _setup_zb, bool _clear);
    void render_sorted();
    void render_emissive();
    void render_wmarks();
    void render_distort();
    void render_R1_box(IRender_Sector::sector_id_t sector_id, Fbox& _bb, int _element);

    bool build_subspace_static(bool defer_main_thread_work);
    void build_subspace_dynamic();
    void build_subspace();
};
} // namespace xray::render::RENDER_NAMESPACE
