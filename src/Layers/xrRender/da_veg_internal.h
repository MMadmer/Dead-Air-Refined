// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The vegetation core (docs/dead-air/TREES.md, SHRUBS.md): what the core and its two populations - the procedural
// shrubs (da_shrubs.cpp) and the upgraded level trees (da_trees.cpp) - share. One set of frame buffers (instance
// records, chunks, bends, the species and season tables), one latch, one grid walk with frustum and occlusion tests,
// one chunk emission, one bend pass and the same draws in every pass; each population brings its own data, census
// classes, templates (their element, axis and aux buffers) and tier rules.
#pragma once

#include "da_shrub_gen.h"
#include "da_grass_data.h"
#include "xrRender_console.h"
#include "xrEngine/da_season.h"

#include <atomic>

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class dxRender_Visual;
class FHierrarhyVisual;
class FTreeVisual;

namespace da_veg
{
// ---- The layout of the shaders' buffers (r3\da_shrub_common.h mirrors every number here) ---------------------
constexpr u32 kRec = 32, kAxisRec = 32, kInstRec = 96, kChunkRec = 16, kChunk = 64;
// The species rows of the one tables buffer: the shrubs' first, the trees' after them (a record's species is 8 bits).
constexpr u32 kShrubSpecies = 48, kTreeSpecies = 80, kSpecies = kShrubSpecies + kTreeSpecies;
constexpr u32 kProfF4 = 22, kSeasF4 = 16;
constexpr u32 kTabSeas = kSpecies * kProfF4, kTabTotal = kTabSeas + kSpecies * kSeasF4;
// Element kinds: the shrubs use the first five; the trees add their stock wood (tubes on the stock rings) and the stock
// triangles kept as they are (patches).
enum ElemKind : u32
{
    kKindWood,
    kKindLeaf,
    kKindBloom,
    kKindCluster,
    kKindBlob,
    kKindStock,
    kKindPatch,
    kKinds
};
// The frame's instances and chunks at most (shared by the populations).
constexpr u32 kMaxInstances = 16384, kMaxChunks = 1u << 18;
// A chunk slot's bend record (r3\da_shrub_common.h SHRUB_BEND_REC), and the most chunks the bend buffer grows to, the
// frame's and the view's a frame back (past these the vertex shaders walk the chain of axes themselves: a forest's
// shadow passes ran past 16384 chunks).
constexpr u32 kBendRec = 36, kBendMaxChunks = 1u << 15, kBendMaxViewChunks = 1u << 14;
// The culling grid's cell (m).
constexpr float kCell = 32.f;
// Instance record flags (word 12, bits 8..15): a dead plant, a tree (the wide element layout, the trunk sway), a Red
// Forest pine's dead rust-red needles kept all year; the word's top byte a young tree's spring lead (its share of the
// trees' policy's, r3\da_shrub_common.h shrub_spring).
constexpr u32 kInstDead = 1, kInstTree = 2, kInstRed = 4;
// A chunk's third word: count (8) | crossfade lo (8) | hi (8) | flags; rigid: its elements ride fixed axes (no bend);
// far: the far path (a stock patch's bark without its runs: its tile under two pixels).
constexpr u32 kChunkRigid = 0x80000000u, kChunkFar = 0x40000000u;
// The flags byte's low six bits: a chunk's thinning past the pixel floor - the share 1 - k/63 of its parts kept (a prefix:
// they go by importance, the outer ones first), their cover made up by the vertex shader (r3\da_shrub_cluster_vs.h).
inline u32 thin_flags(float share) { return u32(std::lround((1.f - std::clamp(share, 0.f, 1.f)) * 63.f)) << 24; }
// An axis record's level byte: 0x40 a fixed axis (the stock wood: no bend of its own, the chain stops there).
constexpr u32 kAxisFixed = 0x40;
// The aux buffer's slot (a ring is two, a patch vertex one).
constexpr u32 kAuxSlot = 32;

// The vertex classes of the index buffer, one chunk (64 elements) each: five-, eight-, sixteen- and thirty-two-sided
// tube segments (two rings, each closing on a copy of its first vertex for the bark's wrap), a leaf's hull (base,
// three pairs, tip), a quad, a near spray's six hulls, its six diamonds, a near needle sprig's two crossed quads, a
// triangle (the trees' stock patches).
enum VertexClass : u32
{
    vcTube,
    vcTube8,
    vcLeaf8,
    vcQuad,
    vcSpray,
    vcSpray4,
    vcQuad2,
    vcTube16,
    vcTube32,
    vcTri,
    vcCount
};
// The most leaves a near spray stands as (r3\da_shrub_leaf_vs.h).
constexpr u32 kSprayLeaves = 6;
struct VertexClassInfo
{
    u32 verts, indices, first;
};

// The draws of a frame (r3\da_shrub_leaf_vs.h tests drLeafCut, drSprayCut and drNeedle2 by number).
enum Draw : u32
{
    drWood,
    drLeaf8,
    drLeaf4,
    drLeafCut,
    drBloom,
    drCluster,
    drBlob,
    drSpray,
    drSprayCut,
    drSpray4,   // a spray past d0: its own leaves as textured quads
    drWood8,    // the wood within d0: eight-sided tubes
    drNeedle2,  // a needle sprig near: two crossed quads, a brush round its shoot
    // The trees': the grafted wood of sixteen sides near, the stock wood's tubes (its rings with their profiles) of
    // thirty-two, sixteen and eight sides, the stock triangles kept as they are.
    drWood16,
    drStock32,
    drStock16,
    drStock8,
    drPatch,
    // The far wood (r3\da_shrub_wood_far.ps): the grafted wood, the stock tubes, the patches - the bark's mean.
    drWoodFar,
    drStockFar,
    drPatchFar,
    drCount
};
constexpr u32 kDrawKind[drCount] = {kKindWood, kKindLeaf, kKindLeaf, kKindLeaf, kKindBloom, kKindCluster, kKindBlob, kKindLeaf,
    kKindLeaf, kKindLeaf, kKindWood, kKindLeaf, kKindWood, kKindStock, kKindStock, kKindStock, kKindPatch, kKindWood, kKindStock, kKindPatch};
constexpr VertexClass kDrawClass[drCount] = {vcTube, vcLeaf8, vcQuad, vcQuad, vcQuad, vcQuad, vcQuad, vcSpray, vcSpray,
    vcSpray4, vcTube8, vcQuad2, vcTube16, vcTube32, vcTube16, vcTube8, vcTri, vcTube, vcTube8, vcTri};
// The order a pass's draws take the frame's chunks in: a full chunk buffer cuts the last ones, so the stock wood and its
// patches come first, the wood next, the foliage after (cut in the enum's order, a near tree lost its trunk and limbs
// and kept its twigs floating).
constexpr Draw kAppendOrder[drCount] = {drStock32, drStock16, drStock8, drStockFar, drPatch, drPatchFar, drWood16, drWood8, drWood,
    drWoodFar, drLeaf8, drNeedle2, drSpray, drLeaf4, drLeafCut, drSprayCut, drSpray4, drBloom, drCluster, drBlob};
// Passes: the view, the sun cascades, the rain's occlusion map (the crowns from above).
constexpr u32 kCascades = 3, kRainPass = 1 + kCascades, kPasses = kRainPass + 1;
// The local lights (flashlights, lamps, fires) the vegetation casts into: the shadowed ones within this distance of the
// camera, as the grass details' light shadows (r2_R_lights.cpp).
constexpr float kLightReach = 40.f;
// The crossfade band of a tier switch, as a share of its distance.
constexpr float kBand = 0.15f;
// A cut's table: its key at every 32nd of its elements, the last one's too.
constexpr u32 kRq = 33;
// The trees' GPU cut of the view (r3\da_tree_cut.cs): a candidate chunk's class - its leaves (near, quads, far by their
// own distance), its grafted wood and its stock tubes (four draws by the sides each element needs at its own depth), its
// stock patches (near or far by their bark's tile) -, its route's bytes (the class and four draws, three parameters), the
// candidate chunks a frame and the lists' words at the most (the lists past the chunk records in the chunk buffer:
// r3\da_shrub_common.h SHRUB_CUT_WORDS).
enum CutClass : u32
{
    ccLeaf,
    ccWood,
    ccStock,
    ccPatch
};
constexpr u32 kCutRoute = 16, kMaxCutChunks = 1u << 16, kMaxCutWords = 1u << 22;

// Trees: the tables a view or a cascade cuts the wood by (da_trees.cpp pack_tree), each a set's radii falling in its
// elements' order - the grafted wood's three groups (levels 0-1, 2, 3) axis by axis by its thickest mean radius (a
// segment's ring is shared with the next one's beside it), the stock tubes' three levels of detail by their thicker
// ring's, the patches near and far by their limb's -; and each grafted group's thickest mean radius. Template metres:
// the prefix over a pass's floor over the tree's scale is every element the shaders would stand (their wire rules by
// the radius at half the floor: sub-pixel wood was most of the trees' cost, its vertices run to be thrown away), and a
// split of it by radius gives each part the sides its silhouette needs.
struct WoodCut
{
    float wood[3][kRq]{}, stock[3][kRq]{}, patch[2][kRq]{};
    float wood_r[3]{};
};

// ======================================================================================================
// GPU buffers
// ======================================================================================================
struct GpuBuffer
{
    ID3D11Buffer* buf{};
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    u32 bytes{};
    void release()
    {
        _RELEASE(uav);
        _RELEASE(srv);
        _RELEASE(buf);
        bytes = 0;
    }
};
// A raw (ByteAddress) buffer the shaders read (srv) and a compute pass writes (uav), its misc flags (an indirect draw's
// arguments); init may be null.
bool make_buffer(GpuBuffer& b, u32 bytes, const void* init, bool uav = false, bool srv = true, u32 misc = 0);

// ======================================================================================================
// Species (dead_air_x64_shrubs.ltx [shrub_species_N], dead_air_x64_trees.ltx [tree_species_N])
// ======================================================================================================
// Season scalars of a species, in the order of the GPU rows 0..2.
enum SeasonKey : u32
{
    skLeafOut,
    skLeafGrow,
    skAutumn,
    skFallen,
    skBloom,
    skBloomStage,
    skFruit,
    skRipe,
    skCatkin,
    skCatkinLen,
    skAutumnSpread,
    skFruitKeep,
    skCount
};
// Season colours, rows 3..15.
enum SeasonColour : u32
{
    scYoung,
    scSummer,
    scAutumnA,
    scAutumnB,
    scDead,
    scUnder,
    scBark,
    scTwig,
    scPetal,
    scCentre,
    scFruitUnripe,
    scFruitRipe,
    scCatkin,
    scCount
};

struct Species
{
    bool used{};
    shared_str name;
    da_shrub::Arch arch;
    float height[2]{1.f, 3.f}; // the real plant's height range (m)
    float width[2]{0.6f, 1.f}; // crown width as a share of the height
    u32 classes{};             // stock classes it stands in for (bit per class, the population's)
    float dead{};              // share of the plants standing dead (no leaves, grey wood)
    float spread_days{6.f};    // phenology spread between plants (days)
    // Drawing (profile rows 0..6, r3\da_shrub_common.h).
    float outline{}, leaflets{}, leaflet_angle{60.f}, leaflet_ratio{0.55f};
    float translucency{0.6f}, gloss{0.15f}, flutter{0.1f}, fold{0.15f};
    float under_light{1.2f}, under_blue{0.f}, jitter_value{0.08f}, jitter_hue{0.03f};
    float bloom_shape{}, petals{5.f}, fruit_shape{}, fruits{1.f};
    float catkin_shape{}, bark_rough{0.6f}, twig_gloss{0.1f}, lenticels{};
    float droop{0.1f}, needle_density{1.f}, evergreen{}, lobes{};
    // The texture layers (rows 8..10, tools/shrubs/textures): the leaf's a and b layers, the spray card, a compound
    // leaf's leaflet; the near leaf's hull (four rows: the base's width, two rows' places and widths, the tip's width);
    // the bark layer and its lichen.
    float leaf_tex[4]{0.f, 0.f, -1.f, -1.f};
    float leaf_hull[6]{1.f, 0.3333f, 1.f, 0.6667f, 1.f, 1.f};
    float bark_tex{}, lichen{};
    // The leaf and card squares over the generator's elements: length, width, start behind the base, and the clusters'
    // leaf coverage of its square (rows 11, 12).
    float leaf_fit[4]{1.f, 1.f, 0.f, 0.f}, card_fit[3]{1.f, 1.f, 0.f};
    // The bark texture as the species wears it (rows 12 w, 13, 14 w), its prickles' colour (row 14).
    float young_relief{0.25f}, bark_tint[3]{1.f, 1.f, 1.f}, bark_contrast{1.f}, bark_scale{1.f}, prickle_colour[3]{};
    // The young growth's flush on its upper, sunlit side (row 15: linear rgb and its share).
    float twig_blush[4]{};
    // The far parts' value and chroma on their open, light top against the near leaves (row 16 y, z); trees: the
    // radius (m) under which the wood wears the young twig's skin, not its bark (row 16 w; 0 none).
    float far_value{1.f}, far_chroma{1.f}, twig_r{};
    float autumn_mix[4]{0.f, 0.5f, 0.5f, 0.3f};
    // Trees (row 17): the upper bark's layer (-1 none) and the zone it takes over in (height above the ground in
    // metres; the wood's radius in metres: bark_zone_mode age, 1; shares of the tree's height above the ground: share,
    // 2 - a young tree's rough foot as low as its height asks), 4 more where the upper bark takes the lower's ridges
    // first (bark_zone_order ridges: a poplar's smooth bark over its rough foot, the fissures left as dark lenses).
    float bark_tex_upper{-1.f}, bark_zone[2]{4.f, 7.f}, bark_zone_age{};
    // Row 18: the share of its leaves a sapling keeps withered through the winter, the last to fall (a young oak's, a
    // hornbeam's; an old tree keeps a twelfth of it); the bark tiles' stretch along the stem (an aspen's rhombs taller
    // than wide); the upper bark's tile scale and contrast (-1: the lower's).
    float marcescent{}, bark_stretch{1.f}, bark_scale_upper{-1.f}, bark_contrast_upper{-1.f};
    // Rows 19, 20 (trees): the wood a cut end shows - the sapwood's colour (linear rgb), the heartwood's (its own or the
    // sapwood's) and its share of the radius, a year's ring width (m).
    float wood_sap[3]{0.73f, 0.58f, 0.36f}, wood_heart[4]{-1.f, -1.f, -1.f, 0.f}, ring_width{0.003f};
    // Row 21 (trees, z and w): the upper bark's value and chroma against its layer's (a pine's mirror bark less
    // saturated than its painted tile).
    float bark_value_upper{1.f}, bark_chroma_upper{1.f};
    DaSeasonCurve keys[skCount];
    DaColourCurve colours[scCount];
};

// Parses a species section into s (every key of a shrub section, and the trees' own); class_bits maps the section's
// `classes` list to the population's class bits (null: none).
void parse_species(const CInifile::Sect& sect, Species& s, u32 (*class_bits)(pcstr list));
// Profile rows (kProfF4) and season rows (kSeasF4) of species S at the day; elem_area: the species' mean leaf element
// (m2) over its templates; lead: the days a young tree's spring leads by (rows 3 w, 4 w and 5 w: the leaf out, growth
// and fall then while that day has more leaves on, else today's).
void fill_profile(const Species& S, float elem_area, Fvector4* rows);
void fill_season(const Species& S, float day, Fvector4* rows, float lead = 0.f);

struct Community
{
    bool used{};
    shared_str name;
    u32 count{};
    u8 species[16]{};
    float share[16]{};
};
// "species = id:share, ..." against the population's species (by_name: an index or -1).
void parse_community(const CInifile::Sect& sect, Community& c, u32 max_species, s32 (*by_name)(pcstr name));

// The INI helpers of the data files.
pcstr skip_space(pcstr s);
void read_float(const CInifile::Sect& s, pcstr key, float& out);
void read_floats(const CInifile::Sect& s, pcstr key, float* out, u32 n);

// ======================================================================================================
// Templates and instances
// ======================================================================================================
struct Template
{
    u32 species{}, height_class{}, variant{};
    float height{}, width{};
    u32 axis_first{}, axis_count{};
    u32 first[kKinds]{}, count[kKinds]{};
    u32 wood_level[4]{}; // wood segments of level <= 0 .. 3 (the shrubs' wood by level, the trees' by group: WoodCut)
    Fvector centre{};    // the bounding sphere (template space)
    float radius{};
    float leaf_len{}, cluster_r{}, blob_r{}; // mean sizes, for the pixel floor
    // Trees: the stock tubes' levels of detail inside the stock range (the rings every 6, 12 and 25 degrees of bend),
    // the template's first slot in the aux buffer (its rings and patch vertices), the top of its climbable wood
    // (template space), the stock kind and tilt bucket it was built for.
    u32 stock_lod_first[3]{}, stock_lod_count[3]{};
    u32 aux_first{};
    float climb_top{};
    float leaf_area{};
    u64 kind{};
    u32 bucket{};
    // Trees: the template's box (template space), its stock wood's thickest ring, the stock triangles as the stock has
    // them after the patch range's tessellated ones (the far levels of detail').
    Fvector lo{}, hi{};
    float stock_r{};
    u32 patch_far{};
    WoodCut cut{};
};

// The prefix of a set of n elements (their keys falling, rq its table) whose keys reach k_min: every one of them, a few
// past them at the most.
inline u32 key_prefix(const float (&rq)[kRq], u32 n, float k_min)
{
    if (!n || rq[kRq - 1] >= k_min)
        return n;
    u32 i = 0;
    while (i + 1 < kRq && rq[i] >= k_min)
        ++i;
    return std::min(n, (i * n + kRq - 2) / (kRq - 1));
}

// The key of element x (fractional) of a set of n elements by its table, between its 32nds.
inline float key_at(const float (&rq)[kRq], u32 n, float x)
{
    if (!n)
        return 0.f;
    const float q = std::clamp(x * float(kRq - 1) / float(n), 0.f, float(kRq - 1));
    const u32 i = std::min(u32(q), kRq - 2);
    return rq[i] + (rq[i + 1] - rq[i]) * (q - float(i));
}

// A wood set's soft end. A crown's wood is cut at multiples of the pixel floor (its leaves cover the thin limbs), so its
// prefix ended far over the shaders' wire rule and whole limbs a pixel wide blinked in and out at its end, a 32nd of
// them at once (the table's step). The elements whose key passes the cut by kWoodBand stand whole; the ones down to the
// cut shrink away by their key (read off the table), in runs of their own: out(first, count, keep), keep the run's
// shrink (1 / f^2).
constexpr float kWoodBand = 0.5f;
template <class Out>
void wood_runs(const float (&rq)[kRq], u32 n, float k_min, Out&& out)
{
    const u32 n_end = key_prefix(rq, n, k_min);
    // (QA r__veg_soft without bit 2: the hard end.)
    const u32 n_full = k_min > 0.f && (ps_r__veg_soft & 2) ? std::min(key_prefix(rq, n, k_min * (1.f + kWoodBand)), n_end) : n_end;
    if (n_full)
        out(0u, n_full, 1.f);
    if (n_end <= n_full)
        return;
    // (Eight runs over the band at the least, a chunk each at the most.)
    const u32 run = std::clamp((n_end - n_full + 7u) / 8u, 1u, kChunk);
    for (u32 a = n_full; a < n_end; a += run)
    {
        const u32 c = std::min(run, n_end - a);
        const float f = std::clamp((key_at(rq, n, float(a) + 0.5f * float(c)) - k_min) / (kWoodBand * k_min), 0.f, 1.f);
        if (f > 0.02f)
            out(a, c, 1.f / (f * f));
    }
}

struct Instance
{
    u32 tmpl{};
    u32 species{}; // the tables' row (a tree's is kShrubSpecies + its own)
    float scale_h{}, scale_w{};
    float hemi{1.f}, sun{1.f};
    u16 seed{};
    s8 pheno{};
    bool dead{};
    Fsphere bound{};
    Fbox box{};
    float row[3][4]{}; // template -> world
    // Trees: the element sizes' scale (the cube root of the transform's determinant), the stock parts' wind root, the
    // tree's height above its root (the stock trunk profile's), the top of its climbable wood above the root (world).
    float scale{1.f};
    const FTreeVisual* wind{};
    float tree_h{}, climb_top{};
    u32 placement{};
    bool red{};
    // A tree's youth (1 a sapling .. 0 a grown tree: marcescence, the juvenile phase without flowers and fruit) and its
    // spring lead (a share of the policy's days, 8 bits).
    float youth{};
    u8 lead{};
    // A tree's ground at its trunk over its root (m): the bark's zones count their heights from it - a tree on a slope
    // or sunk into it by the level stands its root a metre and more under its foot.
    float ground{};
};

struct Cell
{
    Fbox box;
    u32 first{}, count{};
};

// What a frame latches (the populations' tier distances are their own).
struct Latch
{
    u32 frame{u32(-1)};
    Fvector cam{}, fwd{}, sun{};
    float time{}, time_prev{};
    bool motion{};
    float px_per_m{}; // render pixels per metre at 1 m of view depth
    // The wind service's field as the shaders get it now and a frame back (da_wind_field, da_wind_field_prev) and its
    // heading (da_wind_state.xy).
    Fvector4 wind_field{}, wind_field_prev{};
    float wind_hx{}, wind_hz{1.f};
    float snow{};
    // The stock trees' frame-wide sway (FTreeVisual: the wave's phase and the wind's heading times its amplitude), now
    // and a frame back.
    Fvector4 tree_wave{}, tree_wind{}, tree_wave_prev{}, tree_wind_prev{};
    float far_plane{};
};

struct ChunkRec
{
    u32 inst, first, packed, keep;
};

// One population of the core: its templates on the GPU, its instances and their grid, its frame's chunk lists. The
// core walks the grid and calls the population for each instance a pass needs.
class Population
{
public:
    explicit Population(u32 index) : index(index) {}
    virtual ~Population() = default;

