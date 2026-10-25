// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The mesh simplifier (da_mesh_simplify.h).
//
// Each vertex is a wedge: a corner's position with its attributes; vertices sharing a position are welded into one
// position, and an edge is a pair of positions. A vertex's quadric is the sum of its triangles': the squared distance to
// the triangle's plane and, per attribute, to the triangle's linear attribute field (its gradient along the triangle,
// none across it), each weighted by the triangle's area. A collapse moves the removed position onto the kept one and
// hands each of its vertices to the kept position's vertex whose attributes fit its triangles best (the same seam id
// only): its cost is the merged quadrics at the kept position with those attributes, plus the border lines' constraints
// and the penalties. Positions are scaled so the mean triangle is a quarter of a unit across, the attribute weights read
// against that. A collapse that would turn a triangle over or move a locked position is not taken at all: a crack
// between a group and its neighbours, or a hole in a level of detail, never pays.
#include "da_mesh_simplify.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace da_mesh
{
namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
constexpr u32 kNone = ~0u;

struct D3
{
    double x{}, y{}, z{};
};
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(D3 a) { return std::sqrt(dot(a, a)); }

// A quadric's layout in doubles: the area, A (xx xy xz yy yz zz), b, c, then per attribute alpha, beta (3), gamma.
// E(p, s) = p'Ap + 2b'p + c + sum_j (alpha_j s_j^2 - 2 s_j (beta_j.p + gamma_j)).
constexpr u32 kQa = 0, kQA = 1, kQb = 7, kQc = 10, kQhead = 11;
// A border line's quadric: A, b, c.
constexpr u32 kEq = 10;

// The squared distance from p to the triangle (a, b, c), by the region of its closest point (Ericson, "Real-Time
// Collision Detection", 5.1.5).
double point_triangle_d2(D3 p, D3 a, D3 b, D3 c)
{
    const D3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return dot(ap, ap);
    const D3 bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return dot(bp, bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
    {
        const D3 r = p - (a + ab * (d1 / (d1 - d3)));
        return dot(r, r);
    }
    const D3 cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return dot(cp, cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
    {
        const D3 r = p - (a + ac * (d2 / (d2 - d6)));
        return dot(r, r);
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0)
    {
        const D3 r = p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))));
        return dot(r, r);
    }
    const double denom = 1.0 / (va + vb + vc);
    const D3 r = p - (a + ab * (vb * denom) + ac * (vc * denom));
    return dot(r, r);
}

u64 pair_key(u32 a, u32 b) { return (u64(a) << 32) | b; }

class Simplifier
{
public:
    Simplifier(const SimplifyInput& in_, std::vector<u32>& indices_, const SimplifyOptions& opt_)
        : in(in_), opt(opt_), indices(indices_), n(in_.attr_weights ? in_.attr_count : 0), stride(kQhead + 5 * n)
    {
    }

    SimplifyResult run();

private:
    struct Tri
    {
        u32 v[3];
        bool alive;
    };
    struct Entry
    {
        float cost;
        u32 keep, remove;
        u32 stamp_keep, stamp_remove;
        bool operator>(const Entry& o) const { return cost > o.cost; }
    };
    using Heap = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;

    const SimplifyInput& in;
    const SimplifyOptions& opt;
    std::vector<u32>& indices;
    const u32 n, stride;

    double scale{1.0};
    std::vector<D3> pos; // per position, scaled
    std::vector<u32> pos_of; // per vertex
    std::vector<u32> seam_of; // per vertex: its first triangle's seam id
    std::vector<u32> uses; // per vertex: alive corners
    std::vector<std::vector<u32>> adj; // per position: alive triangles
    std::vector<std::vector<u32>> verts_at; // per position: the vertices in use there
    std::vector<uint8_t> locked; // per position
    std::vector<uint8_t> had_tris; // per position: in use at the start
    std::vector<u32> stamp; // per position
    std::vector<u32> parent; // per position: where a collapse sent it
    std::vector<Tri> tris;
    std::vector<double> q; // per vertex
    std::vector<double> eq; // per position
    std::vector<double> scratch;
    u32 alive_tris{};
    float max_cost{};

