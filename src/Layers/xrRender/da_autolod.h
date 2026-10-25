// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The automatic levels of detail of the models drawn whole (docs/dead-air/CLUSTERS.md, "Automatic levels of detail"):
// a skinned or rigid model mesh that ships without its own (a progressive mesh has its sliding windows) gets a chain
// the production way - each level reduced from the full one, not from the last, to a half, a quarter and an eighth of its
// triangles by the quadric reducer (da_mesh_simplify.h: the reducer's weights, a normal 16, a tangent frame 0.1, a texture
// set 0.5 over its range, the border lines 512 times their length, the degree penalty 100), the bones' regions kept apart
// - and its screen sizes from each level's deviation (the size at which the deviation is the pixel error's share of a
// 1920 x 1080 view of 90 degrees). A model takes the coarsest level whose size its own still reaches; a change crosses
// over a dithered half second in the G-buffer (the two levels' pixels complementary), the shadow maps take the camera's
// level. Built on worker threads after the model loads and kept on disk; only the indices are new (every level reuses the
// model's vertices, bones and weights).
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
struct DaAutoLod;

// A model mesh's data for its chain: the bind pose (three floats a vertex), the attributes (the normal, tangent and
// binormal, the texture coordinates: eleven floats a vertex), each vertex's strongest bone, the triangles, the drawing
// geometry (its declaration and vertex buffer, borrowed for the levels' own geometry).
struct DaAutoLodSource
{
    const float* positions{};
    const float* attributes{};
    const u16* bones{};
    u32 vertex_count{};
    const u16* indices{};
    u32 index_count{};
    const SGeometry* geom{};
    float radius{};
};

// A new chain for the mesh (one reference, the caller's), or nullptr for a mesh too small to reduce.
DaAutoLod* da_autolod_request(const DaAutoLodSource& source);
void da_autolod_addref(DaAutoLod* lod);
void da_autolod_release(DaAutoLod* lod);
// The frame's start: finished chains to the GPU.
void da_autolod_frame_begin();

// A model's choice and its crossfade, per instance.
struct DaAutoLodState
{
    u8 current{};
    u8 previous{};
    float since{-1.f};
};
// What to draw: one level, or two crossfading (the old one keeps the pixels under 1 - t, the new one the rest).
struct DaAutoLodDraw
{
    const ref_geom* geom{};
    u32 first[2]{}, tris[2]{};
    u32 count{};
    float t{1.f};
};
// The draw for the instance with the world transform set (cmd.xforms): false while the chain is not on the GPU. The main
// scene's G-buffer pass moves the choice and its crossfade; every other pass takes the current level.
bool da_autolod_choose(DaAutoLod* lod, CBackend& cmd, const Fsphere& sphere, DaAutoLodState& state, DaAutoLodDraw& out);
void da_autolod_stats();
void da_autolod_destroy();
} // namespace xray::render::RENDER_NAMESPACE
