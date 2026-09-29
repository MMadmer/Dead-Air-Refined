// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// GPU-driven vegetation (docs/dead-air/dx12-features/gpudriven.md): the level's tree instances in a
// GPU registry, culled and drawn through ExecuteIndirect per view. Inert on R4 and with
// r__gpu_driven 0.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class CSector;
class dxRender_Visual;
class FTreeVisual;
struct SPass;
struct R_dsgraph_structure;
namespace R_dsgraph
{
struct mapNormalItems;
}

// The registry is built and in use this session (r__caps gd=).
bool da_gpu_trees_live();
// level_Load after LoadOverlayVisuals, level_Unload before the sectors go.
void da_gpu_trees_level_load();
void da_gpu_trees_level_unload();
// Top of CRender::Calculate (main thread, before any graph build): applies a changed r__gpu_driven (or a
// changed auto decision, da_offload.h) and zeroes the frame's CPU stats slot.
void da_gpu_trees_apply_switch();
// Top of CRender::Render after GpuTimers.FrameBegin(): the wind state of this frame.
void da_gpu_trees_frame_begin(CBackend& cmd_list);
// The ray tracing lane classified its visuals: refresh the per-instance rt_static bits.
void da_gpu_trees_refresh_flags();

// render_graph: the registry draws this tree (its wind is integrated at the frame's start).
bool da_gpu_trees_draws(const FTreeVisual* tree);
// render_graph, after set_Pass(pass): draws the registered trees among the pass's items and takes
// them out of items. True when it drew (the pass has to be set again for the rest). The views whose
// build culled the nodes on the CPU (r__gpu_driven 2, local light faces, a view list that did not fit)
// draw this way.
bool da_gpu_trees_draw_pass(CBackend& cmd_list, SPass* pass, R_dsgraph::mapNormalItems& items, u32 phase);

// How a view's build treats the nodes the registry moved out of the sector hierarchies.
enum class DaGpuTreeWalk : u8
{
    cpu, // every node through add_static, as the hierarchy had it (draws through da_gpu_trees_draw_pass)
    skip, // the GPU tests the nodes (shadow views)
    main, // the main view: the CPU decides nodes (R_dsgraph_structure::gd_node_main), the GPU instances
};
// build_subspace_static after the portal traversal: sets up the view (R_dsgraph_structure::gd_view).
DaGpuTreeWalk da_gpu_trees_view_begin(R_dsgraph_structure& graph);
// The main view: the walk kept this node's children. False when it was listed already.
bool da_gpu_trees_view_node(R_dsgraph_structure& graph, u32 node);
bool da_gpu_trees_view_listed(const R_dsgraph_structure& graph, u32 node);
// build_subspace_static at its end: the view's list is complete.
void da_gpu_trees_view_end(R_dsgraph_structure& graph);
// render_graph(0) after its CPU passes: the view's cull and ExecuteIndirects (once per build).
void da_gpu_trees_view_render(R_dsgraph_structure& graph);

// A sector's vegetation nodes while the registry draws them: they left their hierarchies (which every
// render context shares), so each context's walk of the sector visits them after its hierarchy. The
// root's tests and each node's own gate them, and so does the value filter of the hierarchies between
// (ancestors); their frustum and occlusion tests follow from the node's box, which lies inside theirs.
struct DaGpuTreeNode
{
    dxRender_Visual* visual;
    u32 node; // registry index
    u32 first_ancestor, ancestor_count;
};
struct DaGpuTreeSector
{
    xr_vector<DaGpuTreeNode> nodes;
    xr_vector<dxRender_Visual*> ancestors;
};
// Null while nothing of this sector (CSector::unique_id) is on the GPU path.
const DaGpuTreeSector* da_gpu_trees_sector(u32 sector_id);

// Every node the registry moved out of this sector's hierarchies.
using DaGpuTreeNodeFn = void (*)(dxRender_Visual* node, void* user);
void da_gpu_trees_for_each_node(CSector* sector, DaGpuTreeNodeFn fn, void* user);

template <typename Fn>
void da_gpu_trees_for_each_node(CSector* sector, Fn&& fn)
{
    using callable = std::remove_reference_t<Fn>;
    da_gpu_trees_for_each_node(sector, [](dxRender_Visual* node, void* user) { (*static_cast<callable*>(user))(node); },
        const_cast<void*>(static_cast<const void*>(&fn)));
}
} // namespace xray::render::RENDER_NAMESPACE
