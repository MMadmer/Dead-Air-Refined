#include "stdafx.h"

#include <functional>

#include "xrEngine/IRenderable.h"
#include "xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "FTreeVisual.h"
#include "SkeletonCustom.h"
#include "FLOD.h"
#include "xrCore/Threading/ParallelFor.hpp"
#if RENDER == R_R4
#include "da_temporal.h"
#include "da_clusters.h"
#include "LightTrack.h"
#include "FSkinned.h"
#endif
#ifdef XR_NATIVE
#include "Layers/xrRenderPC_R4/da_gpu_trees.h"
#endif

extern ENGINE_API float psHUD_FOV;

namespace xray::render::RENDER_NAMESPACE
{
using namespace R_dsgraph;

// Batching does not just draw differently - it SORTS the draw list and REMOVES whatever
// it drew in batches. Any change that alters the draw list composition must carry a kill
// switch: without one, "an object vanished at a certain angle" has to be debugged by
// reasoning instead of by flipping a single value. Outside the anonymous namespace: the
// console handle links against it.
int ps_r__tree_batch = 1;

extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

ICF float calcLOD(float ssa /*fDistSq*/, float /*R*/)
{
    return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

// A world matrix of negative determinant (the left-handed hud, the actor's mirrored shadow) turns the visual's
// triangles over. Not a particle's: it draws world-space quads built to face the camera, the matrix unused.
ICF bool draws_mirrored(const dxRender_Visual* visual, const Fmatrix& m)
{
    return visual->Type != MT_PARTICLE_EFFECT &&
        m._11 * (m._22 * m._33 - m._23 * m._32) - m._12 * (m._21 * m._33 - m._23 * m._31) +
            m._13 * (m._21 * m._32 - m._22 * m._31) < 0.f;
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

// The share of its pixels a visual draws this frame: what the main view's walk gave it (r__dsgraph_build.cpp).
float fade_cover(const dxRender_Visual* visual)
{
    return visual->vis.fade_frame == Device.dwFrame ? visual->vis.fade_draw : 1.f;
}

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

bool render_tree_batches(CBackend& cmd_list, mapNormalItems& items, bool fade)
{
    if (!ps_r__tree_batch)
        return false;

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
        if (!tree || (fade && fade_cover(item.pVisual) < 1.f))
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

template <typename PassList>
void sort_pass_items(PassList& passes)
{
    const auto sort_range = [&passes](const TaskRange<size_t>& range)
    {
        for (size_t i = range.begin(); i != range.end(); ++i)
        {
            auto& items = passes[i]->second;
            if (items.size() > 1)
                std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.ssa > b.ssa; });
        }
    };
    size_t item_count = 0;
    for (const auto& pass : passes)
        item_count += pass->second.size();
    // Each pass owns a separate item array. Sorting touches no visuals or render context;
    // all sorts finish before any draw or list compaction begins.
    if (passes.size() >= 4 && item_count >= 2048 && TaskScheduler && TaskScheduler->GetWorkersCount() > 1)
        xr_parallel_for(TaskRange<size_t>(0, passes.size(), 1), sort_range);
    else
        sort_range(TaskRange<size_t>(0, passes.size(), 1));
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
    // Group passes by their constant table first. set_Constants has an early-out when the
    // table did not change, but pure ssa order interleaves the (few) tables across the (many)
    // passes and the early-out almost never fires; grouping removes most of those calls
    // outright - the sibling engine measured render 3.79 -> 3.66 ms with an unchanged GPU
    // time (ported from 4ed0004). Front-to-back ssa order is kept inside each group.
    const void* leftTable = left->first ? left->first->constants._get() : nullptr;
    const void* rightTable = right->first ? right->first->constants._get() : nullptr;
    if (leftTable != rightTable)
        return std::less<const void*>{}(leftTable, rightTable);
    if (left->second.ssa != right->second.ssa)
        return left->second.ssa > right->second.ssa;
    return left->first < right->first;
}

#if RENDER == R_R4
// QA: 0 draws the main view's moving objects on the immediate context.
int ps_r__mt_dynamic = 1;

namespace
{
constexpr u32 kMatrixChunks = R_dsgraph_structure::MATRIX_CHUNKS;
// Below this the lists' setup costs the main thread more than the draws they take off it (measured: 30 items, 0.4 ms
// against 0.2 ms).
constexpr size_t kMatrixChunkMinItems = 64;

// All of an object's parts share a context: whichever draws first advances the model's previous pose for the others.
u32 matrix_chunk(const IRenderable* object, u32 chunks)
{
    return (u32(uintptr_t(object) >> 8) * 2654435761u >> 16) % chunks;
}

// What a deferred context may record: a model's own geometry. Particles and software skinning fill the shared dynamic
// vertex stream, which only the immediate context may lock and map.
bool matrix_item_deferrable(const _MatrixItem& item)
{
    switch (item.pVisual->Type)
    {
    case MT_NORMAL:
    case MT_PROGRESSIVE: return true;
    case MT_SKELETON_GEOMDEF_ST: return !static_cast<CSkeletonX_ST*>(item.pVisual)->soft_skinned();
    case MT_SKELETON_GEOMDEF_PM: return !static_cast<CSkeletonX_PM*>(item.pVisual)->soft_skinned();
    default: return false;
    }
}

// Whether a worker may set this pass on its deferred context.
bool pass_binds_anywhere(const SPass* pass)
{
    if (const STextureList* list = pass->T._get())
        for (const auto& [stage, texture] : *list)
            if (texture && !texture->bind_anywhere())
                return false;
    return true;
}

xr_vector<mapMatrix_T::value_type*> g_matrix_passes[SHADER_PASSES_MAX];

void draw_matrix_item(CBackend& cmd, const _MatrixItem& item, bool fade, bool mirror)
{
    cmd.set_xform_world(item.Matrix);
    RImplementation.apply_object(cmd, item.pObject);
    cmd.apply_lmaterial();
    if (mirror)
        cmd.set_WindingFlip(draws_mirrored(item.pVisual, item.Matrix));
    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
    cmd.LOD.set_LOD(LOD);
    const float cover = fade ? fade_cover(item.pVisual) : 1.f;
    if (cover < 1.f)
        cmd.LOD.set_fade(cover);
    item.pVisual->Render(cmd, LOD, false);
    if (cover < 1.f)
        cmd.LOD.set_fade(1.f);
}

void record_matrix_chunk(CBackend& cmd, u32 chunk, u32 chunks, bool fade, bool mirror)
{
    for (const auto& list : g_matrix_passes)
    {
        for (const auto* pass : list)
        {
            if (!pass_binds_anywhere(pass->first))
                continue;
            bool bound = false;
            for (const auto& item : pass->second)
            {
                if (!matrix_item_deferrable(item) || matrix_chunk(item.pObject, chunks) != chunk)
                    continue;
                if (!bound)
                {
                    cmd.set_Pass(pass->first);
                    bound = true;
                }
                draw_matrix_item(cmd, item, fade, mirror);
            }
        }
    }
    if (mirror)
        cmd.set_WindingFlip(false);
}

// A deferred context's G-buffer state, as the immediate context has it for the main view.
void scene_context_begin(CBackend& cmd, bool velocity)
{
    RImplementation.Target->phase_scene_begin_parallel(cmd, velocity);
    cmd.set_xform_view(Device.mView);
    cmd.set_xform_project(da_jitter_project(Device.mProject));
    cmd.xforms.set_VP_motion(da_vp_nojit(), da_vp_prev());
    SSManager.UseSceneBias(cmd.context_id, da_upscaler_active());
}
} // namespace

// The priority 0 moving objects' passes; returns how many of their items a deferred context may record.
size_t R_dsgraph_structure::matrix_gather()
{
    size_t total = 0;
    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        g_matrix_passes[iPass].clear();
        mapMatrixPasses[0][iPass].get_any_p(g_matrix_passes[iPass]);
        for (const auto* pass : g_matrix_passes[iPass])
            if (pass_binds_anywhere(pass->first))
                for (const auto& item : pass->second)
                    total += matrix_item_deferrable(item);
    }
    return total;
}

void R_dsgraph_structure::matrix_prepare()
{
    for (auto& list : g_matrix_passes)
    {
        if (list.size() > 1)
            std::sort(list.begin(), list.end(), cmp_pass<mapMatrix_T::value_type*>);
        sort_pass_items(list);
        // The objects' light (CROS_impl) takes its queries and its cache here, once a frame an object: the contexts read
        // it.
        for (const auto* pass : list)
            for (const auto& item : pass->second)
                if (item.pObject && item.pObject->renderable_ROS())
                    static_cast<CROS_impl*>(item.pObject->renderable_ROS())->update_smooth(item.pObject);
    }
}

bool R_dsgraph_structure::render_matrix_parallel(bool fade)
{
    const bool velocity = cmd_list.xforms.m_motion_pass;
    if (matrix_lists_n)
    {
        for (u32 k = 0; k < matrix_lists_n; ++k)
            CBackend::execute(matrix_lists[k]);
        matrix_lists_n = 0;
    }
    else
    {
        if (!ps_r__mt_dynamic || !o.is_main_pass || o.phase != CRender::PHASE_NORMAL || context_id != IMM_CTX_ID ||
            !RImplementation.o.mt_render)
            return false;
        if (matrix_gather() < kMatrixChunkMinItems)
            return false;

        u32 contexts[kMatrixChunks];
        u32 chunks = 0;
        while (chunks < kMatrixChunks)
        {
            const u32 id = RImplementation.alloc_context();
            if (id == INVALID_CONTEXT_ID)
                break;
            contexts[chunks++] = id;
        }
        if (chunks < 2)
        {
            for (u32 k = 0; k < chunks; ++k)
                RImplementation.release_context(contexts[k]);
            return false;
        }

        ZoneScopedN("dsgraph_render_dynamic_parallel");
        matrix_prepare();
        ID3D11CommandList* lists[kMatrixChunks]{};
        xr_parallel_for(TaskRange<u32>(0, chunks, 1), [&](const TaskRange<u32>& range) {
            for (u32 k = range.begin(); k != range.end(); ++k)
            {
                CBackend& cmd = RImplementation.get_context(contexts[k]).cmd_list;
                scene_context_begin(cmd, velocity);
                record_matrix_chunk(cmd, k, chunks, fade, o.mirror_winding);
                SSManager.UseSceneBias(cmd.context_id, false);
                lists[k] = cmd.finish();
            }
        });
        for (u32 k = 0; k < chunks; ++k)
        {
            CBackend::execute(lists[k]);
            RImplementation.release_context(contexts[k]);
        }
    }

    // The executed lists cleared the immediate context: its cached state goes, the G-buffer's targets come back.
    cmd_list.Invalidate();
    RImplementation.Target->phase_scene_begin_parallel(cmd_list, velocity);

    // What stayed on the immediate context (its depth against the lists' is settled by the depth test).
    for (const auto& list : g_matrix_passes)
    {
        for (const auto* pass : list)
        {
            const bool anywhere = pass_binds_anywhere(pass->first);
            bool bound = false;
            for (const auto& item : pass->second)
            {
                if (anywhere && matrix_item_deferrable(item))
                    continue;
                if (!bound)
                {
                    cmd_list.set_Pass(pass->first);
                    bound = true;
                }
                draw_matrix_item(cmd_list, item, fade, o.mirror_winding);
            }
        }
    }
    if (o.mirror_winding)
        cmd_list.set_WindingFlip(false);

    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        for (auto* pass : g_matrix_passes[iPass])
        {
            pass->second.ssa = 0;
            pass->second.clear();
        }
        g_matrix_passes[iPass].clear();
        mapMatrixPasses[0][iPass].clear();
        cachedMatrixPasses[0][iPass] = nullptr;
        cachedMatrixItems[0][iPass] = nullptr;
    }
    return true;
}
#endif

