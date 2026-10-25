// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The cluster hierarchy's builder outside the engine (docs/dead-air/CLUSTERS.md): builds the hierarchy of a few test
// meshes - a closed sphere with a texture seam, a terrain grid, a soup of loose triangles, or an OBJ given on the command
// line - and checks it: every cluster within 128 triangles and 256 vertices, every parent's error and sphere over its
// children's, and the cut chosen by the screen error from several viewpoints watertight wherever the input is (each edge
// of a closed mesh used by two triangles of the cut, the cut's area the input's within the error).
//   cluster_check.exe [mesh.obj]
#include "da_cluster_dag.h"
#include "da_mesh_simplify.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace da_mesh;

struct Mesh
{
    std::string name;
    std::vector<float> pos, attr;
    std::vector<uint8_t> kinds;
    std::vector<uint32_t> idx;
    bool closed{};
};

static Mesh sphere(int seg)
{
    Mesh m;
    m.name = "sphere";
    m.closed = true;
    m.kinds = {akNormal, akNormal, akNormal, akUV, akUV};
    const int rings = seg / 2;
    // Rows of vertices with a duplicated column at the texture seam (u 0 and 1 at one position); the poles welded by
    // position, each its own vertex per column (a seam fan).
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= seg; ++s)
        {
            const float th = 3.14159265f * r / rings, ph = 2.f * 3.14159265f * (s % seg) / seg;
            // The poles exact: sinf(pi) is not 0, and a pole of distinct positions a tenth of a micron apart is a hole.
            const float st = r == 0 || r == rings ? 0.f : std::sin(th);
            const float x = st * std::cos(ph), y = r == 0 ? 1.f : (r == rings ? -1.f : std::cos(th)), z = st * std::sin(ph);
            const float bump = r == 0 || r == rings ? 1.f : 1.f + 0.05f * std::sin(7.f * ph) * std::sin(5.f * th);
            m.pos.insert(m.pos.end(), {x * bump, y * bump, z * bump});
            m.attr.insert(m.attr.end(), {x, y, z, float(s) / seg, float(r) / rings});
        }
    const auto id = [&](int r, int s) { return uint32_t(r * (seg + 1) + s); };
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s)
        {
            if (r > 0)
                m.idx.insert(m.idx.end(), {id(r, s), id(r, s + 1), id(r + 1, s)});
            if (r < rings - 1)
                m.idx.insert(m.idx.end(), {id(r, s + 1), id(r + 1, s + 1), id(r + 1, s)});
        }
    return m;
}

// A box of six gridded faces, each face its own vertices (a hard normal seam along every edge), closed by position.
static Mesh box(int n)
{
    Mesh m;
    m.name = "box";
    m.closed = true;
    m.kinds = {akNormal, akNormal, akNormal, akUV, akUV};
    const float axes[6][3][3] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (int f = 0; f < 6; ++f)
    {
        const float* N = axes[f][0];
        const float* U = axes[f][1];
        const float* V = axes[f][2];
        const uint32_t base = uint32_t(m.pos.size() / 3);
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
            {
                // Grid steps in sixteenths so the shared edges land on the same floats from both faces.
                const float u = -1.f + 2.f * i / n, v = -1.f + 2.f * j / n;
                const float bump = 0.04f * std::sin(9.f * u) * std::sin(7.f * v) * (1.f - u * u) * (1.f - v * v);
                for (int k = 0; k < 3; ++k)
                    m.pos.push_back(N[k] * (1.f + bump) + U[k] * u + V[k] * v);
                m.attr.insert(m.attr.end(), {N[0], N[1], N[2], u, v});
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const uint32_t a = base + j * (n + 1) + i, b = a + 1, c = a + (n + 1), d = c + 1;
                m.idx.insert(m.idx.end(), {a, b, c, b, d, c});
            }
    }
    return m;
}

