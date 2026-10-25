// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The graph partitioner of the cluster hierarchy (docs/dead-air/CLUSTERS.md): splits a weighted graph - triangles linked
// by their shared edges, or clusters by their shared borders - into parts of a bounded size cutting as little edge
// weight as it can. Recursive bisection as the multilevel partitioners do it (Karypis and Kumar, 1998): each bisection
// coarsens the graph by heavy-edge matching, splits the coarsest by greedy growing and refines every level back up by
// Fiduccia-Mattheyses boundary moves within its balance. The split's targets follow the production cluster builders: a
// part count rounded from the size halfway between the bounds, each side its share of the parts, held to a tenth of a
// percent where the parts are tight (a loose fifth where they are not). Pure graph code with no engine types.
#pragma once

#include <cstdint>
#include <vector>

namespace da_mesh
{
// An undirected graph in compressed rows: node i's neighbours adj[xadj[i] .. xadj[i + 1]), each with its weight. Every
// edge is listed from both ends with the same weight.
struct Graph
{
    std::vector<uint32_t> xadj, adj;
    std::vector<int32_t> wgt;
    uint32_t nodes() const { return xadj.empty() ? 0 : uint32_t(xadj.size() - 1); }
};

// The parts as ranges of `order`: part i is order[ranges[i] .. ranges[i + 1]).
struct Partition
{
    std::vector<uint32_t> order;
    std::vector<uint32_t> ranges;
    uint32_t parts() const { return ranges.empty() ? 0 : uint32_t(ranges.size() - 1); }
};

// Parts of at most max_size nodes, about (min_size + max_size) / 2 each.
Partition partition(const Graph& g, uint32_t min_size, uint32_t max_size);

// Builds a graph from directed edge lists (a -> b with weight w, both directions or one: they are merged and summed).
Graph make_graph(uint32_t nodes, const std::vector<uint64_t>& edges, const std::vector<int32_t>& weights);
} // namespace da_mesh