#if RENDER == R_R4
int ps_r__mt_static = 1;

namespace
{
constexpr u32 kStaticChunks = 4;
// The lists' setup costs the game thread about 0.2 ms: fewer draws than this are not worth it.
constexpr size_t kStaticChunkMinWeight = 256;
// A pass's state change against a draw of an item.
constexpr size_t kStaticPassWeight = 4;
} // namespace

// The main view's statics without trees, on deferred contexts in their sort order: runs of whole passes, executed
// in order, so the G-buffer gets the same draws as from the immediate context. render_graph draws the rest. The moving
// objects record beside them, however few: the setup is paid already, and their lists wait for their place after the
// statics (render_matrix_parallel).
void R_dsgraph_structure::render_static_parallel(bool fade)
{
    if (!ps_r__mt_static || !o.is_main_pass || o.phase != CRender::PHASE_NORMAL || o.smap_draw != 0 ||
        context_id != IMM_CTX_ID || !RImplementation.o.mt_render)
        return;

    struct Job
    {
        SPass* pass;
        mapNormalItems* items;
        u32 pass_index;
        size_t weight;
    };
    static xr_vector<Job> jobs;
    static xr_vector<mapNormal_T::value_type*> list;
    jobs.clear();
    size_t total = 0;
    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        list.clear();
        mapNormalPasses[0][iPass].get_any_p(list);
        if (list.size() > 1)
            std::sort(list.begin(), list.end(), cmp_pass<mapNormal_T::value_type*>);
        sort_pass_items(list);
        for (auto* pass : list)
        {
            // Trees batch, wind and go through ExecuteIndirect on the immediate context.
            const bool plain = pass_binds_anywhere(pass->first) &&
                std::all_of(pass->second.begin(), pass->second.end(), [](const _NormalItem& item) {
                    return item.pVisual->Type == MT_NORMAL || item.pVisual->Type == MT_PROGRESSIVE;
                });
            if (!plain)
                continue;
            const size_t weight = pass->second.size() + kStaticPassWeight;
            jobs.push_back({pass->first, &pass->second, iPass, weight});
            total += weight;
        }
    }
    if (total < kStaticChunkMinWeight)
        return;

    u32 contexts[kStaticChunks + kMatrixChunks];
    u32 chunks = 0;
    while (chunks < kStaticChunks)
    {
        const u32 id = RImplementation.alloc_context();
        if (id == INVALID_CONTEXT_ID)
            break;
        contexts[chunks++] = id;
    }
    if (chunks < 2)
    {
        for (u32 k = 0; k < chunks; ++k)
            RImplementation.release_context(contexts[k]);
        return;
    }

    u32 matrix_chunks = 0;
    if (ps_r__mt_dynamic && matrix_gather())
    {
        while (matrix_chunks < kMatrixChunks)
        {
            const u32 id = RImplementation.alloc_context();
            if (id == INVALID_CONTEXT_ID)
                break;
            contexts[chunks + matrix_chunks++] = id;
        }
        if (matrix_chunks < 2)
        {
            for (u32 k = 0; k < matrix_chunks; ++k)
                RImplementation.release_context(contexts[chunks + k]);
            matrix_chunks = 0;
        }
        else
            matrix_prepare();
    }

    ZoneScopedN("dsgraph_render_static_parallel");
    // Contiguous runs of about the same weight.
    size_t ends[kStaticChunks + 1]{};
    size_t job = 0, taken = 0;
    for (u32 k = 0; k < chunks; ++k)
    {
        const size_t target = total * (k + 1) / chunks;
        while (job < jobs.size() && taken < target)
            taken += jobs[job++].weight;
        ends[k + 1] = job;
    }
    ends[chunks] = jobs.size();

    const bool velocity = cmd_list.xforms.m_motion_pass;
    ID3D11CommandList* lists[kStaticChunks + kMatrixChunks]{};
    const auto record = [&](const TaskRange<u32>& range)
    {
        for (u32 t = range.begin(); t != range.end(); ++t)
        {
            CBackend& cmd = RImplementation.get_context(contexts[t]).cmd_list;
            scene_context_begin(cmd, velocity);
            if (t >= chunks)
                record_matrix_chunk(cmd, t - chunks, matrix_chunks, fade, o.mirror_winding);
            else
            {
                cmd.set_xform_world(Fidentity);
                for (size_t j = ends[t]; j != ends[t + 1]; ++j)
                {
                    const Job& run = jobs[j];
                    cmd.set_Pass(run.pass);
                    cmd.apply_lmaterial();
                    da_clusters_draw_pass(*this, cmd, run.pass, run.pass_index);
                    for (const auto& item : *run.items)
                    {
                        const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
                        cmd.LOD.set_LOD(LOD);
                        const float cover = fade ? fade_cover(item.pVisual) : 1.f;
                        if (cover < 1.f)
                            cmd.LOD.set_fade(cover);
                        item.pVisual->Render(cmd, LOD, false);
                        if (cover < 1.f)
                            cmd.LOD.set_fade(1.f);
                    }
                }
            }
            SSManager.UseSceneBias(cmd.context_id, false);
            lists[t] = cmd.finish();
        }
    };
    xr_parallel_for(TaskRange<u32>(0, chunks + matrix_chunks, 1), record);

    for (u32 k = 0; k < chunks; ++k)
        CBackend::execute(lists[k]);
    for (u32 k = 0; k < matrix_chunks; ++k)
        matrix_lists[k] = lists[chunks + k];
    matrix_lists_n = matrix_chunks;
    for (u32 k = 0; k < chunks + matrix_chunks; ++k)
        RImplementation.release_context(contexts[k]);

    // The executed lists cleared the immediate context: its cached state goes, the G-buffer's targets come back.
    cmd_list.Invalidate();
    RImplementation.Target->phase_scene_begin_parallel(cmd_list, velocity);
    cmd_list.set_xform_world(Fidentity);

    for (const Job& run : jobs)
        run.items->recorded = true;
}