    const u32 index;
    bool loaded{}, active{}, detached{}, failed{};
    xr_vector<Template> templates;
    xr_vector<Instance> instances;
    xr_vector<Cell> cells;
    xr_vector<u32> cell_items;
    Fvector2 grid_origin{};
    u32 grid_x{}, grid_z{};
    float max_height{};
    GpuBuffer elems_b, axes_b, aux_b;
    // The frame: each instance's record (or ~0u), the chunks of every pass and draw, their place in the frame's chunk
    // buffer.
    xr_vector<u32> frame_slot;
    xr_vector<ChunkRec> pass_chunks[kPasses][drCount];
    u32 draw_first[kPasses][drCount]{}, draw_count[kPasses][drCount]{};
    u32 elements[drCount]{};
    u32 seen[4]{};
    // The view's candidates for the trees' GPU cut (da_veg.cpp cut): their chunks, routes (four words each) and elements.
    xr_vector<ChunkRec> cut_chunks;
    xr_vector<u32> cut_routes;
    u32 cut_elements{};
    u32 cascades{}; // the sun cascades it casts into this frame
    xr_vector<std::pair<float, u32>> view_order;   // the frame's instances in view, by distance
    xr_vector<std::pair<u32, u32>> cascade_order;  // the frame's instances in a cascade's reach (cascade, instance)
    // r__gpu_log: the CPU time of its frame's choice and chunk builds since the last line.
    std::atomic<u64> cpu_ticks{};
    std::atomic<u32> cpu_frames{};

