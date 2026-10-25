// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The graph partitioner (da_graph_partition.h).
#include "da_graph_partition.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <random>

namespace da_mesh
{
namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

// A graph with node weights: the input's nodes weigh 1, a coarse node what it merged.
struct Level
{
    std::vector<u32> xadj, adj;
    std::vector<i32> wgt;
    std::vector<i32> vw;
    u32 nodes() const { return u32(vw.size()); }
    i64 total() const { return std::accumulate(vw.begin(), vw.end(), i64(0)); }
};

// Heavy-edge matching: each node with its unmatched neighbour of the heaviest edge (no coarse node past a cap, so the
// balance stays reachable). Returns false when it no longer shrinks the graph.
bool coarsen(const Level& f, Level& c, std::vector<u32>& map, std::mt19937& rng, i32 cap)
{
    const u32 n = f.nodes();
    std::vector<u32> perm(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::shuffle(perm.begin(), perm.end(), rng);
    std::vector<u32> match(n, ~0u);
    map.assign(n, ~0u);
    u32 cn = 0;
    for (u32 u : perm)
    {
        if (match[u] != ~0u)
            continue;
        u32 best = ~0u;
        i32 best_w = -1;
        for (u32 e = f.xadj[u]; e < f.xadj[u + 1]; ++e)
        {
            const u32 v = f.adj[e];
            if (match[v] != ~0u || v == u || f.vw[u] + f.vw[v] > cap)
                continue;
            if (f.wgt[e] > best_w)
            {
                best_w = f.wgt[e];
                best = v;
            }
        }
        match[u] = best == ~0u ? u : best;
        if (best != ~0u)
            match[best] = u;
        map[u] = cn;
        if (best != ~0u)
            map[best] = cn;
        ++cn;
    }
    if (cn > n * 95 / 100)
        return false;

    c.vw.assign(cn, 0);
    for (u32 u = 0; u < n; ++u)
        c.vw[map[u]] += f.vw[u];
    // The coarse rows: each coarse node's members' edges, merged by a marker array.
    std::vector<std::vector<u32>> members(cn);
    for (u32 u = 0; u < n; ++u)
        members[map[u]].push_back(u);
    std::vector<i32> slot(cn, -1);
    c.xadj.assign(1, 0);
    c.adj.clear();
    c.wgt.clear();
    for (u32 cu = 0; cu < cn; ++cu)
    {
        const u32 row = u32(c.adj.size());
        for (u32 u : members[cu])
            for (u32 e = f.xadj[u]; e < f.xadj[u + 1]; ++e)
            {
                const u32 cv = map[f.adj[e]];
                if (cv == cu)
                    continue;
                if (slot[cv] < 0)
                {
                    slot[cv] = i32(c.adj.size());
                    c.adj.push_back(cv);
                    c.wgt.push_back(f.wgt[e]);
                }
                else
                {
                    c.wgt[slot[cv]] += f.wgt[e];
                }
            }
        for (u32 e = row; e < c.adj.size(); ++e)
            slot[c.adj[e]] = -1;
        c.xadj.push_back(u32(c.adj.size()));
    }
    return true;
}

i64 cut_of(const Level& g, const std::vector<uint8_t>& side)
{
    i64 cut = 0;
    for (u32 u = 0; u < g.nodes(); ++u)
        for (u32 e = g.xadj[u]; e < g.xadj[u + 1]; ++e)
            if (side[u] != side[g.adj[e]])
                cut += g.wgt[e];
    return cut / 2;
}

// Greedy growing on the coarsest level: a region grown from a seed by the node most tied to it, until it holds the left
// side's target; disconnected pieces start a new seed. The best of a few seeds by cut, within the window if any.
void grow(const Level& g, i64 target, i64 lo, i64 hi, std::vector<uint8_t>& best_side, std::mt19937& rng)
{
    const u32 n = g.nodes();
    i64 best_cut = -1;
    bool best_in = false;
    std::vector<uint8_t> side;
    std::vector<i64> tie(n);
    const u32 tries = std::min<u32>(n, 8);
    std::uniform_int_distribution<u32> pick(0, n - 1);
    for (u32 attempt = 0; attempt < tries; ++attempt)
    {
        side.assign(n, 1);
        std::fill(tie.begin(), tie.end(), 0);
        using Item = std::pair<i64, u32>;
        std::priority_queue<Item> frontier;
        i64 w = 0;
        u32 seed = pick(rng);
        while (w < target)
        {
            if (frontier.empty())
            {
                // A new seed in what is left (the next unassigned node after the last).
                u32 tries_seed = 0;
                while (side[seed] == 0 && tries_seed++ < n)
                    seed = (seed + 1) % n;
                if (side[seed] == 0)
                    break;
                frontier.push({0, seed});
            }
            const auto [t, u] = frontier.top();
            frontier.pop();
            if (side[u] == 0 || t != tie[u])
                continue;
            if (w + g.vw[u] > hi && w >= lo)
                break;
            side[u] = 0;
            w += g.vw[u];
            for (u32 e = g.xadj[u]; e < g.xadj[u + 1]; ++e)
            {
                const u32 v = g.adj[e];
                if (side[v] == 0)
                    continue;
                tie[v] += g.wgt[e];
                frontier.push({tie[v], v});
            }
        }
        const bool in = w >= lo && w <= hi;
        const i64 cut = cut_of(g, side);
        if (best_cut < 0 || (in && !best_in) || (in == best_in && cut < best_cut))
        {
            best_cut = cut;
            best_in = in;
            best_side = side;
        }
    }
}

// Fiduccia-Mattheyses: passes of single moves by the best gain that keep the left weight in [lo, hi] (or bring it
// nearer while outside), each pass rolled back to its best prefix; a few passes until one gains nothing.
void refine(const Level& g, i64 lo, i64 hi, std::vector<uint8_t>& side)
{
    const u32 n = g.nodes();
    std::vector<i64> gain(n);
    std::vector<uint8_t> moved(n);
    std::vector<u32> stamp(n, 0);
    i64 left = 0;
    for (u32 u = 0; u < n; ++u)
        if (side[u] == 0)
            left += g.vw[u];
    const auto compute_gain = [&](u32 u) {
        i64 ext = 0, in = 0;
        for (u32 e = g.xadj[u]; e < g.xadj[u + 1]; ++e)
            (side[g.adj[e]] != side[u] ? ext : in) += g.wgt[e];
        return ext - in;
    };
    const auto off = [&](i64 l) { return l < lo ? lo - l : (l > hi ? l - hi : 0); };

    for (u32 pass = 0; pass < 8; ++pass)
    {
        struct Item
        {
            i64 gain;
            u32 node, stamp;
            bool operator<(const Item& o) const { return gain < o.gain; }
        };
        std::priority_queue<Item> heap[2];
        std::fill(moved.begin(), moved.end(), 0);
        for (u32 u = 0; u < n; ++u)
        {
            gain[u] = compute_gain(u);
            // Only the boundary, and anything while the balance is off.
            bool boundary = false;
            for (u32 e = g.xadj[u]; e < g.xadj[u + 1] && !boundary; ++e)
                boundary = side[g.adj[e]] != side[u];
            if (boundary || off(left))
                heap[side[u]].push({gain[u], u, ++stamp[u]});
        }
        std::vector<u32> moves;
        i64 run = 0, best_run = 0;
        size_t best_len = 0;
        i64 best_off = off(left);
        u32 since_best = 0;
        while (since_best < 64)
        {
            // The best move of each side that does not take the balance further off; while it is off the one that
            // brings it nearest, else the better gain.
            int from = -1;
            Item pick{};
            i64 pick_off = 0;
            for (int s = 0; s < 2; ++s)
            {
                auto& h = heap[s];
                while (!h.empty() && (moved[h.top().node] || h.top().stamp != stamp[h.top().node] || side[h.top().node] != s))
                    h.pop();
                if (h.empty())
                    continue;
                const Item it = h.top();
                const i64 after = off(s == 0 ? left - g.vw[it.node] : left + g.vw[it.node]);
                if (after > off(left) && after > 0)
                    continue;
                const bool better = from < 0 || (off(left) > 0 ? after < pick_off || (after == pick_off && it.gain > pick.gain)
                                                               : it.gain > pick.gain);
                if (better)
                {
                    from = s;
                    pick = it;
                    pick_off = after;
                }
            }
            if (from < 0)
                break;
            const u32 u = pick.node;
            heap[from].pop();
            moved[u] = 1;
            side[u] = uint8_t(1 - from);
            left += from == 0 ? -g.vw[u] : g.vw[u];
            run += pick.gain;
            moves.push_back(u);
            for (u32 e = g.xadj[u]; e < g.xadj[u + 1]; ++e)
            {
                const u32 v = g.adj[e];
                if (moved[v])
                    continue;
                gain[v] = compute_gain(v);
                heap[side[v]].push({gain[v], v, ++stamp[v]});
            }
            const i64 o = off(left);
            if (o < best_off || (o == best_off && run > best_run))
            {
                best_off = o;
                best_run = run;
                best_len = moves.size();
                since_best = 0;
            }
            else
            {
                ++since_best;
            }
        }
        // Back to the best prefix.
        for (size_t i = moves.size(); i > best_len; --i)
        {
            const u32 u = moves[i - 1];
            left += side[u] == 0 ? -g.vw[u] : g.vw[u];
            side[u] = uint8_t(1 - side[u]);
        }
        if (best_len == 0 || (best_run <= 0 && best_off == 0 && pass > 0))
            break;
    }
}

// One bisection of the level: the left side's weight in [lo, hi].
void bisect(const Level& g, i64 target, i64 lo, i64 hi, std::vector<uint8_t>& side, std::mt19937& rng)
{
    std::vector<Level> levels;
    std::vector<std::vector<u32>> maps;
    levels.push_back(g);
    const i32 cap = i32(std::max<i64>(1, (hi - lo) > 0 ? std::max<i64>(g.total() / 32, 1) : 1));
    while (levels.back().nodes() > 48)
    {
        Level c;
        std::vector<u32> map;
        if (!coarsen(levels.back(), c, map, rng, std::max<i32>(cap, 2)))
            break;
        levels.push_back(std::move(c));
        maps.push_back(std::move(map));
    }
    grow(levels.back(), target, lo, hi, side, rng);
    refine(levels.back(), lo, hi, side);
    for (size_t l = levels.size() - 1; l > 0; --l)
    {
        const std::vector<u32>& map = maps[l - 1];
        std::vector<uint8_t> fine(levels[l - 1].nodes());
        for (u32 u = 0; u < fine.size(); ++u)
            fine[u] = side[map[u]];
        side = std::move(fine);
        refine(levels[l - 1], lo, hi, side);
    }
}

struct Recursion
{
    const Graph& g;
    u32 min_size, max_size;
    Partition& out;
    std::vector<u32> local; // global node -> its index in the current subgraph
    std::mt19937 rng{0x5eed1234u};

    void run(u32 first, u32 last)
    {
        const u32 count = last - first;
        if (count <= max_size)
        {
            out.ranges.push_back(last);
            return;
        }
        const u32 target_size = (min_size + max_size) / 2;
        const u32 parts = std::max<u32>(2, u32(std::lround(double(count) / target_size)));
        const u32 parts_left = parts / 2;
        const bool loose = parts >= 128 || max_size / std::max(min_size, 1u) > 1;
        const double share = double(parts_left) / parts;
        const i64 target = i64(std::llround(count * share));
        const i64 tol = std::max<i64>(1, i64(std::ceil(count * share * (loose ? 0.2 : 0.001))));

        // The subgraph of these nodes.
        Level sub;
        sub.vw.assign(count, 1);
        for (u32 i = 0; i < count; ++i)
            local[out.order[first + i]] = i;
        sub.xadj.assign(1, 0);
        for (u32 i = 0; i < count; ++i)
        {
            const u32 u = out.order[first + i];
            for (u32 e = g.xadj[u]; e < g.xadj[u + 1]; ++e)
            {
                const u32 v = g.adj[e];
                const u32 lv = local[v];
                if (lv < count && out.order[first + lv] == v)
                {
                    sub.adj.push_back(lv);
                    sub.wgt.push_back(g.wgt[e]);
                }
            }
            sub.xadj.push_back(u32(sub.adj.size()));
        }
        std::vector<uint8_t> side;
        bisect(sub, target, std::max<i64>(1, target - tol), std::min<i64>(count - 1, target + tol), side, rng);

        // Left side first, both in their old order.
        std::vector<u32> left, right;
        for (u32 i = 0; i < count; ++i)
            (side[i] == 0 ? left : right).push_back(out.order[first + i]);
        if (left.empty() || right.empty())
        {
            // A degenerate split (should not happen): halves by order.
            const u32 half = count / 2;
            left.assign(out.order.begin() + first, out.order.begin() + first + half);
            right.assign(out.order.begin() + first + half, out.order.begin() + last);
        }
        std::copy(left.begin(), left.end(), out.order.begin() + first);
        std::copy(right.begin(), right.end(), out.order.begin() + first + left.size());
        const u32 split = first + u32(left.size());
        run(first, split);
        run(split, last);
    }
};
} // namespace

Partition partition(const Graph& g, uint32_t min_size, uint32_t max_size)
{
    Partition out;
    const u32 n = g.nodes();
    out.order.resize(n);
    std::iota(out.order.begin(), out.order.end(), 0u);
    out.ranges.push_back(0);
    if (!n)
        return out;
    Recursion r{g, std::max(min_size, 1u), std::max(max_size, 1u), out, std::vector<u32>(n, ~0u)};
    r.run(0, n);
    return out;
}

Graph make_graph(uint32_t nodes, const std::vector<uint64_t>& edges, const std::vector<int32_t>& weights)
{
    struct E
    {
        u32 a, b;
        i32 w;
    };
    std::vector<E> list;
    list.reserve(edges.size() * 2);
    for (size_t i = 0; i < edges.size(); ++i)
    {
        const u32 a = u32(edges[i] >> 32), b = u32(edges[i]);
        if (a == b || a >= nodes || b >= nodes)
            continue;
        list.push_back({a, b, weights[i]});
        list.push_back({b, a, weights[i]});
    }
    std::sort(list.begin(), list.end(), [](const E& x, const E& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    Graph g;
    g.xadj.assign(nodes + 1, 0);
    for (size_t i = 0; i < list.size();)
    {
        size_t j = i;
        i32 w = 0;
        while (j < list.size() && list[j].a == list[i].a && list[j].b == list[i].b)
            w += list[j++].w;
        g.adj.push_back(list[i].b);
        g.wgt.push_back(w);
        ++g.xadj[list[i].a + 1];
        i = j;
    }
    for (u32 i = 0; i < nodes; ++i)
        g.xadj[i + 1] += g.xadj[i];
    return g;
}
} // namespace da_mesh
