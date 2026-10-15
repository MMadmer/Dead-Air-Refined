// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The shrub generator outside the engine: builds bushes of a species of dead_air_x64_shrubs.ltx with the engine's
// own generator (src/Layers/xrRender/da_shrub_gen.cpp) and writes them as OBJ for render_preview.py.
//
//   shrub_preview <shrubs.ltx> <species> <out.obj> [h,w ...] [--seed n] [--lod 0|1|2]
//   shrub_preview <trees.ltx> <species> <out.obj> --skel <file.dtsk> [--seed n] [--lod 0|1|2] [--cap n] [--tilt deg,az]
//                 [--overlap] [--dump <file.bin>]
//
// Every h,w pair is one bush, placed in a row along x. --lod 1 writes the clusters, --lod 2 the blobs, as the
// middle and far distance draw them. --skel grows one tree of a [tree_species_N] on a stock skeleton
// (gamedata/meshes/da/trees/*.dtsk, generate_on_skeleton): its stock tubes (the rings' profiles, 16 sides), the stock
// patches, the grafted wood, the leaves and spray cards (or the clusters, the blobs), the blooms; --cap the leaf
// elements' cap (the level's budget share, 80000 by default), --tilt a tilted stock tree's bucket (degrees, azimuth),
// --overlap the stock patches against the stock tubes, --dump the template's elements as float sections (the textured
// look-dev's input: 'LEAF', 'CARD', 'SEGM', 'RING', 'AXIS', 'PVRT', 'PTRI', 'BLOM', 'META', each a tag, a count, a
// stride and that many rows of floats). Prints the template statistics.
#include "da_shrub_gen.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace da_shrub;

