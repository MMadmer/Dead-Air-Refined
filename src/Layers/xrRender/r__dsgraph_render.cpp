#include "stdafx.h"

#include <functional>

#include "xrEngine/IRenderable.h"
#include "xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "FTreeVisual.h"
#include "SkeletonCustom.h"
#include "FLOD.h"

extern ENGINE_API float psHUD_FOV;

namespace xray::render::RENDER_NAMESPACE
{
using namespace R_dsgraph;

extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

ICF float calcLOD(float ssa /*fDistSq*/, float /*R*/)
{
    return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

#ifdef USE_DX11
namespace
{
constexpr u32 TreeInstanceBatchCapacity = 64;

struct TreeBatchItem
{
    FTreeVisual* visual{};
    FTreeVisualInstancedDraw draw{};
    float lod{};
    u32 lod_bucket{};
    size_t original_order{};
};

struct TreeBatchSegment
{
    size_t begin{};
    u32 count{};
};

struct TreeBatchScratch
{
    xr_vector<TreeBatchItem> items;
    xr_vector<TreeBatchSegment> segments;
    xr_vector<u8> batched_items;
};

FTreeVisual* get_tree_visual(dxRender_Visual* visual)
{
    switch (visual->Type)
    {
    case MT_TREE_ST:
    case MT_TREE_PM: return static_cast<FTreeVisual*>(visual);
    default: return nullptr;
    }
}

u32 get_lod_bucket(float lod)
{
    return u32(clampr<float>(ceil(lod * lod * lod * lod * lod * 8.0f), 1, 7));
}

bool equal_tree_geometry_state(const TreeBatchItem& left, const TreeBatchItem& right)
{
    const SGeometry& left_geometry = *left.draw.geometry;
    const SGeometry& right_geometry = *right.draw.geometry;
    return left_geometry.vb == right_geometry.vb &&
        left_geometry.ib == right_geometry.ib &&
        left_geometry.dcl._get() == right_geometry.dcl._get() &&
        left_geometry.vb_stride == right_geometry.vb_stride;
}

bool equal_tree_draw(const TreeBatchItem& left, const TreeBatchItem& right)
{
    return equal_tree_geometry_state(left, right) &&
        left.draw.base_vertex == right.draw.base_vertex &&
        left.draw.vertex_count == right.draw.vertex_count &&
        left.draw.start_index == right.draw.start_index &&
        left.draw.primitive_count == right.draw.primitive_count &&
        left.lod_bucket == right.lod_bucket;
}

bool less_tree_draw(const TreeBatchItem& left, const TreeBatchItem& right)
{
    const SGeometry& left_geometry = *left.draw.geometry;
    const SGeometry& right_geometry = *right.draw.geometry;
    if (left_geometry.vb != right_geometry.vb)
        return std::less<VertexBufferHandle>{}(left_geometry.vb, right_geometry.vb);
    if (left_geometry.ib != right_geometry.ib)
        return std::less<IndexBufferHandle>{}(left_geometry.ib, right_geometry.ib);
    if (left_geometry.dcl._get() != right_geometry.dcl._get())
        return std::less<SDeclaration*>{}(left_geometry.dcl._get(), right_geometry.dcl._get());
    if (left_geometry.vb_stride != right_geometry.vb_stride)
        return left_geometry.vb_stride < right_geometry.vb_stride;
    if (left.draw.base_vertex != right.draw.base_vertex)
        return left.draw.base_vertex < right.draw.base_vertex;
    if (left.draw.vertex_count != right.draw.vertex_count)
        return left.draw.vertex_count < right.draw.vertex_count;
    if (left.draw.start_index != right.draw.start_index)
        return left.draw.start_index < right.draw.start_index;
    if (left.draw.primitive_count != right.draw.primitive_count)
        return left.draw.primitive_count < right.draw.primitive_count;
    if (left.lod_bucket != right.lod_bucket)
        return left.lod_bucket < right.lod_bucket;
    return left.original_order < right.original_order;
}

bool get_tree_instance_constants(CBackend& cmd_list, R_constant*& instance_data, R_constant*& instance_control)
{
    instance_data = cmd_list.get_c("tree_instance_data")._get();
    instance_control = cmd_list.get_c("tree_instance_control")._get();
    if (!instance_data || !instance_control)
        return false;
    if (!(instance_data->destination & RC_dest_vertex) || !(instance_control->destination & RC_dest_vertex))
        return false;
    const bool valid_data_class = instance_data->vs.cls == RC_1x4 || instance_data->vs.cls == RC_1x4a;
    if (instance_data->type != RC_float || !valid_data_class ||
        instance_control->type != RC_float || instance_control->vs.cls != RC_1x4)
        return false;

    const u32 data_buffer = instance_data->destination & RC_dest_vertex_cb_index_mask;
    const u32 control_buffer = instance_control->destination & RC_dest_vertex_cb_index_mask;
    return data_buffer != control_buffer && !instance_data->vs.index && !instance_control->vs.index;
}

bool render_tree_batches(CBackend& cmd_list, mapNormalItems& items)
{
    R_constant* instance_data{};
    R_constant* instance_control{};
    if (!get_tree_instance_constants(cmd_list, instance_data, instance_control))
        return false;
    cmd_list.set_c(instance_control, 0.f, 0.f, 0.f, 0.f);
    if (items.size() < 2)
        return false;

    static thread_local TreeBatchScratch scratch;
    xr_vector<TreeBatchItem>& tree_items = scratch.items;
    tree_items.clear();
    if (tree_items.capacity() < items.size())
        tree_items.reserve(items.size());
    for (size_t index = 0; index < items.size(); ++index)
    {
        const _NormalItem& item = items[index];
        FTreeVisual* tree = get_tree_visual(item.pVisual);
        if (!tree)
            continue;

        const float lod = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
        FTreeVisualInstancedDraw draw;
        if (!tree->GetInstancedDraw(lod, draw))
            continue;

        tree_items.push_back(TreeBatchItem{ tree, draw, lod, get_lod_bucket(lod), index });
    }

    if (tree_items.size() < 2)
        return false;

    std::sort(tree_items.begin(), tree_items.end(), less_tree_draw);

    xr_vector<TreeBatchSegment>& segments = scratch.segments;
    segments.clear();
    for (size_t group_begin = 0; group_begin < tree_items.size();)
    {
        size_t group_end = group_begin + 1;
        while (group_end < tree_items.size() && equal_tree_draw(tree_items[group_begin], tree_items[group_end]))
            ++group_end;

        size_t batch_begin = group_begin;
        while (group_end - batch_begin > 1)
        {
            u32 instance_count = u32(std::min<size_t>(TreeInstanceBatchCapacity, group_end - batch_begin));

            // Keep a final pair instanced instead of leaving one tree on the scalar fallback path.
            if (group_end - batch_begin - instance_count == 1)
                --instance_count;

            segments.push_back(TreeBatchSegment{ batch_begin, instance_count });
            batch_begin += instance_count;
        }
        group_begin = group_end;
    }

    if (segments.empty())
        return false;

    static const shared_str instance_data_name = "tree_instance_data";
    void* validated_vertex_data{};
    cmd_list.get_ConstantDirect(instance_data_name,
        TreeInstanceBatchCapacity * sizeof(FTreeVisualInstanceData), &validated_vertex_data, nullptr, nullptr);
    if (!validated_vertex_data)
        return false;

    xr_vector<u8>& batched_items = scratch.batched_items;
    batched_items.assign(items.size(), false);
    FTreeVisual::SetupInstancedGlobals(cmd_list);

    for (size_t page_begin = 0; page_begin < segments.size();)
    {
        size_t page_end = page_begin;
        u32 page_instance_count{};
        while (page_end < segments.size() &&
            page_instance_count + segments[page_end].count <= TreeInstanceBatchCapacity)
        {
            page_instance_count += segments[page_end++].count;
        }

        void* vertex_data{};
        cmd_list.get_ConstantDirect(instance_data_name,
            page_instance_count * sizeof(FTreeVisualInstanceData), &vertex_data, nullptr, nullptr);
        if (!vertex_data)
            return false;

        auto* instance_storage = static_cast<FTreeVisualInstanceData*>(vertex_data);
        u32 page_offset{};
        for (size_t segment_index = page_begin; segment_index < page_end; ++segment_index)
        {
            const TreeBatchSegment& segment = segments[segment_index];
            for (u32 instance = 0; instance < segment.count; ++instance)
            {
                TreeBatchItem& item = tree_items[segment.begin + instance];
                item.visual->FillInstanceData(cmd_list, instance_storage[page_offset + instance]);
                batched_items[item.original_order] = true;
            }

            page_offset += segment.count;
        }

        page_offset = 0;
        for (size_t segment_index = page_begin; segment_index < page_end; ++segment_index)
        {
            const TreeBatchSegment& segment = segments[segment_index];
            const TreeBatchItem& first = tree_items[segment.begin];
            cmd_list.set_c(instance_control, 1.f, float(page_offset), 0.f, 0.f);
            cmd_list.LOD.set_LOD(first.lod);
            cmd_list.set_Geometry(first.draw.geometry);
            cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, first.draw.base_vertex, 0,
                first.draw.vertex_count, first.draw.start_index, first.draw.primitive_count, segment.count);
            cmd_list.stat.r.s_flora.add(first.draw.vertex_count * segment.count);
            page_offset += segment.count;
        }

        page_begin = page_end;
    }

    cmd_list.set_c(instance_control, 0.f, 0.f, 0.f, 0.f);
    size_t write_index{};
    for (size_t read_index = 0; read_index < items.size(); ++read_index)
    {
        if (!batched_items[read_index])
            items[write_index++] = items[read_index];
    }
    items.resize(write_index);
    return true;
}


} // namespace
#endif

template <class T>
bool cmp_ssa(const T &lhs, const T &rhs)
{
    return lhs.ssa > rhs.ssa;
}

// Sorting by SSA and changes minimizations
// The old form short-circuited on SPass::equal() before comparing ssa. Two content-equal
// passes with different ssa values then compared as equivalent while ordering differently
// against a third pass, which breaks strict weak ordering. std::sort is undefined with such
// a comparator: introsort's partition loop has no bounds check and relies on the comparator
// to stop it, so once the data lines up it walks past the end of the vector and dereferences
// garbage as an SPass* — typically only after tens of thousands of frames. Plain ssa order
// with the pass pointer as a tie-break is a proper strict weak ordering and keeps the
// intent: front-to-back by ssa, identical passes (same pointer key) adjacent.
template <typename T>
bool cmp_pass(const T& left, const T& right)
{
    if (left->second.ssa != right->second.ssa)
        return left->second.ssa > right->second.ssa;
    return left->first < right->first;
}

void R_dsgraph_structure::render_graph(u32 _priority)
{
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later

    cmd_list.set_xform_world(Fidentity);

    // **************************************************** NORMAL
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    {
        ZoneScopedN("dsgraph_render_static");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_static);

        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapNormalPasses[_priority][iPass];

            map.get_any_p(nrmPasses);
            if (nrmPasses.size() > 1)
                std::sort(nrmPasses.begin(), nrmPasses.end(), cmp_pass<mapNormal_T::value_type*>);
            for (const auto& it : nrmPasses)
            {
                cmd_list.set_Pass(it->first);
                cmd_list.apply_lmaterial();

                mapNormalItems& items = it->second;
                items.ssa = 0;

                if (items.size() > 1)
                    std::sort(items.begin(), items.end(), cmp_ssa<_NormalItem>);
#ifdef USE_DX11
                if (render_tree_batches(cmd_list, items) && items.empty())
                    continue;
#endif
                for (const auto& item : items)
                {
                    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                    cmd_list.LOD.set_LOD(LOD);
#endif
                    // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                    // RCache.hemi.c_update(item.pVisual);

                    item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
                }
                items.clear();

            }
            nrmPasses.clear();
            map.clear();
            cachedNormalPasses[_priority][iPass] = nullptr;
            cachedNormalItems[_priority][iPass] = nullptr;
        }
    }

    // **************************************************** MATRIX
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    {
        ZoneScopedN("dsgraph_render_dynamic");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_dynamic);

        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapMatrixPasses[_priority][iPass];

            map.get_any_p(matPasses);
            if (matPasses.size() > 1)
                std::sort(matPasses.begin(), matPasses.end(), cmp_pass<mapMatrix_T::value_type*>);
            for (const auto& it : matPasses)
            {
                cmd_list.set_Pass(it->first);

                mapMatrixItems& items = it->second;
                items.ssa = 0;

                if (items.size() > 1)
                    std::sort(items.begin(), items.end(), cmp_ssa<_MatrixItem>);
                for (auto& item : items)
                {
                    cmd_list.set_xform_world(item.Matrix);
                    RImplementation.apply_object(cmd_list, item.pObject);
                    cmd_list.apply_lmaterial();

                    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                    cmd_list.LOD.set_LOD(LOD);
#endif
                    // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                    // RCache.hemi.c_update(item.pVisual);

                    item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
                }
                items.clear();
            }
            matPasses.clear();
            map.clear();
            cachedMatrixPasses[_priority][iPass] = nullptr;
            cachedMatrixItems[_priority][iPass] = nullptr;
        }
    }

    RImplementation.BasicStats.Primitives.End(); // XXX: Refactor a bit later
}

