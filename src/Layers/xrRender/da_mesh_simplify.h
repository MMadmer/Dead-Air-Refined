// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The mesh simplifier of the cluster hierarchy and the automatic levels of detail (docs/dead-air/CLUSTERS.md): greedy
// edge collapse ordered by wedge-attribute quadrics (Garland and Heckbert 1997; Hoppe 1999, "New quadric metric for
// simplifying meshes with appearance attributes"; Hoppe and Marschner 2000, the compact form), area weighted, with
// line constraints on open borders and seams, locked positions, and the degree, inversion and lock penalties of the
// production simplifiers. Every surviving vertex is one of the input's: a collapse moves one end onto the other, so a
// level of detail reuses its mesh's own vertex buffer - the level's vertices carry baked light (the hemisphere term, the
// lightmap's coordinates) that a moved vertex would have to interpolate - and only its indices are new. Pure geometry with
// no engine types.
#pragma once

#include <cstdint>
#include <vector>

namespace da_mesh
{
struct SimplifyInput
{
    // Vertex positions, three floats each, and attributes, attr_count floats each (none when 0).
    const float* positions{};
    const float* attributes{};
    uint32_t vertex_count{};
    uint32_t attr_count{};
    // The attributes' weights in the quadric (attr_count of them), against positions scaled so the mean triangle is
    // a quarter of a unit across; a weight of 0 leaves the attribute out of the cost.
    const float* attr_weights{};
    // Per triangle: an id whose change across an edge makes it a seam (the material, a UV mirroring bit); none when null.
    const uint32_t* tri_ids{};
    // Per vertex: 1 where its position may not move (a group's border, a mesh's border shared with a neighbour); none
    // when null. Any vertex at a locked position locks it.
    const uint8_t* locked{};
};

struct SimplifyOptions
{
    // Stops once the triangles are at or under this count, or when no collapse is left.
    uint32_t target_tris{};
    // The line constraint of an open border (a seam's sides take half each), times the edge's length.
    float edge_weight{2.f};
    // Added per triangle over the degree limit at the merged position.
    float degree_penalty{0.5f};
    uint32_t degree_limit{24};
    // Error past which a collapse is not taken (in the input's units): none by default.
    float max_error{-1.f};
    // The error capped by the region's surface area, per wedge and in all.
    bool limit_error_to_area{true};
    // The positions' scale for the quadrics and the penalties: 0 the cluster builder's (the mean triangle a quarter of a
    // unit across, by a power of two), else this factor (the reducer of the levels of detail: 100, metres to the
    // centimetres its penalties were tuned in).
    float position_scale{0.f};
};

struct SimplifyResult
{
    // The deviation of the input's vertices from the simplified surface, in the input's units: for every position a
    // collapse removed, its distance to the triangles round the position it went into (or to that position, where they
    // are all gone) - the one-sided Hausdorff distance over a vertex's own neighbourhood, which bounds it from above.
    float deviation{};
    // The largest collapse cost taken (the quadric's, unscaled), for diagnostics.
    float max_cost{};
    uint32_t tris{};
};

// Simplifies `indices` (three per triangle, into the input's vertices) in place toward the target.
SimplifyResult simplify(const SimplifyInput& in, std::vector<uint32_t>& indices, const SimplifyOptions& opt);
} // namespace da_mesh