namespace
{
using Section = std::vector<std::pair<std::string, std::string>>;

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// A minimal ltx reader: [section] and key = value lines, ';' comments, no includes or inheritance.
std::map<std::string, Section> read_ltx(const char* path)
{
    std::map<std::string, Section> out;
    std::ifstream f(path);
    std::string line, cur;
    while (std::getline(f, line))
    {
        const size_t c = line.find(';');
        if (c != std::string::npos)
            line.resize(c);
        line = trim(line);
        if (line.empty())
            continue;
        if (line[0] == '[')
        {
            const size_t e = line.find(']');
            cur = trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            out[cur];
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || cur.empty())
            continue;
        out[cur].emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return out;
}

struct Obj
{
    FILE* f{};
    uint32_t base = 1;
    void v(V3 p) { std::fprintf(f, "v %.4f %.4f %.4f\n", p.x, p.y, p.z); }
    void face(std::initializer_list<uint32_t> ids)
    {
        std::fputc('f', f);
        for (uint32_t i : ids)
            std::fprintf(f, " %u", base + i);
        std::fputc('\n', f);
    }
};

V3 any_perp(V3 t)
{
    const V3 a = std::fabs(t.y) < 0.9f ? v3(0.f, 1.f, 0.f) : v3(1.f, 0.f, 0.f);
    return normalize(cross(t, a));
}

void write_segment(Obj& o, const Segment& g, V3 off)
{
    const int sides = 6;
    const V3 t = normalize(g.b - g.a);
    const V3 u = any_perp(t), w = cross(t, u);
    for (int end = 0; end < 2; ++end)
        for (int i = 0; i < sides; ++i)
        {
            const float a = 6.2831853f * float(i) / float(sides);
            const V3 r = u * std::cos(a) + w * std::sin(a);
            o.v((end ? g.b : g.a) + off + r * (end ? g.rb : g.ra));
        }
    for (int i = 0; i < sides; ++i)
    {
        const uint32_t j = uint32_t((i + 1) % sides);
        o.face({uint32_t(i), j, sides + j, uint32_t(sides + i)});
    }
    o.base += 2 * sides;
}

void write_blade(Obj& o, const Leaf& l, float droop, V3 off);

// A leaf as the shaders draw it: a blade, a needle tuft, a compound leaf's leaflets on its rachis (ratio: a
// leaflet's width over its length), or a spray's leaves round its stretch of shoot.
void write_leaf(Obj& o, const Leaf& l, const Arch& a, float ratio, V3 off)
{
    const float droop = a.leaf_droop;
    const bool needles = a.leaf_kind == lkNeedles;
    if (a.leaf_kind == lkSpray)
    {
        // As the near spray draws (r3\da_shrub_leaf_vs.h): leaf j at its node, round the shoot by the phyllotaxis
        // from the spray's own side, an opposite pair sharing a node.
        const int n = std::max(int(std::lround(a.leaflets * l.len / std::max(a.spray_len, 0.03f))), 1);
        const int per = a.leaf_per_node > 1.5f ? 2 : 1;
        const int nodes = (n + per - 1) / per;
        const float splay = 0.5f * (a.leaf_angle[0] + a.leaf_angle[1]) * 0.0174533f;
        const float ll = l.wid / (2.f * std::max(std::sin(splay), 0.3f) + ratio);
        const V3 u = l.side, w = normalize(cross(l.dir, l.side), any_perp(l.dir));
        for (int j = 0; j < n; ++j)
        {
            const int node = j / per, m = j % per;
            const float az = float(node) * a.leaf_phyllo * 0.0174533f + float(m) * 3.14159265f;
            const V3 radial = u * std::cos(az) + w * std::sin(az);
            Leaf q = l;
            q.p = l.p + l.dir * (l.len * (float(node) + 0.25f) / float(nodes));
            q.dir = normalize(l.dir * std::cos(splay) + radial * std::sin(splay));
            q.side = normalize(cross(q.dir, radial), l.side);
            q.len = ll;
            q.wid = ll * ratio;
            write_blade(o, q, droop, off);
        }
        return;
    }
    if (a.leaf_kind == lkCompound && a.leaflets >= 1.f)
    {
        const int n = int(std::lround(a.leaflets));
        const int pairs = n / 2;
        const float ll = l.wid / (2.f * std::max(std::sin(a.leaflet_angle * 0.0174533f), 0.3f));
        const float rach = l.len - ll * ((n & 1) ? 1.f : 0.5f);
        const V3 up = normalize(cross(l.side, l.dir), v3(0.f, 1.f, 0.f));
        for (int j = 0; j < pairs; ++j)
        {
            const float v = pairs > 1 ? float(j) / float(pairs - 1) : 0.5f;
            for (int sg = -1; sg <= 1; sg += 2)
            {
                Leaf q = l;
                const float an = float(sg) * a.leaflet_angle * 0.0174533f;
                q.p = l.p + l.dir * (rach * (0.2f + 0.65f * v));
                q.dir = normalize(l.dir * std::cos(an) + l.side * std::sin(an));
                q.side = normalize(cross(up, q.dir), l.side);
                q.len = ll * (0.8f + 0.2f * v);
                q.wid = q.len * ratio;
                write_blade(o, q, droop, off);
            }
        }
        if (n & 1)
        {
            Leaf q = l;
            q.p = l.p + l.dir * rach;
            q.len = ll;
            q.wid = ll * ratio;
            write_blade(o, q, droop, off);
        }
        return;
    }
    if (!needles)
    {
        write_blade(o, l, droop, off);
        return;
    }
    {
        // A tuft: two crossed strips along the shoot.
        const V3 u = any_perp(l.dir), w = cross(l.dir, u);
        for (int k = 0; k < 2; ++k)
        {
            const V3 s = (k ? w : u) * (0.5f * l.wid);
            o.v(l.p + off - s);
            o.v(l.p + off + s);
            o.v(l.p + off + l.dir * l.len + s);
            o.v(l.p + off + l.dir * l.len - s);
            o.face({0, 1, 2, 3});
            o.base += 4;
        }
    }
}

// A blade outline: 7 points a side, widest a third up, the tip drooping.
void write_blade(Obj& o, const Leaf& l, float droop, V3 off)
{
    const int n = 7;
    const V3 up = normalize(cross(l.side, l.dir), v3(0.f, 1.f, 0.f));
    std::vector<V3> mid(n + 1);
    for (int i = 0; i <= n; ++i)
    {
        const float t = float(i) / float(n);
        mid[i] = l.p + off + l.dir * (t * l.len) - up * (droop * l.len * t * t);
    }
    // Width profile: an ovate blade.
    auto width = [](float t) { return std::pow(std::sin(3.14159265f * std::pow(t, 0.75f)), 0.9f); };
    o.v(mid[0]);
    for (int i = 1; i < n; ++i)
    {
        const float t = float(i) / float(n);
        o.v(mid[i] + l.side * (0.5f * l.wid * width(t)));
        o.v(mid[i] - l.side * (0.5f * l.wid * width(t)));
    }
    o.v(mid[n]);
    // Fan: 0, then pairs (1,2), (3,4) ..., tip 2n-1.
    const uint32_t tip = uint32_t(2 * (n - 1) + 1);
    o.face({0, 1, 2});
    for (int i = 0; i < n - 2; ++i)
    {
        const uint32_t a = 1 + 2 * i, b = 2 + 2 * i;
        o.face({a, a + 2, b + 2, b});
    }
    o.face({uint32_t(2 * (n - 2) + 1), tip, uint32_t(2 * (n - 2) + 2)});
    o.base += tip + 1;
}

// A stock ring's radius at its 16-gon's vertex i: its Fourier profile, a covered sixteenth stepped back
// (r3\da_tree_stock_vs.h).
float ring_radius(const Ring& r, int i, bool retreat = true)
{
    const int j = i & 15;
    const float th = 6.2831853f * float(j) / 16.f;
    float k = 1.f;
    for (int h = 1; h <= 3; ++h)
        k += r.f[2 * h - 2] * std::cos(float(h) * th) + r.f[2 * h - 1] * std::sin(float(h) * th);
    const float back = retreat ? ring_step_back(r, uint32_t(j), r.r0 * k) : 0.f;
    return r.r0 * k - back;
}

// A stock tube between two rings: each ring's profile at 16 sides about its reference normal (the angle grows
// toward cross(t, n), r3\da_tree_stock_vs.h).
void write_rings(Obj& o, const Ring& a, const Ring& b)
{
    const int sides = 16;
    for (const Ring* r : {&a, &b})
    {
        const V3 bn = cross(r->t, r->n);
        for (int i = 0; i < sides; ++i)
        {
            const float th = 6.2831853f * float(i) / float(sides);
            o.v(r->c + (r->n * std::cos(th) + bn * std::sin(th)) * ring_radius(*r, i));
        }
    }
    for (int i = 0; i < sides; ++i)
    {
        const uint32_t j = uint32_t((i + 1) % sides);
        o.face({uint32_t(i), j, sides + j, uint32_t(sides + i)});
    }
    o.base += 2 * sides;
}

// A spray card: its texture's square along the shoot, across it by its side.
void write_card(Obj& o, const Leaf& l)
{
    const V3 s = l.side * (0.5f * l.wid);
    o.v(l.p - s);
    o.v(l.p + s);
    o.v(l.p + l.dir * l.len + s);
    o.v(l.p + l.dir * l.len - s);
    o.face({0, 1, 2, 3});
    o.base += 4;
}

void write_ball(Obj& o, V3 c, float r)
{
    const V3 p[6] = {v3(r, 0, 0), v3(-r, 0, 0), v3(0, r, 0), v3(0, -r, 0), v3(0, 0, r), v3(0, 0, -r)};
    for (const V3& q : p)
        o.v(c + q);
    o.face({0, 2, 4});
    o.face({4, 2, 1});
    o.face({1, 2, 5});
    o.face({5, 2, 0});
    o.face({4, 3, 0});
    o.face({1, 3, 4});
    o.face({5, 3, 1});
    o.face({0, 3, 5});
    o.base += 6;
}

// A disc facing n.
void write_disc(Obj& o, V3 c, V3 n, float r)
{
    const int k = 10;
    const V3 u = any_perp(n), w = cross(n, u);
    o.v(c);
    for (int i = 0; i < k; ++i)
    {
        const float a = 6.2831853f * float(i) / float(k);
        o.v(c + (u * std::cos(a) + w * std::sin(a)) * r);
    }
    for (int i = 0; i < k; ++i)
        o.face({0, uint32_t(1 + i), uint32_t(1 + (i + 1) % k)});
    o.base += k + 1;
}
} // namespace

// The closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5): its distance.
float tri_dist(V3 p, V3 a, V3 b, V3 c)
{
    const V3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f)
        return length(p - a);
    const V3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3)
        return length(p - b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
        return length(p - (a + ab * (d1 / (d1 - d3))));
    const V3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6)
        return length(p - c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
        return length(p - (a + ac * (d2 / (d2 - d6))));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
        return length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const float den = 1.f / (va + vb + vc);
    return length(p - (a + ab * (vb * den) + ac * (vc * den)));
}