//////////////////////////////////////////////////////////////////////////
// Helper classes and functions

/*
Предназначен для установки режима отрисовки HUD и возврата оригинального после отрисовки.
*/
class hud_transform_helper
{
    Fmatrix Pold;
    static u32 cullMode;
    static bool isActive;

    CBackend& cmd_list;

public:
    explicit hud_transform_helper(CBackend& cmd_list_in)
        : cmd_list(cmd_list_in)
    {
        // Change projection
        Pold  = Device.mProject;

        // XXX: Xottab_DUTY: custom FOV. Implement it someday
        // It should be something like this:
        // float customFOV;
        // if (isCustomFOV)
        //     customFOV = V->getVisData().obj_data->m_hud_custom_fov;
        // else
        //     customFOV = psHUD_FOV * Device.fFOV;
        //
        // Device.mProject.build_projection(deg2rad(customFOV), Device.fASPECT,
        //    VIEWPORT_NEAR, g_pGamePersistent->Environment().CurrentEnv.far_plane);
        //
        // Look at the function:
        // void __fastcall sorted_L1_HUD(mapSorted_Node* N)
        // In the commit:
        // https://github.com/ShokerStlk/xray-16-SWM/commit/869de0b6e74ac05990f541e006894b6fe78bd2a5#diff-4199ef700b18ce4da0e2b45dee1924d0R83

        Fmatrix prj_new;
        prj_new.build_projection(deg2rad(psHUD_FOV * Device.fFOV /* *Device.fASPECT*/), Device.fASPECT,
            HUD_VIEWPORT_NEAR, g_pGamePersistent->Environment().CurrentEnv.far_plane);
        cmd_list.set_xform_project(prj_new);

        RImplementation.rmNear(cmd_list);

        // preserve culling mode
        cullMode = cmd_list.get_CullMode();
        isActive = true;
    }

