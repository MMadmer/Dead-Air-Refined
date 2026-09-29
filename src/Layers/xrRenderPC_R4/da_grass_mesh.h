// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Mesh-shader grass on DirectX 12 (docs/dead-air/dx12-features/mesh.md): the frame's hooks into the
// detail manager's mesh path (DetailManager_MS.cpp). The R4 module and GPUs without mesh shaders
// keep the vertex path.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

// Mesh grass will run (r__grass_mesh on and the path available when the preset replays: the mesh columns).
bool da_grass_mesh_expected();
// The detail blender compiles the mesh elements (2 wave, 3 still, 4/5 their shadow variants).
bool da_grass_mesh_blender();
// This frame draws grass through the mesh path (the parallel G-buffer grass then stays idle).
bool da_grass_mesh_frame();
// After the q_sync_point wait: the rows the calc task produced go to the GPU.
void da_grass_mesh_frame_prepare(CBackend& cmd_list);
// r__dbg_grass_ab: the vertex and the mesh path drawn over the same state and compared.
void da_grass_ab_gbuffer_begin();
void da_grass_ab_gbuffer_end();
void da_grass_ab_cascade_begin(u32 cascade, CBackend& cmd_list);
void da_grass_ab_cascade_end(u32 cascade, CBackend& cmd_list);
// r__grass_stats: the counters of the last frame and, a few frames later, a check of the GPU rows.
void da_grass_mesh_stats();
} // namespace xray::render::RENDER_NAMESPACE