static Mesh terrain(int n)
{
    Mesh m;
    m.name = "terrain";
    m.kinds = {akNormal, akNormal, akNormal, akColor, akUV, akUV};
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    std::vector<float> h((n + 1) * (n + 1));
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
            h[z * (n + 1) + x] = 3.f * std::sin(x * 0.07f) * std::cos(z * 0.05f) + 0.4f * std::sin(x * 0.9f + z * 0.7f) + 0.05f * u(rng);
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            m.pos.insert(m.pos.end(), {float(x), h[z * (n + 1) + x], float(z)});
            m.attr.insert(m.attr.end(), {0.f, 1.f, 0.f, u(rng), x / 8.f, z / 8.f});
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const uint32_t a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
        }
    return m;
}

static Mesh soup(int count)
{
    Mesh m;
    m.name = "soup";
    m.kinds = {akNormal, akNormal, akNormal};
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-20.f, 20.f), s(-0.3f, 0.3f);
    for (int i = 0; i < count; ++i)
    {
        const float cx = u(rng), cy = u(rng) * 0.1f, cz = u(rng);
        for (int k = 0; k < 3; ++k)
        {
            m.pos.insert(m.pos.end(), {cx + s(rng), cy + s(rng), cz + s(rng)});
            m.attr.insert(m.attr.end(), {0.f, 1.f, 0.f});
            m.idx.push_back(uint32_t(m.pos.size() / 3 - 1));
        }
    }
    return m;
}

static bool load_obj(const char* path, Mesh& m)
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    m.name = path;
    m.kinds.clear();
    char line[1024];
    while (std::fgets(line, sizeof(line), f))
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            float x, y, z;
            if (std::sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3)
                m.pos.insert(m.pos.end(), {x, y, z});
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            std::vector<uint32_t> face;
            char* p = line + 2;
            while (*p)
            {
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (!*p || *p == '\n' || *p == '\r')
                    break;
                face.push_back(uint32_t(std::strtoul(p, &p, 10) - 1));
                while (*p && *p != ' ' && *p != '\t')
                    ++p;
            }
            for (size_t k = 2; k < face.size(); ++k)
                m.idx.insert(m.idx.end(), {face[0], face[k - 1], face[k]});
        }
    }
    std::fclose(f);
    return !m.idx.empty();
}

// The projected scale of a sphere for a camera at `eye` looking down `fwd`: the least of depth times the cosine off
// the axis over the sphere, bounded below as the GPU's test bounds it (da_cluster_cull.cs).
static float edge_scale(const float* s, const float* eye, const float* fwd)
{
    const float d[3] = {s[0] - eye[0], s[1] - eye[1], s[2] - eye[2]};
    const float z = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float zmin = std::max(z - s[3], 0.05f);
    return zmin * zmin / (dist + s[3]);
}