    ~hud_transform_helper()
    {
        RImplementation.rmNormal(cmd_list);

        // Restore projection
        cmd_list.set_xform_project(Pold);
        // restore culling mode
        cmd_list.set_CullMode(cullMode);
        isActive = false;
    }

    static void apply_custom_state(CBackend& cmd_list)
    {
        if (!isActive || !psHUD_Flags.test(HUD_LEFT_HANDED))
            return;

        // Change culling mode if HUD meshes were flipped
        if (cullMode != CULL_NONE)
        {
            cmd_list.set_CullMode(cullMode == CULL_CW ? CULL_CCW : CULL_CW);
        }
    }
};

u32 hud_transform_helper::cullMode = CULL_NONE;
bool hud_transform_helper::isActive = false;

template<class T>
void __fastcall render_item(u32 context_id, const T& item)
{
    auto& dsgraph = RImplementation.get_context(context_id);

    dxRender_Visual* V = item.second.pVisual;
    VERIFY(V && V->shader._get());
    dsgraph.cmd_list.set_Element(item.second.se);
    dsgraph.cmd_list.set_xform_world(item.second.Matrix);
    RImplementation.apply_object(dsgraph.cmd_list, item.second.pObject);
    dsgraph.cmd_list.apply_lmaterial();
    hud_transform_helper::apply_custom_state(dsgraph.cmd_list);
    //--#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
    //RCache.hemi.c_update(V);
    V->Render(dsgraph.cmd_list, calcLOD(item.first, V->vis.sphere.R), dsgraph.o.phase == CRender::PHASE_SMAP);
}