#endif

void R_dsgraph_structure::render_graph(u32 _priority)
{
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later

    cmd_list.set_xform_world(Fidentity);
    // The share of each visual's pixels this view draws (r__dsgraph_build.cpp, da_fade.h).
    const bool fade = fade_view();
    if (fade && _priority == 0)
    {
        if (ps_r__fade_stats > 0)
        {
            --ps_r__fade_stats;
            Msg("* [fade] frame %u: whole %u, fading %u, dropped %u", Device.dwFrame, fade_counts[0], fade_counts[1],
                fade_counts[2]);
        }
        fade_counts[0] = fade_counts[1] = fade_counts[2] = 0;
    }

#if RENDER == R_R4
    if (_priority == 0)
        render_static_parallel(fade);
#endif

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
            sort_pass_items(nrmPasses);

            windVisuals.clear();
            for (const auto& pass : nrmPasses)
                for (const auto& item : pass->second)
                    if (item.pVisual->Type == MT_TREE_ST || item.pVisual->Type == MT_TREE_PM)
                    {
                        auto* tree = static_cast<FTreeVisual*>(item.pVisual);
#ifdef XR_NATIVE
                        // The GPU-driven trees integrate their roots at the frame's start.
                        if (da_gpu_trees_draws(tree))
                            continue;
#endif
                        if (tree->NeedsWindUpdate())
                            windVisuals.push_back(tree);
                    }
            FTreeVisual::PrepareWind(windVisuals);
            windVisuals.clear();

            for (const auto& it : nrmPasses)
            {
                if (it->second.recorded)
                {
                    it->second.recorded = false;
                    it->second.ssa = 0;
                    it->second.clear();
                    continue;
                }
                cmd_list.set_Pass(it->first);
                cmd_list.apply_lmaterial();
#ifdef USE_DX11
                // The pass's clusters the view kept (da_clusters.h): one indirect draw a vertex buffer.
                if (_priority == 0)
                    da_clusters_draw_pass(*this, cmd_list, it->first, iPass);
#endif

                mapNormalItems& items = it->second;
                items.ssa = 0;

                // The lamp shadow cache's fill (r2_R_lights.cpp, da_smap_cache.h): the statics now, into the light's
                // slot; the swaying trees stay for the live map's call.
                if (o.smap_draw == 1)
                {
                    size_t kept = 0;
                    for (const auto& item : items)
                    {
                        if (item.pVisual->Type == MT_TREE_ST || item.pVisual->Type == MT_TREE_PM)
                        {
                            items[kept++] = item;
                            continue;
                        }
                        const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                        cmd_list.LOD.set_LOD(LOD);
#endif
                        item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
                    }
                    items.resize(kept);
                    continue;
                }

#ifdef XR_NATIVE
                // The registered trees of this pass through ExecuteIndirect; the rest as before.
                if (da_gpu_trees_draw_pass(cmd_list, it->first, items, o.phase))
                {
                    if (items.empty())
                        continue;
                    cmd_list.set_Pass(it->first);
                    cmd_list.apply_lmaterial();
                }
#endif
#ifdef USE_DX11
                if (render_tree_batches(cmd_list, items, fade) && items.empty())
                    continue;
#endif
                for (const auto& item : items)
                {
                    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                    cmd_list.LOD.set_LOD(LOD);
                    const float cover = fade ? fade_cover(item.pVisual) : 1.f;
                    if (cover < 1.f)
                        cmd_list.LOD.set_fade(cover);
#endif
                    // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                    // RCache.hemi.c_update(item.pVisual);

                    item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
#ifdef USE_DX11
                    if (cover < 1.f)
                        cmd_list.LOD.set_fade(1.f);
#endif
                }
                items.clear();

            }
            nrmPasses.clear();
            if (o.smap_draw == 1)
                continue;
            map.clear();
            cachedNormalPasses[_priority][iPass] = nullptr;
            cachedNormalItems[_priority][iPass] = nullptr;
        }
    }
