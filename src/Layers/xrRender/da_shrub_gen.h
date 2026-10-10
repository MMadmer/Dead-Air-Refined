// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural shrubs (docs/dead-air/SHRUBS.md): the generator of one bush of a species - its axes (stems,
// branches, twigs, shoots by the species' rules of branching), the segments drawn for them, the leaves (simple,
// compound leaflets or needle tufts) with their phyllotaxis, the flowers and fruits, and the clusters and crown
// blobs that stand for them at a distance. Pure geometry with no engine types, so a standalone preview tool
// (tools/shrubs/preview) builds the same bushes. Deterministic for a species, size and seed.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace da_shrub
{
struct V3
{
    float x{}, y{}, z{};
};
inline V3 v3(float x, float y, float z) { return {x, y, z}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a);
V3 normalize(V3 a, V3 fallback = {0.f, 1.f, 0.f});

// The crown's outline (axisymmetric, height H, width W): where branches stop growing.
enum Envelope : uint32_t
{
    envEllipsoid, // rounded bush (willow, alder buckthorn, currant)
    envVase,      // stems fanning out from a narrow base (hazel, lilac, elder)
    envCone,      // a young conifer or a sapling with a leader (pine, spruce, birch)
    envColumn,    // narrow upright (juniper, poplar suckers)
    envDome,      // wide and low from the ground up (blackthorn thicket, broom)
    envFountain,  // arching canes, widest above the middle (dog rose, raspberry, elder)
    envCushion,   // low mat (dewberry, heather)
    envCount
};

// Leaf kinds: a simple blade, a compound leaf (a rachis with leaflets), a needle tuft (a conifer shoot's needles
// drawn as one element), a spray (a stretch of a small-leaved shoot with its leaves drawn as one element: sea
// buckthorn, brooms, the narrow-leaved willows - a leaf apiece would not reach their leaf area).
enum LeafKind : uint32_t
{
    lkSimple,
    lkCompound,
    lkNeedles,
    lkSpray,
};

// Where flowers and fruits sit: at the tip of a leafy shoot, in the leaf axils along the shoots, on the
// previous years' wood (along branches, often before the leaves).
enum BloomPlace : uint32_t
{
    bpTip,
    bpAxil,
    bpOldWood,
};

// One branching level below the stems (1 branches, 2 twigs, 3 shoots).
struct Level
{
    float density{4.f};         // children per metre of the parent between start and end
    float start{0.2f}, end{1.f}; // where children begin and end along the parent (share of its length)
    float angle[2]{45.f, 30.f}; // branching angle from the parent (deg) at the parent's base .. tip
    float phyllo{137.5f};       // azimuth step between successive nodes (deg)
    float per_node{1.f};        // children per node (2 = opposite pairs, the next pair turned by phyllo)
    float len[2]{0.6f, 0.3f};   // child length as a share of the parent's length, at the base .. tip
    float len_jitter{0.25f};
    float up{0.5f};             // bend per metre toward the vertical (+) or the ground (-)
    float outward{0.3f};        // pull away from the bush's axis (light seeking)
    float wobble{0.15f};        // random bend (rad per sqrt metre)
    float radius{0.6f};         // base radius as a share of the parent's radius at the node
    float seg_len{0.12f};       // segment length (m)
    float min_len{0.05f};       // shorter children are not grown
};

// A species' architecture. Lengths in metres for the reference height href; the generator scales the woody
// structure to the bush's height and width and keeps the leaves, flowers and fruits at their real size.
struct Arch
{
    float href{2.f};
    Envelope envelope{envEllipsoid};
    float env_base{0.25f}; // envelope radius at the ground as a share of the half width (vase, cone)
    // Stems from the root crown.
    float stems[2]{3.f, 8.f};
    float root_r{0.15f};           // radius of the root crown the stems leave (m)
    float stem_spread[2]{5.f, 30.f}; // angle from the vertical, inner .. outer stems (deg)
    float stem_len[2]{0.75f, 1.f}; // stem length as a share of the height
    float stem_up{0.15f};          // bend per metre toward the vertical (+) or arching down (-)
    float stem_arch{0.f};          // extra downward bend growing along the stem (canes)
    float stem_wobble{0.1f};
    float stem_radius{0.02f};      // base radius of a stem of the reference height (m)
    float stem_seg{0.18f};
    uint32_t leader{0};            // 1 = one central leader with whorls (saplings)
    float whorl_step{0.f};         // a leader's whorl spacing (m), 0 = no whorls
    uint32_t levels{2};
    Level level[3]{};
    // Leaves: along the current year's growth (the last shoot_len of every axis, the whole of a short terminal
    // one), then on the leafy shoots that fill the crown's lit shell toward the light (each from the nearest wood
    // within its reach: a one-pass space colonisation), then on short shoots (spurs) along the older wood, until
    // the leaf area reaches lai.
    LeafKind leaf_kind{lkSimple};
    float shoot_len{0.35f};      // the current year's growth at an axis tip (m)
    float lai{3.f};              // leaf area index the foliage aims at (leaf area over the crown's ground area)
    float fill_len[2]{0.12f, 0.4f}; // a filling shoot's length (m); its reach to the wood is a fifth more
    float fill_shell{0.55f};     // the depth of the leafy shell the shoots fill, as a share of the crown's radius
    float fill_clump{0.5f};      // 0 even foliage .. 1 clumps and holes (a noise of a third of the crown)
    float fill_up{0.35f};        // the filling shoots' turn toward the vertical
    float fill_from{0.3f};       // the stems' bare foot (share of a stem's length no shoot leaves)
    float spur_share{0.f};       // the share of the leaf area on spurs (plums, pears, apples, currants)
    float spur_leaves[2]{2.f, 4.f}; // leaves of a short shoot
    float spray_len{0.1f};       // lkSpray: the shoot length one element covers (m); leaflets = its leaves
    float leaf_spacing{0.05f};   // node spacing along a shoot (m); for needles the length of a tuft
    float leaf_per_node{1.f};    // 1 alternate, 2 opposite, 3 whorled
    float leaf_phyllo{137.5f};   // azimuth step between nodes (deg)
    float leaf_angle[2]{45.f, 65.f}; // angle of the petiole/leaf from the shoot (deg)
    float leaf_flat{0.6f};       // how far a blade turns to the horizontal, facing up (0..1)
    float leaf_droop{0.1f};      // blade tip bending down (share of its length)
    float leaf_len[2]{0.05f, 0.09f}, leaf_wid[2]{0.03f, 0.06f};
    float petiole{0.01f};
    float tip_shrink{0.5f};      // leaves near a shoot tip shrink to this share
    // Compound leaves (and sprays: their leaves per element).
    float leaflets{0.f};         // leaflets per leaf (odd = with a terminal one)
    float rachis{0.f};           // rachis length (m)
    float leaflet_angle{70.f};   // angle of a leaflet from the rachis (deg)
    // Needles (lkNeedles): tufts along the shoots.
    float needle_len{0.f};
    // Flowers and fruits.
    BloomPlace bloom_place{bpTip};
    float bloom_share{0.f};      // share of the shoots (tip) or nodes (axil, old wood) that bloom
    float bloom_size{0.f};       // inflorescence size (m)
    float bloom_droop{0.f};      // 1 = hanging, -1 = upright
    float fruit_size{0.f};       // fruit (or fruit cluster) size (m)
    // Catkins (hazel, alder, birch, willows, aspen): along the young growth near its tip.
    float catkin_share{0.f};     // share of the young growth's nodes with a catkin
    float catkin_len{0.f};       // catkin length (m)
    float catkin_droop{1.f};     // 1 = hanging (hazel, alder, birch), -1 = upright (goat willow)
    uint32_t max_leaves{14000};  // a cap on the elements of one bush (the least lit go first)
    float sag{1.f};              // how much the long thin axes sag under their own weight (osier 0.3 .. canes 1)
    float prickle_spacing{0.f};  // prickles or thorns along the old wood (m between them, 0 none)
    float prickle_len{0.f};      // their length (m)
    float prickle_hook{0.f};     // 1 hooked back (the roses), 0 straight
    float prickle_base{0.3f};    // their base radius over their length (a rose's broad flat foot 0.45, a thorn 0.12)
    float zig{0.14f};            // the young shoots' kink at every node (rad, side to side; a hawthorn's zigzag 0.3)
    float prickle_young{0.f};    // > 0: only on the young growth - a branch's last metres (this many), every twig
                                 // (hawthorn, sea buckthorn); 0: along the stems and branches (roses, gooseberry)
    // Trees (generate_on_skeleton, docs/dead-air/TREES.md): the grafted levels on the stock axes (l1 .. l3; 0: `levels`),
    // the share of the crown field the grafted shoots fill (its cells by a noise), how closely the grafted first level
    // follows the stock card branches' axes, the autumn's order inside the crown (the weight of the height: the top
    // first; of the shade: + the inner leaves first - birch, aspen, poplar, willow -, - the outer ones: maple, oak;
    // autumn_inside 0/1 sets the shade's alone), the cones at the shoot tips (the bloom keys' when those give no
    // flowers: share, length - cone_len or cone_size -, droop); the apical dominance (apex_cone: the share of the
    // crown's height under the apex narrowed to a cone from it, the branches shortening linearly toward the top;
    // apex_leader: the top metres that are the leader and its short whorls - a spruce's spire).
    uint32_t graft_levels{0};
    float crown_fill{1.f};
    float stock_card_follow{0.5f};
    float apex_cone{0.f}, apex_leader{1.25f};
    float autumn_order[2]{0.4f, 0.f};
    float cone_share{0.f}, cone_len{0.f}, cone_droop{0.5f};
    bool max_leaves_set{};       // the data gave max_leaves (a tree's cap otherwise goes with its leaf area)
};

// An axis: one stem, branch, twig or shoot, from its base along its segments.
struct Axis
{
    V3 base, dir;           // base point and the direction at the base
    float length{};
    float r_base{}, r_tip{};
    int32_t parent{-1};
    float attach{};         // arclength on the parent where it starts (m)
    uint32_t level{};       // 0 stem .. 3 shoot
    uint32_t first_seg{}, segs{};
    float flex{};           // bend at the tip (rad) under the reference wind
    float freq{};           // natural frequency (Hz)
    bool dead{};            // a dead stub of the stool: no children, leaves or shoots, grey weathered wood
    bool leafless{};        // a dead stub or a prickle: no children, leaves or shoots
    // A tree's stock axis (the trunk, a modelled limb): fixed where the stock model has it (no bend of its own), its
    // rings in the template's (first_ring, n_rings: every one, the spine's subdivisions too), the share of it on the
    // collision (climbable).
    bool fixed{};
    uint32_t first_ring{}, n_rings{};
    float climbable{};
    // A stock axis's wood from this arclength on is dead (a conifer's self-pruned limb, a broadleaf's limb bare past the
    // crown): no young growth on it.
    float dead_from{1e9f};
    // A stock axis whose end is cut or broken (the skeleton's skaCut), closed by a stock cut-end cap (skaCap) or by one
    // the generator lays across its last ring (tree_cut_caps): its end grain shows; nothing grows on from it.
    bool cut_end{}, capped{};
};

// A segment of an axis: a tapered cylinder from a to b.
struct Segment
{
    V3 a, b;
    float ra{}, rb{};
    uint32_t axis{};
    float s0{};    // arclength along the axis at a
    float vis{1.f}; // the sky it sees past the leaves (sky_visibility)
};

// A leaf: a blade, a whole compound leaf (its rachis along dir, the leaflets drawn on it) or a needle tuft;
// base, midrib direction, the blade's side direction, the leaf tissue's area.
struct Leaf
{
    V3 p, dir, side;
    float len{}, wid{};
    float area{};
    uint32_t axis{};
    float s{};     // arclength along its axis
    float rank{};  // 0..1: when it unfolds among the bush's leaves
    float fall{};  // 0..1: when it colours and falls
    float light{}; // 0..1: a guess from its place while the crown grows (the cap drops the least lit), then the sky it
                   // sees past the rest of the crown (sky_visibility; the shaders' occlusion)
    float exposure{}; // 0..1: the sky's and the ground's light it gets past the crown; the distance thins the least first
};

enum BloomKind : uint32_t
{
    bkFlower, // a flower or inflorescence, then its fruit
    bkCatkin, // a catkin: from autumn, opening before the leaves
};

// A flower (inflorescence) or the fruit that follows it, or a catkin.
struct Bloom
{
    V3 p, dir;
    float size{};
    uint32_t axis{};
    float s{};
    float rank{};
    BloomKind kind{bkFlower};
    float vis{1.f}; // the sky it sees past the leaves (sky_visibility)
};

// A cluster: the leaves (and blooms) of one stretch of a shoot, one element in the middle distance.
struct Cluster
{
    V3 c, n;
    float r{};
    float area{};       // the leaves' area (m2)
    uint32_t axis{};
    float s{};
    uint32_t first{};   // its first leaf
    uint32_t leaves{};
    uint32_t blooms{};  // its flowers and fruits in leaves' worth of area
    float rank{}, fall{}, light{}, exposure{}; // its leaves' mean, by area
};

// A crown blob: a part of the crown in the far distance.
struct Blob
{
    V3 c, n;
    float r{};
    float area{};
    float bloom_share{};
    float rank{}, fall{}, light{};
    float pull{}; // how far it was drawn in toward the crown's middle (the shadow maps cast it from where it was)
};

// A ring of a tree's stock axis (template space): one of the skeleton's, or one the spine's centripetal Catmull-Rom
// subdivision added between two of them (stock_sides 0). Its cross-section r(th) = r0 (1 + sum ak cos k th + bk sin
// k th) about its reference normal n (th grows toward cross(t, n)); lod bit l: it stands in level of detail l (0 every
// 6 degrees of the spine's bend, 1 every 12, 2 every 25).
struct Ring
{
    V3 c, t, n;
    float s{}, r0{};
    float f[6]{}; // a1 b1 a2 b2 a3 b3
    uint32_t axis{};
    float climbable{};
    uint32_t lod{};
    uint32_t stock_sides{};
    float fit_err{};
    // The sixteenths of its turn (bit j at j / 16 of it from n toward cross(t, n)) the stock patches lie over, seen from
    // the axis (the ray through the tube's own point there and half way to the neighbouring sixteenths and rings; the
    // point alone where it stands out past the patch by more than the tolerance or the fit error is over it): its tube
    // steps back under them there. Bit 16 (kRingHidden): the patches cover the whole ring - a tube
    // stretch between two such rings is not drawn.
    uint32_t cover{};
    // How far the tube steps back under each covered sixteenth, four bits a sixteenth (bits 4j..4j+3, 0 where it is not
    // covered): level L the skeleton's tolerance and ((L - 1) / 14)^2 of the rest of the profile's radius there
    // (ring_step_back), the least that takes the tube a centimetre under the patch there.
    uint64_t back{};
};

// A vertex of the stock triangles a tree keeps as they are (a patch: a fork, a cap, a cut end), skinned to its axis:
// its arclength on it, its turn about it (rad, from the axis's reference normal there), its distance from it and the
// axis's ring radius there (the tube's own: its bark's tiles round it). A bark vertex's bark lies by two tubes
// (tree_patch_pairs): that first axis's and a second's (axis2, none: all ones) by the second's weight w2, the pair in
// the axes' order; w3 the weight of the world's projection where both smear; gap1 and gap2 its distances out of the two
// tubes' profiles (the pairs' own, not packed).
struct PatchVert
{
    V3 p, n;
    float uv[2]{};
    uint32_t axis{};
    float s{}, theta{}, r{}, ring_r{};
    uint32_t axis2{~0u};
    float s2{}, theta2{}, ring_r2{}, w2{}, w3{};
    float gap1{}, gap2{};
    // A tessellated vertex's stock facet: the stock triangle's corners it rose from and their first two weights (the
    // third the rest), ~0u a stock vertex - its lift over the facet's plane eases in near the trunk (the shaders).
    uint32_t facet[3]{~0u, ~0u, ~0u};
    float facet_w[2]{};
};

struct Template
{
    std::vector<Axis> axes;
    std::vector<Segment> segments;
    std::vector<Leaf> leaves;
    std::vector<Bloom> blooms;
    std::vector<Cluster> clusters;
    std::vector<Blob> blobs;
    V3 bmin, bmax;
    float height{}, width{};
    float leaf_area{};
    // Trees: the stock axes' rings, the stock triangles kept (their vertices and three indices a triangle), the top of
    // the climbable stock wood (template space).
    bool tree{};
    std::vector<Ring> rings;
    std::vector<PatchVert> patch_verts;
    std::vector<uint32_t> patch_tris;
    uint32_t patch_bark_tris{}; // the bark's triangles first, then the cut ends' caps (end grain: flat, never tessellated)
    // The stock triangles as the stock has them (the far levels of detail: the near ones are patch_tris, the stock
    // facets rounded by tessellation) over the same vertices (the stock's are the first ones), bark first.
    std::vector<uint32_t> patch_tris_far;
    uint32_t patch_bark_tris_far{};
    float climb_top{};
};

// One bush of the species at height h and width w (m), the seed choosing its variant.
void generate(const Arch& arch, float h, float w, uint32_t seed, Template& out);

// ---- Trees (docs/dead-air/TREES.md) ----------------------------------------------------------------------
// A stock kind's skeleton, gamedata/meshes/da/trees/<skel>.dtsk (little-endian, version 1; the model space of the stock
// kind: metres, y up, the root at its pivot): its axes (0 the trunk, 1 a modelled limb, 2 a card branch's axis), their
// rings, the stock triangles the rings do not fit, the crown cards and the crown's occupancy grid.
// Ring::cover's bit: the stock patches cover the whole ring.
constexpr uint32_t kRingHidden = 0x10000u;

// A covered sixteenth's step back (its level in Ring::back; 0 where it is not covered) at the profile's radius rad
// there, 15 % of it left at the least - as the stock tubes' vertex shader takes it (r3\da_tree_stock_vs.h).
inline float ring_step_back(const Ring& r, uint32_t j, float rad)
{
    const uint32_t level = uint32_t(r.back >> (4u * (j & 15u))) & 15u;
    if (!level)
        return 0.f;
    const float tol = std::max(0.02f, 0.05f * r.r0), q = float(level - 1u) / 14.f;
    return std::min(tol + q * q * std::max(rad - tol, 0.f), 0.85f * rad);
}

enum SkelFlags : uint32_t
{
    skelConifer = 1,
    skelDead = 2,
    skelBrokenTop = 4,
    skelCutStump = 8,
    skelProcedural = 16, // synthetic axes only (a kind without bark: no stock wood to keep, no patches)
};
enum SkelAxisFlags : uint32_t
{
    skaFromCards = 1, // a card branch's axis: no stock wood
    skaCut = 2,       // a cut or broken end
    skaCap = 4,
};
struct SkelAxis
{
    int32_t parent{-1};
    uint32_t level{};
    float attach{};
    uint32_t first_ring{}, n_rings{};
    float climbable{};
    uint32_t flags{};
};
struct SkelRing
{
    V3 c, t, n;
    float s{}, r0{};
    float f[6]{};
    uint32_t stock_sides{};
    float fit_err{};
};
struct SkelPatchVert
{
    V3 p, n;
    float uv[2]{};
    uint32_t axis{};
    float s{};
};
struct SkelCard
{
    V3 base, dir, up;
    float len{}, width{};
    uint32_t axis{0xFFFFFFFFu};
};
struct Skeleton
{
    uint32_t version{}, flags{};
    uint64_t trunk_key{};
    V3 bmin, bmax;
    uint32_t nx{}, ny{}, nz{};
    V3 origin;
    float cell{};
    std::vector<SkelAxis> axes;
    std::vector<SkelRing> rings;
    std::vector<SkelPatchVert> patch_verts;
    std::vector<uint32_t> patch_tris;
    uint32_t bark_tris{}; // the patch triangles of the stock bark come first, the cut ends' caps after them
    std::vector<SkelCard> cards;
    std::vector<uint8_t> grid; // nx * ny * nz, x fastest, then z, then y
    uint32_t rings_tamed{};    // misfit rings held: profiles at 0.6 of the mean radius (a thin twig's made round), radii past
                               // their axis's bound
};
// Parses a .dtsk file's bytes; false (why: the reason) for a malformed one.
bool read_skeleton(const void* data, size_t bytes, Skeleton& out, const char** why = nullptr);

// One tree of the species on a stock skeleton (CONTRACT 2): the stock axes fixed where they are (their rings, the
// spine through them subdivided, the stock triangles kept), the species' grafted levels on them inside the crown field
// by the pipe model's budget, kept off the stock wood and too thin to stand on wherever the player could reach them,
// then the leaves, sprays or needles by its leaf area index over the crown, its flowers, catkins, fruit and cones, the
// light inside the crown, the mechanics, the clusters and blobs. up: the vertical in the model's space
// (a tilted stock tree's tilt bucket; (0, 1, 0) upright), elem_cap: the most leaf elements the level's budget leaves it.
void generate_on_skeleton(const Arch& arch, const Skeleton& skel, uint32_t seed, V3 up, uint32_t elem_cap, Template& out);
// The leaf elements a tree on the skeleton asks for its whole leaf area (its crown field's projection at the species'
// LAI over its mean element: the level's budget is shared by these).
uint32_t tree_leaf_need(const Arch& arch, const Skeleton& skel, V3 up);

// Sets one architecture key of a species section (dead_air_x64_shrubs.ltx: href, envelope, stems, l2_angle,
// leaf_kind, ...) from its text value; false for a key that is not an architecture key.
bool arch_set(Arch& arch, const char* key, const char* value);

// The envelope's radius at height y of a crown of height h and half width rw.
float envelope_radius(const Arch& arch, float y, float h, float rw);
} // namespace da_shrub