// The stock bark patches (4 mm out along their normals, r3\da_tree_patch_vs.h; not the cut ends' caps, which lie
// across the tubes' open ends) against the stock tubes as the engine draws them (straight between the rings, each ring
// a 16-gon), as they were and as drawn now (each covered sixteenth stepped back by its level, a stretch between rings
// the patches wrap whole not drawn):
// the shares of patch points the tube stands out past by more than the tolerance (max(2 cm, 5 % r): the tube shows
// there instead of the stock), within 2 mm of the tube (the two fight for the depth) and in front of it. Then the
// tessellation's lift: each new patch vertex's distance from the stock triangles, its 99th percentile and largest
// against its limit (3 cm on climbable wood, min(5 % r, 4 cm) elsewhere).
void overlap_stats(const Template& t)
{
    struct Pt
    {
        uint32_t axis;
        float s;
        V3 p;
    };
    std::vector<Pt> pts;
    std::vector<uint8_t> bark(t.patch_verts.size(), 0);
    for (size_t i = 0; i < size_t(t.patch_bark_tris) * 3 && i < t.patch_tris.size(); ++i)
        bark[t.patch_tris[i]] = 1;
    for (size_t i = 0; i < t.patch_verts.size(); ++i)
        if (bark[i])
            pts.push_back({t.patch_verts[i].axis, t.patch_verts[i].s, t.patch_verts[i].p + t.patch_verts[i].n * 0.004f});
    for (size_t i = 0; i + 2 < t.patch_tris.size() && i / 3 < t.patch_bark_tris; i += 3)
    {
        const PatchVert& a = t.patch_verts[t.patch_tris[i]];
        const PatchVert& b = t.patch_verts[t.patch_tris[i + 1]];
        const PatchVert& c = t.patch_verts[t.patch_tris[i + 2]];
        if (a.axis == b.axis && b.axis == c.axis)
            pts.push_back({a.axis, (a.s + b.s + c.s) / 3.f, (a.p + b.p + c.p + (a.n + b.n + c.n) * 0.004f) * (1.f / 3.f)});
    }
    const float step = 6.2831853f / 16.f;
    for (int pass = 0; pass < 2; ++pass)
    {
        uint32_t n = 0, deep = 0, seam = 0, fight = 0;
        for (const Pt& q : pts)
        {
            if (q.axis >= t.axes.size() || !t.axes[q.axis].fixed || t.axes[q.axis].n_rings < 2)
                continue;
            const Axis& ax = t.axes[q.axis];
            uint32_t k = ax.first_ring;
            while (k + 2 < ax.first_ring + ax.n_rings && t.rings[k + 1].s <= q.s)
                ++k;
            const Ring& a = t.rings[k];
            const Ring& b = t.rings[k + 1];
            // (A stretch between two hidden rings has no tube.)
            if (pass == 1 && (a.cover & kRingHidden) && (b.cover & kRingHidden))
            {
                ++n;
                continue;
            }
            const float u = std::min(std::max((q.s - a.s) / std::max(b.s - a.s, 1e-6f), 0.f), 1.f);
            const V3 c = a.c + (b.c - a.c) * u;
            const V3 tt = normalize(a.t + (b.t - a.t) * u);
            V3 nn = a.n + (b.n - a.n) * u;
            nn = normalize(nn - tt * dot(nn, tt));
            V3 d = q.p - c;
            d = d - tt * dot(d, tt);
            float th = std::atan2(dot(d, cross(tt, nn)), dot(d, nn));
            if (th < 0.f)
                th += 6.2831853f;
            // The 16-gon's radius toward th (the chord between the vertices around it), straight between the rings.
            const int i0 = std::min(int(th / step), 15);
            const float a0 = step * float(i0);
            const auto chord = [&](const Ring& r) {
                const float r0 = ring_radius(r, i0, pass == 1), r1 = ring_radius(r, i0 + 1, pass == 1);
                const float x0 = r0 * std::cos(a0), y0 = r0 * std::sin(a0);
                const float ex = r1 * std::cos(a0 + step) - x0, ey = r1 * std::sin(a0 + step) - y0;
                const float den = std::cos(th) * ey - std::sin(th) * ex;
                return std::fabs(den) > 1e-9f ? (x0 * ey - y0 * ex) / den : 0.5f * (r0 + r1);
            };
            const float gap = length(d) - (chord(a) + (chord(b) - chord(a)) * u);
            const float tol = std::max(0.02f, 0.05f * (a.r0 + (b.r0 - a.r0) * u));
            ++n;
            deep += gap < -tol;
            seam += gap >= -tol && gap < -0.002f;
            fight += std::fabs(gap) <= 0.002f;
        }
        const float k = 100.f / float(std::max(n, 1u));
        std::printf("overlap %s: %u patch points, the tube out past them by more than the tolerance %.1f%%, by less %.1f%%, "
                    "within 2 mm %.1f%%, the patch in front %.1f%%\n",
            pass ? "as drawn " : "as it was", n, k * float(deep), k * float(seam), k * float(fight), k * float(n - deep - seam - fight));
    }
    // The tessellation's lift (the new vertices after the stock's own).
    uint32_t n0 = 0;
    for (const uint32_t i : t.patch_tris_far)
        n0 = std::max(n0, i + 1);
    const auto vlimit = [&t](const PatchVert& v, bool& climb) {
        float c = 0.f;
        if (v.axis < t.axes.size())
        {
            const Axis& ax = t.axes[v.axis];
            c = ax.climbable;
            for (uint32_t k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
                if (t.rings[k].s <= v.s)
                    c = t.rings[k].climbable;
        }
        climb = c >= 0.25f;
        return climb ? 0.03f : std::min(std::max(0.05f * v.r, 0.005f), 0.04f);
    };
    std::vector<float> rel, cm_climb, cm_other;
    for (uint32_t i = n0; i < t.patch_verts.size(); ++i)
    {
        const PatchVert& v = t.patch_verts[i];
        float dmin = 1e9f;
        size_t near = 0;
        for (size_t f = 0; f + 2 < t.patch_tris_far.size(); f += 3)
        {
            const float d = tri_dist(v.p, t.patch_verts[t.patch_tris_far[f]].p, t.patch_verts[t.patch_tris_far[f + 1]].p,
                t.patch_verts[t.patch_tris_far[f + 2]].p);
            if (d < dmin)
                dmin = d, near = f;
        }
        bool climb = false, c1 = false, c2 = false;
        const float limit = std::min({vlimit(t.patch_verts[t.patch_tris_far[near]], climb),
            vlimit(t.patch_verts[t.patch_tris_far[near + 1]], c1), vlimit(t.patch_verts[t.patch_tris_far[near + 2]], c2)});
        climb = climb || c1 || c2;
        rel.push_back(dmin / limit);
        (climb ? cm_climb : cm_other).push_back(dmin * 100.f);
    }
    const auto p99 = [](std::vector<float> v) {
        if (v.empty())
            return 0.f;
        std::sort(v.begin(), v.end());
        return v[size_t(0.99 * double(v.size() - 1))];
    };
    const float worst = rel.empty() ? 0.f : *std::max_element(rel.begin(), rel.end());
    std::printf("tessellation: %u stock patch vertices, %zu new (%zu triangles near, %zu far); the lift off the stock at the "
                "99th percentile %.2f of its limit (climbable %.2f cm of 3, elsewhere %.2f cm), the largest %.2f\n",
        n0, rel.size(), t.patch_tris.size() / 3, t.patch_tris_far.size() / 3, p99(rel), p99(cm_climb), p99(cm_other), worst);
}

// The stock tubes' exposure as WP1 measured it, from the tube's side: the drawn tube's samples (each ring's 16 sides
// and each quad's middle) under a stock patch seen from the axis (the ray from the ring's centre through the sample
// meets an outward stock triangle first, 15 cm past the sample at the most) standing out past it by more than the
// tolerance (max(2 cm, 5 % r): the tube shows there instead of the stock), by
// rule: 0 the tubes as built, 1 the covered sixteenths stepped back by the tolerance alone, every stretch drawn (phase
// 2), 2 as drawn now (the step back; a hidden stretch's samples count, never out). The ones still out now by where
// they are: an axis's end ring and the middles of its stretch, a covered point whose step back the cap holds (15 % of
// the radius left), another covered one, an open one, a quad's middle with its four corners covered or not. And the
// dents: samples stepped back by more than the tolerance with no patch in front of them (the ray through the sample as
// built meets no outward stock triangle) and not inside another limb's drawn tube (its mean radius, 95 %).
void tube_exposure(const Template& t)
{
    struct Tri
    {
        V3 a, n, c;
        V3 e0, e1;
        float rad;
    };
    std::vector<Tri> tris;
    for (size_t f = 0; f + 2 < t.patch_tris_far.size(); f += 3)
    {
        const V3 a = t.patch_verts[t.patch_tris_far[f]].p, b = t.patch_verts[t.patch_tris_far[f + 1]].p,
                 c = t.patch_verts[t.patch_tris_far[f + 2]].p;
        const V3 n = cross(b - a, c - a);
        if (length(n) < 1e-10f)
            continue;
        const V3 m = (a + b + c) * (1.f / 3.f);
        tris.push_back({a, normalize(n), m, b - a, c - a, std::max({length(a - m), length(b - m), length(c - m)})});
    }
    static constexpr float step = 6.2831853f / 16.f;
    for (int rule = 0; rule < 3; ++rule)
    {
        uint32_t over = 0, bad = 0, moved = 0, dent = 0;
        uint32_t why[7]{};
        const auto in_other_wood = [&t](uint32_t axis, V3 p) {
            for (uint32_t ai = 0; ai < t.axes.size(); ++ai)
            {
                const Axis& ax = t.axes[ai];
                if (ai == axis || !ax.fixed || ax.n_rings < 2)
                    continue;
                for (uint32_t k = ax.first_ring; k + 1 < ax.first_ring + ax.n_rings; ++k)
                {
                    const Ring& a = t.rings[k];
                    const Ring& b = t.rings[k + 1];
                    if (a.cover & b.cover & kRingHidden)
                        continue;
                    const V3 e = b.c - a.c;
                    const float u = std::min(std::max(dot(p - a.c, e) / std::max(dot(e, e), 1e-12f), 0.f), 1.f);
                    if (length(p - (a.c + e * u)) < 0.95f * (a.r0 + (b.r0 - a.r0) * u))
                        return true;
                }
            }
            return false;
        };
        const auto back = [rule](const Ring& r, int i, float rad) {
            if (!((r.cover >> (i & 15)) & 1u) || rule == 0)
                return 0.f;
            return rule == 1 ? std::min(std::max(0.02f, 0.05f * r.r0), 0.3f * r.r0) : ring_step_back(r, uint32_t(i & 15), rad);
        };
        const auto point = [&](const Ring& r, int i) {
            const float th = step * float(i & 15);
            float k = 1.f;
            for (int h = 1; h <= 3; ++h)
                k += r.f[2 * h - 2] * std::cos(float(h) * th) + r.f[2 * h - 1] * std::sin(float(h) * th);
            return r.c + (r.n * std::cos(th) + cross(r.t, r.n) * std::sin(th)) * (r.r0 * k - back(r, i, r.r0 * k));
        };
        const auto built = [&](const Ring& r, int i) {
            const float th = step * float(i & 15);
            float k = 1.f;
            for (int h = 1; h <= 3; ++h)
                k += r.f[2 * h - 2] * std::cos(float(h) * th) + r.f[2 * h - 1] * std::sin(float(h) * th);
            return r.c + (r.n * std::cos(th) + cross(r.t, r.n) * std::sin(th)) * (r.r0 * k);
        };
        // (-1: the ray from the axis through the sample meets no outward stock triangle first, 15 cm past the sample at
        // the most; 1: the sample stands out past it by more than the tolerance; 0: not.)
        const auto test = [&](V3 c, V3 p, float tol) -> int {
            V3 d = p - c;
            const float rho = length(d);
            if (rho < 1e-6f)
                return -1;
            d = d * (1.f / rho);
            const float len = rho + 0.15f;
            float best = len;
            bool outward = false;
            for (const Tri& q : tris)
            {
                // (The triangle's sphere against the ray's stretch.)
                if (length(c + d * std::min(std::max(dot(q.c - c, d), 0.f), len) - q.c) > q.rad)
                    continue;
                const V3 pv = cross(d, q.e1);
                const float det = dot(q.e0, pv);
                if (std::fabs(det) < 1e-12f)
                    continue;
                const V3 tv = c - q.a, qv = cross(tv, q.e0);
                const float u = dot(tv, pv) / det, v = dot(d, qv) / det, t = dot(q.e1, qv) / det;
                if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f || t <= 0.f || t >= best)
                    continue;
                best = t;
                outward = dot(q.n, d) > 0.f;
            }
            return best < len && outward ? int(rho - best > tol) : -1;
        };
        for (const Axis& ax : t.axes)
        {
            if (!ax.fixed || ax.n_rings < 2)
                continue;
            const uint32_t first = ax.first_ring, last = ax.first_ring + ax.n_rings - 1;
            for (uint32_t k = first; k < last; ++k)
            {
                const Ring& a = t.rings[k];
                const Ring& b = t.rings[k + 1];
                const bool hidden = rule == 2 && (a.cover & b.cover & kRingHidden);
                const float tol = std::max(0.02f, 0.05f * a.r0);
                for (int i = 0; i < 16; ++i)
                {
                    const uint32_t i1 = uint32_t(i + 1) & 15u;
                    const int e = test(a.c, point(a, i), tol);
                    over += e >= 0;
                    if (e == 1 && !hidden)
                    {
                        ++bad;
                        const bool cov = (a.cover >> i) & 1u;
                        // (Capped: the top level, or the level's own step back over the 85 % of the radius it may take.)
                        const float rad = length(built(a, i) - a.c), tl = std::max(0.02f, 0.05f * a.r0);
                        const uint32_t lv = uint32_t(a.back >> (4 * i)) & 15u;
                        const float q = float(lv - 1u) / 14.f;
                        ++why[k == first ? 0 : (!cov ? 3 : (lv == 15u || tl + q * q * std::max(rad - tl, 0.f) > 0.85f * rad ? 1 : 2))];
                    }
                    const int m = test((a.c + b.c) * 0.5f, (point(a, i) + point(a, i + 1) + point(b, i) + point(b, i + 1)) * 0.25f, tol);
                    over += m >= 0;
                    if (m == 1 && !hidden)
                    {
                        ++bad;
                        const bool all = ((a.cover >> i) & (a.cover >> i1) & (b.cover >> i) & (b.cover >> i1) & 1u) != 0u;
                        ++why[k == first || k + 1 == last ? 4 : (all ? 5 : 6)];
                    }
                    if (rule == 2 && !hidden)
                    {
                        const V3 o = built(a, i), om = (built(a, i) + built(a, i + 1) + built(b, i) + built(b, i + 1)) * 0.25f;
                        const V3 dm = (point(a, i) + point(a, i + 1) + point(b, i) + point(b, i + 1)) * 0.25f;
                        const bool m0 = length(o - point(a, i)) > tol, m1 = length(om - dm) > tol;
                        moved += uint32_t(m0) + uint32_t(m1);
                        dent += uint32_t(m0 && test(a.c, o, 0.f) < 0 && !in_other_wood(a.axis, o)) +
                            uint32_t(m1 && test((a.c + b.c) * 0.5f, om, 0.f) < 0 && !in_other_wood(a.axis, om));
                    }
                }
            }
        }
        std::printf("tube exposure, %s: %u tube samples over stock patch faces, %.1f%% (%u) out past them by more than the "
                    "tolerance\n",
            rule == 0 ? "the tubes as built    " : (rule == 1 ? "phase 2 (cover, 2 cm) " : "as drawn now          "), over,
            100.f * float(bad) / float(std::max(over, 1u)), bad);
        if (rule == 2)
            std::printf("  still out: end rings %u and their stretches' middles %u; covered points with the step back capped %u, "
                        "other covered %u; open points %u; quads' middles, all four corners covered %u, not all %u\n",
                why[0], why[4], why[1], why[2], why[3], why[5], why[6]);
        if (rule == 2)
            std::printf("  dents: %u of the %u drawn samples stepped back by more than the tolerance have no patch in front of them\n",
                dent, moved);
    }
}