#ifdef USE_DX11
    // The main view's second cluster pass: what the last frame's pyramid hid, against this frame's first draws.
    if (_priority == 0 && o.is_main_pass && o.phase == CRender::PHASE_NORMAL && o.smap_draw == 0)
        da_clusters_second_pass(*this);
    if (_priority == 0)
        da_clusters_view_end(*this);
#endif
    // The fill drew its statics; the moving casters wait for the live map.
    if (o.smap_draw == 1)
    {
        RImplementation.BasicStats.Primitives.End();
        return;
    }

    // **************************************************** MATRIX
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
#if RENDER == R_R4
    if (_priority != 0 || !render_matrix_parallel(fade))
#endif
    {
        ZoneScopedN("dsgraph_render_dynamic");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_dynamic);

        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapMatrixPasses[_priority][iPass];

            map.get_any_p(matPasses);
            if (matPasses.size() > 1)
                std::sort(matPasses.begin(), matPasses.end(), cmp_pass<mapMatrix_T::value_type*>);
            sort_pass_items(matPasses);
            for (const auto& it : matPasses)
            {
                cmd_list.set_Pass(it->first);

                mapMatrixItems& items = it->second;
                items.ssa = 0;

                for (auto& item : items)
                {
                    cmd_list.set_xform_world(item.Matrix);
                    RImplementation.apply_object(cmd_list, item.pObject);
                    cmd_list.apply_lmaterial();
                    if (o.mirror_winding)
                        cmd_list.set_WindingFlip(draws_mirrored(item.pVisual, item.Matrix));

                    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                    cmd_list.LOD.set_LOD(LOD);
                    const float cover = fade ? fade_cover(item.pVisual) : 1.f;
                    if (cover < 1.f)
                        cmd_list.LOD.set_fade(cover);
#endif
                    // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                    // RCache.hemi.c_update(item.pVisual);

                    item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
#ifdef USE_DX11
                    if (cover < 1.f)
                        cmd_list.LOD.set_fade(1.f);
#endif
                }
                items.clear();
            }
            matPasses.clear();
            map.clear();
            cachedMatrixPasses[_priority][iPass] = nullptr;
            cachedMatrixItems[_priority][iPass] = nullptr;
        }
        if (o.mirror_winding)
            cmd_list.set_WindingFlip(false);
    }

