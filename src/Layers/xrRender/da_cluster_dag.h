// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy of a mesh (docs/dead-air/CLUSTERS.md): its triangles cut into clusters of at most 128 (and 256
// vertices) by the graph partitioner, then level by level the clusters grouped by 8 to 32 along their shared borders,
// each group's triangles simplified to about half with the borders it shares with other groups locked, and cut again
// into the next level's clusters - a directed acyclic graph of levels of detail whose cut, chosen per cluster by its
// error on the screen, is free of cracks: a group's children and parents share one error and one bounding sphere, so
// they switch together. The structure and its constants are the shipped engines' cluster pipelines' (Karis et al.,
// SIGGRAPH 2021, the virtualized geometry talk): 128-triangle clusters, groups of 8-32, two parents' worth of triangles a
// parent (126 aimed at), the shared-edge weights of the partition graphs, locality links between disconnected pieces.
// Every cluster's vertices are the input's (da_mesh_simplify.h). Pure geometry with no engine types.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace da_mesh
{
// What an attribute is, for its weight in the simplifier: the production weights against a mean triangle a quarter of a
// unit across - a normal 1, a tangent frame's 1/16, a colour's 1/16, a texture coordinate (the u and v of one set
// adjacent) one over 128 of the set's mean triangle in texture space.
enum AttrKind : uint8_t
{
    akNormal,
    akTangent,
    akColor,
    akUV,
    akIgnore,
};

struct DagInput
{
    const float* positions{}; // three floats a vertex
    uint32_t vertex_count{};
    const float* attributes{}; // attr_count floats a vertex
    uint32_t attr_count{};
    const uint8_t* attr_kinds{}; // AttrKind per attribute
    const uint32_t* indices{}; // three a triangle
    uint32_t tri_count{};
    // Per vertex: 1 where its position may never move (a border shared with another mesh); none when null.
    const uint8_t* locked{};
    // Runs fn(i) for i in [0, count), on worker threads if given (the groups of a level are independent).
    std::function<void(uint32_t count, const std::function<void(uint32_t)>& fn)> parallel;
};

struct DagCluster
{
    uint32_t vert_first{}, vert_count{}; // its vertex table (input vertex ids) in DagOutput::verts
    uint32_t tri_first{}, tri_count{}; // its triangles in DagOutput::tris: three indices into the table each
    float box_min[3]{}, box_max[3]{};
    // Its own level of detail: the error of the simplification that made it and the sphere that bounds every cluster
    // of its generating group's children (a first-level cluster: 0 and its own sphere).
    float lod_sphere[4]{};
    float lod_error{};
    // The group it is a child of: the error its parents carry and their sphere (1e10 for a root: always too coarse).
    float parent_sphere[4]{};
    float parent_error{};
    uint32_t level{};
};

struct DagOutput
{
    std::vector<DagCluster> clusters;
    std::vector<uint32_t> verts;
    std::vector<uint8_t> tris;
    uint32_t levels{};
    uint32_t groups{};
    // Groups whose simplification stopped short (all borders locked): their children stayed roots.
    uint32_t stuck{};
};

void build_dag(const DagInput& in, DagOutput& out);

// The production constants.
constexpr uint32_t kClusterTris = 128;
constexpr uint32_t kClusterVerts = 256;
constexpr uint32_t kGroupMin = 8;
constexpr uint32_t kGroupMax = 32;
constexpr float kRootError = 1e10f;
} // namespace da_mesh