// A ring's largest step back (m), as its levels give it before the shader's cap.
float back_max(const Ring& r)
{
    float m = 0.f;
    for (uint32_t j = 0; j < 16; ++j)
        m = std::max(m, ring_step_back(r, j, 1e6f));
    return m;
}

// The template's elements as float sections (--dump).
void dump_template(const Template& t, const char* path)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    const auto section = [f](const char* tag, const std::vector<float>& rows, uint32_t stride) {
        const uint32_t head[2] = {stride ? uint32_t(rows.size() / stride) : 0u, stride};
        std::fwrite(tag, 1, 4, f);
        std::fwrite(head, 4, 2, f);
        if (!rows.empty())
            std::fwrite(rows.data(), 4, rows.size(), f);
    };
    const auto leaves = [](const std::vector<Leaf>& src) {
        std::vector<float> v;
        for (const Leaf& l : src)
            v.insert(v.end(), {l.p.x, l.p.y, l.p.z, l.dir.x, l.dir.y, l.dir.z, l.side.x, l.side.y, l.side.z, l.len, l.wid,
                                  l.area, float(l.axis), l.s, l.rank, l.fall, l.light, l.exposure});
        return v;
    };
    std::fwrite("DTLD", 1, 4, f);
    section("LEAF", leaves(t.leaves), 18);
    section("CARD", leaves(t.cards), 18);
    std::vector<float> v;
    for (const Segment& g : t.segments)
    {
        const Axis& ax = t.axes[g.axis];
        v.insert(v.end(), {g.a.x, g.a.y, g.a.z, g.b.x, g.b.y, g.b.z, g.ra, g.rb, float(g.axis), g.s0, g.vis, float(ax.level),
                              ax.dead ? 1.f : 0.f, ax.fixed ? 1.f : 0.f});
    }
    section("SEGM", v, 14);
    v.clear();
    for (const Ring& r : t.rings)
        v.insert(v.end(), {r.c.x, r.c.y, r.c.z, r.t.x, r.t.y, r.t.z, r.n.x, r.n.y, r.n.z, r.s, r.r0, r.f[0], r.f[1], r.f[2], r.f[3],
                              r.f[4], r.f[5], float(r.axis), r.climbable, float(r.cover), float(r.lod), back_max(r),
                              float(r.back & 0xFFFFu), float((r.back >> 16) & 0xFFFFu), float((r.back >> 32) & 0xFFFFu),
                              float((r.back >> 48) & 0xFFFFu)});
    section("RING", v, 26);
    v.clear();
    for (const Axis& a : t.axes)
        v.insert(v.end(), {a.base.x, a.base.y, a.base.z, a.dir.x, a.dir.y, a.dir.z, a.length, a.r_base, a.r_tip, float(a.parent),
                              a.attach, float(a.level), a.fixed ? 1.f : 0.f, float(a.first_ring), float(a.n_rings), a.dead ? 1.f : 0.f,
                              std::min(a.dead_from, 1e6f), float(a.first_seg), float(a.segs)});
    section("AXIS", v, 19);
    v.clear();
    for (const PatchVert& p : t.patch_verts)
        v.insert(v.end(), {p.p.x, p.p.y, p.p.z, p.n.x, p.n.y, p.n.z, p.uv[0], p.uv[1], float(p.axis), p.s, p.theta, p.r});
    section("PVRT", v, 12);
    v.clear();
    for (uint32_t i : t.patch_tris)
        v.push_back(float(i));
    section("PTRI", v, 3);
    v.clear();
    for (const Bloom& b : t.blooms)
        v.insert(v.end(), {b.p.x, b.p.y, b.p.z, b.dir.x, b.dir.y, b.dir.z, b.size, float(b.axis), b.s, b.rank, float(b.kind)});
    section("BLOM", v, 11);
    section("META", {t.height, t.width, t.bmin.x, t.bmin.y, t.bmin.z, t.bmax.x, t.bmax.y, t.bmax.z, t.leaf_area, t.climb_top}, 10);
    std::fclose(f);
}