#ifdef XR_NATIVE
    // The GPU-driven trees of this view after every CPU-drawn object of priority 0: the depth already
    // holds the occluders when the crowns rasterise (and when the main view's Hi-Z is built).
    if (_priority == 0 && gd_view)
        da_gpu_trees_view_render(*this);
#endif

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
#if RENDER == R_R4
    Fmatrix vp_nojit_old, vp_prev_old; // the motion vectors' view-projections of the world
#endif
    R_dsgraph_structure& dsgraph;
    CBackend& cmd_list;
    bool mirror_winding_old;

public:
    explicit hud_transform_helper(R_dsgraph_structure& dsgraph_in)
        : dsgraph(dsgraph_in), cmd_list(dsgraph_in.cmd_list), mirror_winding_old(dsgraph_in.o.mirror_winding)
    {
        // Change projection. Both carry the frame's jitter (da_temporal.h): the HUD is part of
        // the scene the temporal pass resolves.
#if RENDER == R_R4
        Pold = da_jitter_project(Device.mProject);
#else
        Pold  = Device.mProject;
#endif

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
#if RENDER == R_R4
        cmd_list.set_xform_project(da_jitter_project(prj_new));
        // The motion vectors of the HUD are measured in its own projection, current against its
        // previous frame's; the world's pair would compare two different cameras.
        vp_nojit_old = cmd_list.xforms.m_vp_nojit;
        vp_prev_old = cmd_list.xforms.m_vp_prev;
        Fmatrix hud_vp_nojit, hud_vp_prev;
        da_hud_vp(prj_new, hud_vp_nojit, hud_vp_prev);
        cmd_list.xforms.set_VP_motion(hud_vp_nojit, hud_vp_prev);
#else
        cmd_list.set_xform_project(prj_new);
#endif

        RImplementation.rmNear(cmd_list);

        // The left-handed hud's items have mirrored matrices: render_item flips their front face, this context only.
        dsgraph.o.mirror_winding = true;
    }

    ~hud_transform_helper()
    {
        RImplementation.rmNormal(cmd_list);

        // Restore projection
        cmd_list.set_xform_project(Pold);
#if RENDER == R_R4
        cmd_list.xforms.set_VP_motion(vp_nojit_old, vp_prev_old);
#endif
        dsgraph.o.mirror_winding = mirror_winding_old;
    }
};

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
    // Each material keeps its own cull mode; a mirrored matrix only swaps which winding faces front.
    if (dsgraph.o.mirror_winding)
        dsgraph.cmd_list.set_WindingFlip(draws_mirrored(V, item.second.Matrix));
    //--#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
    //RCache.hemi.c_update(V);
    V->Render(dsgraph.cmd_list, calcLOD(item.first, V->vis.sphere.R), dsgraph.o.phase == CRender::PHASE_SMAP);
}