    // Its name in the logs.
    virtual pcstr name() const = 0;
    // Level load (da_veg.cpp da_veg_level_load): its data, the census's models it takes, their placements and species
    // (false: nothing to do on this level, or off - with its own log line; shaders: the core's shader files ship,
    // own_shaders: its own too); then, the core's GPU objects made, its templates, instances and grid (it logs its line)
    // and the detach. release: everything of the level gone, the stock models back first.
    virtual bool prepare(bool shaders, bool own_shaders) = 0;
    virtual bool pending() const = 0;
    virtual void finish_load() = 0;
    virtual void release() = 0;
    // r__shrubs / r__trees: its stock models stand replaced (out of their hierarchies) or back (QA).
    virtual bool wanted() const = 0;
    virtual void set_detached(bool detach, bool announce) = 0;
    // The stock models it replaces, its stand-ins in the ray tracing scene.
    virtual void replaced_visuals(xr_vector<dxRender_Visual*>& out) const = 0;
    // The preset's columns (0..4), from the next frame.
    virtual void apply_preset(u32 preset) = 0;
    // Its species' rows of the tables at the day.
    virtual void fill_tables(float day, Fvector4* tables) = 0;
    // QA: draw d of pass pass left out (r__dbg_shrubs, r__dbg_trees).
    virtual bool skip_draw(u32 d, u32 pass) const = 0;
    // The G-buffer after its draws (the trees' far billboards).
    virtual void render_extra(CBackend& /*cmd*/) {}
    // CRender::Render before the bends and the G-buffer, on the immediate context with no target bound yet (the trees'
    // billboard atlas filled).
    virtual void pre_render(CBackend& /*cmd*/) {}
    // The ray tracing stand-in visual (a replaced FLOD or one of its parts): the species row and template it stands
    // for, and whether its stock cards keep their look all year (dead).
    virtual bool stand_in(const dxRender_Visual* /*visual*/, u32& /*species*/, u32& /*tmpl*/, bool& /*dead*/) const { return false; }
    // The farthest any of its passes needs an instance this frame (the view's last tier, the cascades', its
    // billboards'), and the cascades it casts into (both from its preset columns).
    virtual float reach() const = 0;
    virtual u32 frame_cascades() const = 0;
    // The frame begins (after the latch, before the walk).
    virtual void frame_begin() {}
    // Instance i at view distance d draws from its record (false: the view needs none - the trees' billboards - and
    // takes it past the frame's record cap).
    virtual bool needs_record(u32 /*i*/, float /*d*/) const { return true; }
    // Instance i in the view at distance d, in sun cascade c (texel: its texel in metres), in the rain map, in a
    // shadowed local light (texel: the light's at half its reach; the main wood and the clusters into pass 0's lists).
    virtual void view(u32 i, float d) = 0;
    virtual void cascade(u32 c, u32 i, float texel) = 0;
    virtual void rain(u32 i) = 0;
    virtual void light(u32 i, float texel) = 0;
    // The population's words of instance i's record (words 20..23: zero for the shrubs).
    virtual void record_tail(u32 /*i*/, u32* /*words*/) const {}
    // The view's draws in their order (near to far for early-Z) and the shadow maps' draws.
    virtual const xr_vector<u32>& view_draws() const = 0;
    virtual const xr_vector<u32>& shadow_draws() const = 0;
};

// The populations (null while their module is off).
Population* population(u32 index);
constexpr u32 kPopShrubs = 0, kPopTrees = 1, kPops = 2;

// ======================================================================================================
// The core's services to the populations
// ======================================================================================================
const Latch& latch();
// The record of instance i of population P this frame (written once, whichever pass asks first); ~0u when the frame's
// records are full.
u32 frame_record(Population& P, u32 i);
bool record_room(const Population& P, u32 i);
// The chunks of n elements of record rec from element `first` into draw d of pass p: the share kept (its area made up
// by the shaders), the crossfade's window [lo, hi) of the element hashes, the chunk flags (kChunkRigid); a rigid chunk's
// keep word carries `aux` in its upper half (a non-rigid one's bend slot goes there: the trees' stock wood its morphs).
void emit(Population& P, u32 p, u32 d, u32 rec, u32 first, u32 n, float keep, float lo, float hi, u32 flags = 0, float aux = 0.f);
// A thinned set's soft end. The pixel floor draws a prefix of a set whose elements go by importance (the lit outer
// ones first); with a hard end each element at the prefix's end popped in and out, grown, as the distance moved it - a
// near crown's leaves blinking by thousands as one walked. The last kThinBand of the prefix shrinks away by rank instead.
constexpr float kThinBand = 0.3f;
// The chunks of a thinned set: the share `share` of its n elements (kept()), its last kThinBand shrinking away by rank in
// groups of their own (a group's keep word carries its shrink: the shaders grow an element by 1 / sqrt(keep)), the rest
// grown by `keep` and a little more, so the set's area holds with the band's.
void emit_thinned(Population& P, u32 p, u32 d, u32 rec, u32 first, u32 n, float share, float keep, float lo, float hi, u32 flags = 0);
// The trees' GPU cut stands (its shaders and buffers made, r__trees_cut); the chunks of n elements of record rec from
// element `first` as the view's candidates for it: class cls, the draws its elements may take (d: by the class's rules),
// the share kept, the crossfade's window (the leaves' its lower end alone), the chunk flags and the class's parameters
// (leaves: the hulls' and the quads' ends, metres; wood, stock, patches: the bark's scale). Past the cut's room the
// CPU's choice stands (d[1], a patch's d[0]).
bool cut_on();
void cut(Population& P, CutClass cls, const Draw (&d)[4], u32 rec, u32 first, u32 n, float keep, float lo, float hi, u32 flags, float p0,
    float p1 = 0.f, float p2 = 0.f, float aux = 0.f);
// QA: the words each of the cut's lists takes, read back and logged a frame or two later (r__trees_stats).
void cut_readback_request();
// The share of the elements the pixel floor keeps for an element of `size` metres at `px` pixels a metre.
inline float floor_keep(float size, float px, float kpx) { return std::clamp((size * px / kpx) * (size * px / kpx), 0.03f, 1.f); }
inline u32 kept(u32 n, float keep) { return n ? std::max(u32(std::ceil(float(n) * keep)), 1u) : 0u; }
// The season tables of the species row (CPU side): its leaves are on, it blooms, fruits or carries catkins.
bool leafy(u32 species);
bool blooming(u32 species);
// A cascade's box (render_phase_sun.cpp), the rain map's reach.
float cascade_size(u32 c);
float rain_reach();
// The culling grid over the population's instances.
void build_grid(Population& P);
// The tables are refilled at the next frame (a population rebuilt its templates).
void tables_dirty();
// The frame's figures for the populations' stats; the preset; the core's GPU objects are made.
u32 frame_records();
u32 frame_chunks();
// The frame's chunks that bend (their bend slots) and the bend buffer's slots, the frame's and the view's a frame back.
u32 frame_bends();
u32 bend_capacity();
u32 bend_view_capacity();
u32 lights_cast();
u32 lights_offered();
u32 lights_near();
u32 light_chunks(u32 population);
u32 stale_passes();
u32 preset();
bool gpu_ready();
// count quads (four vertices each, SV_VertexID 4 x quad + corner across instances of 64) with the shader element and
// constants already set (the trees' billboard atlas fill).
void draw_quads(CBackend& cmd, u32 count);
// The billboards' bake (da_trees_bake.cpp): a chunk's vertices of draw d, and `chunks` chunks of it with the shader
// element, its constants and its buffers already set (the draws' own geometry); the season tables of the frame.
u32 chunk_verts(u32 d);
void draw_chunks(CBackend& cmd, u32 d, u32 chunks);
Fvector4* tables();

// ======================================================================================================
// Packing helpers (the shaders' formats)
// ======================================================================================================
float smoothstep(float a, float b, float x);
float box_distance(const Fbox& b, const Fvector& p);
u32 f16(float v);
inline u32 h2(float a, float b) { return f16(a) | (f16(b) << 16); }
inline u32 u8f(float v) { return u32(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }
inline u32 sn16(float v) { return u32(u16(s16(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)))); }
// A unit vector as an octahedral snorm16 pair (r3\da_shrub_common.h shrub_oct: y up), and at 12 bits a coordinate
// (shrub_oct12: 24 bits).
u32 oct(const da_shrub::V3& n);
u32 oct12(const da_shrub::V3& n);
// The same at eight bits a coordinate (16 bits: r3\da_tree_patch_vs.h tree_oct8).
u32 oct8(const da_shrub::V3& n);
// A leaf's side as its roll about the midrib from the level side across it, eight bits over a turn.
u32 leaf_roll8(const da_shrub::V3& dir, const da_shrub::V3& side);
u32 hash32(u32 x);
inline float hash01(u32 x) { return float(hash32(x) >> 8) * (1.f / 16777216.f); }
u32 pos_hash(const Fvector& p);
inline void put(xr_vector<u32>& out, const da_shrub::V3& p, u32 aw, u32 b0, u32 b1, u32 b2, u32 b3)
{
    out.insert(out.end(), {std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y), std::bit_cast<u32>(p.z), aw, b0, b1, b2, b3});
}

