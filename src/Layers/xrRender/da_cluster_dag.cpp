// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy of a mesh (da_cluster_dag.h).
#include "da_cluster_dag.h"
#include "da_graph_partition.h"
#include "da_mesh_simplify.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numeric>
#include <unordered_map>

namespace da_mesh
{
namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;

// The partition graphs' weights: a shared edge between two triangles; between two clusters per shared edge (less
// between siblings of one generating group, so the next level's groups cross the last level's borders) plus a base; a
// locality link between disconnected pieces close in space.
constexpr i32 kTriEdgeWeight = 4 * 65;
constexpr i32 kClusterEdgeWeight = 16, kSiblingEdgeWeight = 12, kClusterBaseWeight = 4;
constexpr i32 kLocalityWeight = 1;
// A parent aims at two triangles under the cluster size; failing to cut into the parents it was given, it aims lower by
// two at a time down to half the cluster size.
constexpr u32 kParentTarget = kClusterTris - 2;
// A group simplified to more than this share of its triangles is stuck (its borders locked all round): its children
// stay roots instead of repeating themselves as their own parents.
constexpr float kStuckShare = 0.85f;

struct Sphere
{
    float c[3]{};
    float r{};
};

// The smallest sphere about the first that takes each next one in whole: every sphere given stays inside.
Sphere bound_spheres(const Sphere* s, size_t count)
{
    Sphere out = s[0];
    for (size_t i = 1; i < count; ++i)
    {
        const float dx = s[i].c[0] - out.c[0], dy = s[i].c[1] - out.c[1], dz = s[i].c[2] - out.c[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d + s[i].r <= out.r)
            continue;
        if (d + out.r <= s[i].r)
        {
            out = s[i];
            continue;
        }
        const float r = 0.5f * (out.r + d + s[i].r);
        const float k = (r - out.r) / d;
        out.c[0] += dx * k;
        out.c[1] += dy * k;
        out.c[2] += dz * k;
        out.r = r;
    }
    // A hair over, against rounding: a child's sphere poking out of its parents' would break the cut's monotony.
    out.r *= 1.0001f;
    return out;
}

u32 morton3(u32 x)
{
    x &= 0x3ff;
    x = (x | (x << 16)) & 0x030000ff;
    x = (x | (x << 8)) & 0x0300f00f;
    x = (x | (x << 4)) & 0x030c30c3;
    x = (x | (x << 2)) & 0x09249249;
    return x;
}

struct DisjointSet
{
    std::vector<u32> p;
    explicit DisjointSet(u32 n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    u32 find(u32 x)
    {
        while (p[x] != x)
        {
            p[x] = p[p[x]];
            x = p[x];
        }
        return x;
    }
    void unite(u32 a, u32 b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            p[std::max(a, b)] = std::min(a, b);
    }
};

// Locality links: elements sorted along a Morton curve of their centres; one whose island (its connected piece) takes
// fewer than 128 places of the order links to the five nearest of other islands within 16 steps each way, skipping past
// runs of its own.
void locality_links(const std::vector<float>& centers, DisjointSet& islands, std::vector<u64>& edges, std::vector<i32>& weights)
{
    const u32 n = u32(centers.size() / 3);
    if (n < 2)
        return;
    float lo[3] = {centers[0], centers[1], centers[2]}, hi[3] = {lo[0], lo[1], lo[2]};
    for (u32 i = 0; i < n; ++i)
        for (u32 k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], centers[i * 3 + k]);
            hi[k] = std::max(hi[k], centers[i * 3 + k]);
        }
    const float extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-12f});
    std::vector<u32> key(n), order(n);
    for (u32 i = 0; i < n; ++i)
    {
        u32 m = 0;
        for (u32 k = 0; k < 3; ++k)
            m |= morton3(u32((centers[i * 3 + k] - lo[k]) / extent * 1023.f)) << k;
        key[i] = m;
    }
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return key[a] < key[b]; });
    std::vector<u32> island(n), run_begin(n), run_end(n);
    for (u32 i = 0; i < n; ++i)
        island[i] = islands.find(order[i]);
    for (u32 i = 0; i < n;)
    {
        u32 j = i;
        while (j < n && island[j] == island[i])
            ++j;
        for (u32 k = i; k < j; ++k)
        {
            run_begin[k] = i;
            run_end[k] = j;
        }
        i = j;
    }
    for (u32 i = 0; i < n; ++i)
    {
        if (run_end[i] - run_begin[i] >= 128)
            continue;
        const u32 self = order[i];
        u32 best[5];
        float best_d[5];
        std::fill(best, best + 5, ~0u);
        std::fill(best_d, best_d + 5, 3.4e38f);
        for (int dir = 0; dir < 2; ++dir)
        {
            i32 a = i32(i);
            for (u32 it = 0; it < 16; ++it)
            {
                a += dir ? 1 : -1;
                if (a < 0 || a >= i32(n))
                    break;
                if (island[a] == island[i])
                {
                    a = dir ? i32(run_end[a]) - 1 : i32(run_begin[a]);
                    continue;
                }
                const u32 other = order[a];
                const float dx = centers[other * 3] - centers[self * 3], dy = centers[other * 3 + 1] - centers[self * 3 + 1],
                            dz = centers[other * 3 + 2] - centers[self * 3 + 2];
                float d = dx * dx + dy * dy + dz * dz;
                u32 o = other;
                for (u32 k = 0; k < 5; ++k)
                    if (d < best_d[k])
                    {
                        std::swap(d, best_d[k]);
                        std::swap(o, best[k]);
                    }
            }
        }
        for (u32 k = 0; k < 5; ++k)
            if (best[k] != ~0u)
            {
                edges.push_back((u64(self) << 32) | best[k]);
                weights.push_back(kLocalityWeight);
            }
    }
}

