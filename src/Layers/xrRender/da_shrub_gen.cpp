// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The shrub generator (da_shrub_gen.h). A rule-based branching model in the line of Weber and Penn (1995) and
// the production tree generators: stems from a root crown, then up to three levels of children placed along
// their parents by density, phyllotaxis and branching angle, bent by tropism and noise, cut by the crown's
// envelope. Leaves grow on the current year's growth at the axis tips; then leafy shoots fill the crown's lit
// shell, each grown toward a point of it from the nearest wood within its reach (one pass of Runions et al.'s
// space colonisation, 2007: the foliage takes the crown's volume, clumped by a noise, with holes where no wood
// reaches), and short shoots along the older wood carry the rest, until the bush reaches its species' leaf area
// index; flowers and fruits sit at the shoot tips, in the axils or on the old wood, catkins near the young tips.
// Clusters group a shoot's leaves for the middle distance, blobs the clusters for the far one.
#include "da_shrub_gen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <queue>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace da_shrub
{
float length(V3 a) { return std::sqrt(dot(a, a)); }

V3 normalize(V3 a, V3 fallback)
{
    const float l = length(a);
    return l > 1e-8f ? a * (1.f / l) : fallback;
}

namespace
{
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.f;
// An ovate blade fills about two thirds of its length x width.
constexpr float kBladeFill = 0.67f;
const V3 kUp{0.f, 1.f, 0.f};

// PCG32 (O'Neill 2014): small, fast, the same stream on every platform.
struct Rng
{
    uint64_t state, inc;
    explicit Rng(uint64_t seed, uint64_t seq = 0x853c49e6748fea9bULL) : state(0), inc((seq << 1u) | 1u)
    {
        next();
        state += seed;
        next();
    }
    uint32_t next()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = uint32_t(old >> 59u);
        return (xs >> rot) | (xs << ((32u - rot) & 31u));
    }
    float u01() { return float(next() >> 8) * (1.f / 16777216.f); }
    float range(float a, float b) { return a + (b - a) * u01(); }
    float range(const float (&r)[2]) { return range(r[0], r[1]); }
    float jitter(float k) { return 1.f + k * (2.f * u01() - 1.f); }
};

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float clampf(float v, float a, float b) { return std::min(std::max(v, a), b); }
float smooth(float a, float b, float x)
{
    const float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// A place to the millimetre: the stock's soups welded (a mesh split at its uv seams, many a soup of single triangles).
uint64_t place_key(V3 p)
{
    const auto q = [](float x) { return uint64_t(uint32_t(int32_t(std::lround(x * 1000.f))) & 0x1FFFFFu); };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

// A unit vector perpendicular to t.
V3 perpendicular(V3 t)
{
    const V3 a = std::fabs(t.y) < 0.9f ? kUp : v3(1.f, 0.f, 0.f);
    return normalize(cross(t, a));
}

// Rotate v toward w by angle (rad) in their plane (w need not be perpendicular).
V3 tilt_toward(V3 v, V3 w, float angle)
{
    const V3 p = normalize(w - v * dot(w, v), perpendicular(v));
    return normalize(v * std::cos(angle) + p * std::sin(angle), v);
}

// Rodrigues: rotate v about the unit axis k by angle a.
V3 rotate(V3 v, V3 k, float a)
{
    const float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.f - c));
}

// The seasonal order of a leaf (shared by the leaflets of a compound leaf).
struct Order
{
    float rank, fall, light;
};

// A tree's generation state (generate_on_skeleton): its skeleton and the crown field (the occupancy of the stock
// crown, each occupied cell's depth into it, the cells by their occupancy for sampling, the crown's projected areas),
// the stock wood in a grid (the clearance of the new wood), the stock limbs' standable tops (the reach of the thick new
// wood), the card branches' axes the grafted first level follows, and the axis being grown's parent and guide.
struct TreeState
{
    const Skeleton* S{};
    int nx{}, ny{}, nz{};
    V3 o{};
    float cell{1.f};
    std::vector<float> occ, depth;
    std::vector<uint32_t> cells;
    std::vector<float> cum;
    float top_area{}, side_area{};
    float lo_y{}, hi_y{};
    // The stock wood's segments (the template's) in a grid.
    std::vector<uint32_t> segs;
    V3 g_lo{};
    float g_cell{1.f};
    int g_n[3]{};
    std::vector<uint32_t> g_start, g_items;
    float max_r{};
    // The standable tops of the stock wood (its near-level stretches' upper surface, the cut ends) in a plan grid.
    std::vector<V3> stands;
    V3 st_lo{};
    float st_cell{1.5f};
    int st_n[2]{};
    std::vector<uint32_t> st_start, st_items;
    struct Guide
    {
        int32_t parent{-1};
        float attach{};
        std::vector<V3> pts;
        std::vector<float> s;
        float len{};
    };
    std::vector<Guide> guides;
    // The stock's flat strips of bark taken out of the patches (tree_ribbons): each a branch to grow along it - its base
    // (the end nearer the stock wood), direction, length and width.
    struct Ribbon
    {
        V3 base, dir;
        float len{}, width{};
    };
    std::vector<Ribbon> ribbons;
    const Guide* guide{};
    float follow{};
    int32_t parent{-1};
    float attach{}, parent_r{};
    uint32_t fill_seed{};
    float climb_top{};
};

struct Builder
{
    const Arch& A;
    Template& T;
    Rng rng;
    float H, RW;      // height and half width of the crown
    float wood_scale; // the woody structure's lengths against the reference height
    float area = 0.f; // leaf area so far
    uint32_t wood_segments = 0;
    std::vector<float> young_from; // per axis: where its current year's growth begins (arclength)
    // The clumps' noise.
    uint32_t noise_seed = 0;
    uint32_t lump_seed = 0;
    float inv_clump = 1.f;
    // The wood the filling shoots leave, by cell (cell index -> its segments).
    V3 grid_lo{};
    float grid_cell = 1.f;
    int32_t grid_n[3]{};
    std::vector<uint32_t> grid_start, grid_items;
    // The filling shoots already leaving each quarter of a wood segment: a crowded stretch costs more, so shoots
    // spread along the twigs instead of bursting from their tips.
    std::vector<uint8_t> crowd;
    float crowd_cost = 0.f;
    // The most leaf elements (the species' max_leaves; a tree's by its leaf area and the level's budget).
    uint32_t leaf_cap = 0;
    // The vertical the axes grow toward (a tilted stock tree's tilt bucket in its model's space; a bush's is y).
    V3 vert = kUp;
    // Trees (generate_on_skeleton): the stock skeleton and its crown field, the stock wood's grid for the collision
    // rules, the card branches' guides; tree.cpp-side state of the axis being grown.
    TreeState* tree = nullptr;

    Builder(const Arch& a, Template& t, uint32_t seed, float h, float w)
        : A(a), T(t), rng(seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL), H(h), RW(0.5f * w),
          wood_scale(h / std::max(a.href, 0.1f)), lump_seed(seed * 2654435761u + 0x51ED27u), leaf_cap(a.max_leaves)
    {
    }

    float env_r(float y) const { return envelope_radius(A, y, H, RW); }

    // The crown's outline is lumpy, not a clipped hedge: the envelope's radius swells and dents by a fifth over a
    // value noise of about half the crown's radius (each major stem's foliage its own cloud, gaps between).
    float lump(V3 p) const
    {
        const float k = 1.f / std::max(0.45f * RW, 0.15f);
        const float x = p.x * k, y = p.y * k, z = p.z * k;
        const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
        const float tx = smooth(0.f, 1.f, x - fx), ty = smooth(0.f, 1.f, y - fy), tz = smooth(0.f, 1.f, z - fz);
        const auto h = [this](int32_t i, int32_t j, int32_t l) {
            uint32_t v = uint32_t(i) * 73856093u ^ uint32_t(j) * 19349663u ^ uint32_t(l) * 83492791u ^ lump_seed;
            v ^= v >> 16;
            v *= 0x7feb352du;
            v ^= v >> 15;
            v *= 0x846ca68bu;
            v ^= v >> 16;
            return float(v >> 8) * (1.f / 16777216.f);
        };
        const float x00 = lerpf(h(ix, iy, iz), h(ix + 1, iy, iz), tx), x10 = lerpf(h(ix, iy + 1, iz), h(ix + 1, iy + 1, iz), tx);
        const float x01 = lerpf(h(ix, iy, iz + 1), h(ix + 1, iy, iz + 1), tx);
        const float x11 = lerpf(h(ix, iy + 1, iz + 1), h(ix + 1, iy + 1, iz + 1), tx);
        return 1.f + 0.22f * (2.f * lerpf(lerpf(x00, x10, ty), lerpf(x01, x11, ty), tz) - 1.f);
    }
    float env_at(V3 p) const { return env_r(p.y) * lump(p); }

    // The leaf area the bush aims at: the species' LAI over the crown's larger projection, from above or from the
    // side (a tall narrow crown is seen from the side, its leaves over its side's area).
    float leaf_target() const { return tree ? tree_leaf_target() : A.lai * kPi * RW * std::max(RW, 0.5f * H); }

    bool inside(V3 p, float margin = 0.f) const
    {
        if (tree)
            return tree_inside(p, margin);
        if (p.y < -0.05f || p.y > H * (1.f + margin))
            return false;
        return std::sqrt(p.x * p.x + p.z * p.z) <= env_at(p) * (1.f + margin) + 1e-3f;
    }

    // Free length from p along d before the envelope.
    float free_length(V3 p, V3 d, float want) const
    {
        const float step = tree ? tree_step() : std::max(0.02f * H, 0.01f);
        float t = 0.f;
        while (t < want)
        {
            const float n = std::min(t + step, want);
            if (!inside(p + d * n, 0.04f))
                return t;
            t = n;
        }
        return want;
    }

    // How lit a point of the crown sits: the outer shell and the top get the light, the inside and the base little.
    float light_at(V3 p) const
    {
        if (tree)
            return tree_light(p);
        const float outer = clampf(std::sqrt(p.x * p.x + p.z * p.z) / std::max(env_at(p), 0.05f), 0.f, 1.f);
        const float upper = clampf(p.y / std::max(H, 0.1f), 0.f, 1.f);
        return clampf(0.12f + 0.88f * (0.6f * outer * outer + 0.4f * upper), 0.f, 1.f);
    }

    // The lower crown unfolds first (sheltered and warmer); the lit outer leaves colour and fall first.
    Order order_at(V3 p)
    {
        if (tree)
            return tree_order(p);
        const float lit = light_at(p);
        const float upper = clampf(p.y / std::max(H, 0.1f), 0.f, 1.f);
        return {clampf(0.65f * rng.u01() + 0.35f * upper, 0.f, 1.f), clampf(0.6f * rng.u01() + 0.4f * (1.f - lit), 0.f, 1.f),
            lit};
    }

    // The point, tangent and radius of an axis at arclength s.
    void sample(const Axis& ax, float s, V3& p, V3& t, float& r) const
    {
        // (A tree's axis trimmed away whole keeps its base.)
        if (!ax.segs)
        {
            p = ax.base;
            t = ax.dir;
            r = ax.r_base;
            return;
        }
        const uint32_t last = ax.first_seg + ax.segs - 1;
        uint32_t i = ax.first_seg;
        while (i < last && T.segments[i + 1].s0 <= s)
            ++i;
        const Segment& g = T.segments[i];
        const float len = std::max(length(g.b - g.a), 1e-5f);
        const float u = clampf((s - g.s0) / len, 0.f, 1.f);
        p = g.a + (g.b - g.a) * u;
        t = normalize(g.b - g.a);
        r = lerpf(g.ra, g.rb, u);
    }

    // Grows one axis: segments bent by tropism, arching and a wandering curvature, its radius tapering to the tip.
    // (r_tip: the tip's radius when given - a thorn's needle point; else a sixth of the base, a wood's at least 0.8 mm.)
    uint32_t grow(uint32_t level, V3 base, V3 dir, float len, float r0, int32_t parent, float attach, float up,
        float arch, float wobble, float seg_len, float r_tip = -1.f)
    {
        Axis ax;
        ax.base = base;
        ax.dir = dir;
        ax.level = level;
        ax.parent = parent;
        ax.attach = attach;
        ax.r_base = r0;
        ax.r_tip = r_tip >= 0.f ? r_tip : std::max(r0 * 0.15f, 0.0008f);
        ax.first_seg = uint32_t(T.segments.size());
        // (Segments of two centimetres at the least; an axis shorter than four - a hooked prickle - may take two.)
        const uint32_t n = std::max(1u, uint32_t(std::ceil(len / std::max(seg_len, std::min(0.02f, 0.5f * len)) - 1e-3f)));
        const float ds = len / float(n);
        V3 p = base, db = dir;
        float s = 0.f;
        // The axis's course: its direction under the tropism and the arching alone, and a lean off it that wanders
        // (an Ornstein-Uhlenbeck process with a correlation length of a third of a metre, its spread the species'
        // wobble) but always returns - real stems bow and swing in long curves round their heading; a fresh random
        // turn per segment averaged out to straight sticks, and an unbounded turn rate curled them into hoops.
        V3 off = v3(0.f, 0.f, 0.f);
        const float decay = std::exp(-ds / 0.35f), sigma = 1.2f * wobble * std::sqrt(1.f - decay * decay);
        // The young shoots grow on from a side bud at every node (sympodially): a small kink, side to side (not a
        // prickle, which grows no wobble).
        // (A species of strong zigzag - a hawthorn's - kinks its branches' long shoots too, at half of it.)
        const float zig = wobble <= 0.f ? 0.f : (level >= 2 ? A.zig : (level == 1 && A.zig > 0.2f ? 0.5f * A.zig : 0.f));
        // A long thin axis sags under its own weight: a cantilever's tip angle goes with its length cubed over its
        // radius squared (a 1.5 m wand of 8 mm some fifteen degrees), the curvature with the square of the length
        // still beyond, most near the base; the species' stiffness scales it (an osier's wands stand, a rose's canes
        // bow), and a stem that does not arch stiffens with its own wood (bent stools of stems lay flat on the ground).
        const float stiff = level == 0 && arch < 0.3f ? 0.3f : 1.f;
        const float sag = std::min(4e-6f * A.sag * stiff * len * len * len / std::max(r0 * r0, 1e-7f), 0.9f);
        const V3 zax = rotate(perpendicular(dir), dir, rng.u01() * 2.f * kPi);
        // (A stem rooted outside the crown's foot - a wide stool - is not held to the crown.)
        const bool held = inside(base, 0.08f);
        for (uint32_t k = 0; k < n; ++k)
        {
            // Tropism toward the vertical (or the ground), the arching of canes growing with the length, the sag.
            const float rest = std::max(len - s, 0.f);
            const float bend = (up - arch * (s / std::max(len, 1e-3f))) * ds - sag * 3.f * rest * rest / std::max(len * len * len, 1e-6f) * ds;
            if (bend > 0.f)
                db = tilt_toward(db, vert, std::min(bend, std::acos(clampf(dot(db, vert), -1.f, 1.f))));
            else if (bend < 0.f)
                db = tilt_toward(db, vert * -1.f, std::min(-bend, std::acos(clampf(-dot(db, vert), -1.f, 1.f))));
            // A tree's grafted branch along a stock card branch's axis follows it (the stock crown's visible layout).
            if (tree)
                db = tree_follow(db, p, s, ds);
            const V3 n1 = perpendicular(db), n2 = normalize(cross(db, n1), n1);
            off = off * decay + (n1 * (rng.u01() * 2.f - 1.f) + n2 * (rng.u01() * 2.f - 1.f)) * (sigma * 1.732f);
            off = off - db * dot(off, db);
            // (At most some thirty degrees off the heading.)
            const float lo = length(off);
            if (lo > 0.6f)
                off = off * (0.6f / lo);
            V3 d = normalize(db + off, db);
            if (zig > 0.f)
                d = rotate(d, zax, (k & 1u) ? zig : -zig);
            // Keep the woody structure above the ground (an arching cane's tip too).
            V3 q = p + d * ds;
            if (tree)
            {
                // (A tree's wood keeps above its root's ground in its model's vertical.)
                if (dot(q, vert) < 0.02f && (level > 0 || k > 0))
                {
                    d = normalize(d + vert * std::max(0.15f - dot(d, vert), 0.f));
                    db = normalize(db + vert * std::max(0.15f - dot(db, vert), 0.f));
                    q = p + d * ds;
                }
            }
            else if (q.y < 0.02f && (level > 0 || k > 0))
            {
                d = normalize(v3(d.x, std::max(d.y, 0.15f), d.z));
                db = normalize(v3(db.x, std::max(db.y, 0.15f), db.z));
                q = p + d * ds;
            }
            // An axis ends where it leaves the crown (its course wanders and sags past what the straight free length
            // promised): bare rods stuck out of the crowns.
            if (held && k > 0 && !inside(q, 0.08f))
                break;
            // A tree's new wood keeps its radius and two centimetres clear of the stock wood (it has no collision; a
            // branch through a stock limb would be one the player walks through).
            if (tree && k > 0 && !tree_clear(q, lerpf(r0, ax.r_tip, std::pow(std::min((s + ds) / len, 1.f), 0.8f)) + 0.02f))
                break;
            Segment g;
            g.a = p;
            g.b = q;
            g.ra = lerpf(r0, ax.r_tip, std::pow(s / len, 0.8f));
            g.rb = lerpf(r0, ax.r_tip, std::pow((s + ds) / len, 0.8f));
            g.axis = uint32_t(T.axes.size());
            g.s0 = s;
            T.segments.push_back(g);
            p = q;
            s += ds;
        }
        ax.segs = uint32_t(T.segments.size()) - ax.first_seg;
        // A shortened axis tapers to its tip over what it grew (a blunt rod end otherwise).
        if (s < len - 1e-4f && s > 0.f)
            for (uint32_t i = ax.first_seg; i < ax.first_seg + ax.segs; ++i)
            {
                Segment& g = T.segments[i];
                const float sb = g.s0 + length(g.b - g.a);
                g.ra = lerpf(r0, ax.r_tip, std::pow(g.s0 / s, 0.8f));
                g.rb = lerpf(r0, ax.r_tip, std::pow(std::min(sb / s, 1.f), 0.8f));
            }
        ax.length = std::max(s, ds);
        // A tree's thick new wood stands only where the player cannot reach it (it has no collision): within reach of the
        // ground or of a stock limb's top it stays under eight centimetres across.
        if (tree)
        {
            for (uint32_t i = ax.first_seg; i < ax.first_seg + ax.segs; ++i)
            {
                Segment& g = T.segments[i];
                g.ra = tree_radius_cap(g.a, g.ra);
                g.rb = tree_radius_cap(g.b, g.rb);
            }
            ax.r_base = ax.segs ? T.segments[ax.first_seg].ra : ax.r_base;
        }
        T.axes.push_back(ax);
        return uint32_t(T.axes.size() - 1);
    }

    // The stems from the root crown (or one leader with whorls).
    void stems()
    {
        const float r_stem = A.stem_radius * std::pow(wood_scale, 1.2f);
        if (A.leader)
        {
            const float len = H * rng.range(A.stem_len);
            grow(0, v3(0.f, 0.f, 0.f), normalize(v3(rng.range(-0.05f, 0.05f), 1.f, rng.range(-0.05f, 0.05f))), len,
                r_stem, -1, 0.f, std::max(A.stem_up, 0.4f), 0.f, A.stem_wobble * 0.5f, A.stem_seg);
            return;
        }
        // A young bush has fewer stems.
        const float stems = rng.range(A.stems) * clampf(0.35f + 0.65f * wood_scale, 0.35f, 1.f);
        const uint32_t n = std::max(1u, uint32_t(std::lround(stems)));
        const float az0 = rng.u01() * 2.f * kPi;
        for (uint32_t i = 0; i < n; ++i)
        {
            const float az = az0 + float(i) * 2.39996323f + rng.range(-0.3f, 0.3f);
            const float rr = A.root_r * std::sqrt((float(i) + 0.5f) / float(n)) * rng.jitter(0.3f);
            const float outer = n > 1 ? std::sqrt(float(i) / float(n - 1)) : 0.f;
            const V3 out = v3(std::cos(az), 0.f, std::sin(az));
            const V3 base = out * rr;
            const float spread = lerpf(A.stem_spread[0], A.stem_spread[1], outer) * rng.jitter(0.25f) * kDeg;
            const V3 d = normalize(kUp * std::cos(spread) + out * std::sin(spread));
            // Outer stems are shorter, the centre leads.
            const float len = H * lerpf(A.stem_len[1], A.stem_len[0], outer) * rng.jitter(0.12f);
            const float flen = std::max(free_length(base + kUp * 0.01f, d, len * 1.05f), len * 0.6f);
            const float r = r_stem * rng.jitter(0.25f) * std::sqrt(std::max(flen / std::max(H, 0.1f), 0.2f));
            grow(0, base, d, flen, r, -1, 0.f, A.stem_up, A.stem_arch, A.stem_wobble, A.stem_seg);
        }
        // An old stool's dead stems, broken off low: a few grey stubs among the living ones (a coppice stool is never
        // a clean bundle of tubes out of the soil), clear of the living stems (one on a stem sleeved it).
        if (n >= 3)
        {
            const uint32_t stubs = std::min(5u, std::max(1u, uint32_t(std::lround(float(n) * 0.22f * rng.range(0.5f, 1.5f)))));
            const uint32_t live = uint32_t(T.axes.size());
            for (uint32_t i = 0; i < stubs; ++i)
            {
                V3 out{}, base{};
                bool clear = false;
                for (uint32_t tries = 0; tries < 8 && !clear; ++tries)
                {
                    const float az = rng.u01() * 2.f * kPi;
                    out = v3(std::cos(az), 0.f, std::sin(az));
                    base = out * (A.root_r * std::sqrt(rng.u01()));
                    clear = true;
                    for (uint32_t s = 0; s < live && clear; ++s)
                    {
                        const Axis& st = T.axes[s];
                        const float dx = st.base.x - base.x, dz = st.base.z - base.z;
                        clear = std::sqrt(dx * dx + dz * dz) > 2.5f * (st.r_base + r_stem) + 0.02f;
                    }
                }
                if (!clear)
                    continue;
                const float spread = rng.range(A.stem_spread[0], A.stem_spread[1]) * kDeg;
                const V3 d = normalize(kUp * std::cos(spread) + out * std::sin(spread));
                const float len = rng.range(0.08f, 0.35f) * clampf(wood_scale, 0.6f, 1.5f);
                const float r = r_stem * rng.range(0.5f, 0.9f);
                // (Tapering to its splintered end: an open tube's end would show its hollow inside.)
                const uint32_t ai = grow(0, base, d, len, r, -1, 0.f, 0.f, 0.f, A.stem_wobble, std::max(len, 0.05f));
                T.axes[ai].dead = T.axes[ai].leafless = true;
            }
        }
    }

    // Prickles or thorns: a rose's and a gooseberry's along the old wood (the stems and branches), a hawthorn's and a
    // sea buckthorn's on the young growth alone (a branch's last metres, every twig and filling shoot: the old trunks
    // of those carry none) - the axes from `first` on. Short spikes tapering to a point, leaning back toward the base,
    // a rose's broad-footed and hooked over; they ride their wood in the wind and stay leafless.
    void prickles(uint32_t first = 0)
    {
        if (A.prickle_spacing <= 0.f || A.prickle_len <= 0.f)
            return;
        const bool young = A.prickle_young > 0.f;
        const uint32_t count = uint32_t(T.axes.size());
        for (uint32_t ai = first; ai < count; ++ai)
        {
            const Axis ax = T.axes[ai];
            if (ax.leafless || ax.fixed || (young ? ax.level < 1 : ax.level > 1))
                continue;
            const float from = young && ax.level == 1 ? std::max(ax.length - A.prickle_young, 0.f) : 0.f;
            for (float s = from + A.prickle_spacing * rng.range(0.3f, 1.f); s < ax.length * 0.97f; s += A.prickle_spacing * rng.jitter(0.35f))
            {
                V3 p, t;
                float r;
                sample(ax, s, p, t, r);
                const V3 radial = rotate(perpendicular(t), t, rng.u01() * 2.f * kPi);
                const V3 d = normalize(radial * 0.85f - t * 0.45f, radial);
                const float len = A.prickle_len * rng.jitter(0.25f);
                const float r0 = clampf(len * A.prickle_base, 0.0006f, std::max(r * 0.6f, 0.0006f));
                // A hooked one bends down over its length (two segments), a straight one is one; its point a sixth of
                // its foot, however thin (the wood's 0.8 mm floor left a thin thorn a blunt pin, wider at the tip).
                // (A hook's second half turns down by about a radian: the arching's bend at half the length is
                // arch x a quarter of it.)
                const uint32_t pi = grow(3, p + radial * (r * 0.8f), d, len, r0, int32_t(ai), s, 0.f,
                    A.prickle_hook > 0.5f ? 4.f / std::max(len, 1e-3f) : 0.f, 0.f, A.prickle_hook > 0.5f ? 0.5f * len : len,
                    r0 * 0.15f);
                T.axes[pi].leafless = true;
            }
        }
        // (After the filling shoots, the axes' young-growth marks cover the new thorns too: bare end to end.)
        while (!young_from.empty() && young_from.size() < T.axes.size())
            young_from.push_back(T.axes[young_from.size()].length);
    }

    bool is_bare(uint32_t axis) const { return axis < T.axes.size() && T.axes[axis].leafless; }

    // The children of every axis of level l-1.
    void children(uint32_t l)
    {
        const Level& L = A.level[l - 1];
        const uint32_t count = uint32_t(T.axes.size());
        for (uint32_t pi = 0; pi < count; ++pi)
        {
            if (T.axes[pi].level != l - 1 || is_bare(pi))
                continue;
            const Axis parent = T.axes[pi];
            const float s0 = parent.length * L.start, s1 = parent.length * L.end;
            const float spacing = 1.f / std::max(L.density / std::max(std::sqrt(wood_scale), 0.5f), 0.05f);
            float az = rng.u01() * 2.f * kPi;
            V3 nrm = perpendicular(parent.dir);
            // Whorls on a leader: a node every whorl_step with several branches round it.
            const bool whorl = l == 1 && A.leader && A.whorl_step > 0.f;
            const float step = whorl ? A.whorl_step * std::max(wood_scale, 0.5f) : spacing;
            for (float s = s0 + step * rng.range(0.2f, 1.f); s < s1; s += step * (whorl ? rng.jitter(0.1f) : rng.jitter(0.35f)))
            {
                V3 p, t;
                float r;
                sample(parent, s, p, t, r);
                // A frame transported along the parent.
                nrm = normalize(nrm - t * dot(nrm, t), perpendicular(t));
                const V3 bin = cross(t, nrm);
                const float u = s / std::max(parent.length, 1e-3f);
                const uint32_t per = whorl ? std::max(3u, uint32_t(std::lround(L.per_node + rng.range(-0.5f, 1.5f))))
                                           : std::max(1u, uint32_t(std::lround(L.per_node)));
                const float az_node = whorl ? rng.u01() * 2.f * kPi : az;
                for (uint32_t k = 0; k < per; ++k)
                {
                    const float a = az_node + float(k) * 2.f * kPi / float(per) + (whorl ? rng.range(-0.3f, 0.3f) : 0.f);
                    const V3 radial = nrm * std::cos(a) + bin * std::sin(a);
                    const float ang = lerpf(L.angle[0], L.angle[1], u) * rng.jitter(0.15f) * kDeg;
                    V3 d = normalize(t * std::cos(ang) + radial * std::sin(ang), t);
                    // Away from the bush's axis, toward the light.
                    const V3 outv = normalize(v3(p.x, 0.f, p.z), radial);
                    d = normalize(d + outv * L.outward, d);
                    float want = parent.length * lerpf(L.len[0], L.len[1], u) * rng.jitter(L.len_jitter);
                    want = std::min(want, free_length(p, d, want * 1.1f) * 0.97f);
                    if (want < L.min_len)
                        continue;
                    const float rc = std::max(r * L.radius, 0.0008f);
                    grow(l, p, d, want, rc, int32_t(pi), s, L.up, 0.f, L.wobble, L.seg_len);
                }
                az += L.phyllo * kDeg;
            }
        }
    }

    void add_blade(V3 base, V3 dir, float len, float wid, float leaf_area, uint32_t axis, float s, const Order& o)
    {
        Leaf f;
        f.dir = dir;
        // Across the midrib, as level as the midrib lets it, rolled at random: the blades' inclination spreads
        // round the species' mean (a crown's leaf angle distribution is never all flat).
        const V3 side = normalize(cross(vert, dir), perpendicular(dir));
        f.side = normalize(rotate(side, dir, rng.range(-0.6f, 0.6f)), side);
        f.p = base;
        f.len = len;
        f.wid = wid;
        f.axis = axis;
        f.s = s;
        f.rank = o.rank;
        f.fall = o.fall;
        f.light = o.light;
        f.area = leaf_area;
        T.leaves.push_back(f);
        area += leaf_area;
    }

    // One leaf at a node: its petiole leaves the shoot (tangent t, radius r) at p along radial, at ang (rad)
    // from the shoot.
    void leaf_at(uint32_t ai, float s, V3 p, V3 t, float r, V3 radial, float ang, float size)
    {
        V3 d = normalize(t * std::cos(ang) + radial * std::sin(ang), t);
        // Toward the horizontal, facing up (the blade's phototropism).
        const V3 h = normalize(d - vert * dot(d, vert), radial);
        d = normalize(d + (h - d) * A.leaf_flat, d);
        const V3 pb = p + radial * r + d * (A.petiole * size);
        const Order o = order_at(pb);
        const float ll = rng.range(A.leaf_len) * size, lw = rng.range(A.leaf_wid) * size;
        if (A.leaf_kind != lkCompound || A.leaflets < 1.f)
        {
            add_blade(pb, d, ll, lw, ll * lw * kBladeFill, ai, s, o);
            return;
        }
        // A compound leaf is one element: its rachis along d, the leaflets in pairs on it (and a terminal one for
        // an odd count) as wide as they splay; the pixel shader cuts them out.
        const float n = std::max(std::round(A.leaflets), 1.f);
        const float len = A.rachis * size + ll * (std::fmod(n, 2.f) > 0.5f ? 1.f : 0.5f);
        const float wid = std::max(2.f * ll * std::sin(A.leaflet_angle * kDeg), lw);
        add_blade(pb, d, len, wid, n * ll * lw * kBladeFill, ai, s, o);
    }
    void add_bloom(V3 p, V3 t, float size, uint32_t axis, float s, BloomKind kind, float droop)
    {
        Bloom b;
        V3 d = t;
        if (droop > 0.f)
            d = normalize(d + (vert * -1.f - d) * droop, vert * -1.f);
        else if (droop < 0.f)
            d = normalize(d + (vert - d) * -droop, vert);
        b.p = p;
        b.dir = d;
        b.size = size * rng.jitter(0.2f);
        b.axis = axis;
        b.s = s;
        b.rank = order_at(p).rank;
        b.kind = kind;
        T.blooms.push_back(b);
    }

    // Needle tufts along a shoot: one element per leaf_spacing, the needles round it.
    void tufts_on(uint32_t ai, float s_from)
    {
        const Axis& ax = T.axes[ai];
        const float step = std::max(A.leaf_spacing, 0.02f);
        for (float s = s_from; s < ax.length - 0.01f; s += step)
        {
            V3 p, t;
            float r;
            sample(ax, s, p, t, r);
            const float len = std::min(step * 1.1f, ax.length - s);
            // Younger needles at the tip are shorter.
            const float u = (s - s_from) / std::max(ax.length - s_from, 1e-3f);
            const float size = lerpf(1.f, A.tip_shrink, smooth(0.7f, 1.f, u));
            const float w = 2.f * A.needle_len * size;
            add_blade(p, t, len, w, len * w * 0.5f, ai, s, order_at(p));
        }
    }

    // Leafy sprays along a shoot (the small-leaved shrubs): one element a spray_len of shoot, its leaflets
    // leaves on it (the cut pixel shader draws them), its axil flowers and catkins.
    void sprays_on(uint32_t ai, float s_from)
    {
        const Axis& ax = T.axes[ai];
        const float step = std::max(A.spray_len, 0.03f);
        const float n = std::max(std::round(A.leaflets), 1.f);
        const float splay = 0.5f * (A.leaf_angle[0] + A.leaf_angle[1]) * kDeg;
        for (float s = std::max(s_from, 0.01f); s < ax.length - 0.015f; s += step)
        {
            V3 p, t;
            float r;
            sample(ax, s, p, t, r);
            const float len = std::min(step, ax.length - s);
            const float u = (s - s_from) / std::max(ax.length - s_from, 1e-3f);
            // A shoot's first leaves are small, the largest past its middle, the tip's shrink again.
            const float size = lerpf(1.f, A.tip_shrink, smooth(0.6f, 1.f, u)) * lerpf(0.55f, 1.f, smooth(0.f, 0.35f, u)) *
                rng.jitter(0.12f);
            const float ll = rng.range(A.leaf_len) * size, lw = rng.range(A.leaf_wid) * size;
            // A short last spray holds fewer leaves.
            const float k = len / step;
            add_blade(p, t, len, 2.f * ll * std::sin(splay) + lw, n * k * ll * lw * kBladeFill, ai, s, order_at(p));
            // A spray's axils carry at most one flower and one catkin element (one stands for the stretch's few).
            const V3 side = perpendicular(t);
            const float axils = 0.5f * n * k;
            const auto at = [&](V3& pj, V3& tj, float& sj) {
                float rj;
                sj = s + len * rng.u01();
                sample(ax, sj, pj, tj, rj);
                pj = pj + side * rj;
            };
            if (A.bloom_place == bpAxil && rng.u01() < std::min(axils * A.bloom_share, 1.f))
            {
                V3 pj, tj;
                float sj;
                at(pj, tj, sj);
                add_bloom(pj, normalize(tj + side * 0.8f, tj), std::max(A.bloom_size, A.fruit_size), ai, sj, bkFlower, A.bloom_droop);
            }
            if (A.catkin_len > 0.f && u > 0.45f && rng.u01() < std::min(axils * A.catkin_share, 1.f))
            {
                V3 pj, tj;
                float sj;
                at(pj, tj, sj);
                add_bloom(pj, normalize(tj * 0.3f + side, tj), A.catkin_len, ai, sj, bkCatkin, A.catkin_droop * 0.85f);
            }
        }
        if (A.bloom_place == bpTip && rng.u01() < A.bloom_share)
        {
            V3 p, t;
            float r;
            sample(ax, ax.length, p, t, r);
            add_bloom(p, t, std::max(A.bloom_size, A.fruit_size), ai, ax.length, bkFlower, A.bloom_droop);
        }
    }

    // The leaves of an axis's young growth from s_from to its tip, its axil flowers and catkins, its tip flower.
    void shoot_leaves(uint32_t ai, float s_from)
    {
        if (A.leaf_kind == lkNeedles)
        {
            tufts_on(ai, s_from);
            return;
        }
        if (A.leaf_kind == lkSpray)
        {
            sprays_on(ai, s_from);
            return;
        }
        const Axis ax = T.axes[ai];
        const float spacing = std::max(A.leaf_spacing, 0.005f);
        float az = rng.u01() * 2.f * kPi;
        V3 nrm = perpendicular(ax.dir);
        const uint32_t per = std::max(1u, uint32_t(std::lround(A.leaf_per_node)));
        for (float s = std::max(s_from, 0.015f) + spacing * rng.range(0.f, 1.f); s < ax.length; s += spacing * rng.jitter(0.2f))
        {
            V3 p, t;
            float r;
            sample(ax, s, p, t, r);
            nrm = normalize(nrm - t * dot(nrm, t), perpendicular(t));
            const V3 bin = cross(t, nrm);
            const float u = (s - s_from) / std::max(ax.length - s_from, 1e-3f);
            // A shoot's first leaves are small, the largest past its middle, the tip's shrink again.
            const float size = lerpf(1.f, A.tip_shrink, smooth(0.6f, 1.f, u)) * lerpf(0.55f, 1.f, smooth(0.f, 0.35f, u)) *
                rng.jitter(0.12f);
            for (uint32_t k = 0; k < per; ++k)
            {
                const float a = az + float(k) * 2.f * kPi / float(per);
                const V3 radial = nrm * std::cos(a) + bin * std::sin(a);
                leaf_at(ai, s, p, t, r, radial, rng.range(A.leaf_angle) * kDeg, size);
            }
            const V3 side = nrm * std::cos(az + kPi * 0.5f) + bin * std::sin(az + kPi * 0.5f);
            if (A.bloom_place == bpAxil && rng.u01() < A.bloom_share)
                add_bloom(p + side * r, normalize(t + side * 0.8f, t), std::max(A.bloom_size, A.fruit_size), ai, s, bkFlower,
                    A.bloom_droop);
            if (A.catkin_len > 0.f && u > 0.45f && rng.u01() < A.catkin_share)
                add_bloom(p + side * r, normalize(t * 0.3f + side, t), A.catkin_len, ai, s, bkCatkin, A.catkin_droop * 0.85f);
            az += A.leaf_phyllo * kDeg;
        }
        if (A.bloom_place == bpTip && rng.u01() < A.bloom_share)
        {
            V3 p, t;
            float r;
            sample(ax, ax.length, p, t, r);
            add_bloom(p, t, std::max(A.bloom_size, A.fruit_size), ai, ax.length, bkFlower, A.bloom_droop);
        }
    }

    // The current year's growth of every axis carries leaves: a short terminal axis all along, a longer or a
    // branched one its last shoot_len.
    void young_growth()
    {
        const uint32_t count = uint32_t(T.axes.size());
        std::vector<uint32_t> kids(count, 0);
        // (A prickle is no branch: a short axis with prickles alone is still leafy end to end.)
        for (const Axis& a : T.axes)
            if (a.parent >= 0 && !a.leafless)
                ++kids[uint32_t(a.parent)];
        young_from.assign(count, 0.f);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Axis& ax = T.axes[i];
            const float young = A.shoot_len * rng.jitter(0.3f);
            const float from = !kids[i] && ax.length < young * 1.5f ? 0.f : std::max(ax.length - young, 0.f);
            // (A tree's stock trunk and limbs are old wood: the leaves are on what grows from them.)
            const bool bare = is_bare(i) || ax.fixed;
            young_from[i] = bare ? ax.length : from;
            if (!bare)
                shoot_leaves(i, from);
        }
    }

    // A short shoot on the older wood: a few leaves splayed round its tip, or a needle tuft; its tip may flower.
    void spur(uint32_t ai, float s, V3 p, V3 t, float r)
    {
        const V3 outv = normalize(v3(p.x, 0.f, p.z), perpendicular(t));
        V3 sd = outv + vert * 0.5f + perpendicular(t) * rng.range(-0.6f, 0.6f);
        sd = normalize(sd - t * (0.6f * dot(sd, t)), outv);
        const float slen = rng.range(0.005f, 0.025f);
        const V3 tip = p + sd * (r + slen);
        if (A.leaf_kind == lkNeedles)
        {
            const float len = std::max(A.leaf_spacing, 0.02f) * rng.range(0.6f, 1.f);
            const float w = 2.f * A.needle_len * rng.range(0.8f, 1.f);
            add_blade(p + sd * r, sd, len, w, len * w * 0.5f, ai, s, order_at(tip));
            return;
        }
        if (A.leaf_kind == lkSpray)
        {
            // A spur's leaves crowd round its tip: one spray as long as its leaves.
            const float ll = rng.range(A.leaf_len), lw = rng.range(A.leaf_wid);
            const float splay = 0.5f * (A.leaf_angle[0] + A.leaf_angle[1]) * kDeg;
            const float len = std::max(ll, std::max(A.spray_len, 0.03f) * rng.range(0.5f, 0.8f));
            const float n = std::max(std::round(A.leaflets * len / std::max(A.spray_len, 0.03f)), 1.f);
            add_blade(p + sd * r, sd, len, 2.f * ll * std::sin(splay) + lw, n * ll * lw * kBladeFill, ai, s, order_at(tip));
            if ((A.bloom_place == bpOldWood || A.bloom_place == bpTip) && rng.u01() < A.bloom_share)
                add_bloom(tip, sd, std::max(A.bloom_size, A.fruit_size), ai, s, bkFlower, A.bloom_droop);
            return;
        }
        const uint32_t n = std::max(1u, uint32_t(std::lround(rng.range(A.spur_leaves))));
        const V3 u0 = perpendicular(sd), w0 = cross(sd, u0);
        const float az = rng.u01() * 2.f * kPi;
        for (uint32_t k = 0; k < n; ++k)
        {
            const float a = az + float(k) * 2.f * kPi / float(n) + rng.range(-0.3f, 0.3f);
            const V3 radial = u0 * std::cos(a) + w0 * std::sin(a);
            leaf_at(ai, s, tip, sd, 0.f, radial, rng.range(A.leaf_angle) * kDeg * 1.1f, rng.jitter(0.12f));
        }
        if ((A.bloom_place == bpOldWood || A.bloom_place == bpTip) && rng.u01() < A.bloom_share)
            add_bloom(tip, sd, std::max(A.bloom_size, A.fruit_size), ai, s, bkFlower, A.bloom_droop);
    }

    // A value noise in 0..1 over a lattice of a third of the crown (the foliage's clumps and holes).
    float clump_noise(V3 p) const
    {
        const float x = p.x * inv_clump, y = p.y * inv_clump, z = p.z * inv_clump;
        const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
        const float tx = smooth(0.f, 1.f, x - fx), ty = smooth(0.f, 1.f, y - fy), tz = smooth(0.f, 1.f, z - fz);
        const auto h = [this](int32_t i, int32_t j, int32_t k) {
            uint32_t v = uint32_t(i) * 73856093u ^ uint32_t(j) * 19349663u ^ uint32_t(k) * 83492791u ^ noise_seed;
            v ^= v >> 16;
            v *= 0x7feb352du;
            v ^= v >> 15;
            v *= 0x846ca68bu;
            v ^= v >> 16;
            return float(v >> 8) * (1.f / 16777216.f);
        };
        const float x00 = lerpf(h(ix, iy, iz), h(ix + 1, iy, iz), tx), x10 = lerpf(h(ix, iy + 1, iz), h(ix + 1, iy + 1, iz), tx);
        const float x01 = lerpf(h(ix, iy, iz + 1), h(ix + 1, iy, iz + 1), tx);
        const float x11 = lerpf(h(ix, iy + 1, iz + 1), h(ix + 1, iy + 1, iz + 1), tx);
        return lerpf(lerpf(x00, x10, ty), lerpf(x01, x11, ty), tz);
    }

    int32_t cell_of(float v, int32_t axis) const
    {
        const float lo = axis == 0 ? grid_lo.x : axis == 1 ? grid_lo.y : grid_lo.z;
        return std::min(std::max(int32_t(std::floor((v - lo) / grid_cell)), 0), grid_n[axis] - 1);
    }

    // The wood a filling shoot may leave, bucketed by the cells its box touches: not the stems' bare foot, nor the
    // finest level (a leaf's chain of axes stays four long; a thorn's on a shoot five, the shaders' bend chain's most).
    void build_wood_grid(float reach)
    {
        grid_cell = std::max(reach, 0.05f);
        grid_lo = v3(-RW * 1.6f - grid_cell, -grid_cell, -RW * 1.6f - grid_cell);
        grid_n[0] = grid_n[2] = int32_t(std::ceil((RW * 3.2f + 2.f * grid_cell) / grid_cell)) + 1;
        grid_n[1] = int32_t(std::ceil((H * 1.3f + 2.f * grid_cell) / grid_cell)) + 1;
        const size_t cells = size_t(grid_n[0]) * size_t(grid_n[1]) * size_t(grid_n[2]);
        std::vector<uint32_t> count(cells + 1, 0);
        const auto each_cell = [this](const Segment& g, auto&& fn) {
            const int32_t x0 = cell_of(std::min(g.a.x, g.b.x), 0), x1 = cell_of(std::max(g.a.x, g.b.x), 0);
            const int32_t y0 = cell_of(std::min(g.a.y, g.b.y), 1), y1 = cell_of(std::max(g.a.y, g.b.y), 1);
            const int32_t z0 = cell_of(std::min(g.a.z, g.b.z), 2), z1 = cell_of(std::max(g.a.z, g.b.z), 2);
            for (int32_t y = y0; y <= y1; ++y)
                for (int32_t z = z0; z <= z1; ++z)
                    for (int32_t x = x0; x <= x1; ++x)
                        fn((size_t(y) * size_t(grid_n[2]) + size_t(z)) * size_t(grid_n[0]) + size_t(x));
        };
        const auto usable = [this](const Segment& g) {
            const Axis& ax = T.axes[g.axis];
            // (A tree's: its grafted wood, not the stock trunk and limbs - leafy shoots straight out of a trunk read as
            // a hedge round it.)
            if (tree)
                return !ax.fixed && ax.level >= 1 && ax.level <= 3 && !is_bare(g.axis);
            return ax.level <= 2 && !is_bare(g.axis) && !(ax.level == 0 && g.s0 < A.fill_from * ax.length);
        };
        for (uint32_t i = 0; i < wood_segments; ++i)
            if (usable(T.segments[i]))
                each_cell(T.segments[i], [&count](size_t c) { ++count[c + 1]; });
        for (size_t c = 0; c < cells; ++c)
            count[c + 1] += count[c];
        grid_start = count;
        grid_items.assign(count[cells], 0);
        for (uint32_t i = 0; i < wood_segments; ++i)
            if (usable(T.segments[i]))
                each_cell(T.segments[i], [this, &count, i](size_t c) { grid_items[count[c]++] = i; });
    }

    // The nearest usable wood to p within reach, a thick stem counting farther than a twig (the leafy shoots
    // leave the young wood): its segment and the share along it.
    bool nearest_wood(V3 p, float reach, uint32_t& seg, float& at, float& dist) const
    {
        const int32_t cx = cell_of(p.x, 0), cy = cell_of(p.y, 1), cz = cell_of(p.z, 2);
        float best = 1e30f;
        for (int32_t y = std::max(cy - 1, 0); y <= std::min(cy + 1, grid_n[1] - 1); ++y)
            for (int32_t z = std::max(cz - 1, 0); z <= std::min(cz + 1, grid_n[2] - 1); ++z)
                for (int32_t x = std::max(cx - 1, 0); x <= std::min(cx + 1, grid_n[0] - 1); ++x)
                {
                    const size_t c = (size_t(y) * size_t(grid_n[2]) + size_t(z)) * size_t(grid_n[0]) + size_t(x);
                    for (uint32_t k = grid_start[c]; k < grid_start[c + 1]; ++k)
                    {
                        const Segment& g = T.segments[grid_items[k]];
                        const V3 ab = g.b - g.a;
                        const float u = clampf(dot(p - g.a, ab) / std::max(dot(ab, ab), 1e-8f), 0.f, 1.f);
                        const float d = length(p - (g.a + ab * u));
                        if (d > reach)
                            continue;
                        const uint32_t q = grid_items[k] * 4u + std::min(uint32_t(u * 4.f), 3u);
                        const float cost = d + 6.f * lerpf(g.ra, g.rb, u) + crowd_cost * float(crowd[q]);
                        if (cost < best)
                        {
                            best = cost;
                            seg = grid_items[k];
                            at = u;
                            dist = d;
                        }
                    }
                }
        return best < 1e29f;
    }

    // Leafy shoots toward the light: points of the crown's lit shell (by light and the clumps' noise), each grown
    // to from the nearest wood within reach, until the foliage reaches its share of the leaf area (or the wood
    // reaches no more of the shell).
    void fill()
    {
        const float target = leaf_target() * (1.f - clampf(A.spur_share, 0.f, 1.f));
        if (area >= target || A.fill_len[1] < 0.02f || T.leaves.size() >= leaf_cap)
            return;
        // A filling shoot is a current year's shoot: some ten to thirty centimetres (longer ones crossed the empty air
        // to the crown's surface as straight wires); the crown's surface the wood does not reach the spurs fill.
        const float max_len = std::min(A.fill_len[1], 0.32f), reach = max_len * 1.2f;
        build_wood_grid(reach);
        if (grid_items.empty())
            return;
        crowd.assign(size_t(wood_segments) * 4u, 0);
        crowd_cost = 0.3f * reach;
        noise_seed = rng.next();
        inv_clump = 1.f / clampf(0.3f * RW + 0.2f, 0.3f, 1.2f);
        const float shell = clampf(A.fill_shell, 0.05f, 1.f);
        uint32_t misses = 0, tries = 0;
        // The spurs keep their share of the elements too.
        const size_t cap = size_t(float(leaf_cap) * (1.f - clampf(A.spur_share, 0.f, 0.9f)));
        const uint32_t max_tries = tree ? 2000000u : 400000u;
        while (area < target && T.leaves.size() < cap && misses < 3000 && tries < max_tries)
        {
            ++tries;
            V3 p;
            if (tree)
            {
                // A tree's: a point of its crown field's lit shell (and of the share of its cells the shoots fill).
                if (!tree_fill_point(p, shell))
                    continue;
            }
            else
            {
                // Uniform in the crown's cylinder, kept in the envelope's shell: within its depth of the side, or of the
                // top above the point (the crown closes over its middle).
                const float y = H * rng.u01();
                const float rr = RW * 1.25f * std::sqrt(rng.u01());
                const float az = rng.u01() * 2.f * kPi;
                p = v3(std::cos(az) * rr, y, std::sin(az) * rr);
                const float R = env_at(p);
                if (rr > R)
                    continue;
                const float up = shell * 0.5f * std::min(RW, H);
                if (rr < R * (1.f - shell) && y + up < H && rr <= env_at(v3(p.x, y + up, p.z)))
                    continue;
            }
            const float lit = light_at(p);
            if (rng.u01() > lit * lit * lerpf(1.f, smooth(0.3f, 0.75f, clump_noise(p)), A.fill_clump))
                continue;
            uint32_t si = 0;
            float su = 0.f, dist = 0.f;
            if (!nearest_wood(p, reach, si, su, dist))
            {
                ++misses;
                continue;
            }
            const Segment g = T.segments[si];
            // A point past the twig's end would hang every shoot from its tip: they leave along its last stretch.
            if (su > 0.9f)
                su = rng.range(0.45f, 1.f);
            const uint32_t qc = si * 4u + std::min(uint32_t(su * 4.f), 3u);
            crowd[qc] = uint8_t(std::min(crowd[qc] + 1, 255));
            const V3 q = g.a + (g.b - g.a) * su;
            const V3 tg = normalize(g.b - g.a);
            V3 d = normalize(p - q, tg);
            // Not back along the parent, and up and out toward the light.
            const float back = dot(d, tg);
            if (back < -0.25f)
                d = normalize(d + tg * ((-0.25f - back) * 1.5f), d);
            const V3 outv = normalize(v3(p.x, 0.f, p.z), d);
            d = normalize(d + vert * (A.fill_up * (1.f - std::max(dot(d, vert), 0.f))) + outv * 0.15f, d);
            float len = clampf(std::max(dist, A.fill_len[0]) * rng.range(0.9f, 1.15f), std::min(A.fill_len[0], max_len), max_len);
            len = std::min(len, free_length(q, d, len * 1.05f) * 0.97f);
            if (len < A.fill_len[0] * 0.5f)
            {
                ++misses;
                continue;
            }
            misses = 0;
            const float rq = lerpf(g.ra, g.rb, su);
            const float r0 = clampf(0.0011f + 0.004f * len, 0.0009f, std::max(std::min(rq * 0.7f, 0.006f), 0.0009f));
            // It bends on the way as the species' shoots turn, node by node (a birch's hang).
            const uint32_t ai = grow(3, q, d, len, r0, int32_t(g.axis), g.s0 + su * length(g.b - g.a), 2.f * A.fill_up, 0.f, 0.25f,
                clampf(len / 2.5f, 0.06f, 0.12f));
            young_from.push_back(0.f);
            shoot_leaves(ai, 0.f);
        }
    }

    // Short shoots along the older wood, by its length and light, until the leaf area reaches the species' LAI.
    void spurs()
    {
        // (Every broadleaf: what the filling shoots could not reach, the short shoots on the older wood carry - in a real
        // crown most of the inner leaf area sits on them. A conifer's needles stay on its shoots: tufts sticking out of
        // its old wood read as starbursts.)
        const float target = leaf_target();
        if (area >= target || (A.spur_share <= 0.f && A.leaf_kind == lkNeedles))
            return;
        std::vector<float> cum;
        std::vector<uint32_t> segs;
        float total = 0.f;
        for (uint32_t i = 0; i < wood_segments; ++i)
        {
            const Segment& g = T.segments[i];
            const Axis& ax = T.axes[g.axis];
            // Not the young growth, which already leafs, nor the bare lower stems, nor a tree's stock wood.
            if (g.s0 >= young_from[g.axis] || is_bare(g.axis) || ax.fixed)
                continue;
            const V3 m = (g.a + g.b) * 0.5f;
            if (ax.level == 0 && m.y < 0.35f * H)
                continue;
            const float l = light_at(m);
            const float w = length(g.b - g.a) * l * l;
            if (w <= 0.f)
                continue;
            total += w;
            cum.push_back(total);
            segs.push_back(i);
        }
        if (total <= 0.f)
            return;
        while (area < target && T.leaves.size() < leaf_cap)
        {
            const size_t k = std::min<size_t>(size_t(std::lower_bound(cum.begin(), cum.end(), rng.u01() * total) - cum.begin()),
                cum.size() - 1);
            const Segment& g = T.segments[segs[k]];
            const float u = rng.u01();
            const float len = length(g.b - g.a);
            spur(g.axis, g.s0 + u * len, g.a + (g.b - g.a) * u, normalize(g.b - g.a), lerpf(g.ra, g.rb, u));
        }
    }

    // Over the cap the least lit leaves go; then the leaves and blooms sorted by axis and arclength (the
    // clusters take a shoot's stretches in order).
    void finish_leaves()
    {
        if (T.leaves.size() > leaf_cap)
        {
            std::vector<std::pair<float, uint32_t>> order(T.leaves.size());
            for (uint32_t i = 0; i < T.leaves.size(); ++i)
                order[i] = {T.leaves[i].light + 0.2f * rng.u01(), i};
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<Leaf> kept(leaf_cap);
            for (uint32_t i = 0; i < leaf_cap; ++i)
                kept[i] = T.leaves[order[i].second];
            T.leaves.swap(kept);
        }
        const auto by_axis = [](const auto& a, const auto& b) { return a.axis != b.axis ? a.axis < b.axis : a.s < b.s; };
        std::stable_sort(T.leaves.begin(), T.leaves.end(), by_axis);
        std::stable_sort(T.blooms.begin(), T.blooms.end(), by_axis);
    }

    // The sky each leaf, bloom and wood segment sees past the rest of the crown (the production foliage tools' baked
    // occlusion): the leaf area splatted over a grid of a sixteenth of the bush, each cell's view of the upper
    // hemisphere marched through it in twelve cosine-weighted directions - a step of l through a leaf area density d
    // lets exp(-0.5 d l) through (a spherical leaf angle distribution, Ross and Nilson) - and each element
    // interpolated from the cells round it. Measured over a pack of summer photos, a crown's darkest tenth is a
    // twentieth of its lit leaves; a guess from a point's height and radius left it at a fifth (the crown flat and
    // pale against the meadow). The light the leaves scatter on, the shaders' floor, is not in it. A leaf's
    // exposure, the order the pixel floor thins the crown in, adds the ground's light from below (six directions, a
    // third of the sky's for a meadow's albedo): by the sky alone the crown's lower side, open to the meadow, ranked
    // with its inside and thinned with it at distance, a see-through skirt.
    void sky_visibility()
    {
        if (T.leaves.empty())
            return;
        const auto mid = [](const Leaf& f) { return f.p + f.dir * (0.5f * f.len); };
        float lo[3]{1e9f, 1e9f, 1e9f}, hi[3]{-1e9f, -1e9f, -1e9f};
        for (const Leaf& f : T.leaves)
        {
            const V3 m = mid(f);
            const float c[3]{m.x, m.y, m.z};
            for (int a = 0; a < 3; ++a)
            {
                lo[a] = std::min(lo[a], c[a]);
                hi[a] = std::max(hi[a], c[a]);
            }
        }
        // (A tree's grid is finer: cells of about half a metre, 64 a side at the most.)
        const float extent = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
        const float cell = tree ? std::max(extent / 60.f, 0.45f) : std::max(extent / 16.f, 0.04f);
        int n[3];
        float o[3];
        for (int a = 0; a < 3; ++a)
        {
            n[a] = int(std::ceil((hi[a] - lo[a]) / cell)) + 4; // at most 21: the cell is a sixteenth of the extent (a tree's 64)
            o[a] = 0.5f * (lo[a] + hi[a]) - 0.5f * cell * float(n[a]);
        }
        const auto at = [&n](int x, int y, int z) { return size_t((z * n[1] + y) * n[0] + x); };
        // The cell coordinates of a point (cell centres at whole numbers).
        const auto grid = [&o, cell](V3 p, float (&g)[3]) {
            g[0] = (p.x - o[0]) / cell - 0.5f;
            g[1] = (p.y - o[1]) / cell - 0.5f;
            g[2] = (p.z - o[2]) / cell - 0.5f;
        };
        // Trilinear weights of the eight cells round g (clamped into the grid).
        const auto corners = [&n, &at](const float (&g)[3], size_t (&idx)[8], float (&w)[8]) {
            int i0[3];
            float t[3];
            for (int a = 0; a < 3; ++a)
            {
                const float c = clampf(g[a], 0.f, float(n[a] - 1) - 1e-3f);
                i0[a] = std::min(int(c), n[a] - 2);
                t[a] = clampf(c - float(i0[a]), 0.f, 1.f);
            }
            for (int k = 0; k < 8; ++k)
            {
                const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
                idx[k] = at(std::min(i0[0] + dx, n[0] - 1), std::min(i0[1] + dy, n[1] - 1), std::min(i0[2] + dz, n[2] - 1));
                w[k] = (dx ? t[0] : 1.f - t[0]) * (dy ? t[1] : 1.f - t[1]) * (dz ? t[2] : 1.f - t[2]);
            }
        };
        // Leaf area density (m2 per m3), each leaf splatted over its eight cells.
        std::vector<float> lad(size_t(n[0]) * n[1] * n[2], 0.f);
        const float inv_vol = 1.f / (cell * cell * cell);
        for (const Leaf& f : T.leaves)
        {
            float g[3], w[8];
            size_t idx[8];
            grid(mid(f), g);
            corners(g, idx, w);
            for (int k = 0; k < 8; ++k)
                lad[idx[k]] += f.area * w[k] * inv_vol;
        }
        // Twelve directions over the upper hemisphere and six over the lower, cosine-weighted (equal weights),
        // golden-angle spirals.
        V3 up[12], down[6];
        for (int i = 0; i < 12; ++i)
        {
            const float u = (float(i) + 0.5f) / 12.f, r = std::sqrt(u), phi = 2.39996323f * float(i);
            up[i] = v3(r * std::cos(phi), std::sqrt(1.f - u), r * std::sin(phi));
        }
        for (int i = 0; i < 6; ++i)
        {
            const float u = (float(i) + 0.5f) / 6.f, r = std::sqrt(u), phi = 2.39996323f * float(i) + 1.f;
            down[i] = v3(r * std::cos(phi), -std::sqrt(1.f - u), r * std::sin(phi));
        }
        // Half a cell's path through the cell's own leaves, then a cell at a time to the grid's edge.
        const auto through = [&](int x, int y, int z, V3 d, float own) {
            float tau = own;
            for (int k = 1; k < 64 && tau < 8.f; ++k)
            {
                const float s = float(k);
                const int cx = int(std::floor(float(x) + 0.5f + d.x * s)), cy = int(std::floor(float(y) + 0.5f + d.y * s)),
                          cz = int(std::floor(float(z) + 0.5f + d.z * s));
                if (cx < 0 || cy < 0 || cz < 0 || cx >= n[0] || cy >= n[1] || cz >= n[2])
                    break;
                tau += 0.5f * lad[at(cx, cy, cz)] * cell;
            }
            return std::exp(-tau);
        };
        std::vector<float> vis(lad.size(), 1.f), opened(lad.size(), 1.f);
        for (int z = 0; z < n[2]; ++z)
            for (int y = 0; y < n[1]; ++y)
                for (int x = 0; x < n[0]; ++x)
                {
                    const float own = 0.25f * lad[at(x, y, z)] * cell;
                    float above = 0.f, below = 0.f;
                    for (const V3& d : up)
                        above += through(x, y, z, d, own);
                    for (const V3& d : down)
                        below += through(x, y, z, d, own);
                    vis[at(x, y, z)] = above / 12.f;
                    opened[at(x, y, z)] = std::min(above / 12.f + 0.3f * below / 6.f, 1.f);
                }
        const auto sample = [&](const std::vector<float>& field, V3 p) {
            float g[3], w[8];
            size_t idx[8];
            grid(p, g);
            corners(g, idx, w);
            float v = 0.f;
            for (int k = 0; k < 8; ++k)
                v += field[idx[k]] * w[k];
            return clampf(v, 0.f, 1.f);
        };
        for (Leaf& f : T.leaves)
        {
            f.light = sample(vis, mid(f));
            f.exposure = sample(opened, mid(f));
        }
        for (Bloom& b : T.blooms)
            b.vis = sample(vis, b.p);
        for (Segment& sg : T.segments)
            sg.vis = sample(vis, (sg.a + sg.b) * 0.5f);
    }

    // Each axis's bend under the reference wind and its natural frequency: a slender, thin axis bends further
    // and swings faster than a stem (a cantilever's tip slope grows with its length over its radius).
    void mechanics()
    {
        static constexpr float kLevelFreq[4] = {0.9f, 1.5f, 2.3f, 3.1f};
        for (Axis& ax : T.axes)
        {
            const float r = std::max(ax.r_base, 0.0015f);
            ax.flex = clampf(0.35f * ax.length / (ax.length + 0.5f) * std::pow(0.004f / r, 0.8f), 0.03f, 0.6f);
            // (A prickle and a dead stub are rigid: they ride their wood.)
            if (ax.leafless)
                ax.flex = 0.f;
            ax.freq = kLevelFreq[std::min(ax.level, 3u)] * clampf(1.f / std::sqrt(std::max(ax.length, 0.05f) / 0.6f), 0.7f, 1.5f);
            // (A tree's stock trunk and limbs move with its trunk's sway alone.)
            if (ax.fixed)
            {
                ax.flex = 0.f;
                ax.freq = 0.5f;
            }
        }
    }
    // The leaves an element stands for (a cluster draws them one to a cell): a spray's, a compound leaf's leaflets,
    // one otherwise.
    float leaves_of(const Leaf& f) const
    {
        if (A.leaf_kind == lkSpray)
            return std::max(std::round(A.leaflets * f.len / std::max(A.spray_len, 0.03f)), 1.f);
        return A.leaf_kind == lkCompound ? std::max(std::round(A.leaflets), 1.f) : 1.f;
    }

    // Clusters: the leaves in cells of a hand's span or two of the crown (a bigger crown, bigger cells), one element
    // each in the middle distance; it bends with the shoot that holds most of its leaf area.
    void clusters()
    {
        // (A tree's cells reach about a metre.)
        const float cell = clampf(0.1f * std::sqrt(RW * H) + 0.18f, 0.25f, tree ? 1.f : 0.55f);
        const auto key_of = [cell](V3 p) {
            const auto c = [cell](float v) { return uint64_t(int64_t(std::floor(v / cell)) + 4096) & 0x1FFFFF; };
            return (c(p.x) << 42) | (c(p.y) << 21) | c(p.z);
        };
        std::vector<std::pair<uint64_t, uint32_t>> keyed(T.leaves.size());
        for (uint32_t i = 0; i < T.leaves.size(); ++i)
            keyed[i] = {key_of(T.leaves[i].p + T.leaves[i].dir * (0.5f * T.leaves[i].len)), i};
        std::sort(keyed.begin(), keyed.end());
        // The blooms by cell with their areas (a head or a fruit cluster about half its size squared, a catkin a
        // third): a cluster draws them as cells the size of its leaves, so they count in leaves' worth of area - an
        // elder's flat heads show from afar, a hazel's nuts do not.
        std::vector<std::pair<uint64_t, float>> bloom_keys(T.blooms.size());
        for (size_t i = 0; i < T.blooms.size(); ++i)
        {
            const Bloom& b = T.blooms[i];
            bloom_keys[i] = {key_of(b.p), b.size * b.size * (b.kind == bkCatkin ? 0.3f : 0.5f)};
        }
        std::sort(bloom_keys.begin(), bloom_keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        size_t i = 0;
        while (i < keyed.size())
        {
            size_t j = i;
            V3 c{}, n{};
            float area_c = 0.f, rank = 0.f, fall = 0.f, lit = 0.f, expo = 0.f, count = 0.f, best = -1.f;
            uint32_t lead = keyed[i].second;
            while (j < keyed.size() && keyed[j].first == keyed[i].first)
            {
                const Leaf& f = T.leaves[keyed[j].second];
                const float a = f.area;
                c = c + (f.p + f.dir * (0.5f * f.len)) * a;
                const V3 fn = normalize(cross(f.dir, f.side), kUp);
                n = n + (dot(fn, vert) < 0.f ? fn * -1.f : fn) * a;
                area_c += a;
                rank += f.rank * a;
                fall += f.fall * a;
                lit += f.light * a;
                expo += f.exposure * a;
                count += leaves_of(f);
                if (a > best)
                {
                    best = a;
                    lead = keyed[j].second;
                }
                ++j;
            }
            Cluster cl;
            const float inv = area_c > 0.f ? 1.f / area_c : 0.f;
            cl.c = area_c > 0.f ? c * inv : T.leaves[keyed[i].second].p;
            cl.n = normalize(n + normalize(v3(cl.c.x, 0.f, cl.c.z), kUp) * (0.5f * std::sqrt(area_c)), kUp);
            // Its radius: the leaves' area-weighted spread (a disc as wide as the farthest leaf would thin the cover
            // the shaders give its leaf area), but never under the largest leaf.
            float r2 = 0.f, r_leaf = 0.f;
            for (size_t k = i; k < j; ++k)
            {
                const Leaf& f = T.leaves[keyed[k].second];
                const V3 d = f.p + f.dir * (0.5f * f.len) - cl.c;
                r2 += (dot(d, d) + 0.25f * f.len * f.len) * f.area;
                r_leaf = std::max(r_leaf, 0.5f * f.len);
            }
            cl.r = std::max(std::max(std::sqrt(r2 * inv) * 1.4f, r_leaf), 0.02f);
            cl.area = area_c;
            cl.axis = T.leaves[lead].axis;
            cl.s = T.leaves[lead].s;
            cl.first = uint32_t(i);
            cl.leaves = uint32_t(count);
            cl.rank = rank * inv;
            cl.fall = fall * inv;
            cl.light = lit * inv;
            cl.exposure = expo * inv;
            float bloom_area = 0.f;
            for (auto it = std::lower_bound(bloom_keys.begin(), bloom_keys.end(), keyed[i].first,
                     [](const auto& e, uint64_t k) { return e.first < k; });
                 it != bloom_keys.end() && it->first == keyed[i].first; ++it)
                bloom_area += it->second;
            // Rounded by the cell's own hash, so the fractions of a cell add up over the bush.
            const float leaf_mean = count > 0.f ? area_c / count : 1.f;
            const float eq = std::min(bloom_area / std::max(leaf_mean, 1e-5f), 255.f);
            uint32_t kh = uint32_t(keyed[i].first ^ (keyed[i].first >> 32)) * 0x9E3779B1u;
            kh ^= kh >> 15;
            cl.blooms = uint32_t(eq + float(kh >> 8) * (1.f / 16777216.f));
            T.clusters.push_back(cl);
            i = j;
        }
    }

    // Blobs: k-means over the clusters, weighted by their leaf area.
    void blobs()
    {
        if (T.clusters.empty())
            return;
        // (Many small blobs: a dozen large ones read as stacked discs, the crown's clumps and gaps lost; forty over a
        // large bush reached past its shoots - a dog rose's blobs into the sky over its arching canes.)
        // (A tree's crown: 64 to 512 blobs, larger ones; a conifer's up to 2048 smaller ones - its whorls' tiers and its
        // spire are its silhouette, and 512 blobs of a metre made a forty-metre spruce a lumpy column.)
        const bool needles = A.leaf_kind == lkNeedles;
        const uint32_t k = tree ? uint32_t(clampf(std::round(T.leaf_area / (needles ? 0.25f : 0.6f)), needles ? 128.f : 64.f, needles ? 2048.f : 512.f))
                                : uint32_t(clampf(std::round(T.leaf_area / 0.15f), 6.f, 64.f));
        std::vector<V3> centre(k);
        for (uint32_t i = 0; i < k; ++i)
            centre[i] = T.clusters[(size_t(i) * T.clusters.size()) / k].c;
        std::vector<uint32_t> owner(T.clusters.size(), 0);
        for (int iter = 0; iter < 10; ++iter)
        {
            for (size_t c = 0; c < T.clusters.size(); ++c)
            {
                float best = 1e30f;
                for (uint32_t i = 0; i < k; ++i)
                {
                    const V3 d = T.clusters[c].c - centre[i];
                    const float dd = dot(d, d);
                    if (dd < best)
                    {
                        best = dd;
                        owner[c] = i;
                    }
                }
            }
            std::vector<V3> sum(k);
            std::vector<float> w(k, 0.f);
            for (size_t c = 0; c < T.clusters.size(); ++c)
            {
                sum[owner[c]] = sum[owner[c]] + T.clusters[c].c * T.clusters[c].area;
                w[owner[c]] += T.clusters[c].area;
            }
            for (uint32_t i = 0; i < k; ++i)
                if (w[i] > 0.f)
                    centre[i] = sum[i] * (1.f / w[i]);
        }
        // The foliage's centre and half extents (by leaf area): a blob grown past its clusters is drawn in by its
        // growth on the crown's outside, so the outline stays where the clusters end.
        V3 cc{};
        float cw = 0.f;
        for (const Cluster& cl : T.clusters)
        {
            cc = cc + cl.c * cl.area;
            cw += cl.area;
        }
        cc = cc * (1.f / std::max(cw, 1e-6f));
        V3 ext{0.05f, 0.05f, 0.05f};
        for (const Cluster& cl : T.clusters)
            ext = v3(std::max(ext.x, std::abs(cl.c.x - cc.x)), std::max(ext.y, std::abs(cl.c.y - cc.y)), std::max(ext.z, std::abs(cl.c.z - cc.z)));
        for (uint32_t i = 0; i < k; ++i)
        {
            Blob b;
            b.c = centre[i];
            float area_b = 0.f, blooms = 0.f, leaves = 0.f, r2 = 0.f, rank = 0.f, fall = 0.f, lit = 0.f, lit_w = 0.f;
            V3 n{};
            for (size_t c = 0; c < T.clusters.size(); ++c)
            {
                if (owner[c] != i)
                    continue;
                const Cluster& cl = T.clusters[c];
                area_b += cl.area;
                blooms += float(cl.blooms);
                leaves += float(cl.leaves);
                n = n + cl.n * cl.area;
                rank += cl.rank * cl.area;
                fall += cl.fall * cl.area;
                // The sky the blob's face sees: its clusters weighted by their own (the outer ones are the ones seen;
                // a plain mean took a dense crown's dark inside onto its far face).
                lit += cl.light * cl.light * cl.area;
                lit_w += cl.light * cl.area;
                // The blob's radius: the area-weighted spread of its clusters.
                const V3 d = cl.c - centre[i];
                r2 += (dot(d, d) + cl.r * cl.r * 0.5f) * cl.area;
            }
            if (area_b <= 0.f)
                continue;
            b.area = area_b;
            // Two fifths wider than its clusters' spread, so neighbours overlap into one mass (blobs that only met
            // left the meadow showing between them: stacked coins from afar); the shadow maps take the spread
            // (r3\da_shrub_common.h SHRUB_BLOB_GROW mirrors the factor).
            constexpr float kBlobGrow = 1.4f;
            const float r_spread = std::max(std::sqrt(r2 / area_b) * 1.25f, 0.05f);
            b.r = r_spread * kBlobGrow;
            const V3 off = b.c - cc;
            const float outer = clampf(length(v3(off.x / ext.x, off.y / ext.y, off.z / ext.z)), 0.f, 1.f);
            b.pull = (b.r - r_spread) * outer;
            // (Along the crown's normal there - the gradient of the bounds' ellipsoid, the record's normal - which the
            // shadow maps push it back out along.)
            const V3 bc = (T.bmin + T.bmax) * 0.5f, be = (T.bmax - T.bmin) * 0.5f, m = b.c - bc;
            const V3 grad = v3(m.x / std::max(be.x * be.x, 1e-4f), m.y / std::max(be.y * be.y, 1e-4f), m.z / std::max(be.z * be.z, 1e-4f));
            b.c = b.c - normalize(grad, kUp) * b.pull;
            // Its leaves cover the wider disc by the share their own spread covered (r3\da_shrub_cluster_vs.h takes the
            // Beer-Lambert cover of the area over the disc, a needle tuft's projection 0.65, a leaf's 0.5): the same
            // area over a disc twice as big overlapped itself less and grew the far crowns by two fifths.
            const float proj = A.leaf_kind == lkNeedles ? 0.65f : 0.5f;
            const float cover = (1.f - std::exp(-proj * area_b / (kPi * r_spread * r_spread))) * (r_spread * r_spread) / (b.r * b.r);
            b.area = -kPi * b.r * b.r * std::log(std::max(1.f - cover, 1e-4f)) / proj;
            b.n = normalize(n, kUp);
            b.bloom_share = blooms / std::max(leaves + blooms, 1.f);
            b.rank = rank / area_b;
            b.fall = fall / area_b;
            b.light = lit_w > 0.f ? lit / lit_w : 0.f;
            T.blobs.push_back(b);
        }
    }

    // ================================================================================================
    // Trees (generate_on_skeleton)
    // ================================================================================================
    // A field of the crown grid (cell centres at origin + (i + 0.5) * cell) at p, trilinear; outside the grid 0.
    float tree_field(const std::vector<float>& f, V3 p) const
    {
        const TreeState& S = *tree;
        if (f.empty())
            return 0.f;
        const float gx = (p.x - S.o.x) / S.cell - 0.5f, gy = (p.y - S.o.y) / S.cell - 0.5f, gz = (p.z - S.o.z) / S.cell - 0.5f;
        const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy)), z0 = int(std::floor(gz));
        const float tx = gx - float(x0), ty = gy - float(y0), tz = gz - float(z0);
        float v = 0.f;
        for (int k = 0; k < 8; ++k)
        {
            const int x = x0 + (k & 1), y = y0 + ((k >> 1) & 1), z = z0 + (k >> 2);
            if (x < 0 || y < 0 || z < 0 || x >= S.nx || y >= S.ny || z >= S.nz)
                continue;
            const float w = ((k & 1) ? tx : 1.f - tx) * (((k >> 1) & 1) ? ty : 1.f - ty) * ((k >> 2) ? tz : 1.f - tz);
            v += f[(size_t(y) * size_t(S.nz) + size_t(z)) * size_t(S.nx) + size_t(x)] * w;
        }
        return v;
    }

    // The crown's volume in the skeleton's grid (tools/trees/FORMAT.md: an opaque card's cell 1 - exp(-area / cell^2), the
    // closing's cells at least 64/255 and the enclosed ones 40/255 - room for branches): the cells at this occupancy and
    // above; a margin lets the wood a little past it.
    static constexpr float kCrownOcc = 0.12f;
    bool tree_inside(V3 p, float margin) const
    {
        return dot(p, vert) >= -0.05f && tree_field(tree->occ, p) >= kCrownOcc * (1.f - 2.f * margin);
    }
    float tree_step() const { return std::min(std::max(0.02f * H, 0.01f), std::max(0.5f * tree->cell, 0.05f)); }
    float tree_upper(V3 p) const { return clampf((dot(p, vert) - tree->lo_y) / std::max(tree->hi_y - tree->lo_y, 0.5f), 0.f, 1.f); }

    // How lit a point of the crown sits: the outer shell (shallow in the crown field) and the top.
    float tree_light(V3 p) const
    {
        const float outer = 1.f - clampf(tree_field(tree->depth, p), 0.f, 1.f);
        return clampf(0.12f + 0.88f * (0.6f * outer * outer + 0.4f * tree_upper(p)), 0.f, 1.f);
    }

    // A tree's leaves: leaf-out at once over the whole crown (Zahnd et al. 2023); autumn by the species' autumn_order -
    // the weight of the height (the top first: the lower crown keeps its leaves up to two weeks longer) and of the
    // shade (+ the inside first: the early-successional birch, aspen, poplar and willow; - the outside first: maple and
    // oak, Kitao et al. 2022), the rest of the order each leaf's own.
    Order tree_order(V3 p)
    {
        const float lit = tree_light(p), upper = tree_upper(p);
        const float hw = clampf(A.autumn_order[0], 0.f, 0.9f), sw = clampf(A.autumn_order[1], -0.9f, 0.9f);
        const float own = std::max(1.f - hw - std::abs(sw), 0.1f);
        const float shade = sw >= 0.f ? sw * lit : -sw * (1.f - lit);
        return {clampf(0.9f * rng.u01() + 0.1f * upper, 0.f, 1.f), clampf(own * rng.u01() + hw * (1.f - upper) + shade, 0.f, 1.f), lit};
    }

    // The leaf area a tree aims at: its species' LAI over the crown's larger projection (from above, or three fifths
    // of the side's: a narrow crown - a spruce's, a young tree's - is seen from the side). The share of the crown its
    // shoots fill (crown_fill) places them - clumps and gaps - and leaves the leaf area whole: taken off it too, the
    // crowns showed a third to a half of their photos' cover.
    float tree_leaf_target() const
    {
        return A.lai * std::max(tree->top_area, 0.6f * tree->side_area);
    }

    // A point for a filling shoot: a crown cell by its occupancy, in its lit shell (the outer `shell` of its depth), of
    // the share of the cells the species' shoots fill (crown_fill, by a noise of about a metre: clumps and gaps).
    bool tree_fill_point(V3& p, float shell)
    {
        const TreeState& S = *tree;
        if (S.cells.empty())
            return false;
        const float x = rng.u01() * S.cum.back();
        const size_t k = std::min<size_t>(size_t(std::lower_bound(S.cum.begin(), S.cum.end(), x) - S.cum.begin()), S.cells.size() - 1);
        const uint32_t c = S.cells[k];
        const uint32_t ix = c % uint32_t(S.nx), iz = (c / uint32_t(S.nx)) % uint32_t(S.nz), iy = c / (uint32_t(S.nx) * uint32_t(S.nz));
        p = v3(S.o.x + (float(ix) + rng.u01()) * S.cell, S.o.y + (float(iy) + rng.u01()) * S.cell, S.o.z + (float(iz) + rng.u01()) * S.cell);
        if (tree_field(S.depth, p) > shell)
            return false;
        if (A.crown_fill < 0.999f)
        {
            // (The value noise of the shrubs' clumps over cells of about a metre.)
            const float save = inv_clump;
            inv_clump = 1.f / std::max(1.f, 2.f * S.cell);
            const float n = clump_noise(p + v3(17.3f, 5.1f, -9.7f));
            inv_clump = save;
            if (n > A.crown_fill)
                return false;
        }
        return tree_field(S.occ, p) >= kCrownOcc;
    }

    // The clearance of a new wood's point from the stock wood: false where it comes within `need` of a stock tube's
    // surface - the axis being grown's own joint with its stock parent exempt.
    bool tree_clear(V3 q, float need) const
    {
        const TreeState& S = *tree;
        if (S.g_items.empty())
            return true;
        const float reach = need + S.max_r;
        int lo[3], hi[3];
        const float qc[3]{q.x, q.y, q.z}, oc[3]{S.g_lo.x, S.g_lo.y, S.g_lo.z};
        for (int a = 0; a < 3; ++a)
        {
            lo[a] = std::max(int(std::floor((qc[a] - reach - oc[a]) / S.g_cell)), 0);
            hi[a] = std::min(int(std::floor((qc[a] + reach - oc[a]) / S.g_cell)), S.g_n[a] - 1);
        }
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int x = lo[0]; x <= hi[0]; ++x)
                {
                    const size_t c = (size_t(y) * size_t(S.g_n[2]) + size_t(z)) * size_t(S.g_n[0]) + size_t(x);
                    for (uint32_t k = S.g_start[c]; k < S.g_start[c + 1]; ++k)
                    {
                        const Segment& g = T.segments[S.g_items[k]];
                        const V3 ab = g.b - g.a;
                        const float u = clampf(dot(q - g.a, ab) / std::max(dot(ab, ab), 1e-8f), 0.f, 1.f);
                        const float d = length(q - (g.a + ab * u)) - lerpf(g.ra, g.rb, u);
                        if (d >= need)
                            continue;
                        if (int32_t(g.axis) == S.parent && std::abs(g.s0 + u * length(ab) - S.attach) < 3.f * S.parent_r + 0.15f)
                            continue;
                        return false;
                    }
                }
        return true;
    }

    // A grafted wood's radius at p: eight centimetres across and more only where the player cannot reach it - not within
    // two metres above the ground or above a stock limb's standable top within a metre and a half (the new wood has no
    // collision: a branch one can see but not stand on or bump into).
    float tree_radius_cap(V3 p, float r) const
    {
        if (r < 0.04f)
            return r;
        const TreeState& S = *tree;
        const float h = dot(p, vert);
        if (h < 2.f)
            return 0.039f;
        if (S.st_items.empty())
            return r;
        const int x0 = std::max(int(std::floor((p.x - 1.5f - S.st_lo.x) / S.st_cell)), 0);
        const int x1 = std::min(int(std::floor((p.x + 1.5f - S.st_lo.x) / S.st_cell)), S.st_n[0] - 1);
        const int z0 = std::max(int(std::floor((p.z - 1.5f - S.st_lo.z) / S.st_cell)), 0);
        const int z1 = std::min(int(std::floor((p.z + 1.5f - S.st_lo.z) / S.st_cell)), S.st_n[1] - 1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const size_t c = size_t(z) * size_t(S.st_n[0]) + size_t(x);
                for (uint32_t k = S.st_start[c]; k < S.st_start[c + 1]; ++k)
                {
                    const V3& s = S.stands[S.st_items[k]];
                    const float dx = p.x - s.x, dz = p.z - s.z;
                    if (dx * dx + dz * dz < 2.25f && p.y - s.y < 2.f && p.y > s.y - 0.5f)
                        return 0.039f;
                }
            }
        return r;
    }

    // The heading of a grafted branch along a card branch's axis (its guide): pulled toward where the guide runs a step
    // ahead, by the species' stock_card_follow.
    V3 tree_follow(V3 db, V3 p, float s, float ds) const
    {
        const TreeState& S = *tree;
        const TreeState::Guide* G = S.guide;
        if (!G || S.follow <= 0.f || G->pts.size() < 2)
            return db;
        const float at = std::min(s + 2.f * ds, G->len);
        size_t k = 1;
        while (k + 1 < G->pts.size() && G->s[k] < at)
            ++k;
        const float u = clampf((at - G->s[k - 1]) / std::max(G->s[k] - G->s[k - 1], 1e-4f), 0.f, 1.f);
        const V3 target = G->pts[k - 1] + (G->pts[k] - G->pts[k - 1]) * u;
        const V3 aim = normalize(target - p, db);
        return normalize(db + (aim - db) * clampf(S.follow, 0.f, 1.f), db);
    }

    // ---- The crown field ----
    // The skeleton's occupancy grid as a field (0..1), each occupied cell's depth into the crown (a 3-4-5 chamfer
    // distance to the nearest empty cell, over the deepest), the occupied cells and their weights for sampling, the
    // crown's projected areas; without a grid, the upper four fifths of the skeleton's box as an ellipsoid.
    void tree_crown()
    {
        TreeState& S = *tree;
        const Skeleton& K = *S.S;
        S.nx = int(K.nx);
        S.ny = int(K.ny);
        S.nz = int(K.nz);
        S.o = K.origin;
        S.cell = K.cell;
        const size_t n = size_t(S.nx) * size_t(S.ny) * size_t(S.nz);
        if (n == 0 || K.grid.size() < n || S.cell <= 0.f)
        {
            // An ellipsoid in the box's upper four fifths, on a grid of about half a metre.
            const V3 ext = K.bmax - K.bmin;
            S.cell = std::max(std::max(ext.x, std::max(ext.y, ext.z)) / 48.f, 0.25f);
            S.nx = std::max(int(std::ceil(ext.x / S.cell)), 1);
            S.ny = std::max(int(std::ceil(ext.y / S.cell)), 1);
            S.nz = std::max(int(std::ceil(ext.z / S.cell)), 1);
            S.o = K.bmin;
            S.occ.assign(size_t(S.nx) * size_t(S.ny) * size_t(S.nz), 0.f);
            const V3 c = v3(0.5f * (K.bmin.x + K.bmax.x), K.bmin.y + 0.6f * ext.y, 0.5f * (K.bmin.z + K.bmax.z));
            const V3 e = v3(std::max(0.5f * ext.x, 0.3f), std::max(0.4f * ext.y, 0.3f), std::max(0.5f * ext.z, 0.3f));
            for (int y = 0; y < S.ny; ++y)
                for (int z = 0; z < S.nz; ++z)
                    for (int x = 0; x < S.nx; ++x)
                    {
                        const V3 p = S.o + v3(float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f) * S.cell - c;
                        const float q = (p.x * p.x) / (e.x * e.x) + (p.y * p.y) / (e.y * e.y) + (p.z * p.z) / (e.z * e.z);
                        S.occ[(size_t(y) * size_t(S.nz) + size_t(z)) * size_t(S.nx) + size_t(x)] = clampf((1.f - q) * 4.f, 0.f, 1.f);
                    }
        }
        else
        {
            S.occ.resize(n);
            for (size_t i = 0; i < n; ++i)
                S.occ[i] = float(K.grid[i]) * (1.f / 255.f);
            tree_crown_islands();
            tree_apex();
        }
        const size_t cells = S.occ.size();
        const auto at = [&S](int x, int y, int z) { return (size_t(y) * size_t(S.nz) + size_t(z)) * size_t(S.nx) + size_t(x); };
        // The depth: a chamfer distance (3-4-5 in thirds of a cell) to the nearest empty cell, two passes.
        std::vector<float> dist(cells);
        for (size_t i = 0; i < cells; ++i)
            dist[i] = S.occ[i] >= kCrownOcc ? 1e9f : 0.f;
        const auto relax = [&](int x, int y, int z, int dir) {
            float& d = dist[at(x, y, z)];
            if (d == 0.f)
                return;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        // (Only the neighbours already visited in this pass's order.)
                        const int order = (dy * 3 + dz) * 3 + dx;
                        if (order * dir >= 0)
                            continue;
                        const int X = x + dx, Y = y + dy, Z = z + dz;
                        const int m = std::abs(dx) + std::abs(dy) + std::abs(dz);
                        const float w = m == 1 ? 3.f : (m == 2 ? 4.f : 5.f);
                        const float dn = (X < 0 || Y < 0 || Z < 0 || X >= S.nx || Y >= S.ny || Z >= S.nz) ? w : dist[at(X, Y, Z)] + w;
                        d = std::min(d, dn);
                    }
        };
        for (int y = 0; y < S.ny; ++y)
            for (int z = 0; z < S.nz; ++z)
                for (int x = 0; x < S.nx; ++x)
                    relax(x, y, z, 1);
        for (int y = S.ny - 1; y >= 0; --y)
            for (int z = S.nz - 1; z >= 0; --z)
                for (int x = S.nx - 1; x >= 0; --x)
                    relax(x, y, z, -1);
        float deepest = 1.f;
        for (const float d : dist)
            if (d < 1e8f)
                deepest = std::max(deepest, d);
        S.depth.assign(cells, 1.f);
        S.cells.clear();
        S.cum.clear();
        float total = 0.f;
        std::vector<uint8_t> top(size_t(S.nx) * size_t(S.nz), 0), side_x(size_t(S.nx) * size_t(S.ny), 0), side_z(size_t(S.nz) * size_t(S.ny), 0);
        S.lo_y = 1e9f;
        S.hi_y = -1e9f;
        for (int y = 0; y < S.ny; ++y)
            for (int z = 0; z < S.nz; ++z)
                for (int x = 0; x < S.nx; ++x)
                {
                    const size_t i = at(x, y, z);
                    // (The depth's distance is in thirds of a cell; the surface layer at one.)
                    S.depth[i] = dist[i] > 0.f && dist[i] < 1e8f ? clampf((dist[i] - 3.f) / std::max(deepest - 3.f, 1.f), 0.f, 1.f) : 0.f;
                    if (S.occ[i] < kCrownOcc)
                        continue;
                    total += S.occ[i];
                    S.cells.push_back(uint32_t(i));
                    S.cum.push_back(total);
                    top[size_t(z) * size_t(S.nx) + size_t(x)] = 1;
                    side_x[size_t(y) * size_t(S.nx) + size_t(x)] = 1;
                    side_z[size_t(y) * size_t(S.nz) + size_t(z)] = 1;
                    const float yc = S.o.y + (float(y) + 0.5f) * S.cell;
                    S.lo_y = std::min(S.lo_y, yc);
                    S.hi_y = std::max(S.hi_y, yc);
                }
        const auto count = [](const std::vector<uint8_t>& v) { return float(std::count(v.begin(), v.end(), uint8_t(1))); };
        S.top_area = count(top) * S.cell * S.cell;
        S.side_area = std::max(count(side_x), count(side_z)) * S.cell * S.cell;
        if (S.cells.empty())
        {
            S.lo_y = 0.f;
            S.hi_y = H;
        }
    }

    // The crown field's stray parts left out before anything grows into them: a part of the crown under 2 % of its
    // volume, or one under 15 % standing more than 1.5 m off the main body (a few cards' worth of cells: the shoots that
    // reached for them hung out of the crown as wires).
    void tree_crown_islands()
    {
        TreeState& S = *tree;
        const int nx = S.nx, ny = S.ny, nz = S.nz;
        const size_t n = S.occ.size();
        const auto at = [nx, nz](int x, int y, int z) { return (size_t(y) * size_t(nz) + size_t(z)) * size_t(nx) + size_t(x); };
        std::vector<int32_t> label(n, -1);
        std::vector<uint32_t> sizes;
        std::vector<size_t> stack;
        for (size_t i = 0; i < n; ++i)
        {
            if (label[i] >= 0 || S.occ[i] < kCrownOcc)
                continue;
            const int32_t id = int32_t(sizes.size());
            uint32_t count = 0;
            stack.assign(1, i);
            label[i] = id;
            while (!stack.empty())
            {
                const size_t c = stack.back();
                stack.pop_back();
                ++count;
                const int x = int(c % size_t(nx)), z = int((c / size_t(nx)) % size_t(nz)), y = int(c / (size_t(nx) * size_t(nz)));
                const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
                for (const auto& q : nb)
                {
                    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= nx || q[1] >= ny || q[2] >= nz)
                        continue;
                    const size_t k = at(q[0], q[1], q[2]);
                    if (label[k] < 0 && S.occ[k] >= kCrownOcc)
                    {
                        label[k] = id;
                        stack.push_back(k);
                    }
                }
            }
            sizes.push_back(count);
        }
        if (sizes.size() < 2)
            return;
        uint64_t total = 0;
        for (const uint32_t c : sizes)
            total += c;
        const int32_t main_id = int32_t(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
        // The distance from the main body (a chamfer 3-4-5 in thirds of a cell, two passes).
        std::vector<float> dist(n);
        for (size_t i = 0; i < n; ++i)
            dist[i] = label[i] == main_id ? 0.f : 1e9f;
        const auto relax = [&](int x, int y, int z, int dir) {
            float& d = dist[at(x, y, z)];
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int order = (dy * 3 + dz) * 3 + dx;
                        if (order * dir >= 0)
                            continue;
                        const int X = x + dx, Y = y + dy, Z = z + dz;
                        if (X < 0 || Y < 0 || Z < 0 || X >= nx || Y >= ny || Z >= nz)
                            continue;
                        const int m = std::abs(dx) + std::abs(dy) + std::abs(dz);
                        d = std::min(d, dist[at(X, Y, Z)] + (m == 1 ? 3.f : (m == 2 ? 4.f : 5.f)));
                    }
        };
        for (int y = 0; y < ny; ++y)
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x)
                    relax(x, y, z, 1);
        for (int y = ny - 1; y >= 0; --y)
            for (int z = nz - 1; z >= 0; --z)
                for (int x = nx - 1; x >= 0; --x)
                    relax(x, y, z, -1);
        std::vector<float> gap(sizes.size(), 1e9f);
        for (size_t i = 0; i < n; ++i)
            if (label[i] >= 0)
                gap[size_t(label[i])] = std::min(gap[size_t(label[i])], dist[i] * S.cell / 3.f);
        for (size_t i = 0; i < n; ++i)
        {
            if (label[i] < 0 || label[i] == main_id)
                continue;
            const double share = double(sizes[size_t(label[i])]) / double(total);
            if (share < 0.02 || (share < 0.15 && gap[size_t(label[i])] > 1.5f))
                S.occ[i] = 0.f;
        }
    }

    // A point within `reach` metres of the crown field.
    bool tree_near_crown(V3 p, float reach) const
    {
        const TreeState& S = *tree;
        const float step = std::max(S.cell, 0.25f);
        for (float dy = -reach; dy <= reach; dy += step)
            for (float dz = -reach; dz <= reach; dz += step)
                for (float dx = -reach; dx <= reach; dx += step)
                    if (dx * dx + dy * dy + dz * dz <= reach * reach && tree_field(S.occ, p + v3(dx, dy, dz)) >= kCrownOcc)
                        return true;
        return false;
    }

    // A stock limb the grafted crown left bare past its foliage: a conifer's limbs below and outside its live crown are
    // self-pruned - dead, weathered grey, short stubs left of their side shoots; a broadleaf limb whose tip is within
    // 1.5 m of the crown sprouts short leafy shoots along its outer third (epicormic and short shoots), any other dies
    // back as a conifer's does. The trunks keep their wood.
    void tree_bare_limbs()
    {
        const uint32_t count = uint32_t(T.axes.size());
        // How far along each stock axis its foliage reaches: the attachment of each leaf's chain of branches on it.
        std::vector<float> reach(count, -1.f);
        for (const Leaf& f : T.leaves)
        {
            uint32_t a = f.axis;
            float s = f.s;
            while (a < count && !T.axes[a].fixed && T.axes[a].parent >= 0)
            {
                s = T.axes[a].attach;
                a = uint32_t(T.axes[a].parent);
            }
            if (a < count && T.axes[a].fixed)
                reach[a] = std::max(reach[a], s);
        }
        const bool conifer = (tree->S->flags & skelConifer) || A.leaf_kind == lkNeedles;
        for (uint32_t a = 0; a < count; ++a)
        {
            const Axis ax = T.axes[a];
            if (!ax.fixed || ax.parent < 0 || ax.length < 0.5f)
                continue;
            const float have = std::max(reach[a], 0.f), bare = ax.length - have;
            if (bare < std::max(1.f, 0.25f * ax.length))
                continue;
            V3 tip, t;
            float r;
            sample(ax, ax.length, tip, t, r);
            if (!conifer && tree_near_crown(tip, 1.5f))
            {
                // Short leafy shoots along the limb's outer third, three or four a metre.
                const float s0 = std::max(have, ax.length * (2.f / 3.f));
                for (float s = s0 + rng.range(0.05f, 0.3f); s < ax.length - 0.05f; s += rng.range(0.22f, 0.38f))
                {
                    V3 p, tt;
                    float rr;
                    sample(T.axes[a], s, p, tt, rr);
                    const V3 radial = rotate(perpendicular(tt), tt, rng.u01() * 2.f * kPi);
                    const V3 d = normalize(radial + vert * 0.6f + tt * 0.3f, tt);
                    const float len = rng.range(0.15f, 0.4f);
                    const float r0 = clampf(0.0011f + 0.004f * len, 0.0009f, std::max(std::min(rr * 0.5f, 0.006f), 0.0009f));
                    tree->parent = int32_t(a);
                    tree->attach = s;
                    tree->parent_r = rr;
                    const uint32_t ai = grow(3, p + d * rr, d, len, r0, int32_t(a), s, 2.f * A.fill_up, 0.f, 0.25f, 0.06f);
                    young_from.push_back(0.f);
                    shoot_leaves(ai, 0.f);
                }
                tree->parent = -1;
                continue;
            }
            // Dead from a little past its last foliage, short stubs of its side shoots along that stretch.
            const float from = reach[a] >= 0.f ? std::min(have + 0.3f, ax.length) : 0.f;
            T.axes[a].dead_from = from;
            for (float s = from + rng.range(0.1f, 0.3f); s < ax.length - 0.05f; s += rng.range(0.2f, 0.45f))
            {
                V3 p, tt;
                float rr;
                sample(T.axes[a], s, p, tt, rr);
                const float u = (s - from) / std::max(ax.length - from, 1e-3f);
                const V3 radial = rotate(perpendicular(tt), tt, rng.u01() * 2.f * kPi);
                const V3 d = normalize(radial + tt * 0.5f - vert * 0.1f, tt);
                const float len = lerpf(0.25f, 0.06f, u) * rng.range(0.6f, 1.f);
                const float r0 = clampf(rr * 0.35f, 0.003f, 0.02f);
                tree->parent = int32_t(a);
                tree->attach = s;
                tree->parent_r = rr;
                const uint32_t ai = grow(2, p + d * rr, d, len, r0, int32_t(a), s, 0.f, 0.f, 0.05f, 0.08f, r0 * 0.5f);
                T.axes[ai].dead = T.axes[ai].leafless = true;
                young_from.push_back(T.axes[ai].length);
            }
            tree->parent = -1;
        }
    }

    // The sixteenths of each stock ring the stock patches lie over, seen from the axis: the ray from the ring's centre
    // out through the tube's own point there (its profile) meets an outward stock triangle first, 15 cm past the point
    // at the most - any axis's: a fork's patch hangs on the parent -, and so do the rays through the points half way to
    // both neighbouring sixteenths and to both neighbouring rings (a patch's border meets good tube: no dent past it);
    // the own ray alone where the point stands out past that triangle by more than the tolerance or the ring's fit
    // error is over it (the patches carry its shape, FORMAT.md); an axis's end rings only where they stand out so. Each
    // covered sixteenth steps back as far as it and the middles of the quads round it stand out along their rays and a
    // centimetre (Ring::back's level); a ring covered all round is hidden (kRingHidden: no tube between two of them).
    // Then the bark triangles' corners and middles (4 mm out, as drawn; a cut end's cap lies across the tube's open end)
    // the tube as drawn still buries by more than the tolerance, seen from the axis each is skinned to - a fork's
    // crotch, where the rays from the ring's centre run on inside the other limb: the two sixteenths round each such
    // point on both rings of its stretch step back a centimetre under it where that stays out of sight - the corner under
    // a patch (its ray meets one) or inside another limb's drawn tube (no ring hidden by them).
    void tree_patch_cover()
    {
        struct Face
        {
            V3 a, e0, e1, n;
        };
        std::vector<Face> faces;
        std::vector<uint8_t> cap_face;
        // (The stock triangles as the stock has them, in a grid of half a metre by their boxes and 15 cm round them: a
        // ray sampled every 25 cm meets every triangle it crosses in its samples' cells.)
        static constexpr float kCell = 0.5f, kReach = 0.15f;
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        const auto key = [](V3 p) {
            const auto c = [](float x) { return uint64_t(uint32_t(int(std::floor(x / kCell)) + 1048576)); };
            return (c(p.x) << 42) | (c(p.y) << 21) | c(p.z);
        };
        for (size_t f = 0; f + 2 < T.patch_tris_far.size(); f += 3)
        {
            const V3 p0 = T.patch_verts[T.patch_tris_far[f]].p, p1 = T.patch_verts[T.patch_tris_far[f + 1]].p,
                     p2 = T.patch_verts[T.patch_tris_far[f + 2]].p;
            const V3 n = cross(p1 - p0, p2 - p0);
            if (length(n) < 1e-10f)
                continue;
            const uint32_t id = uint32_t(faces.size());
            faces.push_back({p0, p1 - p0, p2 - p0, normalize(n)});
            cap_face.push_back(f / 3 >= T.patch_bark_tris_far ? 1 : 0);
            const V3 lo = v3(std::min({p0.x, p1.x, p2.x}), std::min({p0.y, p1.y, p2.y}), std::min({p0.z, p1.z, p2.z})) - v3(kReach, kReach, kReach);
            const V3 hi = v3(std::max({p0.x, p1.x, p2.x}), std::max({p0.y, p1.y, p2.y}), std::max({p0.z, p1.z, p2.z})) + v3(kReach, kReach, kReach);
            for (float x = std::floor(lo.x / kCell); x <= std::floor(hi.x / kCell); x += 1.f)
                for (float y = std::floor(lo.y / kCell); y <= std::floor(hi.y / kCell); y += 1.f)
                    for (float z = std::floor(lo.z / kCell); z <= std::floor(hi.z / kCell); z += 1.f)
                        grid[key(v3(x + 0.5f, y + 0.5f, z + 0.5f) * kCell)].push_back(id);
        }
        if (faces.empty())
            return;
        // How far the tube's point p stands out past the first stock triangle the ray from c through it meets (under it:
        // negative), when that one faces outward and lies 15 cm past p at the most.
        bool hit_cap = false;
        float hit_cos = 0.f;
        const auto probe = [&](V3 c, V3 p, float& out) {
            hit_cap = false;
            hit_cos = 0.f;
            V3 d = p - c;
            const float rho = length(d);
            if (rho < 1e-6f)
                return false;
            d = d * (1.f / rho);
            const float len = rho + kReach;
            float best = len;
            bool outward = false;
            uint64_t last = ~0ull;
            for (float s = 0.f;; s += 0.25f)
            {
                const uint64_t k = key(c + d * std::min(s, len));
                const auto it = k != last ? grid.find(k) : grid.end();
                last = k;
                if (it != grid.end())
                    for (const uint32_t id : it->second)
                    {
                        // (Moller and Trumbore.)
                        const Face& F = faces[id];
                        const V3 pv = cross(d, F.e1);
                        const float det = dot(F.e0, pv);
                        if (std::fabs(det) < 1e-12f)
                            continue;
                        const V3 tv = c - F.a, qv = cross(tv, F.e0);
                        const float u = dot(tv, pv) / det, v = dot(d, qv) / det, t = dot(F.e1, qv) / det;
                        if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f || t <= 0.f || t >= best)
                            continue;
                        best = t;
                        outward = dot(F.n, d) > 0.f;
                        hit_cap = cap_face[id] != 0;
                        hit_cos = std::fabs(dot(F.n, d));
                    }
                if (s >= len)
                    break;
            }
            out = rho - best;
            return best < len && outward;
        };
        const auto point = [](const Ring& r, float th) {
            float k = 1.f;
            for (int m = 1; m <= 3; ++m)
                k += r.f[2 * m - 2] * std::cos(float(m) * th) + r.f[2 * m - 1] * std::sin(float(m) * th);
            return r.c + (r.n * std::cos(th) + cross(r.t, r.n) * std::sin(th)) * (r.r0 * k);
        };
        const float step = 2.f * kPi / 16.f;
        // A sixteenth covered with a step back of `need` at least (ring_step_back's level rounded up; a deeper one kept).
        const auto deepen = [&point, step](Ring& r, uint32_t j, float need) {
            const float tol = std::max(0.02f, 0.05f * r.r0), rest = length(point(r, step * float(j & 15u)) - r.c) - tol;
            const uint32_t want = need <= tol || rest <= 0.f
                ? 1u
                : std::min(1u + uint32_t(std::ceil(14.f * std::sqrt(std::min((need - tol) / rest, 1.f)) - 1e-4f)), 15u);
            const uint32_t sh = 4u * (j & 15u);
            const uint32_t level = std::max(uint32_t(r.back >> sh) & 15u, want);
            r.back = (r.back & ~(uint64_t(15u) << sh)) | (uint64_t(level) << sh);
            r.cover |= 1u << (j & 15u);
        };
        for (const Axis& ax : T.axes)
        {
            if (!ax.fixed || ax.n_rings < 2)
                continue;
            const uint32_t first = ax.first_ring, last = ax.first_ring + ax.n_rings - 1;
            for (uint32_t k = first; k <= last; ++k)
            {
                Ring& r = T.rings[k];
                const Ring& prev = T.rings[k > first ? k - 1 : k];
                const Ring& next = T.rings[k < last ? k + 1 : k];
                const float tol = std::max(0.02f, 0.05f * r.r0);
                const bool carried = r.fit_err > tol, end = k == first || k == last;
                // (Half way to a neighbouring ring at a turn: the two points' middle seen from the centres' middle.)
                const auto halfway = [&](const Ring& nb, float th, float& out) {
                    return probe((r.c + nb.c) * 0.5f, (point(r, th) + point(nb, th)) * 0.5f, out);
                };
                r.cover = 0;
                r.back = 0;
                for (uint32_t j = 0; j < 16; ++j)
                {
                    const float th = step * float(j);
                    float out = 0.f, o2 = 0.f;
                    if (!probe(r.c, point(r, th), out))
                        continue;
                    // (An end ring through its cut end's cap - a gable's low face crossing it - past the patch's 4 mm out
                    // along the face's normal: the tube's end poked a bark scrap through the cut's face.)
                    const bool stands = out > tol || (end && hit_cap && out * hit_cos > 0.004f);
                    if (end ? !stands
                            : !carried && !stands &&
                                  !(probe(r.c, point(r, th - 0.5f * step), o2) && probe(r.c, point(r, th + 0.5f * step), o2) &&
                                      halfway(prev, th, o2) && halfway(next, th, o2)))
                        continue;
                    float need = out + 0.01f;
                    // (The quads round the point: their middles come in with it.)
                    for (const Ring* nb : {&prev, &next})
                        for (const float dj : {-0.5f, 0.5f})
                            if (nb != &r && halfway(*nb, th + dj * step, o2))
                                need = std::max(need, o2 + 0.01f);
                    deepen(r, j, need);
                }
                if ((r.cover & 0xFFFFu) == 0xFFFFu)
                    r.cover |= kRingHidden;
            }
        }
        // The stock tubes' drawn stretches in a grid of half a metre by their boxes: a point inside another axis's (its
        // mean radius, 95 %) lies hidden in that wood.
        struct Seg
        {
            V3 a, b;
            float ra, rb;
            uint32_t axis;
        };
        std::vector<Seg> segs;
        std::unordered_map<uint64_t, std::vector<uint32_t>> seg_grid;
        for (uint32_t ai = 0; ai < T.axes.size(); ++ai)
        {
            const Axis& ax = T.axes[ai];
            if (!ax.fixed || ax.n_rings < 2)
                continue;
            for (uint32_t k = ax.first_ring; k + 1 < ax.first_ring + ax.n_rings; ++k)
            {
                const Ring& a = T.rings[k];
                const Ring& b = T.rings[k + 1];
                if (a.cover & b.cover & kRingHidden)
                    continue;
                const uint32_t id = uint32_t(segs.size());
                segs.push_back({a.c, b.c, a.r0, b.r0, ai});
                const float m = std::max(a.r0, b.r0);
                const V3 lo = v3(std::min(a.c.x, b.c.x), std::min(a.c.y, b.c.y), std::min(a.c.z, b.c.z)) - v3(m, m, m);
                const V3 hi = v3(std::max(a.c.x, b.c.x), std::max(a.c.y, b.c.y), std::max(a.c.z, b.c.z)) + v3(m, m, m);
                for (float x = std::floor(lo.x / kCell); x <= std::floor(hi.x / kCell); x += 1.f)
                    for (float y = std::floor(lo.y / kCell); y <= std::floor(hi.y / kCell); y += 1.f)
                        for (float z = std::floor(lo.z / kCell); z <= std::floor(hi.z / kCell); z += 1.f)
                            seg_grid[key(v3(x + 0.5f, y + 0.5f, z + 0.5f) * kCell)].push_back(id);
            }
        }
        const auto in_other_wood = [&](uint32_t axis, V3 p) {
            const auto it = seg_grid.find(key(p));
            if (it == seg_grid.end())
                return false;
            for (const uint32_t id : it->second)
            {
                const Seg& g = segs[id];
                if (g.axis == axis)
                    continue;
                const V3 e = g.b - g.a;
                const float u = std::min(std::max(dot(p - g.a, e) / std::max(dot(e, e), 1e-12f), 0.f), 1.f);
                if (length(p - (g.a + e * u)) < 0.95f * (g.ra + (g.rb - g.ra) * u))
                    return true;
            }
            return false;
        };
        // The patch points the tube as drawn buries: the 16-gon's chord toward each, straight between its stretch's rings.
        const auto bury = [&](uint32_t axis, float s, V3 p) {
            if (axis >= T.axes.size() || !T.axes[axis].fixed || T.axes[axis].n_rings < 2)
                return;
            const Axis& ax = T.axes[axis];
            uint32_t k = ax.first_ring;
            while (k + 2 < ax.first_ring + ax.n_rings && T.rings[k + 1].s <= s)
                ++k;
            Ring& a = T.rings[k];
            Ring& b = T.rings[k + 1];
            if (a.cover & b.cover & kRingHidden)
                return;
            const float u = std::min(std::max((s - a.s) / std::max(b.s - a.s, 1e-6f), 0.f), 1.f);
            const V3 c = a.c + (b.c - a.c) * u, tt = normalize(a.t + (b.t - a.t) * u);
            V3 nn = a.n + (b.n - a.n) * u;
            nn = normalize(nn - tt * dot(nn, tt));
            V3 d = p - c;
            d = d - tt * dot(d, tt);
            float th = std::atan2(dot(d, cross(tt, nn)), dot(d, nn));
            if (th < 0.f)
                th += 2.f * kPi;
            const uint32_t i0 = std::min(uint32_t(th / step), 15u), i1 = (i0 + 1u) & 15u;
            const float a0 = step * float(i0);
            const auto drawn = [&](const Ring& r, uint32_t j) {
                const float rad = length(point(r, step * float(j)) - r.c);
                return rad - ring_step_back(r, j, rad);
            };
            const auto chord = [&](const Ring& r) {
                const float r0 = drawn(r, i0), r1 = drawn(r, i1);
                const float x0 = r0 * std::cos(a0), y0 = r0 * std::sin(a0);
                const float ex = r1 * std::cos(a0 + step) - x0, ey = r1 * std::sin(a0 + step) - y0;
                const float den = std::cos(th) * ey - std::sin(th) * ex;
                return std::fabs(den) > 1e-9f ? (x0 * ey - y0 * ex) / den : 0.5f * (r0 + r1);
            };
            const float gap = length(d) - (chord(a) + (chord(b) - chord(a)) * u);
            if (gap >= -std::max(0.02f, 0.05f * (a.r0 + (b.r0 - a.r0) * u)))
                return;
            // (And the sixteenths beside the pair, where they stay out of sight as well: a tube of sixteen sides or fewer
            // steps a vertex back by the least level of the three it spans, and a pair alone stepped none.)
            for (Ring* r : {&a, &b})
                for (const uint32_t j : {(i0 + 15u) & 15u, i0, i1, (i1 + 1u) & 15u})
                {
                    const V3 v = point(*r, step * float(j));
                    float o = 0.f;
                    if (!((r->cover >> j) & 1u) && !probe(r->c, v, o) && !in_other_wood(axis, v))
                        continue;
                    const float rad = length(v - r->c);
                    deepen(*r, j, ring_step_back(*r, j, rad) - gap + 0.01f);
                }
        };
        std::vector<uint8_t> bark(T.patch_verts.size(), 0);
        for (size_t i = 0; i < size_t(T.patch_bark_tris) * 3 && i < T.patch_tris.size(); ++i)
            bark[T.patch_tris[i]] = 1;
        for (size_t i = 0; i < T.patch_verts.size(); ++i)
            if (bark[i])
                bury(T.patch_verts[i].axis, T.patch_verts[i].s, T.patch_verts[i].p + T.patch_verts[i].n * 0.004f);
        for (size_t f = 0; f + 2 < T.patch_tris.size() && f / 3 < T.patch_bark_tris; f += 3)
        {
            const PatchVert& a = T.patch_verts[T.patch_tris[f]];
            const PatchVert& b = T.patch_verts[T.patch_tris[f + 1]];
            const PatchVert& c = T.patch_verts[T.patch_tris[f + 2]];
            if (a.axis == b.axis && b.axis == c.axis)
                bury(a.axis, (a.s + b.s + c.s) / 3.f, (a.p + b.p + c.p + (a.n + b.n + c.n) * 0.004f) * (1.f / 3.f));
        }
    }

    // How far the tessellation may lift a patch vertex's stock: 3 cm on climbable wood (its ring a quarter on the trunk
    // collision and more), min(5 % of its distance from the axis, 4 cm) elsewhere (a centimetre's half at the least).
    float patch_limit(const PatchVert& v) const
    {
        float c = 0.f;
        if (v.axis < T.axes.size())
        {
            const Axis& ax = T.axes[v.axis];
            c = ax.climbable;
            for (uint32_t k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
                if (T.rings[k].s <= v.s)
                    c = T.rings[k].climbable;
        }
        return c >= 0.25f ? 0.03f : std::min(std::max(0.05f * v.r, 0.005f), 0.04f);
    }

    // A patch vertex's skinning to its axis: its turn about the axis (from the reference normal there), its distance and
    // the ring's radius there.
    void patch_skin(PatchVert& p) const
    {
        V3 c, t, n;
        float r;
        if (!T.axes.empty() && p.axis < T.axes.size() && tree_frame(T.axes[p.axis], p.s, c, t, n, r))
        {
            V3 d = p.p - c;
            d = d - t * dot(d, t);
            p.r = length(d);
            p.theta = std::atan2(dot(d, cross(t, n)), dot(d, n));
            p.ring_r = r;
        }
    }

    // The stock's flat strips of bark: a branch the stock drew as a quad of two to four triangles, its painted twig gone
    // with the stock texture - a plank across the view (a dead snag's drooping branches). A component of the bark patches
    // that thin across its plane (under 8 % of its width) and four times longer than wide leaves the patches; a thin
    // branch grows along it instead (tree_ribbons_grow). (The components by their triangles and their places welded: a
    // stock mesh split at its uv seams - many a soup of single triangles - is one piece where its places meet; by the
    // triangles alone every long sliver of a trunk was a strip, cut out with a dead stick grown along it.)
    void tree_ribbons_find()
    {
        const uint32_t nb = std::min(T.patch_bark_tris, uint32_t(T.patch_tris.size() / 3));
        if (!nb)
            return;
        std::vector<uint32_t> up(T.patch_verts.size());
        for (uint32_t i = 0; i < up.size(); ++i)
            up[i] = i;
        const auto find = [&up](uint32_t i) {
            while (up[i] != i)
                i = up[i] = up[up[i]];
            return i;
        };
        for (uint32_t t = 0; t < nb; ++t)
            for (int e = 1; e < 3; ++e)
                up[find(T.patch_tris[size_t(t) * 3 + e])] = find(T.patch_tris[size_t(t) * 3]);
        {
            std::unordered_map<uint64_t, uint32_t> at_place;
            for (uint32_t t = 0; t < nb; ++t)
                for (int e = 0; e < 3; ++e)
                {
                    const uint32_t v = T.patch_tris[size_t(t) * 3 + e];
                    if (const auto [it, fresh] = at_place.try_emplace(place_key(T.patch_verts[v].p), v); !fresh)
                        up[find(v)] = find(it->second);
                }
        }
        std::unordered_map<uint32_t, std::vector<uint32_t>> comps;
        for (uint32_t t = 0; t < nb; ++t)
            comps[find(T.patch_tris[size_t(t) * 3])].push_back(t);
        std::vector<uint8_t> drop(nb, 0);
        for (const auto& [root, tris] : comps)
        {
            if (tris.size() > 4)
                continue;
            std::vector<uint32_t> vs;
            for (const uint32_t t : tris)
                for (int e = 0; e < 3; ++e)
                    vs.push_back(T.patch_tris[size_t(t) * 3 + e]);
            std::sort(vs.begin(), vs.end());
            vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
            V3 c{};
            for (const uint32_t v : vs)
                c = c + T.patch_verts[v].p;
            c = c * (1.f / float(vs.size()));
            // (Its long direction: the farthest pair of its points; across it in its plane, the farthest from that line.)
            float best = 0.f;
            V3 a = c, b = c;
            for (const uint32_t i : vs)
                for (const uint32_t j : vs)
                    if (const float d = length(T.patch_verts[i].p - T.patch_verts[j].p); d > best)
                    {
                        best = d;
                        a = T.patch_verts[i].p;
                        b = T.patch_verts[j].p;
                    }
            if (best < 0.05f)
                continue;
            const V3 dir = normalize(b - a);
            float width = 0.f, thick = 0.f;
            V3 nrm{};
            for (const uint32_t t : tris)
            {
                const V3 p0 = T.patch_verts[T.patch_tris[size_t(t) * 3]].p, p1 = T.patch_verts[T.patch_tris[size_t(t) * 3 + 1]].p,
                         p2 = T.patch_verts[T.patch_tris[size_t(t) * 3 + 2]].p;
                nrm = nrm + cross(p1 - p0, p2 - p0);
            }
            nrm = normalize(nrm - dir * dot(nrm, dir), perpendicular(dir));
            const V3 side = cross(dir, nrm);
            float lo_s = 1e9f, hi_s = -1e9f;
            for (const uint32_t v : vs)
            {
                const V3 q = T.patch_verts[v].p - a;
                lo_s = std::min(lo_s, dot(q, side));
                hi_s = std::max(hi_s, dot(q, side));
                thick = std::max(thick, std::abs(dot(q, nrm)));
            }
            width = hi_s - lo_s;
            if (width <= 1e-4f || thick > 0.08f * width || best < 4.f * width)
                continue;
            // (Its base the end nearer the stock wood's axes.)
            const auto near_wood = [this](V3 p) {
                float d = 1e30f;
                for (const Segment& g : T.segments)
                {
                    const V3 ab = g.b - g.a;
                    const float u = clampf(dot(p - g.a, ab) / std::max(dot(ab, ab), 1e-8f), 0.f, 1.f);
                    d = std::min(d, length(p - (g.a + ab * u)) - lerpf(g.ra, g.rb, u));
                }
                return d;
            };
            const bool flip = near_wood(b) < near_wood(a);
            TreeState::Ribbon R;
            R.base = flip ? b : a;
            R.dir = flip ? dir * -1.f : dir;
            R.len = best;
            R.width = width;
            tree->ribbons.push_back(R);
            for (const uint32_t t : tris)
                drop[t] = 1;
        }
        if (tree->ribbons.empty())
            return;
        std::vector<uint32_t> kept;
        kept.reserve(T.patch_tris.size());
        uint32_t kept_bark = 0;
        for (uint32_t t = 0; t < T.patch_tris.size() / 3; ++t)
            if (t >= nb || !drop[t])
            {
                kept.insert(kept.end(), T.patch_tris.begin() + ptrdiff_t(t) * 3, T.patch_tris.begin() + ptrdiff_t(t) * 3 + 3);
                kept_bark += t < nb ? 1u : 0u;
            }
        T.patch_tris = std::move(kept);
        T.patch_bark_tris = kept_bark;
    }

    // The branches along the stock's flat strips (tree_ribbons_find): from the strip's base along it, a quarter of its
    // width thick at the base (4 mm .. 3 cm) tapering to two fifths, dead and leafless (old wood, nothing grows on it),
    // hanging from the nearest stock axis (its sway); after the stock wood's grid, so it keeps clear of the stock wood.
    void tree_ribbons_grow()
    {
        for (const TreeState::Ribbon& R : tree->ribbons)
        {
            int32_t parent = -1;
            float attach = 0.f, best = 1e30f, pr = 0.01f;
            for (const Segment& g : T.segments)
            {
                if (!T.axes[g.axis].fixed)
                    continue;
                const V3 ab = g.b - g.a;
                const float u = clampf(dot(R.base - g.a, ab) / std::max(dot(ab, ab), 1e-8f), 0.f, 1.f);
                if (const float d = length(R.base - (g.a + ab * u)); d < best)
                {
                    best = d;
                    parent = int32_t(g.axis);
                    attach = g.s0 + u * length(ab);
                    pr = lerpf(g.ra, g.rb, u);
                }
            }
            if (parent < 0)
                continue;
            const float r0 = clampf(0.25f * R.width, 0.004f, 0.03f);
            tree->parent = parent;
            tree->attach = attach;
            tree->parent_r = pr;
            const uint32_t ai = grow(2, R.base, R.dir, R.len, r0, parent, attach, 0.f, 0.f, 0.02f, 0.1f, r0 * 0.4f);
            T.axes[ai].dead = T.axes[ai].leafless = true;
        }
        tree->parent = -1;
    }

    // The stock's coarse facets rounded: Phong tessellation of the patches from the stock normals (Boubekeur and Alexa
    // 2008). Every triangle is split into 4^L by halving its edges (L 2..4 by the bark's edge length - its 90th
    // percentile over 15 cm -, the template's triangles 64k at the most; one L for all, so a shared edge splits alike on
    // both sides) and every new point lifted toward its places' tangent planes by alpha: a bark triangle's own keeps
    // the bulge of every point it makes within its strictest vertex's limit (3 cm on climbable wood, min(5 % r, 4 cm)
    // elsewhere), a place takes the least of its triangles' (a cut end's cap's none: its rim stays on the cap); the caps
    // stay as they are - a cut end is flat. The places welded (a millimetre): a stock soup's copies of one place stand
    // as one, lift along their normals' mean (a crease's two sides meet on its bisector) - an edge two triangles share
    // by place alone splits alike on both, no crack opens along a seam of the stock's uv or a crease. The stock uvs and
    // normals interpolated, the new vertices skinned to their axes again; the stock triangles stay as they are for the
    // far levels of detail (patch_tris_far).
    void tree_patch_tessellate()
    {
        const uint32_t n0 = uint32_t(T.patch_verts.size()), nt = uint32_t(T.patch_tris.size() / 3);
        const uint32_t nb = std::min(T.patch_bark_tris, nt);
        T.patch_tris_far = T.patch_tris;
        T.patch_bark_tris_far = T.patch_bark_tris;
        if (!nb)
            return;
        std::vector<uint32_t> pw(n0, 0u), first;
        std::vector<V3> gn;
        {
            std::vector<uint8_t> bark(n0, 0);
            for (size_t i = 0; i < size_t(nb) * 3; ++i)
                bark[T.patch_tris[i]] = 1;
            std::unordered_map<uint64_t, uint32_t> at_place;
            for (uint32_t v = 0; v < n0; ++v)
            {
                const auto [it, fresh] = at_place.try_emplace(place_key(T.patch_verts[v].p), uint32_t(first.size()));
                if (fresh)
                {
                    first.push_back(v);
                    gn.push_back(v3(0.f, 0.f, 0.f));
                }
                pw[v] = it->second;
                T.patch_verts[v].p = T.patch_verts[first[it->second]].p;
                if (bark[v])
                    gn[it->second] = gn[it->second] + T.patch_verts[v].n;
            }
            for (uint32_t w = 0; w < gn.size(); ++w)
                gn[w] = normalize(gn[w], T.patch_verts[first[w]].n);
        }
        const auto phong = [&](const uint32_t* id, const float* b, float alpha) {
            const uint32_t w[3] = {pw[id[0]], pw[id[1]], pw[id[2]]};
            const V3 p0 = T.patch_verts[id[0]].p, p1 = T.patch_verts[id[1]].p, p2 = T.patch_verts[id[2]].p;
            const V3 p = p0 * b[0] + p1 * b[1] + p2 * b[2];
            const V3 q = (p - gn[w[0]] * dot(p - p0, gn[w[0]])) * b[0] + (p - gn[w[1]] * dot(p - p1, gn[w[1]])) * b[1] +
                (p - gn[w[2]] * dot(p - p2, gn[w[2]])) * b[2];
            return p + (q - p) * alpha;
        };
        std::vector<float> edges;
        edges.reserve(size_t(T.patch_bark_tris) * 3);
        for (uint32_t t = 0; t < T.patch_bark_tris && t < nt; ++t)
            for (int e = 0; e < 3; ++e)
                edges.push_back(length(T.patch_verts[T.patch_tris[size_t(t) * 3 + (e + 1) % 3]].p - T.patch_verts[T.patch_tris[size_t(t) * 3 + e]].p));
        if (edges.empty())
            return;
        std::nth_element(edges.begin(), edges.begin() + ptrdiff_t(edges.size() * 9 / 10), edges.end());
        const float p90 = edges[edges.size() * 9 / 10];
        int L = std::clamp(int(std::ceil(std::log2(std::max(p90 / 0.15f, 1.f)))), 2, 4);
        while (L > 2 && uint64_t(nb) << (2 * L) > 65536u)
            --L;
        const uint32_t N = 1u << L;
        std::vector<float> va(first.size(), 1.f);
        for (uint32_t t = 0; t < nt; ++t)
        {
            const uint32_t* id = &T.patch_tris[size_t(t) * 3];
            float alpha = 0.f;
            if (t < T.patch_bark_tris)
            {
                // (The bulge at alpha 1 on every point the split makes.)
                float d1 = 0.f;
                for (uint32_t i = 0; i <= N; ++i)
                    for (uint32_t j = 0; i + j <= N; ++j)
                    {
                        const float b[3] = {float(N - i - j) / float(N), float(i) / float(N), float(j) / float(N)};
                        d1 = std::max(d1, length(phong(id, b, 1.f) - phong(id, b, 0.f)));
                    }
                const float limit = std::min({patch_limit(T.patch_verts[id[0]]), patch_limit(T.patch_verts[id[1]]),
                    patch_limit(T.patch_verts[id[2]])});
                alpha = std::min(0.75f, limit / std::max(d1, 1e-6f));
            }
            for (int i = 0; i < 3; ++i)
                va[pw[id[i]]] = std::min(va[pw[id[i]]], alpha);
        }
        // A point of a triangle: on an edge from that edge's two vertices alone, in the order of their indices (both
        // triangles of a shared edge build the same vertex); inside from all three.
        std::unordered_map<uint64_t, uint32_t> on_edge;
        std::vector<PatchVert> verts = T.patch_verts;
        const auto make = [&](const uint32_t* id, const float* b) {
            PatchVert v;
            float alpha = 0.f;
            V3 nrm = v3(0.f, 0.f, 0.f);
            int major = 0;
            for (int i = 0; i < 3; ++i)
            {
                const PatchVert& A = T.patch_verts[id[i]];
                alpha += b[i] * va[pw[id[i]]];
                nrm = nrm + A.n * b[i];
                v.uv[0] += A.uv[0] * b[i];
                v.uv[1] += A.uv[1] * b[i];
                if (b[i] > b[major])
                    major = i;
            }
            v.p = phong(id, b, alpha);
            v.n = normalize(nrm, T.patch_verts[id[major]].n);
            // (Its facet: the far set's surface under it, where the tessellation eases out.)
            v.facet[0] = id[0];
            v.facet[1] = id[1];
            v.facet[2] = id[2];
            v.facet_w[0] = b[0];
            v.facet_w[1] = b[1];
            // (Its axis the nearest vertex's, its arclength over the vertices on that axis.)
            v.axis = T.patch_verts[id[major]].axis;
            float s = 0.f, w = 0.f;
            for (int i = 0; i < 3; ++i)
                if (T.patch_verts[id[i]].axis == v.axis)
                {
                    s += T.patch_verts[id[i]].s * b[i];
                    w += b[i];
                }
            v.s = s / std::max(w, 1e-6f);
            patch_skin(v);
            verts.push_back(v);
            return uint32_t(verts.size() - 1);
        };
        const auto at = [&](const uint32_t* id, uint32_t i, uint32_t j) -> uint32_t {
            const uint32_t k0 = N - i - j;
            // (The corners.)
            if (i == 0 && j == 0)
                return id[0];
            if (i == N)
                return id[1];
            if (j == N)
                return id[2];
            // (The edges: a-b where j = 0, a-c where i = 0, b-c where k0 = 0.)
            if (j == 0 || i == 0 || k0 == 0)
            {
                uint32_t u, w, step;
                if (j == 0)
                    u = id[0], w = id[1], step = i;
                else if (i == 0)
                    u = id[0], w = id[2], step = j;
                else
                    u = id[1], w = id[2], step = j;
                if (u > w)
                {
                    std::swap(u, w);
                    step = N - step;
                }
                const uint64_t key = ((uint64_t(u) * n0 + w) << 5) | step;
                if (const auto it = on_edge.find(key); it != on_edge.end())
                    return it->second;
                const uint32_t e[3] = {u, w, w};
                const float f = float(step) / float(N);
                const float b[3] = {1.f - f, f, 0.f};
                const uint32_t v = make(e, b);
                on_edge.emplace(key, v);
                return v;
            }
            const float b[3] = {float(k0) / float(N), float(i) / float(N), float(j) / float(N)};
            return make(id, b);
        };
        std::vector<uint32_t> tris;
        tris.reserve(size_t(nb) * N * N * 3 + size_t(nt - nb) * 3);
        std::vector<uint32_t> grid;
        for (uint32_t t = 0; t < nb; ++t)
        {
            const uint32_t* id = &T.patch_tris[size_t(t) * 3];
            // (The triangle's points, row i along a-b, column j along a-c.)
            grid.assign(size_t(N + 1) * (N + 1), 0u);
            for (uint32_t i = 0; i <= N; ++i)
                for (uint32_t j = 0; i + j <= N; ++j)
                    grid[i * (N + 1) + j] = at(id, i, j);
            for (uint32_t i = 0; i < N; ++i)
                for (uint32_t j = 0; i + j < N; ++j)
                {
                    tris.insert(tris.end(), {grid[i * (N + 1) + j], grid[(i + 1) * (N + 1) + j], grid[i * (N + 1) + j + 1]});
                    if (i + j + 1 < N)
                        tris.insert(tris.end(), {grid[(i + 1) * (N + 1) + j], grid[(i + 1) * (N + 1) + j + 1], grid[i * (N + 1) + j + 1]});
                }
        }
        tris.insert(tris.end(), T.patch_tris.begin() + ptrdiff_t(nb) * 3, T.patch_tris.begin() + ptrdiff_t(nt) * 3);
        T.patch_verts = std::move(verts);
        T.patch_tris = std::move(tris);
        T.patch_bark_tris = nb * N * N;
    }

    // A place's foot on a stock stretch (rings k and k + 1 of a fixed axis), between the two rings' planes - the tube's
    // own cross-sections: its turn and arclength as the tube lays its bark, one foot from both sides of a joint (by the
    // chord across it, a bend's outer wedge took the joint's arclength whole: the bark smeared over it) -, the share
    // along by the place's distances to the two planes; past an axis's end ring no further than a tenth of its radius and
    // a centimetre; none where the planes cross before the place (a kinked stock ring) or it stands off the frame's
    // plane by more than a quarter of its way out. Its ray out, its turn about the rings' reference normal, the profile
    // there straight between the two rings and its normal, the gap past it; anywhere: no end nor frame bounds (a copy's
    // other axis).
    struct Foot
    {
        V3 c, ray, nrm;
        float rad{}, gap{}, s{}, theta{}, r_mean{};
        uint32_t axis{~0u};
    };
    bool stretch_foot(uint32_t k, V3 p, Foot& F, bool anywhere = false) const
    {
        const Ring& A = T.rings[k];
        const Ring& B = T.rings[k + 1];
        const Axis& ax = T.axes[A.axis];
        const float da = dot(p - A.c, A.t), db = dot(p - B.c, B.t);
        const float end = 0.1f * std::max(A.r0, B.r0) + 0.01f;
        const float lo = k == ax.first_ring ? -end : -0.001f, hi = k + 2 == ax.first_ring + ax.n_rings ? end : 0.001f;
        if (!anywhere && (da < lo || db > hi))
            return false;
        if (da - db < 1e-6f)
            return false;
        const float u = clampf(da / (da - db), 0.f, 1.f);
        const V3 c = A.c + (B.c - A.c) * u, t = normalize(A.t + (B.t - A.t) * u, A.t);
        V3 n = A.n + (B.n - A.n) * u;
        n = normalize(n - t * dot(n, t), perpendicular(t));
        V3 d = p - c;
        const float off = dot(d, t);
        d = d - t * off;
        const float rho = length(d);
        if (rho < 1e-5f || (!anywhere && std::fabs(off) > 0.25f * rho + 0.01f))
            return false;
        const V3 bn = cross(t, n);
        const float th = std::atan2(dot(d, bn), dot(d, n));
        // (The profile and its derivative by the turn.)
        float r = 0.f, dr = 0.f;
        for (const Ring* R : {&A, &B})
        {
            float k1 = 1.f, dk = 0.f;
            for (int h = 1; h <= 3; ++h)
            {
                const float ch = std::cos(float(h) * th), sh = std::sin(float(h) * th);
                k1 += R->f[2 * h - 2] * ch + R->f[2 * h - 1] * sh;
                dk += float(h) * (R->f[2 * h - 1] * ch - R->f[2 * h - 2] * sh);
            }
            const float wgt = R == &A ? 1.f - u : u;
            r += wgt * R->r0 * k1;
            dr += wgt * R->r0 * dk;
        }
        const float cs = std::cos(th), sn = std::sin(th);
        F.c = c;
        F.ray = d * (1.f / rho);
        F.nrm = normalize(n * (r * cs + dr * sn) + bn * (r * sn - dr * cs), F.ray);
        F.rad = r;
        F.gap = rho - r;
        F.s = lerpf(A.s, B.s, u);
        F.theta = th;
        F.r_mean = lerpf(A.r0, B.r0, u);
        F.axis = A.axis;
        return true;
    }
    // The stock stretches in a grid of half a metre by their boxes, 1.3 radii and a tenth round them: a place's cell
    // holds every stretch whose foot can be its.
    using StretchGrid = std::unordered_map<uint64_t, std::vector<uint32_t>>;
    static uint64_t stretch_cell(V3 p)
    {
        const auto c = [](float x) { return uint64_t(uint32_t(int(std::floor(x * 2.f)) + 1048576)); };
        return (c(p.x) << 42) | (c(p.y) << 21) | c(p.z);
    }
    StretchGrid stretch_grid() const
    {
        StretchGrid grid;
        for (const Axis& ax : T.axes)
        {
            if (!ax.fixed || ax.n_rings < 2)
                continue;
            for (uint32_t k = ax.first_ring; k + 1 < ax.first_ring + ax.n_rings; ++k)
            {
                const Ring& a = T.rings[k];
                const Ring& b = T.rings[k + 1];
                const float m = 1.3f * std::max(a.r0, b.r0) + 0.1f;
                const V3 lo = v3(std::min(a.c.x, b.c.x), std::min(a.c.y, b.c.y), std::min(a.c.z, b.c.z)) - v3(m, m, m);
                const V3 hi = v3(std::max(a.c.x, b.c.x), std::max(a.c.y, b.c.y), std::max(a.c.z, b.c.z)) + v3(m, m, m);
                for (float x = std::floor(lo.x * 2.f); x <= std::floor(hi.x * 2.f); x += 1.f)
                    for (float y = std::floor(lo.y * 2.f); y <= std::floor(hi.y * 2.f); y += 1.f)
                        for (float z = std::floor(lo.z * 2.f); z <= std::floor(hi.z * 2.f); z += 1.f)
                            grid[stretch_cell(v3(x + 0.5f, y + 0.5f, z + 0.5f) * 0.5f)].push_back(k);
            }
        }
        return grid;
    }
    // A place's feet on the axes round it (the nearest of each axis's stretches by the gap), nearest first.
    void stretch_feet(const StretchGrid& grid, V3 p, std::vector<Foot>& out) const
    {
        out.clear();
        const auto it = grid.find(stretch_cell(p));
        if (it == grid.end())
            return;
        for (const uint32_t k : it->second)
        {
            Foot F;
            if (!stretch_foot(k, p, F))
                continue;
            if (const auto at = std::find_if(out.begin(), out.end(), [&F](const Foot& G) { return G.axis == F.axis; }); at == out.end())
                out.push_back(F);
            else if (std::fabs(F.gap) < std::fabs(at->gap))
                *at = F;
        }
        std::sort(out.begin(), out.end(), [](const Foot& a, const Foot& b) { return std::fabs(a.gap) < std::fabs(b.gap); });
    }

    // The patches' borders laid onto the tubes. A patch keeps whole each stock triangle the refit tube misses anywhere by
    // more than the tolerance (FORMAT.md): where its border meets the tube the stock stands off it by up to that - a
    // step, a ledge under a fork's limb -, and a long sliver of a trunk's mesh stands out along it - a fin. Each place of
    // the bark patches (welded, a millimetre) goes along its ray from the nearest tube's axis toward 4 mm under that
    // tube's profile by 1 - m, keeping its stock's by m: 0 on an open border of the patches over a tube (the tube runs
    // on past it), easing to 1 a blend's length in along the edges (60 % of the ring's radius, 4 .. 25 cm), and 0 where
    // the stock keeps within half the tolerance of the tube, easing to 1 at one and a half (the stock's own shape stays
    // where it parts from the tube); a border's pull fades on places off the tube by more than a border's reach (stock
    // wood inside another limb, a crotch's far side), the weights eased over the edges. The nearest tube: the feet on
    // the stretches round the place, the least gap; a place about as near another axis's (within the tolerance) is a
    // crotch's - its stock stays but by a border. A place a cap shares (a cut end's rim) stays. One laid on a tube
    // takes its skinning, its normals leaning to the profile's.
    void tree_patch_feather()
    {
        const uint32_t nt = uint32_t(T.patch_tris.size() / 3), nb = std::min(T.patch_bark_tris, nt);
        if (!nb)
            return;
        std::vector<uint32_t> pw(T.patch_verts.size(), ~0u), first;
        std::vector<uint8_t> pinned;
        {
            std::unordered_map<uint64_t, uint32_t> at_place;
            for (size_t i = 0; i < size_t(nt) * 3; ++i)
                if (const uint32_t v = T.patch_tris[i]; pw[v] == ~0u)
                {
                    const auto [it, fresh] = at_place.try_emplace(place_key(T.patch_verts[v].p), uint32_t(first.size()));
                    if (fresh)
                    {
                        first.push_back(v);
                        pinned.push_back(0);
                    }
                    pw[v] = it->second;
                }
            for (size_t i = size_t(nb) * 3; i < size_t(nt) * 3; ++i)
                pinned[pw[T.patch_tris[i]]] = 1;
        }
        const uint32_t np = uint32_t(first.size());
        // (The bark's edges between places: an open border where one triangle alone has it.)
        std::unordered_map<uint64_t, uint32_t> uses;
        std::vector<std::vector<std::pair<uint32_t, float>>> adj(np);
        for (size_t i = 0; i < size_t(nb) * 3; i += 3)
            for (size_t e = 0; e < 3; ++e)
            {
                uint32_t a = pw[T.patch_tris[i + e]], b = pw[T.patch_tris[i + (e + 1) % 3]];
                if (a == b)
                    continue;
                if (a > b)
                    std::swap(a, b);
                if (uses[(uint64_t(a) << 32) | b]++ == 0)
                {
                    const float l = length(T.patch_verts[first[a]].p - T.patch_verts[first[b]].p);
                    adj[a].emplace_back(b, l);
                    adj[b].emplace_back(a, l);
                }
            }
        std::vector<uint8_t> open(np, 0);
        for (const auto& [k, n] : uses)
            if (n == 1)
                open[uint32_t(k >> 32)] = open[uint32_t(k & 0xFFFFFFFFu)] = 1;
        const StretchGrid grid = stretch_grid();
        // (Each place's tube, its tolerance and blend's length there; a crotch's.)
        struct Lay
        {
            Foot F;
            float tol{}, len{};
            bool crotch{};
        };
        std::vector<Lay> lay(np);
        std::vector<Foot> feet;
        for (uint32_t w = 0; w < np; ++w)
        {
            if (pinned[w])
                continue;
            stretch_feet(grid, T.patch_verts[first[w]].p, feet);
            if (feet.empty())
                continue;
            Lay& L = lay[w];
            L.F = feet[0];
            L.tol = std::max(0.02f, 0.05f * L.F.r_mean);
            L.len = clampf(0.6f * L.F.r_mean, 0.04f, 0.25f);
            L.crotch = feet.size() > 1 && std::fabs(feet[1].gap) < std::fabs(L.F.gap) + L.tol;
        }
        // (Dijkstra from the open borders over a tube, along the edges, to the longest blend.)
        std::vector<float> dist(np, 1e30f);
        using QE = std::pair<float, uint32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
        // (A border over a tube: off it by twice the tolerance and a centimetre at the most.)
        const auto reach = [](const Lay& L) { return 2.f * L.tol + 0.01f; };
        for (uint32_t w = 0; w < np; ++w)
            if (open[w] && lay[w].F.axis != ~0u && std::fabs(lay[w].F.gap) <= reach(lay[w]))
            {
                dist[w] = 0.f;
                q.emplace(0.f, w);
            }
        while (!q.empty())
        {
            const auto [d, w] = q.top();
            q.pop();
            if (d > dist[w] || d > 0.25f)
                continue;
            for (const auto& [x, l] : adj[w])
                if (d + l < dist[x])
                {
                    dist[x] = d + l;
                    q.emplace(d + l, x);
                }
        }
        std::vector<float> keep(np, 1.f);
        for (uint32_t w = 0; w < np; ++w)
            if (const Lay& L = lay[w]; L.F.axis != ~0u)
            {
                const float g = std::fabs(L.F.gap), by_border = std::max(smooth(0.f, L.len, dist[w]), smooth(reach(L), 2.f * reach(L), g));
                keep[w] = std::min(by_border, L.crotch ? 1.f : smooth(0.5f * L.tol, 1.5f * L.tol, g));
            }
        // (Eased over the edges, three rounds of half its own and half its neighbours' mean - the borders held, a place
        // past a border's reach kept: the stock's facets part from the tube by gaps of their own, a weight from its
        // neighbour's by a facet's.)
        for (int round = 0; round < 3; ++round)
        {
            std::vector<float> next = keep;
            for (uint32_t w = 0; w < np; ++w)
                if (const Lay& L = lay[w]; L.F.axis != ~0u && dist[w] > 0.f && !adj[w].empty())
                {
                    float sum = 0.f;
                    for (const auto& [x, l] : adj[w])
                        sum += keep[x];
                    next[w] = std::max(0.5f * keep[w] + 0.5f * sum / float(adj[w].size()), smooth(reach(L), 2.f * reach(L), std::fabs(L.F.gap)));
                }
            keep.swap(next);
        }
        for (uint32_t v = 0; v < T.patch_verts.size(); ++v)
        {
            const uint32_t w = pw[v];
            if (w == ~0u || lay[w].F.axis == ~0u || keep[w] >= 1.f)
                continue;
            const Foot& F = lay[w].F;
            const float m = keep[w];
            PatchVert& P = T.patch_verts[v];
            const V3 to = F.c + F.ray * std::max(F.rad - 0.004f, 0.5f * F.rad);
            P.p = to + (P.p - to) * m;
            P.n = normalize(F.nrm + (P.n - F.nrm) * m, F.nrm);
            P.axis = F.axis;
            P.s = F.s;
            patch_skin(P);
        }
    }

    // The patches' bark by two tubes. The stock uv laid the bark at its own scale and turned its own way on each island -
    // a decal round a fork, a mitre where two islands met, streaks where it stretched -: each bark vertex takes the two
    // nearest stock axes' mappings instead (the turn round each and its arclength, as their tubes lay them) and the
    // second's weight - a half where the two tubes' surfaces stand as near, none a blend's width (a third of the thinner
    // radius, 2 .. 15 cm) nearer the first -, the pair in the axes' order (the lower first), so a triangle's corners on
    // one pair interpolate alike; a triangle whose corners keep different pairs gets copies of them on the two axes its
    // corners weigh most (an axis without a foot near a copy: its nearest stretch anywhere). A vertex with no stock
    // stretch round it keeps its skinning, alone. Then each mapping's fit on the vertex's triangles moves the weight to
    // the one that lays the bark there (a crotch's face looking along one limb takes the other's), and where the better
    // of the two still smears (a saddle looking along both limbs) the world's projection takes over (w3); both weights
    // eased over the patch (a vertex's fit is its worst triangle's: the second mapping came in blocks a triangle wide),
    // its open borders held. Last, on a pair of a parent and its child, the parent's mapping takes the place against the
    // parent's tube - all of it on the profile, none half the child's radius out (3 .. 12 cm) -, the projection none
    // there: a limb's patch runs into its trunk's tube along their seam, the trunk's patch round the limb's foot meets
    // the limb's tube (its junction record turns it to the trunk's bark), and a limb's own mapping met the trunk's bark
    // along a straight seam.
    void tree_patch_pairs()
    {
        const uint32_t nt = uint32_t(T.patch_tris.size() / 3), nb = std::min(T.patch_bark_tris, nt);
        if (!nb)
            return;
        const StretchGrid grid = stretch_grid();
        // (The second's weight given both feet, its own nearer or not.)
        const auto weight_of = [](const Foot& mine, const Foot& other) {
            const float k = clampf(std::min(mine.r_mean, other.r_mean) / 3.f, 0.02f, 0.15f), d = std::fabs(mine.gap) - std::fabs(other.gap);
            return d <= 0.f ? 1.f - 0.5f * (1.f - smooth(0.f, k, -d)) : 0.5f * (1.f - smooth(0.f, k, d));
        };
        const auto set = [this, &weight_of](PatchVert& P, const Foot& a, const Foot* b) {
            const Foot* f1 = b && b->axis < a.axis ? b : &a;
            const Foot* f2 = b && b->axis < a.axis ? &a : b;
            P.axis = f1->axis;
            P.s = f1->s;
            P.theta = f1->theta;
            P.r = f1->rad + f1->gap;
            P.ring_r = f1->r_mean;
            P.axis2 = f2 ? f2->axis : ~0u;
            P.s2 = f2 ? f2->s : 0.f;
            P.theta2 = f2 ? f2->theta : 0.f;
            P.ring_r2 = f2 ? f2->r_mean : 0.f;
            P.w2 = f2 ? weight_of(*f2, *f1) : 0.f;
            P.gap1 = f1->gap;
            P.gap2 = f2 ? f2->gap : 0.f;
        };
        std::vector<uint8_t> bark(T.patch_verts.size(), 0);
        for (size_t i = 0; i < size_t(nb) * 3; ++i)
            bark[T.patch_tris[i]] = 1;
        for (size_t i = 0; i < T.patch_tris_far.size() && i / 3 < T.patch_bark_tris_far; ++i)
            bark[T.patch_tris_far[i]] = 1;
        std::vector<Foot> feet;
        for (uint32_t v = 0; v < T.patch_verts.size(); ++v)
            if (bark[v])
            {
                stretch_feet(grid, T.patch_verts[v].p, feet);
                if (!feet.empty())
                    set(T.patch_verts[v], feet[0], feet.size() > 1 ? &feet[1] : nullptr);
            }
        // (An axis's foot for a copy: round the place, else its nearest stretch anywhere.)
        const auto foot_on = [&](uint32_t axis, V3 p, Foot& F) {
            if (axis >= T.axes.size() || !T.axes[axis].fixed || T.axes[axis].n_rings < 2)
                return false;
            stretch_feet(grid, p, feet);
            if (const auto at = std::find_if(feet.begin(), feet.end(), [axis](const Foot& G) { return G.axis == axis; }); at != feet.end())
            {
                F = *at;
                return true;
            }
            const Axis& ax = T.axes[axis];
            float best = 1e30f;
            for (uint32_t k = ax.first_ring; k + 1 < ax.first_ring + ax.n_rings; ++k)
                if (Foot G; stretch_foot(k, p, G, true) && length(p - G.c) < best)
                {
                    best = length(p - G.c);
                    F = G;
                }
            return best < 1e29f;
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> copies;
        const auto fix = [&](std::vector<uint32_t>& tris, uint32_t n_bark) {
            for (size_t t = 0; t + 2 < tris.size() && t / 3 < n_bark; t += 3)
            {
                uint32_t* id = &tris[t];
                const auto pair_of = [this](uint32_t v) { return std::pair{T.patch_verts[v].axis, T.patch_verts[v].axis2}; };
                if (pair_of(id[1]) == pair_of(id[0]) && pair_of(id[2]) == pair_of(id[0]))
                    continue;
                // (The axes by the weight the three corners give them.)
                std::vector<std::pair<uint32_t, float>> score;
                const auto add = [&score](uint32_t axis, float w) {
                    if (axis == ~0u)
                        return;
                    if (const auto at = std::find_if(score.begin(), score.end(), [axis](const auto& e) { return e.first == axis; }); at != score.end())
                        at->second += w;
                    else
                        score.emplace_back(axis, w);
                };
                for (int i = 0; i < 3; ++i)
                {
                    const PatchVert& P = T.patch_verts[id[i]];
                    add(P.axis, 1.f - P.w2 + 1e-3f);
                    add(P.axis2, P.w2);
                }
                std::sort(score.begin(), score.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                const uint32_t a1 = score[0].first, a2 = score.size() > 1 ? score[1].first : ~0u;
                const uint32_t lo = std::min(a1, a2), hi = std::max(a1, a2);
                for (int i = 0; i < 3; ++i)
                {
                    if (pair_of(id[i]) == std::pair{lo, hi})
                        continue;
                    const auto key = std::tuple{id[i], lo, hi};
                    if (const auto it = copies.find(key); it != copies.end())
                    {
                        id[i] = it->second;
                        continue;
                    }
                    PatchVert C = T.patch_verts[id[i]];
                    Foot F1, F2;
                    if (!foot_on(lo, C.p, F1))
                        continue;
                    set(C, F1, hi != ~0u && foot_on(hi, C.p, F2) ? &F2 : nullptr);
                    const uint32_t c = uint32_t(T.patch_verts.size());
                    T.patch_verts.push_back(C);
                    copies.emplace(key, c);
                    id[i] = c;
                }
            }
        };
        fix(T.patch_tris, nb);
        fix(T.patch_tris_far, T.patch_bark_tris_far);
        // (Each mapping's fit at a vertex: the least over its triangles of the singular values of the map from the
        // triangle to the turn's arc at the ring's mean radius and the arclength - under a quarter a texel smeared across
        // it, past four the bark squeezed; the second's weight leaning to the better of the two. By the near list's
        // triangles: a stock corner both lists share would take the far list's coarse triangles' poorer fit near by.)
        const auto fit = [](const PatchVert* V[3], bool second) {
            const float th0 = second ? V[0]->theta2 : V[0]->theta;
            float rm = 0.f, c[3][2];
            for (int i = 0; i < 3; ++i)
                rm += (second ? V[i]->ring_r2 : V[i]->ring_r) * (1.f / 3.f);
            for (int i = 0; i < 3; ++i)
            {
                float dth = (second ? V[i]->theta2 : V[i]->theta) - th0;
                dth -= 2.f * kPi * std::round(dth / (2.f * kPi));
                c[i][0] = dth * rm;
                c[i][1] = second ? V[i]->s2 : V[i]->s;
            }
            const V3 e1 = V[1]->p - V[0]->p, e2 = V[2]->p - V[0]->p;
            const float l1 = length(e1);
            if (l1 < 1e-6f)
                return 1.f;
            const V3 xa = e1 * (1.f / l1);
            const float a = dot(e2, xa), ly = length(e2 - xa * a);
            if (ly < 1e-6f)
                return 1.f;
            const float j00 = (c[1][0] - c[0][0]) / l1, j10 = (c[1][1] - c[0][1]) / l1;
            const float j01 = (c[2][0] - c[0][0] - (c[1][0] - c[0][0]) * a / l1) / ly, j11 = (c[2][1] - c[0][1] - (c[1][1] - c[0][1]) * a / l1) / ly;
            const float tr = j00 * j00 + j01 * j01 + j10 * j10 + j11 * j11, det = j00 * j11 - j01 * j10;
            const float disc = std::sqrt(std::max(tr * tr - 4.f * det * det, 0.f));
            const float s1 = std::sqrt(std::max(0.5f * (tr + disc), 0.f)), s2 = std::sqrt(std::max(0.5f * (tr - disc), 0.f));
            return smooth(0.15f, 0.5f, s2) * (1.f - smooth(2.5f, 5.f, s1));
        };
        std::vector<float> q1(T.patch_verts.size(), 1.f), q2(T.patch_verts.size(), 1.f);
        const auto rate = [&](const std::vector<uint32_t>& tris, uint32_t n_bark) {
            for (size_t t = 0; t + 2 < tris.size() && t / 3 < n_bark; t += 3)
            {
                const PatchVert* V[3] = {&T.patch_verts[tris[t]], &T.patch_verts[tris[t + 1]], &T.patch_verts[tris[t + 2]]};
                const float a = fit(V, false), b = V[0]->axis2 != ~0u ? fit(V, true) : 0.f;
                for (int i = 0; i < 3; ++i)
                {
                    q1[tris[t + i]] = std::min(q1[tris[t + i]], a);
                    q2[tris[t + i]] = std::min(q2[tris[t + i]], b);
                }
            }
        };
        rate(T.patch_tris, nb);
        for (uint32_t v = 0; v < T.patch_verts.size(); ++v)
        {
            PatchVert& P = T.patch_verts[v];
            if (P.axis2 != ~0u)
            {
                const float wb = P.w2 * q2[v], wa = (1.f - P.w2) * q1[v];
                if (wa + wb > 1e-4f)
                    P.w2 = wb / (wa + wb);
            }
            // (Up to a fit of 0.65: a crotch's face half smeared along a limb drew pale ribs.)
            P.w3 = 1.f - smooth(0.28f, 0.65f, std::max(q1[v], P.axis2 != ~0u ? q2[v] : 0.f));
        }
        // (Against a parent's tube its mapping: all of it on the profile, none 1.3 of the child's radius out (5 .. 35
        // cm; at half of it, 3 .. 12 cm, the grain turned along a straight line round a limb's foot) - as far as it lays
        // the bark there (its fit: on the limb's own face by a crotch the trunk's mapping smears into stripes, and forced
        // there it drew the seam along the crease); the projection as the fit put it - upright on the trunk's sides as
        // the trunk's grain.)
        for (uint32_t v = 0; v < T.patch_verts.size(); ++v)
        {
            PatchVert& P = T.patch_verts[v];
            if (P.axis2 == ~0u || P.axis >= T.axes.size() || P.axis2 >= T.axes.size())
                continue;
            const bool first_parent = T.axes[P.axis2].parent == int32_t(P.axis), second_parent = T.axes[P.axis].parent == int32_t(P.axis2);
            if (!first_parent && !second_parent)
                continue;
            const float child_r = first_parent ? P.ring_r2 : P.ring_r, k = clampf(1.3f * child_r, 0.05f, 0.35f);
            const float out = smooth(0.f, k, std::fabs(first_parent ? P.gap1 : P.gap2));
            const float forced = first_parent ? std::min(P.w2, out) : std::max(P.w2, 1.f - out);
            P.w2 = lerpf(P.w2, forced, smooth(0.35f, 0.7f, first_parent ? q1[v] : q2[v]));
        }
        // (The weights eased: three rounds of each place's half toward its neighbours' mean along the bark triangles'
        // edges of one pair, the vertices welded by their places and pairs - the stock splits every vertex at its uv
        // seams, and the weights stepped along them -; a place on an open edge - a border laid onto its tube, a pair's
        // own seam - held.)
        {
            const size_t nv = T.patch_verts.size();
            std::vector<uint32_t> grp(nv);
            std::map<std::tuple<uint64_t, uint32_t, uint32_t>, uint32_t> group_of;
            for (uint32_t v = 0; v < nv; ++v)
            {
                const PatchVert& P = T.patch_verts[v];
                grp[v] = group_of.try_emplace({place_key(P.p), P.axis, P.axis2}, uint32_t(group_of.size())).first->second;
            }
            const size_t ng = group_of.size();
            std::vector<std::vector<uint32_t>> adj(ng);
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> uses;
            for (size_t t = 0; t + 2 < T.patch_tris.size() && t / 3 < nb; t += 3)
                for (int i = 0; i < 3; ++i)
                {
                    const uint32_t a = grp[T.patch_tris[t + i]], b = grp[T.patch_tris[t + (i + 1) % 3]];
                    if (a == b)
                        continue;
                    ++uses[{std::min(a, b), std::max(a, b)}];
                    adj[a].push_back(b);
                    adj[b].push_back(a);
                }
            std::vector<uint8_t> held(ng, 0);
            for (const auto& [e, n] : uses)
                if (n == 1)
                    held[e.first] = held[e.second] = 1;
            for (std::vector<uint32_t>& l : adj)
            {
                std::sort(l.begin(), l.end());
                l.erase(std::unique(l.begin(), l.end()), l.end());
            }
            // (A group starts at its copies' mean: each kept its own worst triangle's fit.)
            std::vector<float> w2(ng, 0.f), w3(ng, 0.f), cnt(ng, 0.f), w2n(ng), w3n(ng);
            for (uint32_t v = 0; v < nv; ++v)
            {
                w2[grp[v]] += T.patch_verts[v].w2;
                w3[grp[v]] += T.patch_verts[v].w3;
                cnt[grp[v]] += 1.f;
            }
            for (size_t g = 0; g < ng; ++g)
            {
                w2[g] /= cnt[g];
                w3[g] /= cnt[g];
            }
            // (Where a place's copies keep different pairs - the triangles round it settled on different axes, each
            // showing its own first axis's bark: a straight seam in one patch - every copy takes the axis the pairs
            // share, else the world's projection, and one projection's weight, the most of its copies' - both sides of
            // the edge alike; held, the easing carries it in. Both lists' bark corners: a far triangle's copies too.)
            std::map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> pairs_at;
            std::map<uint64_t, float> w3_at;
            const auto corners = [&](const auto& fn) {
                for (size_t t = 0; t < size_t(nb) * 3 && t < T.patch_tris.size(); ++t)
                    fn(T.patch_tris[t]);
                for (size_t t = 0; t < size_t(T.patch_bark_tris_far) * 3 && t < T.patch_tris_far.size(); ++t)
                    fn(T.patch_tris_far[t]);
            };
            corners([&](uint32_t v) {
                const PatchVert& P = T.patch_verts[v];
                std::vector<std::pair<uint32_t, uint32_t>>& l = pairs_at[place_key(P.p)];
                if (const std::pair<uint32_t, uint32_t> pr{P.axis, P.axis2}; std::find(l.begin(), l.end(), pr) == l.end())
                    l.push_back(pr);
            });
            corners([&](uint32_t v) {
                const uint64_t key = place_key(T.patch_verts[v].p);
                if (pairs_at[key].size() >= 2)
                {
                    float& m = w3_at[key];
                    m = std::max(m, w3[grp[v]]);
                }
            });
            corners([&](uint32_t v) {
                const uint32_t g = grp[v];
                const PatchVert& P = T.patch_verts[v];
                const uint64_t key = place_key(P.p);
                const std::vector<std::pair<uint32_t, uint32_t>>& l = pairs_at[key];
                if (l.size() < 2 || held[g] > 1)
                    return;
                uint32_t common = ~0u;
                for (const uint32_t c : {P.axis, P.axis2})
                    if (c != ~0u && std::all_of(l.begin(), l.end(), [c](const auto& pr) { return pr.first == c || pr.second == c; }))
                    {
                        common = c;
                        break;
                    }
                if (common != ~0u)
                {
                    w2[g] = common == P.axis2 ? 1.f : 0.f;
                    w3[g] = w3_at[key];
                }
                else
                    w3[g] = 1.f;
                held[g] = 2;
            });
            for (int round = 0; round < 3; ++round)
            {
                for (size_t g = 0; g < ng; ++g)
                {
                    w2n[g] = w2[g];
                    w3n[g] = w3[g];
                    if (held[g] || adj[g].empty())
                        continue;
                    float s2 = 0.f, s3 = 0.f;
                    for (const uint32_t u : adj[g])
                    {
                        s2 += w2[u];
                        s3 += w3[u];
                    }
                    const float k = 0.5f / float(adj[g].size());
                    w2n[g] = 0.5f * w2[g] + s2 * k;
                    w3n[g] = 0.5f * w3[g] + s3 * k;
                }
                w2.swap(w2n);
                w3.swap(w3n);
            }
            // (Then the fits bound the easing: it carried the other mapping's weight half over a crotch's faces one
            // mapping smears into stripes - a pale zebra wedge where a limb leaves its trunk. A mapping keeps no more
            // weight than its fit there allows, the other the rest, eased once more within the bounds; a place both
            // smear is the projection's, a held one as it was.)
            std::vector<float> lo(ng, 0.f), hi(ng, 1.f);
            for (uint32_t v = 0; v < nv; ++v)
                if (T.patch_verts[v].axis2 != ~0u)
                {
                    lo[grp[v]] = std::max(lo[grp[v]], 1.f - smooth(0.05f, 0.45f, q1[v]));
                    hi[grp[v]] = std::min(hi[grp[v]], smooth(0.05f, 0.45f, q2[v]));
                }
            const auto bound = [&] {
                for (size_t g = 0; g < ng; ++g)
                    if (!held[g] && lo[g] <= hi[g])
                        w2[g] = clampf(w2[g], lo[g], hi[g]);
            };
            bound();
            for (size_t g = 0; g < ng; ++g)
            {
                w2n[g] = w2[g];
                if (held[g] || adj[g].empty())
                    continue;
                float s2 = 0.f;
                for (const uint32_t u : adj[g])
                    s2 += w2[u];
                w2n[g] = 0.5f * w2[g] + s2 * (0.5f / float(adj[g].size()));
            }
            w2.swap(w2n);
            bound();
            // (The projection's weight steepened after: eased out over its neighbours it covered their good bark with
            // its own, a flat smear round an oak's joins.)
            for (uint32_t v = 0; v < nv; ++v)
            {
                T.patch_verts[v].w2 = w2[grp[v]];
                T.patch_verts[v].w3 = smooth(0.3f, 0.8f, w3[grp[v]]);
            }
        }
    }

    // An open cut end (skaCut without a stock cap: the stock left the broken or sawn limb hollow) gets a flat cap across
    // its last ring, a fan from the centre to the ring's profile 1 / cos(pi / n) out (the tube's sides are its chords),
    // appended to the caps of both lists (its end grain, da_tree_patch_vs.h). A tip under 4 mm ends in a point.
    void tree_cut_caps()
    {
        for (uint32_t a = 0; a < T.axes.size(); ++a)
        {
            Axis& ax = T.axes[a];
            if (!ax.fixed || !ax.cut_end || ax.capped || ax.n_rings < 2)
                continue;
            const Ring& R = T.rings[ax.first_ring + ax.n_rings - 1];
            if (!(R.r0 >= 0.004f))
                continue;
            const uint32_t n = R.r0 > 0.1f ? 32u : 16u;
            const float out = 1.f / std::cos(kPi / float(n));
            const V3 bn = cross(R.t, R.n);
            const uint32_t c0 = uint32_t(T.patch_verts.size());
            PatchVert v;
            v.p = R.c;
            v.n = R.t;
            v.axis = a;
            v.s = R.s;
            patch_skin(v);
            T.patch_verts.push_back(v);
            for (uint32_t j = 0; j < n; ++j)
            {
                const float th = 2.f * kPi * float(j) / float(n);
                const float c1 = std::cos(th), s1 = std::sin(th), c2 = std::cos(2.f * th), s2 = std::sin(2.f * th);
                const float c3 = std::cos(3.f * th), s3 = std::sin(3.f * th);
                const float r = R.r0 * (1.f + R.f[0] * c1 + R.f[1] * s1 + R.f[2] * c2 + R.f[3] * s2 + R.f[4] * c3 + R.f[5] * s3);
                v.p = R.c + (R.n * c1 + bn * s1) * (std::max(r, 0.15f * R.r0) * out);
                patch_skin(v);
                T.patch_verts.push_back(v);
            }
            // (Counter-clockwise round t: the stock's winding, the face out along the axis.)
            for (uint32_t j = 0; j < n; ++j)
            {
                const uint32_t tri[3] = {c0, c0 + 1 + j, c0 + 1 + (j + 1) % n};
                T.patch_tris.insert(T.patch_tris.end(), std::begin(tri), std::end(tri));
                T.patch_tris_far.insert(T.patch_tris_far.end(), std::begin(tri), std::end(tri));
            }
            ax.capped = true;
        }
    }

    // Apical dominance (apex_cone): the crown field's top share narrowed to a cone from the apex, its radius going
    // linearly from the crown's own where the cone starts to the leader's at the top, and the top apex_leader metres
    // to the leader's short whorls (15 cm round it and 15 more a metre down) - the branches growing in the field
    // shorten toward the top as the field does: a spruce's spire, not the stock cards' rounded head.
    void tree_apex()
    {
        TreeState& S = *tree;
        if (A.apex_cone <= 0.f || S.occ.empty())
            return;
        const auto at = [&S](int x, int y, int z) { return (size_t(y) * size_t(S.nz) + size_t(z)) * size_t(S.nx) + size_t(x); };
        int top = -1, base = S.ny;
        for (int y = 0; y < S.ny; ++y)
            for (int z = 0; z < S.nz; ++z)
                for (int x = 0; x < S.nx; ++x)
                    if (S.occ[at(x, y, z)] >= kCrownOcc)
                    {
                        top = std::max(top, y);
                        base = std::min(base, y);
                    }
        if (top < 0)
            return;
        // The apex's axis: the middle of the crown's top half metre (cells).
        float ax = 0.f, az = 0.f, w = 0.f;
        const int band = std::max(int(std::ceil(0.5f / S.cell)), 1);
        for (int y = std::max(top - band + 1, 0); y <= top; ++y)
            for (int z = 0; z < S.nz; ++z)
                for (int x = 0; x < S.nx; ++x)
                    if (const float o = S.occ[at(x, y, z)]; o >= kCrownOcc)
                    {
                        ax += (float(x) + 0.5f) * o;
                        az += (float(z) + 0.5f) * o;
                        w += o;
                    }
        ax /= std::max(w, 1e-6f);
        az /= std::max(w, 1e-6f);
        const float apex = float(top + 1), len = std::max(clampf(A.apex_cone, 0.f, 1.f) * float(top + 1 - base), 1.f);
        // The crown's radius where the cone starts (its widest cell there).
        const int y0 = std::clamp(int(std::floor(apex - len)), base, top);
        float r0 = 1.f;
        for (int z = 0; z < S.nz; ++z)
            for (int x = 0; x < S.nx; ++x)
                if (S.occ[at(x, y0, z)] >= kCrownOcc)
                    r0 = std::max(r0, std::hypot(float(x) + 0.5f - ax, float(z) + 0.5f - az));
        const float leader = std::max(A.apex_leader, 0.f) / S.cell;
        for (int y = y0; y <= top; ++y)
        {
            const float d = apex - (float(y) + 0.5f);
            float r = r0 * d / len;
            if (d < leader)
                r = std::min(r, (0.15f + 0.15f * d * S.cell) / S.cell);
            // (The leader's own column at the least.)
            r = std::max(r, 0.75f);
            for (int z = 0; z < S.nz; ++z)
                for (int x = 0; x < S.nx; ++x)
                    if (std::hypot(float(x) + 0.5f - ax, float(z) + 0.5f - az) > r + 0.25f)
                        S.occ[at(x, y, z)] = 0.f;
        }
    }

    // ---- The stock axes ----
    // A point of the centripetal Catmull-Rom spline through p0..p3 between p1 and p2 (u 0..1; Barry and Goldman's
    // pyramid): it passes through every stock ring's centroid.
    static V3 catmull(V3 p0, V3 p1, V3 p2, V3 p3, float u)
    {
        const auto knot = [](V3 a, V3 b) { return std::sqrt(std::max(length(b - a), 1e-9f)); };
        const float t0 = 0.f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        const float t = lerpf(t1, t2, u);
        const V3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
        const V3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
        const V3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
        const V3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
        const V3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
        return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
    }

    static Ring ring_of(const SkelRing& R, uint32_t axis, float climbable)
    {
        Ring r;
        r.c = R.c;
        r.t = normalize(R.t);
        r.n = normalize(R.n - r.t * dot(R.n, r.t), perpendicular(r.t));
        r.s = R.s;
        r.r0 = std::max(R.r0, 0.002f);
        std::copy(std::begin(R.f), std::end(R.f), std::begin(r.f));
        r.axis = axis;
        r.climbable = climbable;
        r.stock_sides = R.stock_sides;
        r.fit_err = R.fit_err;
        return r;
    }

    // The levels of detail of an axis's rings: every ring in the first; in the next ones each kept while the spine
    // turns less than 12 (25) degrees, runs under 3 (8) metres and the radius changes less than a quarter since the
    // last kept ring (AAA_TREES 8.1: the length rule at 6, 12, 25 degrees a segment).
    void tree_lods(uint32_t first, uint32_t n)
    {
        for (uint32_t k = 0; k < n; ++k)
            T.rings[first + k].lod = 1u;
        static constexpr float kTurn[3] = {0.f, 12.f, 25.f}, kLen[3] = {0.f, 3.f, 8.f};
        for (uint32_t l = 1; l < 3; ++l)
        {
            const float cos_max = std::cos(kTurn[l] * kDeg);
            uint32_t last = 0;
            T.rings[first].lod |= 1u << l;
            for (uint32_t k = 1; k < n; ++k)
            {
                Ring& K = T.rings[first + k];
                if (k + 1 == n)
                {
                    K.lod |= 1u << l;
                    break;
                }
                const Ring& L = T.rings[first + last];
                const Ring& N = T.rings[first + k + 1];
                if (dot(L.t, N.t) < cos_max || N.s - L.s > kLen[l] || std::abs(N.r0 - L.r0) > 0.25f * std::max(L.r0, 1e-3f))
                {
                    K.lod |= 1u << l;
                    last = k;
                }
            }
        }
    }

    // The point, tangent, reference normal and radius of a stock axis at arclength s (between its rings).
    bool tree_frame(const Axis& ax, float s, V3& c, V3& t, V3& n, float& r) const
    {
        if (!ax.n_rings)
            return false;
        uint32_t k = ax.first_ring;
        const uint32_t last = ax.first_ring + ax.n_rings - 1;
        while (k < last && T.rings[k + 1].s <= s)
            ++k;
        const Ring& A0 = T.rings[k];
        const Ring& B0 = T.rings[std::min(k + 1, last)];
        const float u = B0.s > A0.s ? clampf((s - A0.s) / (B0.s - A0.s), 0.f, 1.f) : 0.f;
        c = A0.c + (B0.c - A0.c) * u;
        t = normalize(A0.t + (B0.t - A0.t) * u, A0.t);
        n = normalize(A0.n + (B0.n - A0.n) * u, A0.n);
        n = normalize(n - t * dot(n, t), perpendicular(t));
        r = lerpf(A0.r0, B0.r0, u);
        return true;
    }

    // The slopes of the monotone cubic Hermite through (x, y) (tools/trees/FORMAT.md: Fritsch-Carlson, the ends'
    // secants, 0 where the secants change sign) and its value between two knots.
    static void monotone_slopes(const float* x, const float* y, uint32_t n, float* m)
    {
        if (n < 2)
        {
            std::fill(m, m + n, 0.f);
            return;
        }
        const auto h = [x](uint32_t i) { return std::max(x[i + 1] - x[i], 1e-9f); };
        const auto d = [&](uint32_t i) { return (y[i + 1] - y[i]) / h(i); };
        m[0] = d(0);
        m[n - 1] = d(n - 2);
        for (uint32_t i = 1; i + 1 < n; ++i)
        {
            const float d0 = d(i - 1), d1 = d(i);
            if (d0 * d1 <= 0.f)
                m[i] = 0.f;
            else
            {
                const float w1 = 2.f * h(i) + h(i - 1), w2 = h(i) + 2.f * h(i - 1);
                m[i] = (w1 + w2) / (w1 / d0 + w2 / d1);
            }
        }
    }
    static float hermite(float y0, float y1, float m0, float m1, float h, float u)
    {
        const float u2 = u * u, u3 = u2 * u;
        return (2.f * u3 - 3.f * u2 + 1.f) * y0 + (u3 - 2.f * u2 + u) * h * m0 + (-2.f * u3 + 3.f * u2) * y1 + (u3 - u2) * h * m1;
    }

    // The stock axes from the skeleton, fixed where the stock model has them: their rings, subdivided where the spine
    // bends more than six degrees or runs over a metre by the file's own reconstruction (tools/trees/FORMAT.md: the
    // centripetal Catmull-Rom spine through the centroids, the tangent and the reference normal lerped, the radius the
    // monotone cubic Hermite over the arclengths, the profiles lerped), their segments, the stock triangles kept and
    // skinned to their axes (the bark's, then the cut ends' caps), the top of the climbable wood, the card branches'
    // axes as guides.
    void tree_stock()
    {
        TreeState& TS = *tree;
        const Skeleton& K = *TS.S;
        std::vector<int32_t> map(K.axes.size(), -1);
        // (The arclengths from each axis's first ring: a file whose rings start past zero is taken from there.)
        std::vector<float> s_off(K.axes.size(), 0.f);
        for (uint32_t a = 0; a < K.axes.size(); ++a)
            if (K.axes[a].n_rings && size_t(K.axes[a].first_ring) < K.rings.size())
                s_off[a] = K.rings[K.axes[a].first_ring].s;
        for (uint32_t a = 0; a < K.axes.size(); ++a)
        {
            const SkelAxis& X = K.axes[a];
            if ((X.flags & skaFromCards) || X.n_rings < 2 || size_t(X.first_ring) + X.n_rings > K.rings.size())
                continue;
            const uint32_t index = uint32_t(T.axes.size());
            map[a] = int32_t(index);
            Axis ax;
            ax.level = 0;
            ax.fixed = true;
            ax.climbable = X.climbable;
            ax.attach = X.attach;
            ax.cut_end = (X.flags & (skaCut | skaCap)) != 0;
            ax.capped = (X.flags & skaCap) != 0;
            ax.first_ring = uint32_t(T.rings.size());
            const uint32_t r0 = X.first_ring, n = X.n_rings;
            std::vector<float> ks(n), kr(n), slope(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                ks[i] = K.rings[r0 + i].s;
                kr[i] = K.rings[r0 + i].r0;
            }
            monotone_slopes(ks.data(), kr.data(), n, slope.data());
            for (uint32_t i = 0; i < n; ++i)
            {
                const SkelRing& RA = K.rings[r0 + i];
                T.rings.push_back(ring_of(RA, index, X.climbable));
                if (i + 1 == n)
                    break;
                const SkelRing& RB = K.rings[r0 + i + 1];
                const V3 p1 = RA.c, p2 = RB.c;
                const V3 p0 = i > 0 ? K.rings[r0 + i - 1].c : p1 * 2.f - p2;
                const V3 p3 = i + 2 < n ? K.rings[r0 + i + 2].c : p2 * 2.f - p1;
                const float turn = std::acos(clampf(dot(normalize(RA.t), normalize(RB.t)), -1.f, 1.f));
                const uint32_t m = std::min(std::max({uint32_t(std::ceil(turn / (6.f * kDeg))), uint32_t(std::ceil(length(p2 - p1) / 1.f)), 1u}), 8u);
                for (uint32_t j = 1; j < m; ++j)
                {
                    const float u = float(j) / float(m);
                    Ring r = ring_of(RA, index, X.climbable);
                    r.c = catmull(p0, p1, p2, p3, u);
                    r.t = normalize(RA.t + (RB.t - RA.t) * u, normalize(RA.t));
                    const V3 nb = RA.n + (RB.n - RA.n) * u;
                    r.n = normalize(nb - r.t * dot(nb, r.t), perpendicular(r.t));
                    r.s = lerpf(RA.s, RB.s, u);
                    r.r0 = std::max(hermite(RA.r0, RB.r0, slope[i], slope[i + 1], RB.s - RA.s, u), 0.002f);
                    for (int f = 0; f < 6; ++f)
                        r.f[f] = lerpf(RA.f[f], RB.f[f], u);
                    r.stock_sides = 0;
                    r.fit_err = std::max(RA.fit_err, RB.fit_err);
                    T.rings.push_back(r);
                }
            }
            ax.n_rings = uint32_t(T.rings.size()) - ax.first_ring;
            for (uint32_t k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
                T.rings[k].s = std::max(T.rings[k].s - s_off[a], 0.f);
            tree_lods(ax.first_ring, ax.n_rings);
            const Ring& first = T.rings[ax.first_ring];
            const Ring& tip = T.rings[ax.first_ring + ax.n_rings - 1];
            ax.base = first.c;
            ax.dir = first.t;
            ax.length = std::max(tip.s, 0.01f);
            ax.r_base = first.r0;
            ax.r_tip = tip.r0;
            ax.first_seg = uint32_t(T.segments.size());
            for (uint32_t k = ax.first_ring; k + 1 < ax.first_ring + ax.n_rings; ++k)
            {
                Segment g;
                g.a = T.rings[k].c;
                g.b = T.rings[k + 1].c;
                g.ra = T.rings[k].r0;
                g.rb = T.rings[k + 1].r0;
                g.axis = index;
                g.s0 = T.rings[k].s;
                T.segments.push_back(g);
            }
            ax.segs = uint32_t(T.segments.size()) - ax.first_seg;
            T.axes.push_back(ax);
        }
        // The parents (a stock axis's parent is a stock axis; one hanging from a card branch's axis or none is a root).
        for (uint32_t a = 0; a < K.axes.size(); ++a)
            if (map[a] >= 0 && K.axes[a].parent >= 0 && size_t(K.axes[a].parent) < map.size())
            {
                Axis& ax = T.axes[size_t(map[a])];
                ax.parent = map[size_t(K.axes[a].parent)];
                ax.attach = std::max(ax.attach - s_off[size_t(K.axes[a].parent)], 0.f);
            }
        // The stock triangles kept, skinned to their axes: the turn about the axis and the distance from it at the
        // vertex's arclength (the bark's cylindrical mapping).
        T.patch_verts.reserve(K.patch_verts.size());
        for (const SkelPatchVert& v : K.patch_verts)
        {
            PatchVert p;
            p.p = v.p;
            p.n = normalize(v.n);
            p.uv[0] = v.uv[0];
            p.uv[1] = v.uv[1];
            int32_t ax = v.axis < map.size() ? map[v.axis] : -1;
            if (ax < 0 && v.axis < K.axes.size() && K.axes[v.axis].parent >= 0 && size_t(K.axes[v.axis].parent) < map.size())
                ax = map[size_t(K.axes[v.axis].parent)];
            p.axis = ax >= 0 ? uint32_t(ax) : 0u;
            p.s = std::max(v.s - (v.axis < s_off.size() ? s_off[v.axis] : 0.f), 0.f);
            patch_skin(p);
            T.patch_verts.push_back(p);
        }
        T.patch_tris.reserve(K.patch_tris.size());
        T.patch_bark_tris = 0;
        for (size_t i = 0; i + 2 < K.patch_tris.size(); i += 3)
            if (K.patch_tris[i] < T.patch_verts.size() && K.patch_tris[i + 1] < T.patch_verts.size() && K.patch_tris[i + 2] < T.patch_verts.size())
            {
                T.patch_tris.insert(T.patch_tris.end(), {K.patch_tris[i], K.patch_tris[i + 1], K.patch_tris[i + 2]});
                T.patch_bark_tris += i / 3 < K.bark_tris ? 1u : 0u;
            }
        tree_ribbons_find();
        tree_patch_tessellate();
        tree_patch_feather();
        tree_patch_cover();
        tree_cut_caps();
        tree_patch_pairs();
        // The top of the climbable wood (an axis a quarter on the trunk collision and more).
        TS.climb_top = 0.f;
        for (const Ring& r : T.rings)
            if (r.climbable >= 0.25f)
                TS.climb_top = std::max(TS.climb_top, dot(r.c, vert) + r.r0);
        // The card branches' axes and the cards without one: the grafted first level's guides, from their joint on the
        // nearest stock wood.
        const auto nearest_stock = [this](V3 p, int32_t& axis, float& s) {
            float best = 1e30f;
            for (const Segment& g : T.segments)
            {
                const V3 ab = g.b - g.a;
                const float u = clampf(dot(p - g.a, ab) / std::max(dot(ab, ab), 1e-8f), 0.f, 1.f);
                const float d = length(p - (g.a + ab * u));
                if (d < best)
                {
                    best = d;
                    axis = int32_t(g.axis);
                    s = g.s0 + u * length(ab);
                }
            }
            return best;
        };
        for (uint32_t a = 0; a < K.axes.size(); ++a)
        {
            const SkelAxis& X = K.axes[a];
            if (!(X.flags & skaFromCards) || X.n_rings < 2 || size_t(X.first_ring) + X.n_rings > K.rings.size())
                continue;
            TreeState::Guide G;
            for (uint32_t i = 0; i < X.n_rings; ++i)
            {
                const V3 c = K.rings[X.first_ring + i].c;
                G.s.push_back(G.pts.empty() ? 0.f : G.s.back() + length(c - G.pts.back()));
                G.pts.push_back(c);
            }
            G.len = G.s.back();
            if (X.parent >= 0 && size_t(X.parent) < map.size() && map[size_t(X.parent)] >= 0)
            {
                G.parent = map[size_t(X.parent)];
                G.attach = std::max(X.attach - s_off[size_t(X.parent)], 0.f);
            }
            else if (nearest_stock(G.pts.front(), G.parent, G.attach) > 1.5f)
                continue;
            if (G.len > 0.2f && G.parent >= 0)
                TS.guides.push_back(std::move(G));
        }
        for (const SkelCard& c : K.cards)
        {
            if (c.axis != 0xFFFFFFFFu || c.len < 0.3f)
                continue;
            TreeState::Guide G;
            G.pts = {c.base, c.base + normalize(c.dir) * c.len};
            G.s = {0.f, c.len};
            G.len = c.len;
            if (nearest_stock(c.base, G.parent, G.attach) <= 1.5f && G.parent >= 0)
                TS.guides.push_back(std::move(G));
        }
    }

    // The stock wood in a grid of cells (the clearance), its standable tops in a plan grid (the reach of thick new wood).
    void tree_stock_grid()
    {
        TreeState& S = *tree;
        S.segs.clear();
        V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (uint32_t i = 0; i < T.segments.size(); ++i)
        {
            if (!T.axes[T.segments[i].axis].fixed)
                continue;
            S.segs.push_back(i);
            const Segment& g = T.segments[i];
            const float r = std::max(g.ra, g.rb);
            S.max_r = std::max(S.max_r, r);
            lo = v3(std::min({lo.x, g.a.x - r, g.b.x - r}), std::min({lo.y, g.a.y - r, g.b.y - r}), std::min({lo.z, g.a.z - r, g.b.z - r}));
            hi = v3(std::max({hi.x, g.a.x + r, g.b.x + r}), std::max({hi.y, g.a.y + r, g.b.y + r}), std::max({hi.z, g.a.z + r, g.b.z + r}));
        }
        if (!S.segs.empty())
        {
            S.g_cell = 0.5f;
            S.g_lo = lo - v3(S.g_cell, S.g_cell, S.g_cell);
            const float ext[3]{hi.x - S.g_lo.x, hi.y - S.g_lo.y, hi.z - S.g_lo.z};
            for (int a = 0; a < 3; ++a)
                S.g_n[a] = std::min(int(std::ceil(ext[a] / S.g_cell)) + 1, 512);
            const size_t cells = size_t(S.g_n[0]) * size_t(S.g_n[1]) * size_t(S.g_n[2]);
            std::vector<uint32_t> count(cells + 1, 0);
            const auto each = [&S](const Segment& g, auto&& fn) {
                const float r = std::max(g.ra, g.rb);
                int c0[3], c1[3];
                const float a[3]{g.a.x, g.a.y, g.a.z}, b[3]{g.b.x, g.b.y, g.b.z}, o[3]{S.g_lo.x, S.g_lo.y, S.g_lo.z};
                for (int k = 0; k < 3; ++k)
                {
                    c0[k] = std::clamp(int(std::floor((std::min(a[k], b[k]) - r - o[k]) / S.g_cell)), 0, S.g_n[k] - 1);
                    c1[k] = std::clamp(int(std::floor((std::max(a[k], b[k]) + r - o[k]) / S.g_cell)), 0, S.g_n[k] - 1);
                }
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int z = c0[2]; z <= c1[2]; ++z)
                        for (int x = c0[0]; x <= c1[0]; ++x)
                            fn((size_t(y) * size_t(S.g_n[2]) + size_t(z)) * size_t(S.g_n[0]) + size_t(x));
            };
            for (const uint32_t i : S.segs)
                each(T.segments[i], [&count](size_t c) { ++count[c + 1]; });
            for (size_t c = 0; c < cells; ++c)
                count[c + 1] += count[c];
            S.g_start = count;
            S.g_items.assign(count[cells], 0);
            for (const uint32_t i : S.segs)
                each(T.segments[i], [&S, &count, i](size_t c) { S.g_items[count[c]++] = i; });
        }
        // The standable tops: the upper surface of every stretch of the stock wood within fifty degrees of the level, the
        // cut and broken ends.
        S.stands.clear();
        const Skeleton& K = *S.S;
        for (const Axis& ax : T.axes)
        {
            if (!ax.fixed)
                continue;
            for (uint32_t k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
            {
                const Ring& r = T.rings[k];
                if (std::abs(dot(r.t, vert)) < 0.766f || k + 1 == ax.first_ring + ax.n_rings)
                    S.stands.push_back(r.c + vert * r.r0);
            }
        }
        (void)K;
        if (S.stands.empty())
            return;
        V3 slo{1e9f, 0.f, 1e9f}, shi{-1e9f, 0.f, -1e9f};
        for (const V3& p : S.stands)
        {
            slo = v3(std::min(slo.x, p.x), 0.f, std::min(slo.z, p.z));
            shi = v3(std::max(shi.x, p.x), 0.f, std::max(shi.z, p.z));
        }
        S.st_lo = slo;
        S.st_n[0] = std::max(int(std::ceil((shi.x - slo.x) / S.st_cell)) + 1, 1);
        S.st_n[1] = std::max(int(std::ceil((shi.z - slo.z) / S.st_cell)) + 1, 1);
        const size_t cells = size_t(S.st_n[0]) * size_t(S.st_n[1]);
        std::vector<uint32_t> count(cells + 1, 0);
        const auto cell_of_pt = [&S](const V3& p) {
            const int x = std::clamp(int(std::floor((p.x - S.st_lo.x) / S.st_cell)), 0, S.st_n[0] - 1);
            const int z = std::clamp(int(std::floor((p.z - S.st_lo.z) / S.st_cell)), 0, S.st_n[1] - 1);
            return size_t(z) * size_t(S.st_n[0]) + size_t(x);
        };
        for (const V3& p : S.stands)
            ++count[cell_of_pt(p) + 1];
        for (size_t c = 0; c < cells; ++c)
            count[c + 1] += count[c];
        S.st_start = count;
        S.st_items.assign(count[cells], 0);
        for (uint32_t i = 0; i < S.stands.size(); ++i)
            S.st_items[count[cell_of_pt(S.stands[i])]++] = i;
    }

    // ---- The grafted levels ----
    // The species' level l grafted on the tree: the first on the stock trunk and limbs (and along the card branches'
    // axes, which it follows), every next one on the one before - placed along each parent by the shrubs' rules of
    // branching inside the crown field; each child's radius held by the pipe model's budget of its parent (r_parent^2.15
    // caps the cross-section of everything beyond the node: the stock tip, the stock limbs, the grafted children; a
    // child never thicker than nine tenths of its parent there).
    void tree_graft(uint32_t l)
    {
        const Level& L = A.level[l - 1];
        constexpr float kPipe = 2.15f;
        struct Child
        {
            float s;
            V3 p, d;
            float want, rc, rp;
            int32_t guide;
        };
        std::vector<Child> kids;
        std::vector<std::pair<float, float>> stock_kids;
        const uint32_t count = uint32_t(T.axes.size());
        // A conifer's stock limb the stock left without a card branch is a dead one (self-pruned below the live crown,
        // broken): nothing grafts on it - the tufts grown there read as a dead pine stub sprouting.
        const bool conifer = (tree->S->flags & skelConifer) || A.leaf_kind == lkNeedles;
        std::vector<uint8_t> carded(count, 0);
        if (l == 1 && conifer)
            for (const TreeState::Guide& G : tree->guides)
                if (G.parent >= 0 && uint32_t(G.parent) < count)
                    carded[size_t(G.parent)] = 1;
        for (uint32_t pi = 0; pi < count; ++pi)
        {
            const Axis parent = T.axes[pi];
            const bool on = l == 1 ? parent.fixed : (!parent.fixed && parent.level == l - 1);
            if (!on || is_bare(pi) || (l == 1 && conifer && parent.parent >= 0 && !carded[pi] && !tree->guides.empty()))
                continue;
            kids.clear();
            // (Nothing grows out of a cut end's face: a stock limb's last one and a half radii of it.)
            const float s_cut = parent.fixed && parent.cut_end ? parent.length - std::max(1.5f * parent.r_tip, 0.03f) : parent.length;
            const float s0 = parent.length * L.start, s1 = std::min(parent.length * L.end, s_cut);
            const float spacing = 1.f / std::max(L.density / std::max(std::sqrt(wood_scale), 0.5f), 0.05f);
            float az = rng.u01() * 2.f * kPi;
            V3 nrm = perpendicular(parent.dir);
            const bool whorl = l == 1 && A.leader && A.whorl_step > 0.f;
            const float step = whorl ? A.whorl_step * std::max(wood_scale, 0.5f) : spacing;
            for (float s = s0 + step * rng.range(0.2f, 1.f); s < s1; s += step * (whorl ? rng.jitter(0.1f) : rng.jitter(0.35f)))
            {
                V3 p, t;
                float r;
                sample(parent, s, p, t, r);
                nrm = normalize(nrm - t * dot(nrm, t), perpendicular(t));
                const V3 bin = cross(t, nrm);
                const float u = s / std::max(parent.length, 1e-3f);
                const uint32_t per = whorl ? std::max(3u, uint32_t(std::lround(L.per_node + rng.range(-0.5f, 1.5f))))
                                           : std::max(1u, uint32_t(std::lround(L.per_node)));
                const float az_node = whorl ? rng.u01() * 2.f * kPi : az;
                for (uint32_t k = 0; k < per; ++k)
                {
                    const float a = az_node + float(k) * 2.f * kPi / float(per) + (whorl ? rng.range(-0.3f, 0.3f) : 0.f);
                    const V3 radial = nrm * std::cos(a) + bin * std::sin(a);
                    const float ang = lerpf(L.angle[0], L.angle[1], u) * rng.jitter(0.15f) * kDeg;
                    V3 d = normalize(t * std::cos(ang) + radial * std::sin(ang), t);
                    // Away from the tree's axis (its root's vertical), toward the light.
                    const V3 outv = normalize(p - vert * dot(p, vert), radial);
                    d = normalize(d + outv * L.outward, d);
                    float want = parent.length * lerpf(L.len[0], L.len[1], u) * rng.jitter(L.len_jitter);
                    want = std::min(want, free_length(p, d, want * 1.1f) * 0.97f);
                    // (A shoot of the last level: its leafy young growth and at most a short bare run before it.)
                    if (l >= 3)
                        want = std::min(want, A.shoot_len + std::max(8.f * A.leaf_spacing, 0.25f));
                    if (want < L.min_len)
                        continue;
                    kids.push_back(Child{s, p, d, want, std::max(r * L.radius, 0.0008f), r, -1});
                }
                az += L.phyllo * kDeg;
            }
            // The card branches' axes from this stock axis: the grafted first level along them (the stock crown's own
            // branch layout), a little thicker than the rest.
            if (l == 1 && A.stock_card_follow > 0.f)
                for (uint32_t gi = 0; gi < tree->guides.size(); ++gi)
                {
                    const TreeState::Guide& G = tree->guides[gi];
                    if (G.parent != int32_t(pi) || G.pts.size() < 2)
                        continue;
                    V3 p, t;
                    float r;
                    sample(parent, std::clamp(G.attach, 0.f, std::max(s_cut, 0.f)), p, t, r);
                    const V3 d = normalize(G.pts[1] - G.pts[0], t);
                    kids.push_back(Child{std::clamp(G.attach, 0.f, std::max(s_cut, 0.f)), p, d, G.len * rng.range(0.9f, 1.05f),
                        std::max(r * L.radius * 1.2f, 0.0008f), r, int32_t(gi)});
                }
            // The pipe model, from the tip down: the stock tip's cross-section, the stock children's past each node, the
            // grafted children's as they are given theirs.
            std::sort(kids.begin(), kids.end(), [](const Child& a, const Child& b) { return a.s > b.s; });
            stock_kids.clear();
            for (uint32_t ci = 0; ci < count; ++ci)
                if (T.axes[ci].fixed && T.axes[ci].parent == int32_t(pi))
                    stock_kids.emplace_back(T.axes[ci].attach, T.axes[ci].r_base);
            std::sort(stock_kids.begin(), stock_kids.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            float C = std::pow(std::max(parent.r_tip, 0.f), kPipe);
            size_t si = 0;
            for (Child& c : kids)
            {
                while (si < stock_kids.size() && stock_kids[si].first >= c.s)
                    C += std::pow(stock_kids[si++].second, kPipe);
                const float room = std::pow(c.rp, kPipe) - C;
                c.rc = std::min({c.rc, room > 0.f ? std::pow(room, 1.f / kPipe) : 0.f, 0.9f * c.rp});
                if (c.rc >= 0.0008f)
                    C += std::pow(c.rc, kPipe);
            }
            // Grown from the base up (the clearance sees the ones grown before them).
            for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            {
                if (it->rc < 0.0008f)
                    continue;
                tree->parent = int32_t(pi);
                tree->attach = it->s;
                tree->parent_r = it->rp;
                tree->guide = it->guide >= 0 ? &tree->guides[size_t(it->guide)] : nullptr;
                tree->follow = it->guide >= 0 ? A.stock_card_follow : 0.f;
                grow(l, it->p, it->d, it->want, it->rc, int32_t(pi), it->s, L.up, 0.f, L.wobble, L.seg_len);
            }
            tree->guide = nullptr;
            tree->follow = 0.f;
            tree->parent = -1;
        }
    }

    // A tree's leaf-out and fall orders as their quantiles by leaf area: the season keys' leaf_out and fallen are shares
    // of the leaves (the shaders compare each element's order with them), the orders sums of their height, shade and
    // own terms (a birch's fall order spanned 0.30 to 0.86: a third of its leaves down showed three per cent gone).
    void tree_order_quantiles()
    {
        if (T.leaves.empty())
            return;
        std::vector<uint32_t> idx(T.leaves.size());
        double total = 0.0;
        for (uint32_t i = 0; i < idx.size(); ++i)
        {
            idx[i] = i;
            total += std::max(T.leaves[i].area, 1e-9f);
        }
        for (const bool fall : {false, true})
        {
            const auto order = [this, fall](uint32_t i) { return fall ? T.leaves[i].fall : T.leaves[i].rank; };
            std::stable_sort(idx.begin(), idx.end(), [&order](uint32_t a, uint32_t b) { return order(a) < order(b); });
            double below = 0.0;
            for (const uint32_t i : idx)
            {
                const double a = std::max(T.leaves[i].area, 1e-9f);
                (fall ? T.leaves[i].fall : T.leaves[i].rank) = float((below + 0.5 * a) / total);
                below += a;
            }
        }
    }

    // A tree's bare ends cut back: a grafted axis ends a little past the last leaf, flower or child it carries (a
    // terminal bud's length: four centimetres on a shoot, eight on a twig, fifteen on a branch) - where the crown
    // field's edge left a branch leafless it stuck out of the crown as a bare rod. From the finest level down, so a
    // parent sees what its trimmed children still reach; a dead stub and a prickle keep their wood.
    void tree_trim()
    {
        tree_drop_wires();
        std::vector<float> reach(T.axes.size(), -1.f);
        for (const Leaf& f : T.leaves)
            reach[f.axis] = std::max(reach[f.axis], f.s);
        for (const Bloom& b : T.blooms)
            reach[b.axis] = std::max(reach[b.axis], b.s);
        const auto margin = [](uint32_t level) { return level >= 3 ? 0.04f : (level == 2 ? 0.08f : 0.15f); };
        for (uint32_t level = 3; level >= 1; --level)
            for (uint32_t a = 0; a < T.axes.size(); ++a)
            {
                const Axis& ax = T.axes[a];
                if (ax.fixed || ax.level != level || ax.parent < 0)
                    continue;
                if (ax.dead || ax.leafless)
                    reach[a] = ax.length;
                if (reach[a] >= 0.f)
                    reach[size_t(ax.parent)] = std::max(reach[size_t(ax.parent)], ax.attach);
            }
        std::vector<Segment> kept;
        kept.reserve(T.segments.size());
        for (uint32_t a = 0; a < T.axes.size(); ++a)
        {
            Axis& ax = T.axes[a];
            const uint32_t first = uint32_t(kept.size());
            const bool all = ax.fixed || ax.dead || ax.leafless || ax.level == 0;
            const float end = reach[a] < 0.f ? -1.f : reach[a] + margin(ax.level);
            for (uint32_t i = ax.first_seg; i < ax.first_seg + ax.segs; ++i)
                if (all || T.segments[i].s0 < end)
                    kept.push_back(T.segments[i]);
            ax.first_seg = first;
            ax.segs = uint32_t(kept.size()) - first;
        }
        T.segments.swap(kept);
    }

    // A grafted twig or shoot that runs bare from its base past its level's cap and most of its length before its first
    // leaf, flower or child goes whole, its leaves and its children with it (a shoot that reached for a thin part of the
    // crown hung out of it as a wire with a tuft at its end): a shoot's cap eight of its leaf spacings (a quarter metre
    // at least) and seven tenths of it, a twig's three of its shoots' spacings (60 cm at least) and four fifths; a
    // conifer's grafted branch a metre and seven tenths of it (a pine's four-metre bare rod with one tuft at its end).
    void tree_drop_wires()
    {
        const bool conifer = (tree->S->flags & skelConifer) || A.leaf_kind == lkNeedles;
        const uint32_t count = uint32_t(T.axes.size());
        std::vector<float> first(count, 1e9f);
        for (const Leaf& f : T.leaves)
            first[f.axis] = std::min(first[f.axis], f.s);
        for (const Bloom& b : T.blooms)
            first[b.axis] = std::min(first[b.axis], b.s);
        for (uint32_t a = 0; a < count; ++a)
        {
            const Axis& ax = T.axes[a];
            if (ax.parent >= 0 && !ax.leafless && !ax.dead)
                first[size_t(ax.parent)] = std::min(first[size_t(ax.parent)], ax.attach);
        }
        const float cap3 = std::max(8.f * A.leaf_spacing, 0.25f);
        const float cap2 = std::max(3.f / std::max(A.level[2].density, 0.5f), 0.6f);
        std::vector<uint8_t> gone(count, 0);
        // (A parent precedes its children: a gone twig takes its shoots in one pass.)
        for (uint32_t a = 0; a < count; ++a)
        {
            const Axis& ax = T.axes[a];
            if (ax.fixed || ax.parent < 0 || ax.dead || ax.leafless)
                continue;
            if (gone[size_t(ax.parent)] || (ax.level >= 3 && first[a] > std::max(cap3, 0.7f * ax.length)) ||
                (ax.level == 2 && first[a] > std::max(cap2, 0.8f * ax.length)) ||
                (conifer && ax.level == 1 && first[a] > std::max(1.f, 0.7f * ax.length)))
                gone[a] = 1;
        }
        if (std::find(gone.begin(), gone.end(), uint8_t(1)) == gone.end())
            return;
        T.leaves.erase(std::remove_if(T.leaves.begin(), T.leaves.end(), [&gone](const Leaf& f) { return gone[f.axis] != 0; }), T.leaves.end());
        T.blooms.erase(std::remove_if(T.blooms.begin(), T.blooms.end(), [&gone](const Bloom& b) { return gone[b.axis] != 0; }), T.blooms.end());
        for (uint32_t a = 0; a < count; ++a)
            if (gone[a])
                T.axes[a].segs = 0;
    }

    void bounds()
    {
        V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        auto grow_box = [&lo, &hi](V3 p, float r) {
            lo = v3(std::min(lo.x, p.x - r), std::min(lo.y, p.y - r), std::min(lo.z, p.z - r));
            hi = v3(std::max(hi.x, p.x + r), std::max(hi.y, p.y + r), std::max(hi.z, p.z + r));
        };
        for (const Segment& g : T.segments)
        {
            grow_box(g.a, g.ra);
            grow_box(g.b, g.rb);
        }
        T.leaf_area = 0.f;
        for (const Leaf& f : T.leaves)
        {
            grow_box(f.p + f.dir * f.len, 0.5f * f.wid);
            T.leaf_area += f.area;
        }
        for (const Bloom& b : T.blooms)
            grow_box(b.p + b.dir * b.size, 0.5f * b.size);
        // (A tree's stock rings and kept triangles.)
        for (const Ring& r : T.rings)
            grow_box(r.c, r.r0 * 1.3f);
        for (const PatchVert& v : T.patch_verts)
            grow_box(v.p, 0.f);
        T.bmin = lo;
        T.bmax = hi;
        T.height = hi.y;
        T.width = std::max(hi.x - lo.x, hi.z - lo.z);
    }
};
} // namespace

float envelope_radius(const Arch& a, float y, float h, float rw)
{
    const float v = std::min(std::max(y / std::max(h, 0.05f), 0.f), 1.f);
    const float rb = rw * a.env_base;
    switch (a.envelope)
    {
    case envVase:
        return lerpf(rb, rw, std::pow(v, 0.6f)) * std::sqrt(std::max(1.f - std::pow(std::max(v - 0.75f, 0.f) / 0.25f, 2.f), 0.f));
    case envCone: return rw * std::pow(1.f - v, 0.9f) * std::min(1.f, v / 0.06f + 0.3f);
    case envColumn: return rw * std::sqrt(std::max(1.f - std::pow(2.f * v - 1.f, 6.f), 0.f)) * lerpf(1.f, 0.7f, v);
    case envDome: return rw * std::sqrt(std::max(1.f - v * v, 0.f));
    case envFountain:
        return rw * std::pow(std::max(std::sin(kPi * std::min(v / 0.85f, 1.f) * 0.5f + kPi * 0.25f), 0.f), 0.55f) *
            (v > 0.8f ? std::sqrt(std::max(1.f - (v - 0.8f) / 0.2f, 0.f)) : 1.f) + rb * (1.f - v);
    case envCushion: return rw * std::sqrt(std::max(1.f - v * v * v, 0.f));
    case envEllipsoid:
    default: return std::max(rw * std::sqrt(std::max(1.f - (2.f * v - 1.f) * (2.f * v - 1.f), 0.f)), rb * (1.f - v));
    }
}

void generate(const Arch& arch, float h, float w, uint32_t seed, Template& out)
{
    out = Template{};
    Builder b(arch, out, seed, std::max(h, 0.2f), std::max(w, 0.2f));
    b.stems();
    for (uint32_t l = 1; l <= std::min(arch.levels, 3u); ++l)
        b.children(l);
    b.prickles();
    b.wood_segments = uint32_t(out.segments.size());
    b.young_growth();
    const uint32_t filled_from = uint32_t(out.axes.size());
    b.fill();
    // (The thorns of the young growth on the filling shoots too.)
    if (arch.prickle_young > 0.f)
        b.prickles(filled_from);
    b.spurs();
    b.finish_leaves();
    b.sky_visibility();
    b.mechanics();
    b.bounds();
    b.clusters();
    b.blobs();
}

bool read_skeleton(const void* data, size_t bytes, Skeleton& out, const char** why)
{
    out = Skeleton{};
    const auto fail = [why](const char* reason) {
        if (why)
            *why = reason;
        return false;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (!p || bytes < 96)
        return fail("shorter than its header");
    const auto u32_at = [p](size_t at) {
        uint32_t v;
        std::memcpy(&v, p + at, 4);
        return v;
    };
    const auto f32_at = [p](size_t at) {
        float v;
        std::memcpy(&v, p + at, 4);
        return v;
    };
    const auto v3_at = [&f32_at](size_t at) { return v3(f32_at(at), f32_at(at + 4), f32_at(at + 8)); };
    if (std::memcmp(p, "DTSK", 4) != 0)
        return fail("not a DTSK file");
    out.version = u32_at(4);
    if (out.version != 1)
        return fail("an unknown version");
    std::memcpy(&out.trunk_key, p + 8, 8);
    out.bmin = v3_at(16);
    out.bmax = v3_at(28);
    const uint32_t n_axes = u32_at(40), n_rings = u32_at(44), n_pv = u32_at(48), n_pt = u32_at(52), n_cards = u32_at(56);
    out.flags = u32_at(60);
    out.nx = u32_at(64);
    out.ny = u32_at(68);
    out.nz = u32_at(72);
    out.origin = v3_at(76);
    out.cell = f32_at(88);
    out.bark_tris = std::min(u32_at(92), n_pt);
    if (n_axes > (1u << 20) || n_rings > (1u << 22) || n_pv > (1u << 22) || n_pt > (1u << 22) || n_cards > (1u << 20) ||
        out.nx > 1024 || out.ny > 1024 || out.nz > 1024)
        return fail("counts out of range");
    const size_t grid = size_t(out.nx) * size_t(out.ny) * size_t(out.nz);
    // Every number finite, every point within the box and a quarter of it (a metre at the least), the box itself a
    // tree's (200 m at the most), the crown grid's cell over nothing and two metres at the most.
    const auto finite3 = [](V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite3(out.bmin) || !finite3(out.bmax) || out.bmax.x < out.bmin.x || out.bmax.y < out.bmin.y || out.bmax.z < out.bmin.z)
        return fail("its box not finite");
    const V3 ext = out.bmax - out.bmin;
    const float extent = std::max({ext.x, ext.y, ext.z}), diag = length(ext);
    if (extent > 200.f)
        return fail("its box past 200 m");
    const float margin = std::max(0.25f * extent, 1.f);
    const auto in_box = [&](V3 v) {
        return finite3(v) && v.x >= out.bmin.x - margin && v.y >= out.bmin.y - margin && v.z >= out.bmin.z - margin &&
            v.x <= out.bmax.x + margin && v.y <= out.bmax.y + margin && v.z <= out.bmax.z + margin;
    };
    if (grid && (!finite3(out.origin) || !(out.cell > 0.f && out.cell <= 2.f)))
        return fail("its crown grid's cell out of range");
    const size_t fixed = 96 + size_t(n_axes) * 32 + size_t(n_pv) * 40 + size_t(n_pt) * 12 + size_t(n_cards) * 48 + grid;
    // A ring's record: the contract's field list (c, t, n, s, r0, f[6], stock_sides, fit_err) is 76 bytes, its stated
    // size 72; either is read (a 72-byte ring has no fit_err).
    size_t ring_rec = 76;
    if (n_rings)
    {
        if (bytes < fixed || (bytes - fixed) % n_rings != 0)
            return fail("its size does not match its counts");
        ring_rec = (bytes - fixed) / n_rings;
        if (ring_rec != 76 && ring_rec != 72)
            return fail("an unknown ring record size");
    }
    else if (bytes != fixed)
        return fail("its size does not match its counts");
    size_t at = 96;
    out.axes.resize(n_axes);
    for (SkelAxis& a : out.axes)
    {
        std::memcpy(&a.parent, p + at, 4);
        a.level = u32_at(at + 4);
        a.attach = f32_at(at + 8);
        a.first_ring = u32_at(at + 12);
        a.n_rings = u32_at(at + 16);
        a.climbable = f32_at(at + 20);
        a.flags = u32_at(at + 24);
        // (A parent precedes its children.)
        if (size_t(a.first_ring) + a.n_rings > n_rings || (a.parent >= 0 && size_t(a.parent) >= size_t(&a - out.axes.data())))
            return fail("an axis out of range");
        if (!std::isfinite(a.attach) || !std::isfinite(a.climbable))
            return fail("an axis not finite");
        at += 32;
    }
    out.rings.resize(n_rings);
    for (SkelRing& r : out.rings)
    {
        r.c = v3_at(at);
        r.t = v3_at(at + 12);
        r.n = v3_at(at + 24);
        r.s = f32_at(at + 36);
        r.r0 = f32_at(at + 40);
        for (int k = 0; k < 6; ++k)
            r.f[k] = f32_at(at + 44 + 4 * size_t(k));
        r.stock_sides = u32_at(at + 68);
        r.fit_err = ring_rec >= 76 ? f32_at(at + 72) : 0.f;
        at += ring_rec;
        if (!in_box(r.c) || !finite3(r.t) || !finite3(r.n) || !std::isfinite(r.s) || !std::isfinite(r.r0) || r.r0 < 0.f ||
            r.s < -1.f || r.s > 1.5f * diag + 1.f)
            return fail("a ring not finite or out of its box");
        if (!std::isfinite(r.fit_err))
            r.fit_err = 0.f;
        // A profile whose harmonics sum past 0.6 of the mean radius is a thin twig's misfit (a few stock vertices, a
        // first harmonic of ten radii and more): the tube would turn inside out and the pixel floor's growth would carry
        // it across the sky. A twig under 3 cm stays round; a thicker ring keeps its shape at 0.6.
        float amp = 0.f;
        for (int k = 0; k < 6; k += 2)
            amp += std::isfinite(r.f[k]) && std::isfinite(r.f[k + 1]) ? std::sqrt(r.f[k] * r.f[k] + r.f[k + 1] * r.f[k + 1]) : 1e9f;
        if (amp > 0.6f)
        {
            const float keep = r.r0 < 0.03f ? 0.f : 0.6f / amp;
            for (float& f : r.f)
                f = std::isfinite(f) ? f * keep : 0.f;
            ++out.rings_tamed;
        }
    }
    // Along each axis the arclength never runs back (a misfit station's is held at the one before). A ring past its
    // axis's bound - a limb's two and a half times its parent's widest ring, the trunk's half the box across - is a
    // misfit (a willow's stub of ten metres walled the level, a one-metre ring swelled a limb): it takes the median of
    // its axis's rings within the bound, or seven tenths of its parent's ring where it leaves it, the bound at the most.
    const float wide = std::max(0.5f * std::max(ext.x, ext.z), 0.05f);
    std::vector<float> within;
    for (const SkelAxis& a : out.axes)
    {
        float cap = wide, fallback = 0.25f * wide;
        if (a.parent >= 0)
        {
            const SkelAxis& P = out.axes[size_t(a.parent)];
            float widest = 0.f, best = 1e30f;
            for (uint32_t k = P.first_ring; k < P.first_ring + P.n_rings; ++k)
            {
                const SkelRing& r = out.rings[k];
                widest = std::max(widest, r.r0);
                if (const float d = std::abs(r.s - a.attach); r.r0 > 0.f && d < best)
                {
                    best = d;
                    fallback = 0.7f * r.r0;
                }
            }
            cap = std::min(cap, std::max(2.5f * widest, 0.02f));
        }
        within.clear();
        for (uint32_t k = a.first_ring; k < a.first_ring + a.n_rings; ++k)
        {
            SkelRing& r = out.rings[k];
            if (k > a.first_ring)
                r.s = std::max(r.s, out.rings[k - 1].s);
            if (r.r0 <= cap)
                within.push_back(r.r0);
        }
        if (within.size() == a.n_rings)
            continue;
        if (!within.empty())
        {
            std::nth_element(within.begin(), within.begin() + ptrdiff_t(within.size() / 2), within.end());
            fallback = within[within.size() / 2];
        }
        for (uint32_t k = a.first_ring; k < a.first_ring + a.n_rings; ++k)
            if (SkelRing& r = out.rings[k]; r.r0 > cap)
            {
                r.r0 = std::min(fallback, cap);
                ++out.rings_tamed;
            }
    }
    out.patch_verts.resize(n_pv);
    for (SkelPatchVert& v : out.patch_verts)
    {
        v.p = v3_at(at);
        v.n = v3_at(at + 12);
        v.uv[0] = f32_at(at + 24);
        v.uv[1] = f32_at(at + 28);
        v.axis = u32_at(at + 32);
        v.s = f32_at(at + 36);
        at += 40;
        if (!in_box(v.p) || !finite3(v.n) || !std::isfinite(v.s) || (n_axes && v.axis >= n_axes))
            return fail("a patch vertex not finite, out of its box or of the axes");
        if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]))
            v.uv[0] = v.uv[1] = 0.f;
    }
    out.patch_tris.resize(size_t(n_pt) * 3);
    for (size_t i = 0; i < out.patch_tris.size(); ++i, at += 4)
        if ((out.patch_tris[i] = u32_at(at)) >= n_pv)
            return fail("a patch triangle out of range");
    out.cards.resize(n_cards);
    for (SkelCard& c : out.cards)
    {
        c.base = v3_at(at);
        c.dir = v3_at(at + 12);
        c.up = v3_at(at + 24);
        c.len = f32_at(at + 36);
        c.width = f32_at(at + 40);
        c.axis = u32_at(at + 44);
        at += 48;
        if (!in_box(c.base) || !finite3(c.dir) || !finite3(c.up) || !std::isfinite(c.len) || !std::isfinite(c.width) ||
            (c.axis != 0xFFFFFFFFu && c.axis >= n_axes))
            return fail("a card not finite or out of range");
        c.len = std::clamp(c.len, 0.f, extent);
        c.width = std::clamp(c.width, 0.f, extent);
    }
    out.grid.assign(p + at, p + at + grid);
    return true;
}

namespace
{
// A species' mean leaf element (m2: a leaf, a compound leaf, a spray, a needle tuft).
float leaf_element(const Arch& arch)
{
    const float leaf = std::max(0.25f * (arch.leaf_len[0] + arch.leaf_len[1]) * (arch.leaf_wid[0] + arch.leaf_wid[1]) * kBladeFill, 1e-5f);
    if (arch.leaf_kind == lkCompound || arch.leaf_kind == lkSpray)
        return leaf * std::max(std::round(arch.leaflets), 1.f);
    if (arch.leaf_kind == lkNeedles)
        return std::max(arch.leaf_spacing, 0.02f) * arch.needle_len;
    return leaf;
}
} // namespace

uint32_t tree_leaf_need(const Arch& arch, const Skeleton& skel, V3 up)
{
    if (arch.max_leaves_set)
        return arch.max_leaves;
    Template scratch;
    scratch.tree = true;
    TreeState ts;
    ts.S = &skel;
    const V3 ext = skel.bmax - skel.bmin;
    Builder b(arch, scratch, 1u, std::max(skel.bmax.y, 1.f), std::max(std::max(ext.x, ext.z), 0.5f));
    b.tree = &ts;
    b.vert = normalize(up, kUp);
    b.tree_crown();
    return uint32_t(clampf(b.leaf_target() / leaf_element(arch) * 1.1f, 4000.f, 4.0e6f));
}

void generate_on_skeleton(const Arch& arch_in, const Skeleton& skel, uint32_t seed, V3 up, uint32_t elem_cap, Template& out)
{
    out = Template{};
    out.tree = true;
    Arch arch = arch_in;
    // A conifer's cones at its shoot tips where the data gives no flowers.
    if (arch.bloom_share <= 0.f && arch.cone_share > 0.f && arch.cone_len > 0.f)
    {
        arch.bloom_place = bpTip;
        arch.bloom_share = arch.cone_share;
        arch.bloom_size = arch.fruit_size = arch.cone_len;
        arch.bloom_droop = arch.cone_droop;
    }
    TreeState ts;
    ts.S = &skel;
    const V3 ext = skel.bmax - skel.bmin;
    const float h = std::max(skel.bmax.y, 1.f);
    const float w = std::max(std::max(ext.x, ext.z), 0.5f);
    Builder b(arch, out, seed, h, w);
    b.tree = &ts;
    b.vert = normalize(up, kUp);
    b.tree_crown();
    b.tree_stock();
    b.tree_stock_grid();
    b.tree_ribbons_grow();
    // The leaf elements: the species' cap, or its leaf area over its mean element's (a leaf, a compound leaf, a spray, a
    // needle tuft), the level's budget at the most.
    const uint32_t by_area = uint32_t(clampf(b.leaf_target() / leaf_element(arch) * 1.1f, 4000.f, 4.0e6f));
    b.leaf_cap = std::min(arch.max_leaves_set ? arch.max_leaves : by_area, std::max(elem_cap, 1000u));
    const uint32_t levels = std::min(arch.graft_levels ? arch.graft_levels : arch.levels, 3u);
    for (uint32_t l = 1; l <= levels; ++l)
        b.tree_graft(l);
    b.prickles();
    b.wood_segments = uint32_t(out.segments.size());
    b.young_growth();
    const uint32_t filled_from = uint32_t(out.axes.size());
    b.fill();
    if (arch.prickle_young > 0.f)
        b.prickles(filled_from);
    b.spurs();
    b.tree_bare_limbs();
    b.finish_leaves();
    b.tree_trim();
    b.tree_order_quantiles();
    b.sky_visibility();
    b.mechanics();
    b.bounds();
    b.clusters();
    // A crown the budget thinned (a forty-metre spruce's tufts: a twentieth of its leaf area, a bare pole with a few dots
    // from 150 m) stands in its far parts at its whole leaf area: the clusters' and so the blobs' cover from the leaf area
    // the cap left out too, three times the near leaves' at the most.
    if (!arch.max_leaves_set && by_area > b.leaf_cap)
    {
        double have = 0.0;
        for (const Leaf& f : out.leaves)
            have += f.area;
        const float k = have > 1e-6 ? clampf(float(double(b.leaf_target()) / have), 1.f, 3.f) : 1.f;
        for (Cluster& c : out.clusters)
            c.area *= k;
    }
    b.blobs();
    out.climb_top = ts.climb_top;
}

namespace
{
// Up to n numbers of a comma separated list.
int parse_floats(const char* s, float* v, int n)
{
    int k = 0;
    while (s && *s && k < n)
    {
        char* end = nullptr;
        const float f = std::strtof(s, &end);
        if (end == s)
        {
            ++s;
            continue;
        }
        v[k++] = f;
        s = end;
    }
    return k;
}

bool set_pair(float (&d)[2], const float* v, int n)
{
    d[0] = v[0];
    d[1] = n > 1 ? v[1] : v[0];
    return true;
}

bool set_level(Level& L, std::string_view k, const float* v, int n)
{
    const std::pair<std::string_view, float*> scalars[] = {{"density", &L.density}, {"start", &L.start}, {"end", &L.end},
        {"phyllo", &L.phyllo}, {"per_node", &L.per_node}, {"len_jitter", &L.len_jitter}, {"up", &L.up},
        {"outward", &L.outward}, {"wobble", &L.wobble}, {"radius", &L.radius}, {"seg_len", &L.seg_len},
        {"min_len", &L.min_len}};
    for (const auto& [name, dst] : scalars)
        if (k == name)
            return *dst = v[0], true;
    if (k == "angle")
        return set_pair(L.angle, v, n);
    if (k == "len")
        return set_pair(L.len, v, n);
    return false;
}
} // namespace

bool arch_set(Arch& a, const char* key, const char* value)
{
    const std::string_view k = key ? key : "";
    const std::string_view s = value ? value : "";
    // Named values.
    if (k == "envelope")
    {
        static constexpr std::string_view names[envCount] = {"ellipsoid", "vase", "cone", "column", "dome", "fountain", "cushion"};
        for (uint32_t i = 0; i < envCount; ++i)
            if (s.find(names[i]) != std::string_view::npos)
                return a.envelope = Envelope(i), true;
        a.envelope = Envelope(std::min<uint32_t>(uint32_t(std::atoi(value)), envCount - 1));
        return true;
    }
    if (k == "leaf_kind")
    {
        a.leaf_kind = s.find("compound") != std::string_view::npos ? lkCompound
            : s.find("needle") != std::string_view::npos           ? lkNeedles
            : s.find("spray") != std::string_view::npos            ? lkSpray
                                                                   : lkSimple;
        return true;
    }
    if (k == "bloom_place")
    {
        a.bloom_place = s.find("axil") != std::string_view::npos ? bpAxil
            : s.find("old") != std::string_view::npos           ? bpOldWood
                                                                : bpTip;
        return true;
    }
    float v[4]{};
    const int n = parse_floats(value, v, 4);
    if (n < 1)
        return false;
    // The branching levels: l1_ .. l3_.
    if (k.size() > 3 && k[0] == 'l' && k[1] >= '1' && k[1] <= '3' && k[2] == '_')
        return set_level(a.level[k[1] - '1'], k.substr(3), v, n);
    const std::pair<std::string_view, float*> scalars[] = {{"href", &a.href}, {"env_base", &a.env_base},
        {"root_r", &a.root_r}, {"stem_up", &a.stem_up}, {"stem_arch", &a.stem_arch}, {"stem_wobble", &a.stem_wobble},
        {"stem_radius", &a.stem_radius}, {"stem_seg", &a.stem_seg}, {"whorl_step", &a.whorl_step},
        {"shoot_len", &a.shoot_len}, {"lai", &a.lai}, {"fill_shell", &a.fill_shell}, {"fill_clump", &a.fill_clump},
        {"fill_up", &a.fill_up}, {"fill_from", &a.fill_from}, {"spur_share", &a.spur_share}, {"spray_len", &a.spray_len},
        {"leaf_spacing", &a.leaf_spacing},
        {"leaf_per_node", &a.leaf_per_node}, {"leaf_phyllo", &a.leaf_phyllo}, {"leaf_flat", &a.leaf_flat},
        {"leaf_droop", &a.leaf_droop}, {"petiole", &a.petiole}, {"tip_shrink", &a.tip_shrink},
        {"leaflets", &a.leaflets}, {"rachis", &a.rachis}, {"leaflet_angle", &a.leaflet_angle},
        {"needle_len", &a.needle_len}, {"bloom_share", &a.bloom_share}, {"bloom_size", &a.bloom_size},
        {"bloom_droop", &a.bloom_droop}, {"fruit_size", &a.fruit_size}, {"catkin_share", &a.catkin_share},
        {"catkin_len", &a.catkin_len}, {"catkin_droop", &a.catkin_droop}, {"sag", &a.sag},
        {"prickle_spacing", &a.prickle_spacing}, {"prickle_len", &a.prickle_len}, {"prickle_hook", &a.prickle_hook},
        {"prickle_base", &a.prickle_base}, {"prickle_young", &a.prickle_young}, {"zig", &a.zig}};
    for (const auto& [name, dst] : scalars)
        if (k == name)
            return *dst = v[0], true;
    const std::pair<std::string_view, float (*)[2]> pairs[] = {{"stems", &a.stems}, {"stem_spread", &a.stem_spread},
        {"stem_len", &a.stem_len}, {"fill_len", &a.fill_len}, {"spur_leaves", &a.spur_leaves}, {"leaf_angle", &a.leaf_angle},
        {"leaf_len", &a.leaf_len}, {"leaf_wid", &a.leaf_wid}};
    for (const auto& [name, dst] : pairs)
        if (k == name)
            return set_pair(*dst, v, n);
    if (k == "leader")
        return a.leader = uint32_t(v[0]), true;
    if (k == "levels")
        return a.levels = uint32_t(clampf(v[0], 0.f, 3.f)), true;
    if (k == "max_leaves")
        return a.max_leaves = uint32_t(std::max(v[0], 0.f)), a.max_leaves_set = true, true;
    // The trees' keys (generate_on_skeleton).
    if (k == "graft_levels")
        return a.graft_levels = uint32_t(clampf(v[0], 0.f, 3.f)), true;
    // (cone_size: cone_len's other name.)
    const std::pair<std::string_view, float*> tree_scalars[] = {{"crown_fill", &a.crown_fill},
        {"stock_card_follow", &a.stock_card_follow}, {"cone_share", &a.cone_share}, {"cone_len", &a.cone_len},
        {"cone_size", &a.cone_len}, {"cone_droop", &a.cone_droop},
        {"apex_cone", &a.apex_cone}, {"apex_leader", &a.apex_leader}};
    for (const auto& [name, dst] : tree_scalars)
        if (k == name)
            return *dst = v[0], true;
    if (k == "autumn_order")
        return set_pair(a.autumn_order, v, n);
    // (autumn_inside: autumn_order's shade weight alone, 1 the inner leaves first, 0 the outer ones.)
    if (k == "autumn_inside")
        return a.autumn_order[1] = v[0] > 0.5f ? 0.5f : -0.5f, true;
    return false;
}
} // namespace da_shrub
