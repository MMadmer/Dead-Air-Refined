// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The level's static geometry by clusters (docs/dead-air/CLUSTERS.md): every level visual's triangles in a cluster
// hierarchy (da_cluster_dag.h), built once on worker threads after the level loads and kept on disk; each view - the
// main one, the sun's cascades, the lamps' faces - culls the clusters of the visuals its portal walk found on the GPU
// (da_cluster_cull.cs: the cut by the error on the screen, the frustum, in the main view the depth pyramid in two passes)
// and draws each shader pass's kept clusters with one indirect draw a vertex buffer over the level's own vertices
// (da_cluster_args.cs, da_cluster_expand.cs). The visuals the view leaves on its lists - a fading one, a translucent or
// decal pass, a level without its hierarchy yet - draw as they always did.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
struct R_dsgraph_structure;
class CBackend;
struct SPass;

// A view's projection for its cut: its view and projection (orthographic for the sun's cascades), its viewport in pixels,
// the error on the screen a cluster may carry, and whether it is the main view (the depth pyramid's two passes).
struct DaClusterView
{
    Fmatrix view;
    Fmatrix project;
    float width{};
    float height{};
    float threshold_px{1.f};
    bool main{};
    bool shadow{};
    // Which of r__clusters_views it is: 1 the main view, 2 a sun cascade, 4 a lamp face.
    u32 kind{1};
};

// The level's visuals are loaded (r2_loader.cpp): their hierarchy from the disk cache or built on worker threads.
void da_clusters_level_load();
void da_clusters_level_unload();
// The frame's start before any view walks (CRender::Calculate: the sun's cascades may cull on workers from there): a
// finished hierarchy goes to the GPU, the frame's pools start empty.
void da_clusters_frame_prepare();
// The G-buffer stage's start, on the immediate context ahead of every view's culls: the GPU's counters cleared.
void da_clusters_frame_begin(CBackend& cmd);
// The views' descriptions: the main view's (the frame's camera), a sun cascade's, a lamp face's.
DaClusterView da_clusters_main_view();
DaClusterView da_clusters_shadow_view(const Fmatrix& view, const Fmatrix& project, float size, u32 kind);
// A view's cull on its context, after its caster or visible build and before its targets are set: the cluster visuals
// leave its lists for the GPU.
void da_clusters_cull(R_dsgraph_structure& g, const DaClusterView& view);
// render_graph: the view's cluster draws of the pass just set, then the view done with them.
void da_clusters_draw_pass(R_dsgraph_structure& g, SPass* pass, u32 pass_index);
void da_clusters_view_end(R_dsgraph_structure& g);
// The main view's second pass, after its first static draws: the clusters the last frame's pyramid hid, against the
// pyramid of this frame's own first draws. Leaves the G-buffer's targets set again.
void da_clusters_second_pass(R_dsgraph_structure& g);
bool da_clusters_ready();
void da_clusters_stats();
void da_clusters_destroy();
} // namespace xray::render::RENDER_NAMESPACE