    // An evaluation's mapping: each removed vertex and the kept position's vertex it goes to.
    std::vector<u32> map_from, map_to;
    std::vector<u32> survivors, ring_keep, opposite;

    const double* Q(u32 v) const { return &q[size_t(v) * stride]; }
    double* Q(u32 v) { return &q[size_t(v) * stride]; }
    const float* attr(u32 v) const { return n ? in.attributes + size_t(v) * in.attr_count : nullptr; }
    D3 corner(u32 t, u32 c) const { return pos[pos_of[tris[t].v[c]]]; }
    bool has_pos(u32 t, u32 p) const
    {
        const Tri& T = tris[t];
        return pos_of[T.v[0]] == p || pos_of[T.v[1]] == p || pos_of[T.v[2]] == p;
    }

    double evaluate(const double* Qv, D3 p, const float* s) const;
    static double evaluate_line(const double* E, D3 p);
    void add_tri_quadric(u32 t);
    void add_border_lines();
    void add_line(u32 p0, u32 p1, D3 nrm, double weight);
    bool edge_exists(u32 a, u32 b) const;
    bool plan(u32 keep, u32 remove, float& cost);
    void push_edge(Heap& heap, u32 a, u32 b);
    void collapse(u32 keep, u32 remove);
    void unlink(u32 t, u32 skip_a = kNone, u32 skip_b = kNone);
    void retire(u32 v);
    u32 find(u32 p)
    {
        while (parent[p] != p)
        {
            parent[p] = parent[parent[p]];
            p = parent[p];
        }
        return p;
    }
};

double Simplifier::evaluate(const double* Qv, D3 p, const float* s) const
{
    const double* A = Qv + kQA;
    const double* b = Qv + kQb;
    double e = A[0] * p.x * p.x + 2.0 * A[1] * p.x * p.y + 2.0 * A[2] * p.x * p.z + A[3] * p.y * p.y +
        2.0 * A[4] * p.y * p.z + A[5] * p.z * p.z + 2.0 * (b[0] * p.x + b[1] * p.y + b[2] * p.z) + Qv[kQc];
    const double* at = Qv + kQhead;
    for (u32 j = 0; j < n; ++j, at += 5)
    {
        if (at[0] <= 0.0)
            continue;
        const double sj = s[j];
        e += at[0] * sj * sj - 2.0 * sj * (at[1] * p.x + at[2] * p.y + at[3] * p.z + at[4]);
    }
    return std::max(e, 0.0);
}

double Simplifier::evaluate_line(const double* E, D3 p)
{
    const double e = E[0] * p.x * p.x + 2.0 * E[1] * p.x * p.y + 2.0 * E[2] * p.x * p.z + E[3] * p.y * p.y +
        2.0 * E[4] * p.y * p.z + E[5] * p.z * p.z + 2.0 * (E[6] * p.x + E[7] * p.y + E[8] * p.z) + E[9];
    return std::max(e, 0.0);
}