int main(int argc, char** argv)
{
    std::vector<Mesh> meshes;
    if (argc > 1)
    {
        Mesh m;
        if (!load_obj(argv[1], m))
        {
            std::printf("cannot read %s\n", argv[1]);
            return 2;
        }
        meshes.push_back(std::move(m));
    }
    else
    {
        meshes.push_back(sphere(256));
        meshes.push_back(box(64));
        meshes.push_back(terrain(300));
        meshes.push_back(soup(3000));
    }

    int failures = 0;
    const auto open_edges = [](const Mesh& m, const std::vector<uint32_t>& idx, uint32_t& over) {
        std::map<std::pair<std::array<long long, 3>, std::array<long long, 3>>, int> edges;
        const auto key = [&](uint32_t vi) {
            const float* p = &m.pos[vi * 3];
            return std::array<long long, 3>{std::llround(p[0] * 1e5), std::llround(p[1] * 1e5), std::llround(p[2] * 1e5)};
        };
        for (size_t t = 0; t < idx.size(); t += 3)
            for (int k = 0; k < 3; ++k)
            {
                auto x = key(idx[t + k]), y = key(idx[t + (k + 1) % 3]);
                if (x > y)
                    std::swap(x, y);
                edges[{x, y}]++;
            }
        uint32_t open = 0;
        over = 0;
        for (const auto& [e, n] : edges)
        {
            if (n == 1 && open < 4 && std::getenv("CC_VERBOSE"))
                std::printf("      open (%.4f %.4f %.4f) - (%.4f %.4f %.4f)\n", e.first[0] * 1e-5, e.first[1] * 1e-5,
                    e.first[2] * 1e-5, e.second[0] * 1e-5, e.second[1] * 1e-5, e.second[2] * 1e-5);
            open += n == 1;
            over += n > 2;
        }
        return open;
    };
    // The simplifier alone on the closed meshes: a closed surface stays closed at any target.
    for (Mesh& m : meshes)
    {
        if (!m.closed)
            continue;
        for (uint32_t target : {20000u, 4000u, 600u, 120u})
        {
            std::vector<uint32_t> idx = m.idx;
            std::vector<float> w(m.kinds.size(), 0.f);
            for (size_t j = 0; j < m.kinds.size(); ++j)
                w[j] = m.kinds[j] == akNormal ? 1.f : 0.01f;
            SimplifyInput si;
            si.positions = m.pos.data();
            si.attributes = m.attr.data();
            si.vertex_count = uint32_t(m.pos.size() / 3);
            si.attr_count = uint32_t(m.kinds.size());
            si.attr_weights = w.data();
            SimplifyOptions so;
            so.target_tris = target;
            const SimplifyResult r = simplify(si, idx, so);
            uint32_t over = 0;
            const uint32_t open = open_edges(m, idx, over);
            std::printf("   simplify %s to %u: %u tris, deviation %.4f, open edges %u, over-shared %u\n", m.name.c_str(), target,
                r.tris, r.deviation, open, over);
            failures += open + over ? 1 : 0;
        }
    }
    for (Mesh& m : meshes)
    {
        DagInput in;
        in.positions = m.pos.data();
        in.vertex_count = uint32_t(m.pos.size() / 3);
        in.attributes = m.attr.empty() ? nullptr : m.attr.data();
        in.attr_count = uint32_t(m.kinds.size());
        in.attr_kinds = m.kinds.data();
        in.indices = m.idx.data();
        in.tri_count = uint32_t(m.idx.size() / 3);
        in.parallel = [](uint32_t count, const std::function<void(uint32_t)>& fn) {
            std::atomic<uint32_t> next{0};
            std::vector<std::thread> pool;
            const uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
            for (uint32_t t = 0; t < threads; ++t)
                pool.emplace_back([&] {
                    for (uint32_t i; (i = next++) < count;)
                        fn(i);
                });
            for (std::thread& t : pool)
                t.join();
        };
        DagOutput out;
        const auto t0 = std::chrono::steady_clock::now();
        build_dag(in, out);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        // Structure.
        uint32_t bad_size = 0, bad_error = 0, bad_sphere = 0, roots = 0;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> per_level;
        for (const DagCluster& c : out.clusters)
        {
            bad_size += c.tri_count > kClusterTris || c.vert_count > kClusterVerts || !c.tri_count;
            bad_error += c.parent_error < c.lod_error;
            const float dx = c.parent_sphere[0] - c.lod_sphere[0], dy = c.parent_sphere[1] - c.lod_sphere[1],
                        dz = c.parent_sphere[2] - c.lod_sphere[2];
            bad_sphere += std::sqrt(dx * dx + dy * dy + dz * dz) + c.lod_sphere[3] > c.parent_sphere[3] * 1.0002f + 1e-5f;
            roots += c.parent_error >= kRootError;
            per_level[c.level].first++;
            per_level[c.level].second += c.tri_count;
        }
        std::printf("== %s: %u tris -> %zu clusters, %u levels, %u groups (%u stuck), %u roots, %.0f ms\n", m.name.c_str(),
            in.tri_count, out.clusters.size(), out.levels, out.groups, out.stuck, roots, ms);
        for (const auto& [lv, cnt] : per_level)
        {
            float emax = 0.f;
            for (const DagCluster& c : out.clusters)
                if (c.level == lv)
                    emax = std::max(emax, c.lod_error);
            std::printf("   level %u: %u clusters, %u tris, max error %.4f\n", lv, cnt.first, cnt.second, emax);
        }
        std::printf("   bad size %u, parent error under own %u, sphere not inside parent's %u\n", bad_size, bad_error, bad_sphere);
        failures += bad_size + bad_error + bad_sphere;

        // Cuts: from a few viewpoints at a few pixel thresholds, the clusters whose own error is under the threshold and
        // whose parents' is over it.
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < m.pos.size(); i += 3)
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], m.pos[i + k]);
                hi[k] = std::max(hi[k], m.pos[i + k]);
            }
        const float ctr[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
        const float size = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        for (float dist_k : {0.2f, 1.f, 4.f, 16.f})
            for (float px : {1.f, 4.f})
            {
                const float eye[3] = {ctr[0] + size * dist_k, ctr[1] + size * 0.3f * dist_k, ctr[2] + size * 0.2f};
                float fwd[3] = {ctr[0] - eye[0], ctr[1] - eye[1], ctr[2] - eye[2]};
                const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
                for (float& f : fwd)
                    f /= fl;
                // Pixels a unit of error at unit depth: 1080 lines over a 67 degree field.
                const float lod_scale = 0.5f * 1080.f / std::tan(0.5f * 67.f * 3.14159265f / 180.f);
                std::map<std::pair<std::array<long long, 3>, std::array<long long, 3>>, int> edges;
                uint32_t cut_tris = 0, cut_clusters = 0;
                double area = 0.0;
                for (const DagCluster& c : out.clusters)
                {
                    const float own = c.lod_error * lod_scale / edge_scale(c.lod_sphere, eye, fwd);
                    const float parent = c.parent_error >= kRootError ? 1e30f : c.parent_error * lod_scale / edge_scale(c.parent_sphere, eye, fwd);
                    if (!(own <= px && parent > px))
                        continue;
                    ++cut_clusters;
                    cut_tris += c.tri_count;
                    for (uint32_t t = 0; t < c.tri_count; ++t)
                    {
                        uint32_t v[3];
                        for (int k = 0; k < 3; ++k)
                            v[k] = out.verts[c.vert_first + out.tris[(c.tri_first + t) * 3 + k]];
                        const float* a = &m.pos[v[0] * 3];
                        const float* b = &m.pos[v[1] * 3];
                        const float* cc = &m.pos[v[2] * 3];
                        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {cc[0] - a[0], cc[1] - a[1], cc[2] - a[2]};
                        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                        area += 0.5 * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
                        // Edges by position, so the texture seam's two vertex columns count as one edge.
                        for (int k = 0; k < 3; ++k)
                        {
                            const auto key = [&](uint32_t vi) {
                                const float* p = &m.pos[vi * 3];
                                return std::array<long long, 3>{std::llround(p[0] * 1e5), std::llround(p[1] * 1e5), std::llround(p[2] * 1e5)};
                            };
                            auto x = key(v[k]), y = key(v[(k + 1) % 3]);
                            if (x > y)
                                std::swap(x, y);
                            edges[{x, y}]++;
                        }
                    }
                }
                uint32_t open = 0;
                for (const auto& [e, n] : edges)
                    open += n != 2;
                std::printf("   cut at %.1fx size, %.0f px: %u clusters, %u tris, area %.1f, open edges %u\n", dist_k, px,
                    cut_clusters, cut_tris, area, open);
                if (m.closed && open)
                    ++failures;
            }
    }
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