template<class T>
ICF void sort_front_to_back_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_left_right(context_id, render_item);
    vec.clear();
}

template<class T>
ICF void sort_back_to_front_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_right_left(context_id, render_item);
    vec.clear();
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void R_dsgraph_structure::render_hud()
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud);

    if (!mapHUD.empty())
    {
        hud_transform_helper helper{ cmd_list };
        sort_front_to_back_render_and_clean(context_id, mapHUD);
    }

#if RENDER == R_R1
    if (g_pGameLevel->pHUD && g_pGameLevel->pHUD->RenderActiveItemUIQuery())
        render_hud_ui(); // hud ui
#endif
}

void R_dsgraph_structure::render_hud_ui()
{
    ZoneScoped;
    CCustomHUD* levelHud = g_pGameLevel->pHUD;
    VERIFY(levelHud && levelHud->RenderActiveItemUIQuery());

    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud_ui);

    hud_transform_helper helper{ cmd_list };

#if RENDER != R_R1
    // Targets, use accumulator for temporary storage
    const ref_rt rt_null;
    cmd_list.set_RT(0, 1);
    cmd_list.set_RT(0, 2);
    auto zb = RImplementation.Target->rt_Base_Depth;

#if (RENDER == R_R3) || (RENDER == R_R4) || (RENDER==R_GL)
    if (RImplementation.o.msaa)
        zb = RImplementation.Target->rt_MSAADepth;