// The passes' own front face again after a list whose items may have flipped it.
ICF void restore_winding(u32 context_id)
{
    auto& dsgraph = RImplementation.get_context(context_id);
    if (dsgraph.o.mirror_winding)
        dsgraph.cmd_list.set_WindingFlip(false);
}

template<class T>
ICF void sort_front_to_back_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_left_right(context_id, render_item);
    vec.clear();
    restore_winding(context_id);
}

template<class T>
ICF void sort_back_to_front_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_right_left(context_id, render_item);
    vec.clear();
    restore_winding(context_id);
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void R_dsgraph_structure::render_hud()
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud);

    if (!mapHUD.empty())
    {
        hud_transform_helper helper{ *this };
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

    hud_transform_helper helper{ *this };

#if RENDER != R_R1
    // Targets, use accumulator for temporary storage
    const ref_rt rt_null;
    cmd_list.set_RT(0, 1);
    cmd_list.set_RT(0, 2);
    auto zb = RImplementation.Target->rt_Base_Depth;

#if (RENDER == R_R3) || (RENDER == R_R4) || (RENDER==R_GL)
    // The scene depth: separate from the base depth under MSAA and while an upscaler runs.
    if (RImplementation.o.msaa || RImplementation.Target->rt_MSAADepth != RImplementation.Target->rt_Base_Depth)
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
        hud_transform_helper helper{ *this };
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
        hud_transform_helper helper{ *this };
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

    // The hud's distortion over the world's, in the depth range and projection the hud was drawn with.
    if (!mapHUDDistort.empty())
    {
        hud_transform_helper helper{ *this };
        sort_back_to_front_render_and_clean(context_id, mapHUDDistort);
    }
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