void Simplifier::add_tri_quadric(u32 t)
{
    const D3 p0 = corner(t, 0), p1 = corner(t, 1), p2 = corner(t, 2);
    const D3 e1 = p1 - p0, e2 = p2 - p0;
    D3 nrm = cross(e1, e2);
    const double len = length(nrm);
    if (len < 1e-14)
        return;
    const double area = 0.5 * len;
    nrm = nrm * (1.0 / len);
    const double d0 = -dot(nrm, p0);

    double* T = scratch.data();
    std::fill(scratch.begin(), scratch.end(), 0.0);
    T[kQa] = area;
    double* A = T + kQA;
    double* b = T + kQb;
    const auto add_plane = [&](D3 g, double d, double w) {
        A[0] += w * g.x * g.x;
        A[1] += w * g.x * g.y;
        A[2] += w * g.x * g.z;
        A[3] += w * g.y * g.y;
        A[4] += w * g.y * g.z;
        A[5] += w * g.z * g.z;
        b[0] += w * d * g.x;
        b[1] += w * d * g.y;
        b[2] += w * d * g.z;
        T[kQc] += w * d * d;
    };
    add_plane(nrm, d0, area);

    // Each attribute's field over the triangle: the gradient g with g.e1 = ds1, g.e2 = ds2, g.nrm = 0 (the rows'
    // inverse by their cross products), and its value at the origin d = s0 - g.p0.
    if (n)
    {
        const D3 c0 = cross(e2, nrm), c1 = cross(nrm, e1);
        const double det = dot(e1, c0);
        if (std::abs(det) > 1e-20)
        {
            const double inv = 1.0 / det;
            const Tri& Tr = tris[t];
            const float* s0 = attr(Tr.v[0]);
            const float* s1 = attr(Tr.v[1]);
            const float* s2 = attr(Tr.v[2]);
            double* at = T + kQhead;
            for (u32 j = 0; j < n; ++j, at += 5)
            {
                const double w = in.attr_weights[j];
                if (w == 0.0)
                    continue;
                const double ds1 = double(s1[j]) - s0[j], ds2 = double(s2[j]) - s0[j];
                const D3 g = (c0 * ds1 + c1 * ds2) * inv;
                const double d = s0[j] - dot(g, p0);
                const double ww = w * w * area;
                add_plane(g, d, ww);
                at[0] += ww;
                at[1] += ww * g.x;
                at[2] += ww * g.y;
                at[3] += ww * g.z;
                at[4] += ww * d;
            }
        }
    }
    for (u32 c = 0; c < 3; ++c)
    {
        double* Qv = Q(tris[t].v[c]);
        for (u32 i = 0; i < stride; ++i)
            Qv[i] += T[i];
    }
}

void Simplifier::add_line(u32 p0, u32 p1, D3 nrm, double weight)
{
    const D3 e = pos[p1] - pos[p0];
    D3 m = cross(e, nrm);
    const double len = length(m);
    if (len < 1e-14)
        return;
    m = m * (1.0 / len);
    const double d = -dot(m, pos[p0]);
    const double row[kEq] = {m.x * m.x, m.x * m.y, m.x * m.z, m.y * m.y, m.y * m.z, m.z * m.z, d * m.x, d * m.y,
        d * m.z, d * d};
    for (u32 p : {p0, p1})
    {
        double* E = &eq[size_t(p) * kEq];
        for (u32 i = 0; i < kEq; ++i)
            E[i] += weight * row[i];
    }
}

// A half-edge with no opposite of the same vertices and seam id is constrained to its line: a mesh's open border at the
// full weight, a seam (the opposite there by position only) half on each side, so its two sides count once.
void Simplifier::add_border_lines()
{
    std::unordered_map<u64, u32> half;
    std::unordered_set<u64> half_pos;
    half.reserve(tris.size() * 3);
    half_pos.reserve(tris.size() * 3);
    for (u32 t = 0; t < tris.size(); ++t)
    {
        if (!tris[t].alive)
            continue;
        const u32 id = in.tri_ids ? in.tri_ids[t] : 0;
        for (u32 c = 0; c < 3; ++c)
        {
            const u32 a = tris[t].v[c], b = tris[t].v[(c + 1) % 3];
            half.emplace(pair_key(a, b), id);
            half_pos.insert(pair_key(pos_of[a], pos_of[b]));
        }
    }
    for (u32 t = 0; t < tris.size(); ++t)
    {
        if (!tris[t].alive)
            continue;
        const u32 id = in.tri_ids ? in.tri_ids[t] : 0;
        const D3 p0 = corner(t, 0), p1 = corner(t, 1), p2 = corner(t, 2);
        D3 nrm = cross(p1 - p0, p2 - p0);
        const double len = length(nrm);
        if (len < 1e-14)
            continue;
        nrm = nrm * (1.0 / len);
        for (u32 c = 0; c < 3; ++c)
        {
            const u32 a = tris[t].v[c], b = tris[t].v[(c + 1) % 3];
            const auto it = half.find(pair_key(b, a));
            if (it != half.end() && it->second == id)
                continue;
            const bool seam = half_pos.count(pair_key(pos_of[b], pos_of[a])) != 0;
            const double weight = opt.edge_weight * length(pos[pos_of[b]] - pos[pos_of[a]]) * (seam ? 0.5 : 1.0);
            add_line(pos_of[a], pos_of[b], nrm, weight);
        }
    }
}