#endif

    RImplementation.Target->u_setrt(cmd_list,
        RImplementation.o.albedo_wo ? RImplementation.Target->rt_Accumulator : RImplementation.Target->rt_Color,
        rt_null, rt_null, zb);
#endif // RENDER!=R_R1

    levelHud->RenderActiveItemUI();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_sorted()
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_sorted);

    sort_back_to_front_render_and_clean(context_id, mapSorted);

    if (!mapHUDSorted.empty())
    {
        hud_transform_helper helper{ cmd_list };
        sort_back_to_front_render_and_clean(context_id, mapHUDSorted);
    }
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_emissive()
{
#if RENDER != R_R1
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_emissive);

    sort_front_to_back_render_and_clean(context_id, mapEmissive);

    if (!mapHUDEmissive.empty())
    {
        hud_transform_helper helper{ cmd_list };
        sort_front_to_back_render_and_clean(context_id, mapHUDEmissive);
    }
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_wmarks()
{
#if RENDER != R_R1
    ZoneScoped;
    PIX_EVENT(dsgraph_render_wmarks);

    sort_front_to_back_render_and_clean(context_id, mapWmark);
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_distort()
{
    ZoneScoped;
    PIX_EVENT(dsgraph_render_distort);

    sort_back_to_front_render_and_clean(context_id, mapDistort);
}

void R_dsgraph_structure::render_R1_box(IRender_Sector::sector_id_t sector_id, Fbox& BB, int sh)
{
    VERIFY(sector_id != IRender_Sector::INVALID_SECTOR_ID);
    auto* S = Sectors[sector_id];

    PIX_EVENT(dsgraph_render_R1_box);

    lstVisuals.clear();
    lstVisuals.push_back(((CSector*)S)->root());

    for (size_t test = 0; test < lstVisuals.size(); ++test)
    {
        dxRender_Visual* V = lstVisuals[test];

        // Visual is 100% visible - simply add it
        switch (V->Type)
        {
        case MT_HIERRARHY:
        {
            // Add all children
            FHierrarhyVisual* pV = (FHierrarhyVisual*)V;
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        case MT_SKELETON_ANIM:
        case MT_SKELETON_RIGID:
        {
            // Add all children	(s)
            CKinematics* pV = (CKinematics*)V;
            pV->CalculateBones(TRUE);
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        case MT_LOD:
        {
            FLOD* pV = (FLOD*)V;
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        default:
        {
            // Renderable visual
            ShaderElement* E2 = V->shader->E[sh]._get();
            if (E2 && !(E2->flags.bDistort))
            {
                for (u32 pass = 0; pass < E2->passes.size(); pass++)
                {
                    cmd_list.set_Element(E2, pass);
                    V->Render(cmd_list, -1.f, o.phase == CRender::PHASE_SMAP);
                }
            }
        }
        break;
        }
    }
}
} // namespace xray::render::RENDER_NAMESPACE