struct Work
{
    std::vector<u32> tris; // input vertex ids, three a triangle
    float box_min[3]{}, box_max[3]{};
    Sphere lod_sphere;
    float lod_error{};
    Sphere parent_sphere;
    float parent_error{kRootError};
    u32 level{};
    u32 generating_group{~0u};
    bool has_parent{};
};

class Builder
{
public:
    Builder(const DagInput& in_, DagOutput& out_) : in(in_), out(out_) {}
    void run();

private:
    const DagInput& in;
    DagOutput& out;
    std::vector<u32> pos_of;
    std::vector<float> pos;
    std::vector<Work> clusters;
    std::mutex mutex;

    const float* P(u32 v) const { return &pos[size_t(pos_of[v]) * 3]; }
    void weld();
    std::vector<std::vector<u32>> clusterize(const std::vector<u32>& tris);
    void split_by_verts(const std::vector<u32>& tris, std::vector<u32>& part, std::vector<std::vector<u32>>& parts);
    Work make_cluster(std::vector<u32> tris) const;
    std::vector<std::vector<u32>> group(const std::vector<u32>& level);
    void reduce(const std::vector<u32>& children, u32 group_id, const std::vector<uint8_t>& level_locked,
        std::vector<Work>& parents, bool& stuck);
};

void Builder::weld()
{
    struct Key
    {
        u32 x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct Hash
    {
        size_t operator()(const Key& k) const { return (size_t(k.x) * 73856093u) ^ (size_t(k.y) * 19349663u) ^ (size_t(k.z) * 83492791u); }
    };
    std::unordered_map<Key, u32, Hash> map;
    map.reserve(in.vertex_count);
    pos_of.resize(in.vertex_count);
    for (u32 v = 0; v < in.vertex_count; ++v)
    {
        const float* p = in.positions + size_t(v) * 3;
        // Minus zero is zero: a position's bits are compared.
        const float x = p[0] + 0.f, y = p[1] + 0.f, z = p[2] + 0.f;
        Key k;
        std::memcpy(&k.x, &x, 4);
        std::memcpy(&k.y, &y, 4);
        std::memcpy(&k.z, &z, 4);
        const auto [it, fresh] = map.emplace(k, u32(pos.size() / 3));
        if (fresh)
            pos.insert(pos.end(), {p[0], p[1], p[2]});
        pos_of[v] = it->second;
    }
}

// Cuts triangles into clusters of at most 128 triangles and 256 vertices: the triangle graph by shared edges plus the
// locality links, partitioned to 124..128 triangles a part.
std::vector<std::vector<u32>> Builder::clusterize(const std::vector<u32>& tris)
{
    const u32 count = u32(tris.size() / 3);
    std::vector<std::vector<u32>> parts;
    if (!count)
        return parts;
    std::vector<u64> edges;
    std::vector<i32> weights;
    DisjointSet islands(count);
    {
        // Edges keyed by their positions (either way round), each with the triangles on it.
        std::vector<std::pair<u64, u32>> keyed;
        keyed.reserve(size_t(count) * 3);
        for (u32 t = 0; t < count; ++t)
            for (u32 c = 0; c < 3; ++c)
            {
                u32 a = pos_of[tris[t * 3 + c]], b = pos_of[tris[t * 3 + (c + 1) % 3]];
                if (a == b)
                    continue;
                if (a > b)
                    std::swap(a, b);
                keyed.push_back({(u64(a) << 32) | b, t});
            }
        std::sort(keyed.begin(), keyed.end());
        for (size_t i = 0; i < keyed.size();)
        {
            size_t j = i;
            while (j < keyed.size() && keyed[j].first == keyed[i].first)
                ++j;
            for (size_t a = i; a < j; ++a)
                for (size_t b = a + 1; b < j; ++b)
                    if (keyed[a].second != keyed[b].second)
                    {
                        edges.push_back((u64(keyed[a].second) << 32) | keyed[b].second);
                        weights.push_back(kTriEdgeWeight);
                        islands.unite(keyed[a].second, keyed[b].second);
                    }
            i = j;
        }
    }
    std::vector<float> centers(size_t(count) * 3);
    for (u32 t = 0; t < count; ++t)
        for (u32 k = 0; k < 3; ++k)
            centers[t * 3 + k] = (P(tris[t * 3])[k] + P(tris[t * 3 + 1])[k] + P(tris[t * 3 + 2])[k]) * (1.f / 3.f);
    locality_links(centers, islands, edges, weights);
    const Graph g = make_graph(count, edges, weights);
    const Partition p = partition(g, kClusterTris - 4, kClusterTris);
    for (u32 i = 0; i < p.parts(); ++i)
    {
        std::vector<u32> part(p.order.begin() + p.ranges[i], p.order.begin() + p.ranges[i + 1]);
        split_by_verts(tris, part, parts);
    }
    return parts;
}

// A part over 256 vertices (a soup of loose triangles) halves along its order until each fits.
void Builder::split_by_verts(const std::vector<u32>& tris, std::vector<u32>& part, std::vector<std::vector<u32>>& parts)
{
    std::vector<u32> verts;
    verts.reserve(part.size() * 3);
    for (u32 t : part)
        for (u32 c = 0; c < 3; ++c)
            verts.push_back(tris[t * 3 + c]);
    std::sort(verts.begin(), verts.end());
    const size_t unique = std::unique(verts.begin(), verts.end()) - verts.begin();
    if (unique <= kClusterVerts || part.size() < 2)
    {
        std::vector<u32> out_tris;
        out_tris.reserve(part.size() * 3);
        for (u32 t : part)
            out_tris.insert(out_tris.end(), {tris[t * 3], tris[t * 3 + 1], tris[t * 3 + 2]});
        parts.push_back(std::move(out_tris));
        return;
    }
    std::vector<u32> a(part.begin(), part.begin() + part.size() / 2), b(part.begin() + part.size() / 2, part.end());
    split_by_verts(tris, a, parts);
    split_by_verts(tris, b, parts);
}

Work Builder::make_cluster(std::vector<u32> tris) const
{
    Work w;
    w.tris = std::move(tris);
    for (u32 k = 0; k < 3; ++k)
    {
        w.box_min[k] = 3.4e38f;
        w.box_max[k] = -3.4e38f;
    }
    for (u32 v : w.tris)
        for (u32 k = 0; k < 3; ++k)
        {
            w.box_min[k] = std::min(w.box_min[k], P(v)[k]);
            w.box_max[k] = std::max(w.box_max[k], P(v)[k]);
        }
    Sphere s;
    for (u32 k = 0; k < 3; ++k)
        s.c[k] = 0.5f * (w.box_min[k] + w.box_max[k]);
    float r2 = 0.f;
    for (u32 v : w.tris)
    {
        const float dx = P(v)[0] - s.c[0], dy = P(v)[1] - s.c[1], dz = P(v)[2] - s.c[2];
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    s.r = std::sqrt(r2) * 1.0001f;
    w.lod_sphere = s;
    w.parent_sphere = s;
    return w;
}

// The level's clusters grouped by 8..32 along their shared borders (all in one group when they are 32 or fewer).
std::vector<std::vector<u32>> Builder::group(const std::vector<u32>& level)
{
    std::vector<std::vector<u32>> groups;
    const u32 count = u32(level.size());
    if (count <= kGroupMax)
    {
        groups.push_back(level);
        return groups;
    }
    std::vector<std::pair<u64, u32>> keyed;
    for (u32 i = 0; i < count; ++i)
    {
        const Work& w = clusters[level[i]];
        for (size_t t = 0; t < w.tris.size(); t += 3)
            for (u32 c = 0; c < 3; ++c)
            {
                u32 a = pos_of[w.tris[t + c]], b = pos_of[w.tris[t + (c + 1) % 3]];
                if (a == b)
                    continue;
                if (a > b)
                    std::swap(a, b);
                keyed.push_back({(u64(a) << 32) | b, i});
            }
    }
    std::sort(keyed.begin(), keyed.end());
    keyed.erase(std::unique(keyed.begin(), keyed.end()), keyed.end());
    // Shared edges per pair of clusters.
    std::unordered_map<u64, u32> shared;
    DisjointSet islands(count);
    for (size_t i = 0; i < keyed.size();)
    {
        size_t j = i;
        while (j < keyed.size() && keyed[j].first == keyed[i].first)
            ++j;
        for (size_t a = i; a < j; ++a)
            for (size_t b = a + 1; b < j; ++b)
            {
                const u32 x = std::min(keyed[a].second, keyed[b].second), y = std::max(keyed[a].second, keyed[b].second);
                ++shared[(u64(x) << 32) | y];
                islands.unite(x, y);
            }
        i = j;
    }
    std::vector<u64> edges;
    std::vector<i32> weights;
    edges.reserve(shared.size());
    for (const auto& [key, n] : shared)
    {
        const Work& a = clusters[level[u32(key >> 32)]];
        const Work& b = clusters[level[u32(key)]];
        const bool siblings = a.generating_group != ~0u && a.generating_group == b.generating_group;
        edges.push_back(key);
        weights.push_back(i32(n) * (siblings ? kSiblingEdgeWeight : kClusterEdgeWeight) + kClusterBaseWeight);
    }
    std::vector<float> centers(size_t(count) * 3);
    for (u32 i = 0; i < count; ++i)
        for (u32 k = 0; k < 3; ++k)
            centers[i * 3 + k] = 0.5f * (clusters[level[i]].box_min[k] + clusters[level[i]].box_max[k]);
    locality_links(centers, islands, edges, weights);
    const Graph g = make_graph(count, edges, weights);
    const Partition p = partition(g, kGroupMin, kGroupMax);
    for (u32 i = 0; i < p.parts(); ++i)
    {
        std::vector<u32> members;
        for (u32 k = p.ranges[i]; k < p.ranges[i + 1]; ++k)
            members.push_back(level[p.order[k]]);
        groups.push_back(std::move(members));
    }
    return groups;
}

// One group: its triangles simplified with the level's locked positions held, cut into its parents.
void Builder::reduce(const std::vector<u32>& children, u32 group_id, const std::vector<uint8_t>& level_locked,
    std::vector<Work>& parents, bool& stuck)
{
    std::vector<u32> merged;
    float child_error = 0.f;
    std::vector<Sphere> child_spheres;
    for (u32 c : children)
    {
        merged.insert(merged.end(), clusters[c].tris.begin(), clusters[c].tris.end());
        child_error = std::max(child_error, clusters[c].lod_error);
        child_spheres.push_back(clusters[c].lod_sphere);
    }
    const u32 tri_count = u32(merged.size() / 3);

    // The group's own vertex numbering, so the simplifier's tables are the group's size.
    std::vector<u32> table = merged;
    std::sort(table.begin(), table.end());
    table.erase(std::unique(table.begin(), table.end()), table.end());
    std::unordered_map<u32, u32> local;
    local.reserve(table.size());
    for (u32 i = 0; i < table.size(); ++i)
        local.emplace(table[i], i);
    const u32 nv = u32(table.size()), na = in.attr_count;
    std::vector<float> lpos(size_t(nv) * 3), lattr(size_t(nv) * na);
    std::vector<uint8_t> llocked(nv);
    for (u32 i = 0; i < nv; ++i)
    {
        const u32 v = table[i];
        std::memcpy(&lpos[size_t(i) * 3], in.positions + size_t(v) * 3, 12);
        if (na)
            std::memcpy(&lattr[size_t(i) * na], in.attributes + size_t(v) * na, size_t(na) * 4);
        llocked[i] = (level_locked[pos_of[v]] || (in.locked && in.locked[v])) ? 1 : 0;
    }
    std::vector<u32> lidx(merged.size());
    for (size_t i = 0; i < merged.size(); ++i)
        lidx[i] = local[merged[i]];

    // The weights: the texture sets by their mean triangle in texture space over the group; the UV mirroring of each set
    // a seam (a triangle's winding in texture space in its id).
    std::vector<float> weights(na, 0.f);
    std::vector<u32> tri_ids(tri_count, 0);
    u32 uv_set = 0;
    for (u32 j = 0; j < na; ++j)
    {
        switch (in.attr_kinds[j])
        {
        case akNormal: weights[j] = 1.f; break;
        case akTangent:
        case akColor: weights[j] = 0.0625f; break;
        case akUV:
        {
            if (j + 1 >= na || in.attr_kinds[j + 1] != akUV)
                break;
            double area = 0.0;
            for (u32 t = 0; t < tri_count; ++t)
            {
                const float* a = &lattr[size_t(lidx[t * 3]) * na + j];
                const float* b = &lattr[size_t(lidx[t * 3 + 1]) * na + j];
                const float* c = &lattr[size_t(lidx[t * 3 + 2]) * na + j];
                const float signed_area = 0.5f * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
                area += std::abs(signed_area);
                if (signed_area >= 0.f)
                    tri_ids[t] |= 1u << (24 + std::min(uv_set, 7u));
            }
            const float size = std::max(float(std::sqrt(area / std::max(tri_count, 1u))), 1.f / 1024.f);
            weights[j] = weights[j + 1] = 1.f / (128.f * size);
            ++uv_set;
            ++j;
            break;
        }
        default: break;
        }
    }

    SimplifyInput si;
    si.positions = lpos.data();
    si.attributes = na ? lattr.data() : nullptr;
    si.vertex_count = nv;
    si.attr_count = na;
    si.attr_weights = na ? weights.data() : nullptr;
    si.tri_ids = tri_ids.data();
    si.locked = llocked.data();

    u32 parent_count = (tri_count + kClusterTris * 2 - 1) / (kClusterTris * 2);
    u32 target_size = kParentTarget;
    std::vector<std::vector<u32>> cut;
    float deviation = 0.f;
    u32 result_tris = tri_count;
    for (;;)
    {
        std::vector<u32> idx = lidx;
        SimplifyOptions so;
        so.target_tris = parent_count * target_size;
        const SimplifyResult r = simplify(si, idx, so);
        deviation = r.deviation;
        result_tris = r.tris;
        if (float(result_tris) > kStuckShare * tri_count)
            break;
        for (u32& v : idx)
            v = table[v];
        cut = clusterize(idx);
        if (cut.size() <= parent_count)
            break;
        target_size -= 2;
        if (target_size <= kClusterTris / 2)
            break;
    }
    if (float(result_tris) > kStuckShare * tri_count || cut.empty())
    {
        stuck = true;
        return;
    }

    const float error = std::max(child_error, deviation);
    const Sphere bounds = bound_spheres(child_spheres.data(), child_spheres.size());
    for (u32 c : children)
    {
        clusters[c].parent_sphere = bounds;
        clusters[c].parent_error = error;
        clusters[c].has_parent = true;
    }
    const u32 level = clusters[children[0]].level + 1;
    for (std::vector<u32>& tris : cut)
    {
        Work w = make_cluster(std::move(tris));
        w.lod_sphere = bounds;
        w.lod_error = error;
        w.level = level;
        w.generating_group = group_id;
        parents.push_back(std::move(w));
    }
}

void Builder::run()
{
    weld();
    std::vector<u32> tris;
    tris.reserve(size_t(in.tri_count) * 3);
    for (u32 t = 0; t < in.tri_count; ++t)
    {
        const u32 a = in.indices[t * 3], b = in.indices[t * 3 + 1], c = in.indices[t * 3 + 2];
        if (pos_of[a] == pos_of[b] || pos_of[b] == pos_of[c] || pos_of[a] == pos_of[c])
            continue;
        tris.insert(tris.end(), {a, b, c});
    }
    for (std::vector<u32>& part : clusterize(tris))
        clusters.push_back(make_cluster(std::move(part)));

    std::vector<u32> level(clusters.size());
    std::iota(level.begin(), level.end(), 0u);
    u32 levels = 1, group_base = 0;
    std::vector<uint8_t> level_locked;
    while (level.size() > 1)
    {
        const std::vector<std::vector<u32>> groups = group(level);
        // The positions on edges two groups share: locked for both.
        level_locked.assign(pos.size() / 3, 0);
        {
            std::vector<u32> group_of(clusters.size(), ~0u);
            for (u32 g = 0; g < groups.size(); ++g)
                for (u32 c : groups[g])
                    group_of[c] = g;
            std::vector<std::pair<u64, u32>> keyed;
            for (u32 c : level)
            {
                const Work& w = clusters[c];
                for (size_t t = 0; t < w.tris.size(); t += 3)
                    for (u32 k = 0; k < 3; ++k)
                    {
                        u32 a = pos_of[w.tris[t + k]], b = pos_of[w.tris[t + (k + 1) % 3]];
                        if (a > b)
                            std::swap(a, b);
                        keyed.push_back({(u64(a) << 32) | b, group_of[c]});
                    }
            }
            std::sort(keyed.begin(), keyed.end());
            for (size_t i = 0; i < keyed.size();)
            {
                size_t j = i;
                bool mixed = false;
                while (j < keyed.size() && keyed[j].first == keyed[i].first)
                {
                    mixed |= keyed[j].second != keyed[i].second;
                    ++j;
                }
                if (mixed)
                {
                    level_locked[u32(keyed[i].first >> 32)] = 1;
                    level_locked[u32(keyed[i].first)] = 1;
                }
                i = j;
            }
        }

        std::vector<std::vector<Work>> parents(groups.size());
        std::vector<uint8_t> stuck(groups.size(), 0);
        const auto task = [&](u32 g) {
            bool s = false;
            reduce(groups[g], group_base + g, level_locked, parents[g], s);
            stuck[g] = s ? 1 : 0;
        };
        if (in.parallel && groups.size() > 1)
            in.parallel(u32(groups.size()), task);
        else
            for (u32 g = 0; g < groups.size(); ++g)
                task(g);

        // The next level: the parents, and the children of a stuck group carried on as they are - grouped again with
        // their neighbours there, their borders locked by that level's groups (left as roots, a border between them and
        // a neighbour's parents would be no group's and simplify away: a crack).
        const size_t was = level.size();
        level.clear();
        u32 made = 0;
        for (u32 g = 0; g < groups.size(); ++g)
        {
            out.stuck += stuck[g];
            if (stuck[g])
                level.insert(level.end(), groups[g].begin(), groups[g].end());
            for (Work& w : parents[g])
            {
                level.push_back(u32(clusters.size()));
                clusters.push_back(std::move(w));
                ++made;
            }
        }
        out.groups += u32(groups.size());
        group_base += u32(groups.size());
        // No group simplified, or the level did not shrink: what is left stays roots.
        if (!made || level.size() >= was)
            break;
        ++levels;
    }

    // Out: each cluster's vertex table in first-use order and its triangles by it.
    out.levels = levels;
    out.clusters.reserve(clusters.size());
    std::unordered_map<u32, u32> local;
    for (Work& w : clusters)
    {
        DagCluster c;
        c.vert_first = u32(out.verts.size());
        c.tri_first = u32(out.tris.size() / 3);
        c.tri_count = u32(w.tris.size() / 3);
        local.clear();
        for (u32 v : w.tris)
        {
            const auto [it, fresh] = local.emplace(v, u32(local.size()));
            if (fresh)
                out.verts.push_back(v);
            out.tris.push_back(uint8_t(it->second));
        }
        c.vert_count = u32(local.size());
        std::memcpy(c.box_min, w.box_min, 12);
        std::memcpy(c.box_max, w.box_max, 12);
        std::memcpy(c.lod_sphere, w.lod_sphere.c, 12);
        c.lod_sphere[3] = w.lod_sphere.r;
        c.lod_error = w.lod_error;
        const Sphere& ps = w.has_parent ? w.parent_sphere : w.lod_sphere;
        std::memcpy(c.parent_sphere, ps.c, 12);
        c.parent_sphere[3] = ps.r;
        c.parent_error = w.has_parent ? w.parent_error : kRootError;
        c.level = w.level;
        out.clusters.push_back(c);
    }
}
} // namespace

void build_dag(const DagInput& in, DagOutput& out)
{
    out = {};
    if (!in.positions || !in.indices || !in.tri_count)
        return;
    Builder b(in, out);
    b.run();
}
} // namespace da_mesh