bool Simplifier::edge_exists(u32 a, u32 b) const
{
    for (u32 t : adj[a])
        if (has_pos(t, b))
            return true;
    return false;
}

// The collapse of `remove` onto `keep`: false where it may not be taken; its cost, and the vertex mapping in map_from /
// map_to.
bool Simplifier::plan(u32 keep, u32 remove, float& cost)
{
    if (locked[remove])
        return false;
    const D3 pk = pos[keep];
    u32 shared = 0;
    survivors.clear();
    for (u32 t : adj[remove])
    {
        if (has_pos(t, keep))
        {
            ++shared;
            continue;
        }
        // The triangle with its corner moved: it may not turn over, nor fold flat.
        D3 c[3], o[3];
        for (u32 i = 0; i < 3; ++i)
        {
            o[i] = corner(t, i);
            c[i] = pos_of[tris[t].v[i]] == remove ? pk : o[i];
            if (pos_of[tris[t].v[i]] == remove)
                survivors.push_back(tris[t].v[i]);
        }
        const D3 n_old = cross(o[1] - o[0], o[2] - o[0]);
        const D3 n_new = cross(c[1] - c[0], c[2] - c[0]);
        const double l_old = length(n_old);
        if (l_old > 1e-14 && dot(n_old, n_new) <= 1e-3 * l_old * length(n_new))
            return false;
    }
    if (!shared)
        return false;

    // The link condition (Dey et al. 1999): a position next to both ends must be the far corner of a triangle on the
    // edge, or the collapse pinches the surface into an edge of three or four triangles.
    {
        ring_keep.clear();
        opposite.clear();
        for (u32 t : adj[keep])
            for (u32 c = 0; c < 3; ++c)
            {
                const u32 p = pos_of[tris[t].v[c]];
                if (p != keep)
                    ring_keep.push_back(p);
            }
        for (u32 t : adj[remove])
            if (has_pos(t, keep))
                for (u32 c = 0; c < 3; ++c)
                {
                    const u32 p = pos_of[tris[t].v[c]];
                    if (p != keep && p != remove)
                        opposite.push_back(p);
                }
        for (u32 t : adj[remove])
            for (u32 c = 0; c < 3; ++c)
            {
                const u32 p = pos_of[tris[t].v[c]];
                if (p == remove || p == keep)
                    continue;
                if (std::find(ring_keep.begin(), ring_keep.end(), p) != ring_keep.end() &&
                    std::find(opposite.begin(), opposite.end(), p) == opposite.end())
                    return false;
            }
    }

    map_from.clear();
    map_to.clear();
    const std::vector<u32>& kept = verts_at[keep];
    for (u32 v : verts_at[remove])
    {
        const bool lives = std::find(survivors.begin(), survivors.end(), v) != survivors.end();
        u32 best = kNone;
        double best_e = std::numeric_limits<double>::max();
        for (u32 t : kept)
        {
            if (lives && seam_of[t] != seam_of[v])
                continue;
            const double e = evaluate(Q(v), pk, attr(t));
            if (e < best_e)
            {
                best_e = e;
                best = t;
            }
        }
        // A vertex whose triangles all fold away has no attributes to keep: its quadric goes to the best fit.
        if (best == kNone)
            return false;
        map_from.push_back(v);
        map_to.push_back(best);
    }

    double wedges = 0.0, area = 0.0;
    for (u32 t : kept)
    {
        double e = evaluate(Q(t), pk, attr(t));
        double a = Q(t)[kQa];
        for (size_t i = 0; i < map_from.size(); ++i)
        {
            if (map_to[i] != t)
                continue;
            e += evaluate(Q(map_from[i]), pk, attr(t));
            a += Q(map_from[i])[kQa];
        }
        wedges += opt.limit_error_to_area ? std::min(e, a) : e;
        area += a;
    }
    double c = opt.limit_error_to_area ? std::min(wedges, area) : wedges;
    c += evaluate_line(&eq[size_t(keep) * kEq], pk) + evaluate_line(&eq[size_t(remove) * kEq], pk);
    const u32 degree = u32(adj[keep].size() + adj[remove].size()) - 2 * shared;
    if (degree > opt.degree_limit)
        c += double(opt.degree_penalty) * (degree - opt.degree_limit);
    cost = float(c);
    return true;
}