// ======================================================================================================
// The census of the stock models (one pass for both populations)
// ======================================================================================================
constexpr u64 kFnvOffset = 0xcbf29ce484222325ULL, kFnvPrime = 0x100000001b3ULL;
u64 fnv(const void* data, size_t n, u64 h = kFnvOffset);
u64 fnv_shader(pcstr shader, u64 h);

// One tree-format part of a stock model as level.geom holds it: its vertex range, its full-detail triangles, its level
// shader string; then its key, local box and area.
struct StockPart
{
    const FTreeVisual* visual{};
    u32 vb{}, vbase{}, vcount{}, ib{}, ibase{}, tris{};
    pcstr shader{};
    xr_vector<u16> indices;
    u64 key{};
    Fbox box{};
    double area{};
};
// An FLOD under a sector hierarchy whose children are all tree parts (a placed multiple-use model).
struct Candidate
{
    FHierrarhyVisual* parent{};
    dxRender_Visual* flod{};
    xr_vector<u32> parts;
    u32 taken{~0u}; // the population that replaces it
};
struct Census
{
    bool done{};
    xr_vector<Candidate> candidates;
    xr_vector<StockPart> parts;
};
// The level's census (walked and keyed once a level, on first use).
Census& census();
// Another compile of a known model: its kind's key, its local box and area (the tolerant signature).
struct FamilyRow
{
    u64 kind{};
    Fbox box{};
    double area{};
};
// The kind key of a candidate's parts (their keys sorted) if `kinds` has it, else its tolerant signature's match in
// `families` (the box within 2.5 mm, the area within 0.2 %); 0 when neither knows it.
template <class KindMap>
u64 kind_of(const Census& C, const Candidate& c, const KindMap& kinds, const std::unordered_multimap<u64, FamilyRow>& families);
// Parses "[..._families]": "<signature> = <kind key>:x0:y0:z0:x1:y1:z1:area, ..." (box in mm).
void parse_families(const CInifile::Sect& sect, std::unordered_multimap<u64, FamilyRow>& out);
// The ground under (x, z) from y_top down `span` metres: the first static triangle that is not passable, or fallback.
// The level's static ground under a point (the nearest hit down from y_top within span, past passable surfaces; with
// skip_trees past the trees' and the bushes' own collision too); fallback where nothing is hit.
float ground_at(float x, float z, float y_top, float span, float fallback, bool skip_trees = false);

// ---- implementation of kind_of ----
u64 kind_key(const Census& C, const Candidate& c);
u64 family_signature(const Census& C, const Candidate& c, Fbox& box, double& area);
template <class KindMap>
u64 kind_of(const Census& C, const Candidate& c, const KindMap& kinds, const std::unordered_multimap<u64, FamilyRow>& families)
{
    const u64 key = kind_key(C, c);
    if (kinds.find(key) != kinds.end())
        return key;
    Fbox box;
    double area = 0.0;
    const u64 sig = family_signature(C, c, box, area);
    const auto range = families.equal_range(sig);
    for (auto it = range.first; it != range.second; ++it)
    {
        const FamilyRow& f = it->second;
        bool close = std::abs(area - f.area) <= 0.002 * std::max(f.area, 1e-6);
        for (u32 i = 0; i < 3 && close; ++i)
            close = std::abs(box.vMin[i] - f.box.vMin[i]) <= 0.0025f && std::abs(box.vMax[i] - f.box.vMax[i]) <= 0.0025f;
        if (close && kinds.find(f.kind) != kinds.end())
            return f.kind;
    }
    return 0;
}

} // namespace da_veg
} // namespace xray::render::RENDER_NAMESPACE