// One tree of the species on a stock skeleton (the engine's own generate_on_skeleton), written whole.
int grow_tree(const Arch& arch, float ratio, const char* skel_path, const char* out, uint32_t seed, uint32_t lod, uint32_t cap, V3 up,
    bool overlap, const char* dump)
{
    std::ifstream in(skel_path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Skeleton skel;
    const char* why = nullptr;
    if (!read_skeleton(bytes.data(), bytes.size(), skel, &why))
    {
        std::fprintf(stderr, "%s: %s\n", skel_path, why ? why : "unreadable");
        return 1;
    }
    uint32_t fixed = 0, cards = 0;
    for (const SkelAxis& a : skel.axes)
        (a.flags & skaFromCards ? cards : fixed) += 1;
    std::printf("skeleton %s: version %u, flags %u, %zu axes (%u stock, %u card branches), %zu rings, %zu patch vertices, %zu "
                "patch triangles (%u bark), %zu cards, grid %ux%ux%u of %.2f m, box %.2f..%.2f m high\n",
        skel_path, skel.version, skel.flags, skel.axes.size(), fixed, cards, skel.rings.size(), skel.patch_verts.size(),
        skel.patch_tris.size() / 3, skel.bark_tris, skel.cards.size(), skel.nx, skel.ny, skel.nz, skel.cell, skel.bmin.y, skel.bmax.y);
    Template t;
    const auto t0 = std::chrono::steady_clock::now();
    generate_on_skeleton(arch, skel, seed, up, cap, t);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    uint32_t stock_axes = 0, grafted[4]{};
    for (const Axis& a : t.axes)
        a.fixed ? ++stock_axes : ++grafted[std::min(a.level, 3u)];
    std::printf("tree: %zu axes (%u stock, grafted %u / %u / %u by level), %zu rings, %zu patch triangles (%u bark), %zu segments, "
                "%zu leaves, %zu cards, %zu blooms, %zu clusters, %zu blobs, leaf area %.1f m2, climbable to %.2f m, box %.2f x %.2f "
                "x %.2f, %.0f ms\n",
        t.axes.size(), stock_axes, grafted[1], grafted[2], grafted[3], t.rings.size(), t.patch_tris.size() / 3, t.patch_bark_tris,
        t.segments.size(), t.leaves.size(), t.cards.size(), t.blooms.size(), t.clusters.size(), t.blobs.size(), t.leaf_area,
        t.climb_top, t.bmax.x - t.bmin.x, t.bmax.y - t.bmin.y, t.bmax.z - t.bmin.z, ms);
    if (overlap)
    {
        overlap_stats(t);
        tube_exposure(t);
    }
    if (dump)
        dump_template(t, dump);
    FILE* f = std::fopen(out, "w");
    if (!f)
        return 1;
    Obj o{f};
    const V3 zero = v3(0.f, 0.f, 0.f);
    std::fprintf(f, "o stock\n");
    for (const Axis& a : t.axes)
        if (a.fixed)
            for (uint32_t k = a.first_ring; k + 1 < a.first_ring + a.n_rings; ++k)
                if (!(t.rings[k].cover & t.rings[k + 1].cover & kRingHidden))
                    write_rings(o, t.rings[k], t.rings[k + 1]);
    std::fprintf(f, "o patches\n");
    for (const PatchVert& v : t.patch_verts)
        o.v(v.p);
    for (size_t i = 0; i + 2 < t.patch_tris.size(); i += 3)
        o.face({t.patch_tris[i], t.patch_tris[i + 1], t.patch_tris[i + 2]});
    o.base += uint32_t(t.patch_verts.size());
    std::fprintf(f, "o wood\n");
    for (const Segment& g : t.segments)
        if (!t.axes[g.axis].fixed)
            write_segment(o, g, zero);
    std::fprintf(f, "o leaves\n");
    if (lod == 0)
    {
        for (const Leaf& l : t.leaves)
            write_leaf(o, l, arch, ratio, zero);
        std::fprintf(f, "o leaves_cards\n");
        for (const Leaf& l : t.cards)
            write_card(o, l);
        std::fprintf(f, "o blooms\n");
        for (const Bloom& m : t.blooms)
            write_ball(o, m.p + m.dir * (0.5f * m.size), 0.5f * m.size);
    }
    else if (lod == 1)
        for (const Cluster& c : t.clusters)
            write_disc(o, c.c, c.n, c.r * 0.8f);
    else
        for (const Blob& c : t.blobs)
            write_ball(o, c.c, c.r);
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: shrub_preview <shrubs.ltx> <species> <out.obj> [h,w ...] [--seed n] [--lod 0|1|2]\n");
        return 2;
    }
    const auto ltx = read_ltx(argv[1]);
    const std::string want = argv[2];
    const Section* sec = nullptr;
    for (const auto& [name, s] : ltx)
    {
        if (name == want)
            sec = &s;
        for (const auto& [k, v] : s)
            if (k == "name" && v == want)
                sec = &s;
    }
    if (!sec)
    {
        std::fprintf(stderr, "no species %s\n", want.c_str());
        return 1;
    }
    Arch arch;
    float ratio = 0.55f;
    for (const auto& [k, v] : *sec)
    {
        arch_set(arch, k.c_str(), v.c_str());
        if (k == "leaflet_ratio")
            ratio = std::strtof(v.c_str(), nullptr);
    }

    std::vector<std::pair<float, float>> sizes;
    uint32_t seed = 1, lod = 0, cap = 80000;
    const char* skel = nullptr;
    bool overlap = false;
    const char* dump = nullptr;
    V3 up = v3(0.f, 1.f, 0.f);
    for (int i = 4; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--lod") && i + 1 < argc)
            lod = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--skel") && i + 1 < argc)
            skel = argv[++i];
        else if (!std::strcmp(argv[i], "--cap") && i + 1 < argc)
            cap = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--overlap"))
            overlap = true;
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc)
            dump = argv[++i];
        else if (!std::strcmp(argv[i], "--tilt") && i + 1 < argc)
        {
            float tilt = 0.f, az = 0.f;
            std::sscanf(argv[++i], "%f,%f", &tilt, &az);
            tilt *= 0.0174533f;
            az *= 0.0174533f;
            up = v3(std::sin(tilt) * std::cos(az), std::cos(tilt), std::sin(tilt) * std::sin(az));
        }
        else
        {
            float h = 0.f, w = 0.f;
            if (std::sscanf(argv[i], "%f,%f", &h, &w) == 2)
                sizes.emplace_back(h, w);
        }
    }
    if (skel)
        return grow_tree(arch, ratio, skel, argv[3], seed, lod, cap, up, overlap, dump);
    if (sizes.empty())
        sizes.emplace_back(arch.href, arch.href * 0.8f);

    FILE* f = std::fopen(argv[3], "w");
    if (!f)
        return 1;
    Obj o{f};
    float x = 0.f;
    for (size_t b = 0; b < sizes.size(); ++b)
    {
        Template t;
        const auto t0 = std::chrono::steady_clock::now();
        generate(arch, sizes[b].first, sizes[b].second, seed + uint32_t(b) * 7919u, t);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // Elements outside the bush's own reach: a generator fault.
        for (const Bloom& m : t.blooms)
            if (length(m.p) > 30.f || m.size > 2.f || m.size != m.size)
                std::printf("  bad bloom kind %u axis %u s %.3f p %.2f %.2f %.2f size %.3f\n", uint32_t(m.kind), m.axis, m.s, m.p.x, m.p.y, m.p.z, m.size);
        x += -t.bmin.x + 0.3f;
        const V3 off = v3(x, 0.f, 0.f);
        // The generator's target (da_shrub_gen.cpp leaf_target): the LAI over the crown's larger projection.
        const float rw = 0.5f * sizes[b].second;
        const float target = arch.lai * 3.14159265f * rw * std::max(rw, 0.5f * sizes[b].first);
        std::printf("bush %zu h=%.2f w=%.2f: axes %zu segments %zu leaves %zu blooms %zu clusters %zu blobs %zu "
                    "leaf area %.2f m2 (%.0f%% of the LAI) box %.2f x %.2f x %.2f, %.2f ms\n",
            b, sizes[b].first, sizes[b].second, t.axes.size(), t.segments.size(), t.leaves.size(), t.blooms.size(),
            t.clusters.size(), t.blobs.size(), t.leaf_area, 100.f * t.leaf_area / std::max(target, 1e-3f),
            t.bmax.x - t.bmin.x, t.height, t.bmax.z - t.bmin.z, ms);
        std::fprintf(f, "o wood_%zu\n", b);
        for (const Segment& g : t.segments)
            write_segment(o, g, off);
        if (lod == 0)
        {
            std::fprintf(f, "o leaves_%zu\n", b);
            for (const Leaf& l : t.leaves)
                write_leaf(o, l, arch, ratio, off);
            std::fprintf(f, "o blooms_%zu\n", b);
            for (const Bloom& m : t.blooms)
                write_ball(o, m.p + m.dir * (0.5f * m.size) + off, 0.5f * m.size);
        }
        else if (lod == 1)
        {
            std::fprintf(f, "o leaves_%zu\n", b);
            for (const Cluster& c : t.clusters)
                write_disc(o, c.c + off, c.n, c.r * 0.8f);
        }
        else
        {
            std::fprintf(f, "o leaves_%zu\n", b);
            for (const Blob& c : t.blobs)
                write_ball(o, c.c + off, c.r);
        }
        x += t.bmax.x + 0.3f;
    }
    std::fclose(f);
    return 0;
}