void Simplifier::push_edge(Heap& heap, u32 a, u32 b)
{
    float ca = 0.f, cb = 0.f;
    const bool va = plan(a, b, ca);
    const bool vb = plan(b, a, cb);
    if (!va && !vb)
        return;
    if (va && (!vb || ca <= cb))
        heap.push({ca, a, b, stamp[a], stamp[b]});
    else
        heap.push({cb, b, a, stamp[b], stamp[a]});
}

// A triangle gone; a vertex left without triangles retires, except at the positions a collapse is merging (it maps
// those itself).
void Simplifier::unlink(u32 t, u32 skip_a, u32 skip_b)
{
    Tri& T = tris[t];
    T.alive = false;
    --alive_tris;
    for (u32 c = 0; c < 3; ++c)
    {
        const u32 v = T.v[c], p = pos_of[v];
        std::vector<u32>& list = adj[p];
        const auto it = std::find(list.begin(), list.end(), t);
        if (it != list.end())
        {
            *it = list.back();
            list.pop_back();
        }
        if (!--uses[v] && p != skip_a && p != skip_b)
            retire(v);
    }
}

// A vertex with no triangles left hands its quadric to its position's best fit - of its seam if one is there - so the
// surface it stood for is still remembered.
void Simplifier::retire(u32 v)
{
    std::vector<u32>& list = verts_at[pos_of[v]];
    const auto it = std::find(list.begin(), list.end(), v);
    if (it != list.end())
    {
        *it = list.back();
        list.pop_back();
    }
    u32 best = kNone;
    for (u32 pass = 0; pass < 2 && best == kNone; ++pass)
    {
        double best_e = std::numeric_limits<double>::max();
        for (u32 t : list)
        {
            if (!uses[t] || (pass == 0 && seam_of[t] != seam_of[v]))
                continue;
            const double e = evaluate(Q(v), pos[pos_of[v]], attr(t));
            if (e < best_e)
            {
                best_e = e;
                best = t;
            }
        }
    }
    if (best == kNone)
        return;
    double* dst = Q(best);
    const double* src = Q(v);
    for (u32 k = 0; k < stride; ++k)
        dst[k] += src[k];
}

void Simplifier::collapse(u32 keep, u32 remove)
{
    // The triangles on the edge fold away.
    const std::vector<u32> around = adj[remove];
    for (u32 t : around)
        if (tris[t].alive && has_pos(t, keep))
            unlink(t, keep, remove);
    // The rest move their corner onto the kept position, each vertex to its mapped one.
    for (u32 t : adj[remove])
    {
        Tri& T = tris[t];
        for (u32 c = 0; c < 3; ++c)
        {
            if (pos_of[T.v[c]] != remove)
                continue;
            const size_t i = std::find(map_from.begin(), map_from.end(), T.v[c]) - map_from.begin();
            const u32 to = map_to[i];
            --uses[T.v[c]];
            ++uses[to];
            T.v[c] = to;
        }
        adj[keep].push_back(t);
    }
    adj[remove].clear();
    for (size_t i = 0; i < map_from.size(); ++i)
    {
        double* dst = Q(map_to[i]);
        const double* src = Q(map_from[i]);
        for (u32 k = 0; k < stride; ++k)
            dst[k] += src[k];
    }
    double* ek = &eq[size_t(keep) * kEq];
    const double* er = &eq[size_t(remove) * kEq];
    for (u32 k = 0; k < kEq; ++k)
        ek[k] += er[k];
    verts_at[remove].clear();

    // The kept position's vertices whose triangles all folded away retire into its others.
    const std::vector<u32> kept = verts_at[keep];
    for (u32 v : kept)
        if (!uses[v])
            retire(v);

    // Two triangles now on the same three positions the same way round: one goes.
    std::vector<u32>& ring = adj[keep];
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const Tri& A = tris[ring[i]];
        const u32 a0 = pos_of[A.v[0]], a1 = pos_of[A.v[1]], a2 = pos_of[A.v[2]];
        for (size_t j = i + 1; j < ring.size(); ++j)
        {
            const Tri& B = tris[ring[j]];
            const u32 b0 = pos_of[B.v[0]], b1 = pos_of[B.v[1]], b2 = pos_of[B.v[2]];
            const bool same = (a0 == b0 && a1 == b1 && a2 == b2) || (a0 == b1 && a1 == b2 && a2 == b0) ||
                (a0 == b2 && a1 == b0 && a2 == b1);
            if (same)
            {
                unlink(ring[j]);
                --j;
            }
        }
    }
    parent[remove] = keep;
    locked[keep] |= locked[remove];
}

SimplifyResult Simplifier::run()
{
    SimplifyResult result;
    const u32 tri_count = u32(indices.size() / 3);

    // Welded positions.
    pos_of.assign(in.vertex_count, kNone);
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
        std::unordered_map<Key, u32, Hash> weld;
        weld.reserve(in.vertex_count);
        std::vector<D3> raw;
        for (u32 t = 0; t < tri_count; ++t)
            for (u32 c = 0; c < 3; ++c)
            {
                const u32 v = indices[t * 3 + c];
                if (pos_of[v] != kNone)
                    continue;
                const float* p = in.positions + size_t(v) * 3;
                // Minus zero is zero: a position's bits are compared.
                const float x = p[0] + 0.f, y = p[1] + 0.f, z = p[2] + 0.f;
                Key k;
                std::memcpy(&k.x, &x, 4);
                std::memcpy(&k.y, &y, 4);
                std::memcpy(&k.z, &z, 4);
                const auto [it, fresh] = weld.emplace(k, u32(raw.size()));
                if (fresh)
                    raw.push_back({p[0], p[1], p[2]});
                pos_of[v] = it->second;
            }
        pos = std::move(raw);
    }
    const u32 npos = u32(pos.size());

    // The scale: the mean triangle a quarter of a unit across, by a power of two (exact in floats).
    {
        double area = 0.0;
        u32 counted = 0;
        for (u32 t = 0; t < tri_count; ++t)
        {
            const D3 a = pos[pos_of[indices[t * 3]]], b = pos[pos_of[indices[t * 3 + 1]]], c = pos[pos_of[indices[t * 3 + 2]]];
            const double s = 0.5 * length(cross(b - a, c - a));
            if (s > 0.0)
            {
                area += s;
                ++counted;
            }
        }
        const double size = counted ? std::sqrt(area / counted) : 1.0;
        scale = opt.position_scale > 0.f ? double(opt.position_scale) :
            (size > 0.0 ? std::exp2(std::floor(std::log2(0.25)) - std::floor(std::log2(size))) : 1.0);
        for (D3& p : pos)
            p = p * scale;
    }

    // Triangles: those with two corners on one position are dropped.
    tris.resize(tri_count);
    adj.assign(npos, {});
    verts_at.assign(npos, {});
    uses.assign(in.vertex_count, 0);
    seam_of.assign(in.vertex_count, kNone);
    for (u32 t = 0; t < tri_count; ++t)
    {
        Tri& T = tris[t];
        for (u32 c = 0; c < 3; ++c)
            T.v[c] = indices[t * 3 + c];
        const u32 p0 = pos_of[T.v[0]], p1 = pos_of[T.v[1]], p2 = pos_of[T.v[2]];
        T.alive = p0 != p1 && p1 != p2 && p0 != p2;
        if (!T.alive)
            continue;
        ++alive_tris;
        for (u32 c = 0; c < 3; ++c)
        {
            const u32 v = T.v[c];
            adj[pos_of[v]].push_back(t);
            if (!uses[v]++)
                verts_at[pos_of[v]].push_back(v);
            if (seam_of[v] == kNone)
                seam_of[v] = in.tri_ids ? in.tri_ids[t] : 0;
        }
    }
    locked.assign(npos, 0);
    if (in.locked)
        for (u32 v = 0; v < in.vertex_count; ++v)
            if (in.locked[v] && pos_of[v] != kNone)
                locked[pos_of[v]] = 1;
    had_tris.assign(npos, 0);
    for (u32 p = 0; p < npos; ++p)
        had_tris[p] = adj[p].empty() ? 0 : 1;
    stamp.assign(npos, 0);
    parent.resize(npos);
    for (u32 p = 0; p < npos; ++p)
        parent[p] = p;

    // Quadrics.
    q.assign(size_t(in.vertex_count) * stride, 0.0);
    eq.assign(size_t(npos) * kEq, 0.0);
    scratch.assign(stride, 0.0);
    for (u32 t = 0; t < tri_count; ++t)
        if (tris[t].alive)
            add_tri_quadric(t);
    add_border_lines();

    // The edges.
    Heap heap;
    {
        std::unordered_set<u64> seen;
        seen.reserve(size_t(alive_tris) * 2);
        for (u32 t = 0; t < tri_count; ++t)
        {
            if (!tris[t].alive)
                continue;
            for (u32 c = 0; c < 3; ++c)
            {
                u32 a = pos_of[tris[t].v[c]], b = pos_of[tris[t].v[(c + 1) % 3]];
                if (a > b)
                    std::swap(a, b);
                if (seen.insert(pair_key(a, b)).second)
                    push_edge(heap, a, b);
            }
        }
    }

    std::vector<u32> ring;
    while (alive_tris > opt.target_tris && !heap.empty())
    {
        const Entry e = heap.top();
        heap.pop();
        if (adj[e.keep].empty() || adj[e.remove].empty())
            continue;
        if (stamp[e.keep] != e.stamp_keep || stamp[e.remove] != e.stamp_remove)
        {
            // Its neighbourhood changed since: worked out again, if it is still an edge.
            if (edge_exists(e.keep, e.remove))
                push_edge(heap, e.keep, e.remove);
            continue;
        }
        float cost = 0.f;
        if (!plan(e.keep, e.remove, cost))
            continue;
        collapse(e.keep, e.remove);
        max_cost = std::max(max_cost, cost);

        ring.clear();
        for (u32 t : adj[e.keep])
            for (u32 c = 0; c < 3; ++c)
            {
                const u32 p = pos_of[tris[t].v[c]];
                if (p != e.keep && std::find(ring.begin(), ring.end(), p) == ring.end())
                    ring.push_back(p);
            }
        ++stamp[e.keep];
        for (u32 p : ring)
            ++stamp[p];
        for (u32 p : ring)
            push_edge(heap, e.keep, p);
    }

    // The deviation: every removed position against the triangles round the one it went into.
    double dev2 = 0.0;
    for (u32 p = 0; p < npos; ++p)
    {
        if (!had_tris[p] || parent[p] == p)
            continue;
        const u32 r = find(p);
        double d2 = std::numeric_limits<double>::max();
        for (u32 t : adj[r])
            d2 = std::min(d2, point_triangle_d2(pos[p], corner(t, 0), corner(t, 1), corner(t, 2)));
        if (adj[r].empty())
            d2 = dot(pos[p] - pos[r], pos[p] - pos[r]);
        dev2 = std::max(dev2, d2);
    }
    result.deviation = float(std::sqrt(dev2) / scale);
    result.max_cost = max_cost;

    indices.clear();
    for (const Tri& T : tris)
        if (T.alive)
            indices.insert(indices.end(), {T.v[0], T.v[1], T.v[2]});
    result.tris = u32(indices.size() / 3);
    return result;
}
} // namespace

SimplifyResult simplify(const SimplifyInput& in, std::vector<uint32_t>& indices, const SimplifyOptions& opt)
{
    if (indices.size() < 3 || !in.positions)
        return {0.f, 0.f, uint32_t(indices.size() / 3)};
    Simplifier s(in, indices, opt);
    return s.run();
}
} // namespace da_mesh
