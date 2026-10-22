// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The upgraded level trees (docs/dead-air/TREES.md): the trees' population of the vegetation core (da_veg.cpp).
//
// Level load: the species, communities, kinds and policy of dead_air_x64_trees.ltx; every stock tree the core's census
// keyed (an FLOD of tree parts) whose kind the data names a TREE or a YOUNG tree with a readable skeleton
// (gamedata/meshes/da/trees/<skel>.dtsk) becomes a placement on its exact stock transform, its species the kind's or
// one of its community's; one template per kind, species and tilt bucket is grown on the task pool by
// generate_on_skeleton (or read back from the disk cache), packed into the trees' element, axis and aux buffers; the
// stock FLOD leaves its hierarchy whole, its parts staying alive as wind roots and ray tracing stand-ins. Per frame a tree
// in the view takes its tiers by the on-screen size of its species' leaf - hulls, quads, spray cards, clusters, blobs,
// then its own stock billboard from the module's atlas -, each sun cascade its own tier, the rain map its crown, the
// shadowed local lights near the camera its main wood and clusters; its record carries its stock root's oscillator as
// the trunk's sway.

#include "stdafx.h"
#include "da_trees.h"
#include "da_trees_bake.h"
#include "da_veg.h"
#include "da_veg_internal.h"
#include "da_shrub_gen.h"
#include "da_grass_data.h"
#include "da_temporal.h"
#include "FTreeVisual.h"
#include "FHierrarhyVisual.h"
#include "FLOD.h"
#include "xrRender_console.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCommon/xr_unordered_map.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

// The wind field's maths, shared with the vertex shaders (one file, two compilers).
#include "../../../packaging/dead-air-x64/compatibility/gamedata/shaders/r3/da_wind_core.h"

#include <bit>
#include <cstdio>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using namespace da_veg;

constexpr u32 kCommunities = 24;
// The generator's version in the disk cache's names: a template another one grew is grown again.
constexpr u32 kGenVer = 36;
// The trees' leaf elements over a level's templates at most (with their wood and far tiers some 400 MB of buffers),
// shared out among them.
constexpr double kLeafBudget = 9.0e6;
// The billboards' atlas: its width and its cell (px), the cell halved when the level's templates do not fit 4096 lines.
constexpr u32 kBbWidth = 4096, kBbCell = 128;
// The atlas pair's names (the facet shader's s_base and s_hemi) and the bake's foliage atlas's (its s_tree_mtl).
constexpr pcstr kBbAlbedo = "$user$da_tree_bb", kBbNormal = "$user$da_tree_bb_nm", kBbMaterial = "$user$da_tree_bb_mtl";

// The stock classes of [tree_kinds].
enum TreeClass : u8
{
    tcNone,
    tcTree,  // a stock tree on its skeleton
    tcYoung, // an oversize bush model grown as a young tree
    tcStock, // stays stock
    tcCount
};
constexpr pcstr kClassNames[tcCount] = {"NONE", "TREE", "YOUNG", "STOCK"};

TreeClass class_by_name(pcstr s)
{
    s = skip_space(s);
    for (u32 c = tcCount - 1; c > tcNone; --c)
    {
        const size_t n = xr_strlen(kClassNames[c]);
        if (!_strnicmp(s, kClassNames[c], n) && !isalnum(u8(s[n])) && s[n] != '_')
            return TreeClass(c);
    }
    return tcNone;
}

u32 class_bits(pcstr v)
{
    u32 bits = 0;
    string64 item;
    for (u32 i = 0, n = _GetItemCount(v); i < n; ++i)
        if (const TreeClass c = class_by_name(_GetItem(v, i, item)); c != tcNone)
            bits |= 1u << c;
    return bits;
}

// A stock model the data names: its class, its look (dead, the Red Forest's red, a conifer, a sapling), its species or
// community, its skeleton.
struct Kind
{
    TreeClass cls{tcNone};
    bool dead{}, red{}, conifer{}, sapling{};
    s32 species{-1}, community{-1};
    shared_str skel;
};

struct Data
{
    bool file{};
    Species species[kTreeSpecies];
    u64 species_hash[kTreeSpecies]{}; // its section's text (the disk cache's key)
    Community communities[kCommunities];
    xr_unordered_map<u64, Kind> kinds;
    std::unordered_multimap<u64, FamilyRow> families;
    // [tree_policy]: the tilt over which the grafted growth takes its tilt bucket's vertical, the smallest scale, the
    // species templates of one kind on a level at most; a tree's youth by its height (young_h0: a sapling up to it,
    // young_h1: grown from it), the days a sapling's spring leads the old trees' by.
    float max_tilt_graft{15.f}, min_scale{0.05f}, young_lead{8.f};
    float young_h[2]{4.f, 10.f};
    u32 kinds_per_level_species{3};
    s8 red_comm{-1};
    s8 grass_comm[16]{};
    s8 class_comm[tcCount]{};
};
Data g_data;

s32 species_by_name(pcstr name)
{
    for (u32 i = 0; i < kTreeSpecies; ++i)
        if (g_data.species[i].used && !xr_stricmp(g_data.species[i].name.c_str(), name))
            return s32(i);
    return -1;
}

s32 community_by_name(pcstr name)
{
    for (u32 i = 0; i < kCommunities; ++i)
        if (g_data.communities[i].used && !xr_stricmp(g_data.communities[i].name.c_str(), name))
            return s32(i);
    return -1;
}

bool conifer_species(const Species& S) { return S.evergreen > 0.5f || S.arch.leaf_kind == da_shrub::lkNeedles; }

void data_reload()
{
    g_data.~Data();
    new (&g_data) Data();
    for (s8& c : g_data.grass_comm)
        c = -1;
    for (s8& c : g_data.class_comm)
        c = -1;
    if (!FS.exist("$game_config$", "dead_air_x64_trees.ltx"))
        return;
    string_path fn;
    FS.update_path(fn, "$game_config$", "dead_air_x64_trees.ltx");
    CInifile ini(fn, TRUE);
    g_data.file = true;
    for (u32 i = 0; i < kTreeSpecies; ++i)
    {
        string64 name;
        xr_sprintf(name, "tree_species_%u", i + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        parse_species(s, g_data.species[i], &class_bits);
        pcstr id = da_ini_value(s, "name");
        g_data.species[i].name = id ? skip_space(id) : name;
        u64 h = kFnvOffset;
        for (const CInifile::Item& item : s.Data)
        {
            if (item.first.c_str())
                h = fnv(item.first.c_str(), xr_strlen(item.first.c_str()) + 1, h);
            if (item.second.c_str())
                h = fnv(item.second.c_str(), xr_strlen(item.second.c_str()) + 1, h);
        }
        g_data.species_hash[i] = h;
    }
    for (u32 i = 0; i < kCommunities; ++i)
    {
        string64 name;
        xr_sprintf(name, "tree_community_%u", i + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        Community& c = g_data.communities[i];
        pcstr id = da_ini_value(s, "name");
        c.name = id ? skip_space(id) : name;
        parse_community(s, c, 16, &species_by_name);
    }
    // The optional plant communities of a spot: by the grass community under the root, by the class.
    if (ini.section_exist("tree_grass_community"))
    {
        const DaSeasonData& grass = da_season_data();
        for (const CInifile::Item& item : ini.r_section("tree_grass_community").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const s32 tc = community_by_name(skip_space(item.second.c_str()));
            for (u32 gc = 0; gc < da_grass_communities && tc >= 0; ++gc)
                if (grass.communities[gc].used && !xr_stricmp(da_grass_community_name(gc), item.first.c_str()))
                    g_data.grass_comm[gc] = s8(tc);
        }
    }
    if (ini.section_exist("tree_class_community"))
        for (const CInifile::Item& item : ini.r_section("tree_class_community").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const TreeClass cls = class_by_name(item.first.c_str());
            const s32 tc = community_by_name(skip_space(item.second.c_str()));
            if (cls != tcNone && tc >= 0)
                g_data.class_comm[cls] = s8(tc);
        }
    // (The Red Forest's community is the data's: the policy's red_community, none without it.)
    g_data.red_comm = -1;
    if (ini.section_exist("tree_policy"))
    {
        const CInifile::Sect& s = ini.r_section("tree_policy");
        read_float(s, "max_tilt_graft", g_data.max_tilt_graft);
        read_float(s, "min_scale", g_data.min_scale);
        read_float(s, "young_lead", g_data.young_lead);
        g_data.young_lead = std::clamp(g_data.young_lead, 0.f, 60.f);
        read_float(s, "young_h0", g_data.young_h[0]);
        read_float(s, "young_h1", g_data.young_h[1]);
        g_data.young_h[1] = std::max(g_data.young_h[1], g_data.young_h[0] + 0.5f);
        float per = float(g_data.kinds_per_level_species);
        read_float(s, "kinds_per_level_species", per);
        g_data.kinds_per_level_species = u32(std::clamp(per, 1.f, 8.f));
        if (pcstr v = da_ini_value(s, "red_community"))
            g_data.red_comm = s8(community_by_name(skip_space(v)));
    }
    // The stock models: "<kind key> = CLASS[, dead][, red][, conifer][, sapling][, species=<id>|community=<name>][, skel=<file>]".
    if (ini.section_exist("tree_kinds"))
        for (const CInifile::Item& item : ini.r_section("tree_kinds").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            Kind k;
            string256 field;
            pcstr v = item.second.c_str();
            const u32 n = _GetItemCount(v);
            k.cls = class_by_name(_GetItem(v, 0, field));
            if (k.cls == tcNone)
                continue;
            for (u32 i = 1; i < n; ++i)
            {
                _GetItem(v, i, field);
                pcstr f = skip_space(field);
                if (!xr_stricmp(f, "dead"))
                    k.dead = true;
                else if (!xr_stricmp(f, "red"))
                    k.red = true;
                else if (!xr_stricmp(f, "conifer"))
                    k.conifer = true;
                else if (!xr_stricmp(f, "sapling"))
                    k.sapling = true;
                else if (!_strnicmp(f, "species=", 8))
                    k.species = species_by_name(skip_space(f + 8));
                else if (!_strnicmp(f, "community=", 10))
                    k.community = community_by_name(skip_space(f + 10));
                else if (!_strnicmp(f, "skel=", 5))
                    k.skel = skip_space(f + 5);
            }
            g_data.kinds[u64(strtoull(item.first.c_str(), nullptr, 16))] = k;
        }
    if (ini.section_exist("tree_families"))
        parse_families(ini.r_section("tree_families"), g_data.families);
    u32 species = 0, communities = 0;
    for (const Species& s : g_data.species)
        species += s.used ? 1 : 0;
    for (const Community& c : g_data.communities)
        communities += c.used ? 1 : 0;
    Msg("* [trees] data: %u species in %u communities, %u stock models (%u other compiles)", species, communities,
        u32(g_data.kinds.size()), u32(g_data.families.size()));
}

// A kind's skeleton, read once a level.
struct Skel
{
    bool tried{}, ok{};
    da_shrub::Skeleton S;
    u64 hash{};
};

// The columns of preset 0..4 (minimum..extreme): where each foliage tier ends by its species' leaf on the screen (the
// leaf hulls from 16-40 px, the quads, then the quads thinned to the leaf floor - their survivors grown to it - on to
// 1.5-4 px, the clusters to 0.6-2, the blobs under it), the billboard's height on the screen (render pixels: a tree of
// 20 m becomes its billboard at that height - the atlas's cells are 128 px), the pixel floor (the hulls', the clusters'
// and blobs' by it), the leaves' floor past the quads (px), the near cascade's leaves' floor (texels), the sun cascades.
// The crowns' fine detail (`crown`): the leaves' floor past the quads and a leafy crown's wood floors (its limbs, its
// branches, its twigs over the wire rule) divided by it, never under the pixel floor and the wire rule; from 2 a leafy
// crown keeps its twigs and shoots a tier longer as a bare one does. Its price on the QA rig at Extreme: 0.1-0.35 ms of
// the GPU (the thinning saved next to nothing, and drew a near crown's leaves big and few, its twigs gone - leaves
// floating round bare limbs, popping in and out as one walked).
struct PresetColumns
{
    float hull_px, quad_px, leaf_px, clu_px;
    float bb_px;
    float kpx, leaf_kpx, shadow_kpx;
    u32 cascades;
    float crown;
};
// The thinned leaves and the clusters end at half the leaf pixels on Extreme (a third less on High, a fifth on Default)
// of what they did: a poplar's crown stood in clusters - a grainy dot pattern - from 30 m on; 0.15-0.45 ms.
constexpr PresetColumns kPresets[5] = {{40.f, 14.f, 4.f, 2.f, 110.f, 3.f, 8.f, 7.f, 2, 1.f},
    {32.f, 11.f, 3.f, 1.5f, 100.f, 2.5f, 6.6f, 8.f, 3, 1.5f}, {24.f, 8.f, 2.f, 0.8f, 90.f, 2.f, 5.5f, 9.f, 3, 2.f},
    {20.f, 7.f, 1.33f, 0.53f, 80.f, 1.75f, 4.4f, 10.5f, 3, 3.f}, {16.f, 6.f, 0.75f, 0.3f, 72.f, 1.5f, 3.4f, 12.f, 3, 4.f}};
// The billboard's reference tree (m).
constexpr float kBbTreeHeight = 20.f;
// The stock wood's morphs. A ring level's switch moved a trunk's silhouette at once (level 1 drops the spline's rings
// between bends of 12 degrees: a bent pine's trunk turned from a curve into kinks at eight metres), so over the last
// kRingBand of its range a level's dropped rings move onto the coarser level's tube. The patches' tessellation lifts
// its points by up to 4 cm (the stock's facets rounded): it eases in from kTessFar to kTessNear of the trunk's lower
// part, the stock's own facets at kTessFar (an angular trunk turned smooth at once at 2.5 m).
constexpr float kRingBand = 0.3f, kTessNear = 1.75f, kTessFar = 3.5f;

// A stock tree's placement: its FLOD, its exact transform, its wind root, its light, its kind, skeleton and tilt bucket,
// its species.
struct Placement
{
    u32 candidate{};
    FHierrarhyVisual* parent{};
    dxRender_Visual* flod{};
    u64 kind{};
    const Kind* k{};
    Skel* skel{};
    const FTreeVisual* wind{};
    Fmatrix xf{};
    float hemi{1.f}, sun{1.f};
    float top{};
    u32 bucket{};
    da_shrub::V3 up{0.f, 1.f, 0.f};
    s32 species{-1};
    bool dead{}, red{};
    float youth{};
    u32 tmpl{~0u};
};

// A view of a template's billboard: its facet's rect in the module's atlas (its cell inset by the gutter), the stock cell
// its facet had in the level's atlas (uv rects u0 v0 u1 v1), the facet's quad in the template's model space (the stock
// vertex order), each corner's uv in the module's atlas, and its normal.
struct BbView
{
    Fvector4 rect{}, stock{};
    Fvector corner[4]{};
    Fvector2 uv[4]{};
    Fvector normal{};
};
// A template's billboard: its eight views, the stock atlas its cells were first filled from, whether they are to be
// filled again (the bake, da_trees_bake.cpp); the look the bake gives it (its living trees' mean youth, every one dead,
// the Red Forest's needles, their mean hemisphere light over their facets' vertex light). A template some of whose trees
// stand dead among living ones has a second cell set for them in the dead look (bare, the dead bark, the snow; the Red
// Forest's red pines keep their needles: not dead): its index among the atlas's sets (past the templates'; ~0u none, or
// no room), its views and flags, its dead trees' mean youth and light.
struct BbCell
{
    BbView view[8];
    ref_texture stock_d, stock_nm;
    u32 placement{};
    bool filled{}, dirty{true};
    float youth{}, hemi{};
    bool dead_look{}, red{};
    u32 dead_set{~0u};
    BbView dead_view[8];
    bool dead_filled{}, dead_dirty{true};
    float dead_youth{}, dead_hemi{};
};

// The 3x3 of a transform: its determinant, a world direction in its model's space.
float det3(const Fmatrix& m)
{
    Fvector c;
    c.crossproduct(m.j, m.k);
    return m.i.dotproduct(c);
}

Fvector model_dir(const Fmatrix& m, const Fvector& w)
{
    Fvector a, b, c;
    a.crossproduct(m.j, m.k);
    b.crossproduct(m.k, m.i);
    c.crossproduct(m.i, m.j);
    const float d = det3(m);
    Fvector r{a.dotproduct(w), b.dotproduct(w), c.dotproduct(w)};
    r.div(std::abs(d) > 1e-12f ? d : 1.f);
    return r;
}

Fvector model_point(const Fmatrix& m, const Fvector& p)
{
    Fvector d;
    d.sub(p, m.c);
    return model_dir(m, d);
}

// The stock tree's trunk sway on its root's oscillator (da_tree_bend.h da_tree_bend_ex with the shared root sample):
// the downwind push per metre of height (times the height's share of the tree's in the shader). s: the root's state,
// frequency factor, gust and deviation; wave, wind: FTreeVisual's frame-wide sway; field: the wind service's.
Fvector2 trunk_push(const Fvector4& s, const Fvector4& wave, const Fvector4& wind, const Fvector4& field, const Fvector& root, float gate)
{
    const float amp = da_wind_field_amp(s.z), lean = da_wind_field_lean(s.z, field.w);
    const float ang = da_wind_dev_angle(s.w, field.z);
    const float sa = std::sin(ang), ca = std::cos(ang);
    const float wx = wind.x * ca - wind.z * sa, wz = wind.x * sa + wind.z * ca;
    const float resp = s.x > 0.001f ? s.x : amp;
    const float mean = 0.45f + 0.35f * DA_SAT(field.z);
    const float dp = mean + (1.f - mean) * da_sway(wave.w * s.y + root.x * wave.x + root.y * wave.y + root.z * wave.z);
    const float m = (dp * resp + lean * 0.5f) * gate;
    return {wx * m, wz * m};
}

// ======================================================================================================
// Templates
// ======================================================================================================
// One generated tree into the trees' element, axis and aux buffers (the wide layout, r3\da_shrub_common.h): the axes
// (the stock ones fixed); the stock rings (two aux slots each) and the patch vertices (one); the grafted wood ordered
// by level; the stock tubes by their level of detail; the patches; the leaves and the spray cards by importance; the
// blooms from the top; the clusters by importance; the blobs by area. The element words reference the aux slots from
// the template's first (the record's aux0), the axes from its first (axis0): the arrays are position-free (the disk
// cache keeps them as they are).
void pack_tree(const da_shrub::Template& T, Template& out, xr_vector<u32>& elems, xr_vector<u32>& axes, xr_vector<u32>& aux, u32 salt)
{
    using namespace da_shrub;
    for (u32 i = 0; i < T.axes.size(); ++i)
    {
        const Axis& a = T.axes[i];
        const u32 parent = a.parent < 0 ? 0xFFFFFFu : u32(a.parent);
        const u32 level = std::min(a.level, 63u) | (a.fixed ? kAxisFixed : 0u);
        put(axes, a.base, parent | (level << 24), oct(a.dir), h2(a.length, a.attach), h2(a.flex, a.freq),
            f16(a.r_base) | ((hash32(i * 2654435761u + salt) >> 16) << 16));
    }
    // Aux (r3\da_shrub_wood_vs.h, da_tree_stock_vs.h, da_tree_patch_vs.h), from the template's first slot: a header (where
    // the parts start: the patch vertices and their count, their sidecar, the rings; the junction table and its length,
    // the junction records and their count), the grafted axes' junction table (u16 an axis: its record + 1, 0 none), the
    // junction records, the stock rings (two slots each), the patch vertices (one), the caps' copies (one), the patch
    // vertices' sidecar (a word each) and second sidecar (two words each).
    const auto aux_put = [&aux](std::initializer_list<u32> words) { aux.insert(aux.end(), words); };
    const size_t aux_at = aux.size();
    const auto aux_slot = [&aux, aux_at] { return u32((aux.size() - aux_at) / 8); };
    // The sky a stock segment sees (the generator's per segment): by its first ring.
    xr_vector<float> ring_vis(T.rings.size(), 0.6f);
    for (const Segment& sg : T.segments)
        if (T.axes[sg.axis].fixed)
            for (u32 k = T.axes[sg.axis].first_ring; k < T.axes[sg.axis].first_ring + T.axes[sg.axis].n_rings; ++k)
                if (std::abs(T.rings[k].s - sg.s0) < 1e-4f)
                {
                    ring_vis[k] = sg.vis;
                    break;
                }
    // An axis's frame at an arclength as its tube draws it - a stock axis's by its rings (the centre, the tangent, the
    // profile's reference normal: the turn's zero, the mean radius), a grafted one's by its segment there (the wood's
    // reference: its base direction across the segment) -, its sky, its level byte (the wood's outputs' level: a stock
    // axis's 0, 0x80 on a dead stretch) and the segment there (its first and last ring's radii and arclengths: the tiles
    // round it step between them; a stock axis's first ring's index, ~0u a grafted axis).
    const auto frame_at = [&](const Axis& ax, float s, V3& c, V3& t, V3& n, float& r, float& vis, u32& lvl, float (&seg)[4], u32& ring) {
        if (ax.fixed)
        {
            if (!ax.n_rings || ax.first_ring + ax.n_rings > T.rings.size())
                return false;
            u32 k = ax.first_ring;
            const u32 last = ax.first_ring + ax.n_rings - 1;
            while (k < last && T.rings[k + 1].s <= s)
                ++k;
            const Ring& A = T.rings[k];
            const Ring& B = T.rings[std::min(k + 1, last)];
            const float u = B.s > A.s ? std::clamp((s - A.s) / (B.s - A.s), 0.f, 1.f) : 0.f;
            c = A.c + (B.c - A.c) * u;
            t = normalize(A.t + (B.t - A.t) * u, A.t);
            n = A.n + (B.n - A.n) * u;
            n = normalize(n - t * dot(n, t), normalize(cross(t, std::abs(t.y) < 0.9f ? v3(0.f, 1.f, 0.f) : v3(1.f, 0.f, 0.f))));
            r = A.r0 + (B.r0 - A.r0) * u;
            vis = ring_vis[k];
            lvl = s >= ax.dead_from ? 0x80u : 0u;
            seg[0] = A.r0;
            seg[1] = B.r0;
            seg[2] = A.s;
            seg[3] = B.s;
            ring = k;
            return true;
        }
        if (!ax.segs || ax.first_seg + ax.segs > T.segments.size())
            return false;
        u32 g = ax.first_seg;
        while (g + 1 < ax.first_seg + ax.segs && T.segments[g + 1].s0 <= s)
            ++g;
        const Segment& S = T.segments[g];
        const float len = length(S.b - S.a);
        const float u = len > 1e-6f ? std::clamp((s - S.s0) / len, 0.f, 1.f) : 0.f;
        c = S.a + (S.b - S.a) * u;
        t = normalize(S.b - S.a, ax.dir);
        const V3 ref = std::abs(ax.dir.y) < 0.9f ? cross(ax.dir, v3(0.f, 1.f, 0.f)) : cross(ax.dir, v3(1.f, 0.f, 0.f));
        n = normalize(ref - t * dot(ref, t), normalize(cross(t, v3(1.f, 0.f, 0.f))));
        r = S.ra + (S.rb - S.ra) * u;
        vis = S.vis;
        lvl = std::min(ax.level, 3u) | (ax.dead ? 0x80u : 0u) | (ax.leafless && !ax.dead ? 0x40u : 0u);
        seg[0] = S.ra;
        seg[1] = S.rb;
        seg[2] = S.s0;
        seg[3] = S.s0 + len;
        ring = ~0u;
        return true;
    };
    // The junctions: where a stock limb or a thick grafted branch (8 mm and more, the first two levels) leaves its parent,
    // the parent's frame at the attach - its axis's point and tangent, the child's way out across it (the zero of the
    // child's turn round the parent: the child's own reach stays within half a turn of it), the parent's mean radius (its
    // bark's tiles round it) and its profile's toward the child (its surface there: the blend starts on it), its
    // arclength there, the turn of the way out in the parent's own bark, its sky and level byte; then the parent's segment
    // there (its rings' radii and arclengths from the attach: the parent's tiles round it step along it as its tube's
    // do). The blend's length is the shaders' (80 % of the child's radius, 1.2 .. 25 cm). Near the parent the child's tube
    // wears the parent's bark (the pixel shader: the child's place against the junction's frame, its turn round the
    // parent and its weight per pixel), light and zone: no seam where the two meet. A stock limb whose foot the patches
    // wrap (more than six sixteenths of a ring within reach) has its junction in the patch's border; a child leaving
    // its parent where a patch covers the parent toward it (its sixteenth or the next ones) has none - the patch shows
    // its own bark round it. Two slots a record.
    struct Junction
    {
        V3 c, t, d0;
        float r{}, r_out{}, s{}, reach{};
        float seg[4]{};
        u32 tail{};
        bool stock{};
    };
    xr_vector<Junction> joins;
    xr_vector<u32> join_of(T.axes.size(), ~0u);
    for (u32 i = 0; i < T.axes.size() && joins.size() < 0xFFFFu; ++i)
    {
        const Axis& a = T.axes[i];
        if (a.parent < 0 || u32(a.parent) >= T.axes.size() || u32(a.parent) == i || a.r_base < 0.008f || (!a.fixed && a.level > 2))
            continue;
        V3 c, t, n;
        float r = 0.f, vis = 0.6f, seg[4]{};
        u32 lvl = 0, ring = ~0u;
        if (!frame_at(T.axes[a.parent], a.attach, c, t, n, r, vis, lvl, seg, ring) || r < 0.015f || length(a.base - c) > r + 2.f * a.r_base + 0.05f)
            continue;
        Junction J;
        std::copy(std::begin(seg), std::end(seg), J.seg);
        J.stock = ring != ~0u;
        J.reach = 2.f * (r + std::clamp(0.8f * a.r_base, 0.012f, 0.25f)) + 0.05f;
        bool wrapped = false;
        if (a.fixed)
            for (u32 k = a.first_ring; k < a.first_ring + a.n_rings && k < T.rings.size() && !wrapped; ++k)
                if (length(T.rings[k].c - c) - T.rings[k].r0 < J.reach)
                    wrapped = (T.rings[k].cover & da_shrub::kRingHidden) != 0u || std::popcount(T.rings[k].cover & 0xFFFFu) > 6;
        V3 d0 = a.dir - t * dot(a.dir, t);
        if (dot(d0, d0) < 1e-6f)
            d0 = (a.base - c) - t * dot(a.base - c, t);
        J.c = c;
        J.t = t;
        J.d0 = normalize(d0, n);
        J.r = J.r_out = r;
        J.s = a.attach;
        const float th = std::atan2(dot(J.d0, cross(t, n)), dot(J.d0, n)), turn = th * (1.f / 6.2831853f);
        // (The parent's patches toward the child: its sixteenth there and the next ones, on the rings either side.)
        if (ring != ~0u)
        {
            const u32 j = u32(std::lround((turn - std::floor(turn)) * 16.f)) & 15u;
            const u32 last = T.axes[a.parent].first_ring + T.axes[a.parent].n_rings - 1;
            for (const u32 k : {ring, std::min(ring + 1, last)})
                for (const u32 q : {(j + 15u) & 15u, j, (j + 1u) & 15u})
                    wrapped |= (T.rings[k].cover & da_shrub::kRingHidden) != 0u || ((T.rings[k].cover >> q) & 1u) != 0u;
        }
        if (wrapped)
            continue;
        // (A stock parent's profile toward the child: its rings' harmonics there.)
        if (const Axis& pa = T.axes[a.parent]; pa.fixed)
        {
            u32 k = pa.first_ring;
            const u32 last = pa.first_ring + pa.n_rings - 1;
            while (k < last && T.rings[k + 1].s <= a.attach)
                ++k;
            const Ring& A = T.rings[k];
            const Ring& B = T.rings[std::min(k + 1, last)];
            const float u = B.s > A.s ? std::clamp((a.attach - A.s) / (B.s - A.s), 0.f, 1.f) : 0.f;
            float f[6];
            for (u32 h = 0; h < 6; ++h)
                f[h] = A.f[h] + (B.f[h] - A.f[h]) * u;
            J.r_out = r * std::max(1.f + f[0] * std::cos(th) + f[1] * std::sin(th) + f[2] * std::cos(2.f * th) + f[3] * std::sin(2.f * th) +
                f[4] * std::cos(3.f * th) + f[5] * std::sin(3.f * th), 0.5f);
        }
        J.tail = (u32(std::lround((turn - std::floor(turn)) * 65536.f)) & 0xFFFFu) | (u8f(vis) << 16) | (lvl << 24);
        join_of[i] = u32(joins.size());
        joins.push_back(J);
    }
    // (A stretch of the child within the junction's reach of its point carries it; the shaders fade it out by then.)
    const auto join_near = [&](u32 axis, V3 at, float r) {
        if (axis >= join_of.size() || join_of[axis] == ~0u)
            return false;
        const Junction& J = joins[join_of[axis]];
        return length(at - J.c) - r < J.reach;
    };
    u32 jtab_len = 0;
    for (u32 i = 0; i < T.axes.size(); ++i)
        if (join_of[i] != ~0u && !T.axes[i].fixed)
            jtab_len = i + 1;
    const u32 nv = u32(T.patch_verts.size());
    // The header (filled below), the junction table and records.
    aux.resize(aux.size() + 8, 0u);
    const u32 jtab_slot0 = aux_slot();
    for (u32 i = 0; i < jtab_len; i += 2)
    {
        const auto entry = [&](u32 k) { return k < jtab_len && join_of[k] != ~0u && !T.axes[k].fixed ? join_of[k] + 1u : 0u; };
        aux.push_back(entry(i) | (entry(i + 1) << 16));
    }
    aux.resize(aux_at + (aux.size() - aux_at + 7) / 8 * 8, 0u);
    const u32 jrec_slot0 = aux_slot();
    // (A stock parent's segment's radii in full in words 11 and 12 too: its tube counts its tiles round the full float
    // radii, and the halves rounded a count apart at about a half in one case of seven hundred.)
    for (const Junction& J : joins)
        aux_put({std::bit_cast<u32>(J.c.x), std::bit_cast<u32>(J.c.y), std::bit_cast<u32>(J.c.z), oct(J.t), oct(J.d0), h2(J.r, J.r_out),
            std::bit_cast<u32>(J.s), J.tail, h2(J.seg[0], J.seg[1]), std::bit_cast<u32>(J.seg[2] - J.s), std::bit_cast<u32>(J.seg[3] - J.s),
            J.stock ? std::bit_cast<u32>(J.seg[0]) : 0u, J.stock ? std::bit_cast<u32>(J.seg[1]) : 0u, 0u, 0u, 0u});
    // The rings' morph table (the junction records' end, aux header words 6 and 7, tells where it starts): a word a ring -
    // how many rings back and on its axis's rings kept by level 1 (bits 0..7, 8..15) and by level 2 (16..23, 24..31)
    // stand round it, 0 back where it is kept itself: a level's tube eases its dropped rings onto the next level's tube
    // between those two before it switches (r3\da_tree_stock_vs.h).
    xr_vector<u32> ring_morph(T.rings.size(), 0u);
    for (const Axis& ax : T.axes)
    {
        if (!ax.fixed || !ax.n_rings)
            continue;
        const u32 r0 = ax.first_ring, r1 = ax.first_ring + ax.n_rings;
        for (u32 k = r0; k < r1 && k < ring_morph.size(); ++k)
        {
            u32 word = 0;
            for (u32 l = 1; l < 3; ++l)
            {
                if (T.rings[k].lod & (1u << l))
                    continue;
                u32 a = k, b = k;
                while (a > r0 && !(T.rings[a].lod & (1u << l)))
                    --a;
                while (b + 1 < r1 && !(T.rings[b].lod & (1u << l)))
                    ++b;
                // (A dropped ring always has kept ones round it - the axis's ends are kept -, within 255 for the code.)
                if ((T.rings[a].lod & (1u << l)) && (T.rings[b].lod & (1u << l)) && k - a <= 255u && b - k <= 255u)
                    word |= ((k - a) | ((b - k) << 8)) << (16u * (l - 1u));
            }
            ring_morph[k] = word;
        }
    }
    aux.insert(aux.end(), ring_morph.begin(), ring_morph.end());
    aux.resize(aux_at + (aux.size() - aux_at + 7) / 8 * 8, 0u);
    // The rings, then the patch vertices.
    const u32 ring_slot0 = aux_slot();
    for (const Ring& r : T.rings)
    {
        aux_put({std::bit_cast<u32>(r.c.x), std::bit_cast<u32>(r.c.y), std::bit_cast<u32>(r.c.z), std::bit_cast<u32>(r.s), oct(r.t),
            oct(r.n), std::bit_cast<u32>(r.r0), u8f(r.climbable) | ((r.lod & 0xFFu) << 8) | (std::min(r.stock_sides, 0xFFFFu) << 16),
            std::bit_cast<u32>(r.f[0]), std::bit_cast<u32>(r.f[1]), std::bit_cast<u32>(r.f[2]), std::bit_cast<u32>(r.f[3]),
            std::bit_cast<u32>(r.f[4]), std::bit_cast<u32>(r.f[5]), u32(r.back), u32(r.back >> 32)});
        out.stock_r = std::max(out.stock_r, r.r0);
    }
    const u32 patch_slot0 = aux_slot();
    // The patch vertices: the first axis's arclength, the second's turn (0..1) and the first's run (da_shrub_gen.cpp
    // tree_patch_pairs: the bark by two tubes). A run: the stretch of the axis the place's arclength falls in - the
    // template's ring before it (16 bits, 0xFFFF none) and its share of the way to the next (16 bits) -, so the patch
    // takes the very counts of tiles its tube takes round those rings and steps them where the tube does.
    const auto turn_of = [](float theta) {
        const float t = theta * (1.f / 6.2831853f);
        return t - std::floor(t);
    };
    const auto run_of = [&T](u32 axis, float s) {
        if (axis >= T.axes.size() || !T.axes[axis].fixed || T.axes[axis].n_rings < 2)
            return 0xFFFFu;
        const Axis& ax = T.axes[axis];
        u32 k = ax.first_ring;
        while (k + 2 < ax.first_ring + ax.n_rings && T.rings[k + 1].s <= s)
            ++k;
        const float u = std::clamp((s - T.rings[k].s) / std::max(T.rings[k + 1].s - T.rings[k].s, 1e-6f), 0.f, 1.f);
        return k < 0xFFFFu ? k | (u32(std::lround(u * 65535.f)) << 16) : 0xFFFFu;
    };
    for (const PatchVert& v : T.patch_verts)
        aux_put({std::bit_cast<u32>(v.p.x), std::bit_cast<u32>(v.p.y), std::bit_cast<u32>(v.p.z), oct(v.n), v.axis & 0xFFFFFFu,
            std::bit_cast<u32>(v.s), std::bit_cast<u32>(v.axis2 != ~0u ? turn_of(v.theta2) : 0.f), run_of(v.axis, v.s)});
    // The cut ends' caps (the triangles past the bark's: the same in both lists) wear their end grain: each cap - its
    // triangles joined by their vertices and by their places, a stock cap's uv seams - gets copies of its vertices with
    // the cross-section's coordinates in place of the bark's (metres from the cap's centroid, square to its axis there:
    // an oblique cut shows its rings as ellipses), in place of the arclength a pair of halves: its rim gauge (1 on the
    // cap's open border, 0 inside - a cap is a fan or a pyramid round its inner vertices, so the gauge runs linearly out
    // to the border along its rays: the bark round the rim and the callus follow the cut's outline, its straight edges
    // too; -2 on a cap with no inner vertex, which keeps its mean) and its place along its own axis (metres from the
    // centroid: a break's fibres run along it), and the mean rim radius in the axis word's top byte (4 mm steps); every
    // copy the cap's own normal (its triangles' by their areas, outward as its vertices' - a stock cap of two faces read
    // as two lids along their crease); a cap standing off its own plane by more than a fifth of its radius (a gable, a
    // cone: a break more than a saw's cut) keeps its own normals and is marked by 4 added to its gauge: its faces fold
    // along their triangles' edges, hidden by the smooth normals, and the rings - cylinders round the pith - kinked into
    // nested chevrons at every fold, so it shows its torn fibres without rings (drawn by the cross-section alone, they
    // read as a paper fan).
    xr_vector<u32> cap_slot(T.patch_verts.size(), ~0u);
    {
        const size_t c0 = size_t(T.patch_bark_tris) * 3, c1 = T.patch_tris.size() - T.patch_tris.size() % 3;
        xr_vector<u32> up(T.patch_verts.size()), place(T.patch_verts.size(), ~0u);
        std::iota(up.begin(), up.end(), 0u);
        const auto find = [&up](u32 i) {
            while (up[i] != i)
                i = up[i] = up[up[i]];
            return i;
        };
        const auto join = [&](u32 a, u32 b) { up[find(a)] = find(b); };
        xr_unordered_map<u64, u32> at_place;
        for (size_t i = c0; i < c1; ++i)
        {
            const u32 v = T.patch_tris[i];
            if (i % 3)
                join(v, T.patch_tris[i - 1]);
            const V3 p = T.patch_verts[v].p;
            const auto q21 = [](float x) { return u64(u32(s32(std::lround(x * 1e3f))) & 0x1FFFFFu); };
            const auto [it, fresh] = at_place.try_emplace((q21(p.x) << 42) | (q21(p.y) << 21) | q21(p.z), v);
            place[v] = it->second;
            if (!fresh)
                join(v, it->second);
        }
        // (The open border: an edge one cap triangle alone has, by the places of its ends.)
        xr_unordered_map<u64, u32> edge_uses;
        for (size_t i = c0; i + 2 < c1; i += 3)
            for (u32 e = 0; e < 3; ++e)
            {
                const u32 a = place[T.patch_tris[i + e]], b = place[T.patch_tris[i + (e + 1) % 3]];
                ++edge_uses[(u64(std::min(a, b)) << 32) | std::max(a, b)];
            }
        xr_vector<u8> on_rim(T.patch_verts.size(), 0);
        for (const auto& [key, uses] : edge_uses)
            if (uses == 1)
                on_rim[u32(key >> 32)] = on_rim[u32(key)] = 1;
        struct Cap
        {
            V3 sum{}, nrm{}, vn{};
            float area{};
            u32 axis_votes[4]{};
            u32 axis[4]{~0u, ~0u, ~0u, ~0u};
            u32 axis_best{~0u};
            V3 t{}, e1{}, e2{}, c{};
            float rim_sum{}, rim_max{}, off{};
            u32 rim_n{}, inner_n{};
            float rim() const { return rim_n ? rim_sum / float(rim_n) : rim_max; }
        };
        xr_unordered_map<u32, Cap> caps;
        for (size_t i = c0; i + 2 < c1; i += 3)
        {
            const PatchVert& a = T.patch_verts[T.patch_tris[i]];
            const PatchVert& b = T.patch_verts[T.patch_tris[i + 1]];
            const PatchVert& c = T.patch_verts[T.patch_tris[i + 2]];
            Cap& K = caps[find(T.patch_tris[i])];
            const V3 g = cross(b.p - a.p, c.p - a.p);
            const float ar = 0.5f * length(g);
            K.sum = K.sum + (a.p + b.p + c.p) * (ar / 3.f);
            K.nrm = K.nrm + g;
            K.vn = K.vn + a.n + b.n + c.n;
            K.area += ar;
            for (const PatchVert* v : {&a, &b, &c})
                for (u32 k = 0; k < 4; ++k)
                    if (K.axis[k] == v->axis || K.axis[k] == ~0u)
                    {
                        K.axis[k] = v->axis;
                        ++K.axis_votes[k];
                        break;
                    }
        }
        for (auto& [root, K] : caps)
        {
            if (K.area < 1e-8f)
                continue;
            K.c = K.sum * (1.f / K.area);
            const V3 n_cap = normalize(K.nrm, v3(0.f, 1.f, 0.f));
            // (Its axis's tangent at the ring nearest the cap; the cap's own normal where the two disagree badly.)
            const u32 best = u32(std::max_element(std::begin(K.axis_votes), std::end(K.axis_votes)) - std::begin(K.axis_votes));
            K.axis_best = K.axis[best];
            K.t = n_cap;
            if (K.axis[best] < T.axes.size() && T.axes[K.axis[best]].fixed)
            {
                const da_shrub::Axis& ax = T.axes[K.axis[best]];
                float d_best = 1e30f;
                V3 t_best = n_cap;
                for (u32 k = ax.first_ring; k < ax.first_ring + ax.n_rings && k < T.rings.size(); ++k)
                    if (const float d = length(T.rings[k].c - K.c); d < d_best)
                    {
                        d_best = d;
                        t_best = T.rings[k].t;
                    }
                if (std::abs(dot(t_best, n_cap)) >= 0.3f)
                    K.t = normalize(t_best, n_cap);
            }
            K.e1 = normalize(cross(K.t, std::abs(K.t.y) < 0.9f ? v3(0.f, 1.f, 0.f) : v3(1.f, 0.f, 0.f)));
            K.e2 = cross(K.t, K.e1);
        }
        // (Each cap's standing off its plane - the plane through its centroid square to its normal.)
        for (size_t i = c0; i < c1; ++i)
            if (const auto it = caps.find(find(T.patch_tris[i])); it != caps.end() && it->second.area >= 1e-8f)
            {
                Cap& K = it->second;
                K.off = std::max(K.off, std::abs(dot(T.patch_verts[T.patch_tris[i]].p - K.c, normalize(K.nrm, K.t))));
            }
        const auto across = [](const Cap& K, V3 p) {
            const V3 q = p - K.c;
            return std::pair{dot(q, K.e1), dot(q, K.e2)};
        };
        xr_vector<u8> seen(T.patch_verts.size(), 0);
        for (size_t i = c0; i < c1; ++i)
        {
            const u32 v = T.patch_tris[i];
            const auto it = caps.find(find(v));
            if (seen[v] || it == caps.end() || it->second.area < 1e-8f)
                continue;
            seen[v] = 1;
            const auto [x, y] = across(it->second, T.patch_verts[v].p);
            const float d = std::sqrt(x * x + y * y);
            it->second.rim_max = std::max(it->second.rim_max, d);
            if (on_rim[place[v]])
            {
                it->second.rim_sum += d;
                ++it->second.rim_n;
            }
            else
                ++it->second.inner_n;
        }
        const u32 slot0 = patch_slot0 + u32(T.patch_verts.size());
        u32 copies = 0;
        for (size_t i = c0; i < c1; ++i)
        {
            const u32 v = T.patch_tris[i];
            const auto it = caps.find(find(v));
            if (cap_slot[v] != ~0u || it == caps.end() || it->second.area < 1e-8f || it->second.rim() < 0.001f)
                continue;
            const Cap& K = it->second;
            const PatchVert& P = T.patch_verts[v];
            const auto [x, y] = across(K, P.p);
            const bool off_plane = K.off > 0.2f * K.rim();
            const float gauge = K.inner_n ? (on_rim[place[v]] ? 1.f : 0.f) : -2.f;
            const u32 rim_q = u32(std::clamp(std::lround(K.rim() / 0.004f), 1l, 255l));
            // (A flat cap's one normal outward as its vertices'; a cap off its plane its own stock normals as they are;
            // every copy the cap's axis - its end grain's seed: a cap whose vertices were skinned to two axes drew
            // its rings round two piths, broken along its triangles' edges.)
            const V3 nc = normalize(K.nrm, P.n);
            const u32 cap_axis = K.axis_best != ~0u ? K.axis_best : P.axis;
            aux_put({std::bit_cast<u32>(P.p.x), std::bit_cast<u32>(P.p.y), std::bit_cast<u32>(P.p.z),
                oct(off_plane ? P.n : dot(nc, K.vn) < 0.f ? nc * -1.f : nc), (cap_axis & 0xFFFFFFu) | (rim_q << 24),
                h2(gauge + (off_plane ? 4.f : 0.f), dot(P.p - K.c, K.t)),
                std::bit_cast<u32>(x), std::bit_cast<u32>(y)});
            cap_slot[v] = slot0 + copies++;
        }
    }
    // The patch vertices' sidecar: a word each - the turn round its first axis (12 bits), the second mapping's weight
    // (8 bits, from bit 24); then the second sidecar, two words each: the second axis (24 bits, all ones: none) and the
    // world projection's weight (8 bits), the second axis's run.
    const u32 side_slot0 = aux_slot();
    for (u32 v = 0; v < nv; ++v)
    {
        const PatchVert& P = T.patch_verts[v];
        aux.push_back((u32(std::lround(turn_of(P.theta) * 4096.f)) & 0xFFFu) | (u8f(P.axis2 != ~0u ? P.w2 : 0.f) << 24));
    }
    aux.resize(aux_at + (aux.size() - aux_at + 7) / 8 * 8, 0u);
    for (u32 v = 0; v < nv; ++v)
    {
        const PatchVert& P = T.patch_verts[v];
        aux_put({(P.axis2 != ~0u ? P.axis2 & 0xFFFFFFu : 0xFFFFFFu) | (u8f(P.w3) << 24), P.axis2 != ~0u ? run_of(P.axis2, P.s2) : 0xFFFFu});
    }
    aux.resize(aux_at + (aux.size() - aux_at + 7) / 8 * 8, 0u);
    // The third sidecar, two words each: a tessellated vertex's lift over its stock facet (the far set's surface under it:
    // the corners' places by their weights) - x and y, z halves - and the facet's normal there (octahedral, eight bits a
    // coordinate); a stock vertex none. The shaders ease the tessellation in near the trunk (r3\da_tree_patch_vs.h).
    for (u32 v = 0; v < nv; ++v)
    {
        const PatchVert& P = T.patch_verts[v];
        V3 lift = v3(0.f, 0.f, 0.f), fn = P.n;
        if (P.facet[0] < nv && P.facet[1] < nv && P.facet[2] < nv)
        {
            const PatchVert &A = T.patch_verts[P.facet[0]], &B = T.patch_verts[P.facet[1]], &C = T.patch_verts[P.facet[2]];
            const float w0 = P.facet_w[0], w1 = P.facet_w[1], w2 = 1.f - w0 - w1;
            lift = P.p - (A.p * w0 + B.p * w1 + C.p * w2);
            fn = normalize(A.n * w0 + B.n * w1 + C.n * w2, P.n);
        }
        aux_put({h2(lift.x, lift.y), f16(lift.z) | (oct8(fn) << 16)});
    }
    aux.resize(aux_at + (aux.size() - aux_at + 7) / 8 * 8, 0u);
    const u32 header[8] = {patch_slot0, nv, side_slot0, ring_slot0, jtab_slot0, jtab_len, jrec_slot0, u32(joins.size())};
    std::copy(std::begin(header), std::end(header), aux.begin() + ptrdiff_t(aux_at));
    xr_vector<u32> order;
    xr_vector<float> imp;
    // The grafted wood by group - levels 0-1, 2, 3 -, each axis by axis (the vertex shader shares a joint's ring with the
    // next segment beside it), its axes by their thickest mean radius falling (WoodCut); each group's table.
    out.first[kKindWood] = u32(elems.size() / 8);
    for (u32 i = 0; i < T.segments.size(); ++i)
        if (!T.axes[T.segments[i].axis].fixed)
            order.push_back(i);
    const auto group_of = [&T](u32 i) { return std::clamp(T.axes[T.segments[i].axis].level, 1u, 3u) - 1u; };
    xr_vector<float> axis_key(T.axes.size(), 0.f);
    for (u32 i : order)
    {
        const Segment& sg = T.segments[i];
        axis_key[sg.axis] = std::max(axis_key[sg.axis], 0.5f * (sg.ra + sg.rb));
        out.cut.wood_r[group_of(i)] = std::max(out.cut.wood_r[group_of(i)], 0.5f * (sg.ra + sg.rb));
    }
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const u32 ga = group_of(a), gb = group_of(b), xa = T.segments[a].axis, xb = T.segments[b].axis;
        if (ga != gb)
            return ga < gb;
        return axis_key[xa] != axis_key[xb] ? axis_key[xa] > axis_key[xb] : xa < xb;
    });
    const auto fill_rq = [](float (&rq)[kRq], const xr_vector<float>& k) {
        for (u32 q = 0; q < kRq; ++q)
            rq[q] = k.empty() ? 0.f : k[std::min(size_t(k.size() - 1), size_t(q) * k.size() / (kRq - 1))];
    };
    {
        xr_vector<float> k[3];
        for (u32 i : order)
            k[group_of(i)].push_back(axis_key[T.segments[i].axis]);
        for (u32 g = 0; g < 3; ++g)
            fill_rq(out.cut.wood[g], k[g]);
    }
    for (u32 i : order)
    {
        const Segment& sg = T.segments[i];
        const Axis& ax = T.axes[sg.axis];
        const u32 level = std::min(ax.level, 3u);
        for (u32 l = level; l < 4; ++l)
            ++out.wood_level[l];
        const V3 d = sg.b - sg.a;
        // (The axis word's top byte, bit 0: within its junction's reach.)
        put(elems, sg.a, (sg.axis & 0xFFFFFFu) | (join_near(sg.axis, sg.a, sg.ra) ? 1u << 24 : 0u), oct(normalize(d)), h2(length(d), sg.ra),
            h2(sg.rb, sg.s0),
            level | (ax.dead ? 0x80u : 0u) | (ax.leafless && !ax.dead ? 0x40u : 0u) | (u8f(sg.vis) << 8) | ((hash32(i + salt) >> 16) << 16));
    }
    out.count[kKindWood] = u32(order.size());
    // The stock tubes: each level of detail's ring pairs along every stock axis, by their thicker ring's radius falling
    // (WoodCut); each level's table.
    out.first[kKindStock] = u32(elems.size() / 8);
    u32 stock = 0;
    for (u32 l = 0; l < 3; ++l)
    {
        out.stock_lod_first[l] = stock;
        struct Tube
        {
            u32 a, prev, k;
            float key;
        };
        xr_vector<Tube> tubes;
        for (u32 a = 0; a < T.axes.size(); ++a)
        {
            const Axis& ax = T.axes[a];
            if (!ax.fixed || ax.n_rings < 2)
                continue;
            u32 prev = ~0u;
            for (u32 k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
            {
                if (!(T.rings[k].lod & (1u << l)))
                    continue;
                // (A stretch whose rings the patches all wrap whole: only the patches show there.)
                bool hidden = prev != ~0u;
                for (u32 q = prev; hidden && q <= k; ++q)
                    hidden = (T.rings[q].cover & da_shrub::kRingHidden) != 0u;
                // (The far level's tubes all along the axis: it draws whole, without the patches - kChunkFar.)
                if (prev != ~0u && (!hidden || l == 2))
                    tubes.push_back({a, prev, k, std::max(T.rings[prev].r0, T.rings[k].r0)});
                prev = k;
            }
        }
        std::stable_sort(tubes.begin(), tubes.end(), [](const Tube& x, const Tube& y) { return x.key > y.key; });
        xr_vector<float> keys;
        for (const Tube& t : tubes)
        {
            const Axis& ax = T.axes[t.a];
            // (The level byte's top bit: a dead stretch of a limb, weathered grey.)
            const u32 dead = T.rings[t.prev].s >= ax.dead_from ? 0x8000u : 0u;
            // (w: the climbable share; its junction record's slot within the junction's reach.)
            const u32 js = join_near(t.a, T.rings[t.prev].c, T.rings[t.prev].r0) ? jrec_slot0 + 2u * join_of[t.a] : 0u;
            put(elems, T.rings[t.prev].c, (t.a & 0xFFFFFFu) | (l << 24), ring_slot0 + t.prev * 2u, ring_slot0 + t.k * 2u,
                u8f(ring_vis[t.prev]) | dead | ((hash32(t.prev * 977u + t.k + salt) >> 16) << 16),
                u8f(T.rings[t.prev].climbable) | (js << 8));
            keys.push_back(t.key);
            ++stock;
        }
        fill_rq(out.cut.stock[l], keys);
        out.stock_lod_count[l] = stock - out.stock_lod_first[l];
    }
    out.count[kKindStock] = stock;
    // The stock triangles kept: tessellated (the near level of detail), then as the stock has them (the far ones), each
    // set by its limb's radius falling (WoodCut; a vertex's limb the thickest of its rings and its own distance from its
    // axis - a cap's or a gnarl's stands out of it -, none known: kept).
    out.first[kKindPatch] = u32(elems.size() / 8);
    const auto limb_r = [&T](u32 v) {
        const PatchVert& P = T.patch_verts[v];
        const float r = std::max({P.r, P.ring_r, P.ring_r2});
        return r > 0.f ? r : 1e3f;
    };
    const auto put_patches = [&](const auto& tris, u32 bark_tris, float (&rq)[kRq]) {
        xr_vector<u32> by_r(tris.size() / 3);
        xr_vector<float> tri_r(by_r.size()), keys;
        for (u32 t = 0; t < by_r.size(); ++t)
        {
            by_r[t] = t;
            tri_r[t] = std::max({limb_r(tris[3 * t]), limb_r(tris[3 * t + 1]), limb_r(tris[3 * t + 2])});
        }
        std::stable_sort(by_r.begin(), by_r.end(), [&tri_r](u32 a, u32 b) { return tri_r[a] > tri_r[b]; });
        for (const u32 tt : by_r)
        {
            const size_t t = size_t(tt) * 3;
            keys.push_back(tri_r[tt]);
            const u32 a = tris[t], b = tris[t + 1], c = tris[t + 2];
            const PatchVert& v = T.patch_verts[a];
            // (The sky by its axis's ring at the vertex's arclength.)
            float vis = 0.6f;
            if (v.axis < T.axes.size())
            {
                const Axis& ax = T.axes[v.axis];
                for (u32 k = ax.first_ring; k < ax.first_ring + ax.n_rings; ++k)
                    if (T.rings[k].s <= v.s)
                        vis = ring_vis[k];
            }
            // (Bit 8: old wood, no young growth's colour - a dead stretch of a limb; bit 9: a cut end's cap, its end
            // grain from its own copies of the vertices.)
            const bool dead = v.axis < T.axes.size() && v.s >= T.axes[v.axis].dead_from;
            const bool cap = t / 3 >= bark_tris && cap_slot[a] != ~0u && cap_slot[b] != ~0u && cap_slot[c] != ~0u;
            put(elems, v.p, v.axis & 0xFFFFFFu, cap ? cap_slot[a] : patch_slot0 + a, cap ? cap_slot[b] : patch_slot0 + b,
                cap ? cap_slot[c] : patch_slot0 + c,
                u8f(vis) | (dead || t / 3 >= bark_tris ? 0x100u : 0u) | (cap ? 0x200u : 0u) | ((hash32(u32(t) * 31u + salt) >> 16) << 16));
        }
        fill_rq(rq, keys);
    };
    put_patches(T.patch_tris, T.patch_bark_tris, out.cut.patch[0]);
    out.count[kKindPatch] = u32(T.patch_tris.size() / 3);
    put_patches(T.patch_tris_far, T.patch_bark_tris_far, out.cut.patch[1]);
    out.patch_far = u32(T.patch_tris_far.size() / 3);
    // The crown's normal: the gradient of the ellipsoid of the foliage's bounds (the tiers are lit alike).
    V3 flo{1e9f, 1e9f, 1e9f}, fhi{-1e9f, -1e9f, -1e9f};
    for (const Leaf& f : T.leaves)
    {
        flo = v3(std::min(flo.x, f.p.x), std::min(flo.y, f.p.y), std::min(flo.z, f.p.z));
        fhi = v3(std::max(fhi.x, f.p.x), std::max(fhi.y, f.p.y), std::max(fhi.z, f.p.z));
    }
    if (T.leaves.empty())
    {
        flo = T.bmin;
        fhi = T.bmax;
    }
    const V3 cc = (flo + fhi) * 0.5f, ce = (fhi - flo) * 0.5f;
    const V3 inv_e2{1.f / std::max(ce.x * ce.x, 1e-4f), 1.f / std::max(ce.y * ce.y, 1e-4f), 1.f / std::max(ce.z * ce.z, 1e-4f)};
    const auto crown_at = [&cc, &inv_e2](V3 p) {
        const V3 m = p - cc;
        return normalize(v3(m.x * inv_e2.x, m.y * inv_e2.y, m.z * inv_e2.z), v3(0.f, 1.f, 0.f));
    };
    const auto leaf_put = [&](const Leaf& f, u32 i) {
        const V3 crown = crown_at(f.p + f.dir * (0.5f * f.len));
        put(elems, f.p, (f.axis & 0xFFFFFFu) | (u8f(f.rank) << 24), oct12(f.dir) | (u8f(f.fall) << 24),
            leaf_roll8(f.dir, f.side) | (oct12(crown) << 8), h2(f.len, f.wid),
            f16(f.s) | (u8f(f.light) << 16) | ((hash32(i * 3u + 1u + salt) & 0xFFu) << 24));
    };
    // The leaves, the lit outer ones first (the pixel floor draws a prefix).
    out.first[kKindLeaf] = u32(elems.size() / 8);
    order.resize(T.leaves.size());
    imp.resize(T.leaves.size());
    float leaf_len = 0.f;
    for (u32 i = 0; i < order.size(); ++i)
    {
        order[i] = i;
        imp[i] = T.leaves[i].exposure + 0.35f * hash01(i + 7u);
        leaf_len += T.leaves[i].len;
    }
    std::sort(order.begin(), order.end(), [&imp](u32 a, u32 b) { return imp[a] > imp[b]; });
    for (u32 i : order)
        leaf_put(T.leaves[i], i);
    out.count[kKindLeaf] = u32(T.leaves.size());
    out.leaf_len = T.leaves.empty() ? 0.05f : leaf_len / float(T.leaves.size());
    // The blooms, from the top.
    out.first[kKindBloom] = u32(elems.size() / 8);
    order.resize(T.blooms.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&T](u32 a, u32 b) { return T.blooms[a].p.y > T.blooms[b].p.y; });
    for (u32 i : order)
    {
        const Bloom& b = T.blooms[i];
        put(elems, b.p, (b.axis & 0xFFFFFFu) | (u8f(b.rank) << 24), oct(b.dir), h2(b.size, b.s),
            u32(b.kind) | ((hash32(i * 5u + 3u + salt) >> 8) << 8), u8f(b.vis));
    }
    out.count[kKindBloom] = u32(T.blooms.size());
    // The clusters, by importance.
    out.first[kKindCluster] = u32(elems.size() / 8);
    order.resize(T.clusters.size());
    imp.resize(T.clusters.size());
    float cr = 0.f;
    for (u32 i = 0; i < order.size(); ++i)
    {
        order[i] = i;
        imp[i] = (T.clusters[i].exposure + 0.3f * hash01(i + 11u)) * std::sqrt(T.clusters[i].area);
        cr += T.clusters[i].r;
    }
    std::sort(order.begin(), order.end(), [&imp](u32 a, u32 b) { return imp[a] > imp[b]; });
    for (u32 i : order)
    {
        const Cluster& c = T.clusters[i];
        put(elems, c.c, (c.axis & 0xFFFFFFu) | (u8f(c.rank) << 24), oct12(crown_at(c.c)) | (u8f(c.fall) << 24), h2(c.r, c.area),
            f16(c.s) | (u8f(c.light) << 16) | (std::min(c.blooms, 255u) << 24),
            std::min(c.leaves, 65535u) | ((hash32(i * 7u + salt) >> 16) << 16));
    }
    out.count[kKindCluster] = u32(T.clusters.size());
    out.cluster_r = T.clusters.empty() ? 0.1f : cr / float(T.clusters.size());
    // The blobs, by area: they ride the trunk (axis 0, fixed: its sway alone).
    out.first[kKindBlob] = u32(elems.size() / 8);
    order.resize(T.blobs.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&T](u32 a, u32 b) { return T.blobs[a].area > T.blobs[b].area; });
    float br = 0.f;
    for (u32 i : order)
    {
        const Blob& b = T.blobs[i];
        br += b.r;
        put(elems, b.c, u8f(b.rank) << 24, oct12(crown_at(b.c)) | (u8f(b.fall) << 24), h2(b.r, b.area),
            u8f(b.bloom_share) | (u8f(b.light) << 8) | ((hash32(i * 13u + salt) >> 16) << 16), f16(std::max(b.c.y, 0.f)) | (f16(b.pull) << 16));
    }
    out.count[kKindBlob] = u32(T.blobs.size());
    out.blob_r = T.blobs.empty() ? 0.3f : br / float(T.blobs.size());
    const V3 c = (T.bmin + T.bmax) * 0.5f;
    out.centre.set(c.x, c.y, c.z);
    out.radius = 0.5f * length(T.bmax - T.bmin);
    out.lo.set(T.bmin.x, T.bmin.y, T.bmin.z);
    out.hi.set(T.bmax.x, T.bmax.y, T.bmax.z);
    out.height = T.height;
    out.width = T.width;
    out.climb_top = T.climb_top;
    out.leaf_area = T.leaf_area;
}

// ---- The disk cache ($app_data_root$\cache\trees\<kind>_<species>_<datahash>_<genver>.bin) ----
struct CacheHeader
{
    u32 magic{0x43545444u}; // "DTTC"
    u32 version{kGenVer};
    u64 hash{};
    u32 counts[kKinds]{}, firsts[kKinds]{};
    u32 wood_level[4]{}, stock_lod_first[3]{}, stock_lod_count[3]{};
    float centre[3]{}, radius{}, lo[3]{}, hi[3]{};
    float leaf_len{}, cluster_r{}, blob_r{}, stock_r{}, height{}, width{}, climb_top{}, leaf_area{};
    u32 leaves{};
    u32 n_axes{}, n_elems{}, n_aux{};
    u32 patch_far{};
    WoodCut cut{};
};

void cache_path(string_path& out, u64 kind, pcstr species, u64 hash)
{
    string256 name;
    xr_sprintf(name, "cache" DELIMITER "trees" DELIMITER "%016llx_%s_%016llx_%u.bin", (unsigned long long)kind, species,
        (unsigned long long)hash, kGenVer);
    FS.update_path(out, "$app_data_root$", name);
}

bool cache_read(pcstr path, u64 hash, Template& T, xr_vector<u32>& elems, xr_vector<u32>& axes, xr_vector<u32>& aux, u32& leaves)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f)
        return false;
    CacheHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CacheHeader{}.magic && h.version == kGenVer && h.hash == hash &&
        h.n_axes < (1u << 26) && h.n_elems < (1u << 28) && h.n_aux < (1u << 26);
    if (ok)
    {
        axes.resize(size_t(h.n_axes));
        elems.resize(size_t(h.n_elems));
        aux.resize(size_t(h.n_aux));
        ok = (axes.empty() || fread(axes.data(), 4, axes.size(), f) == axes.size()) &&
            (elems.empty() || fread(elems.data(), 4, elems.size(), f) == elems.size()) &&
            (aux.empty() || fread(aux.data(), 4, aux.size(), f) == aux.size());
    }
    // (Nothing past the arrays, whole records, every range inside them, every measure finite: a torn or foreign file
    // is generated again.)
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    const u32 n_rec = h.n_elems / 8;
    ok = ok && h.n_elems % 8 == 0 && h.n_axes % 8 == 0 && h.n_aux % 8 == 0;
    for (u32 k = 0; ok && k < kKinds; ++k)
        ok = u64(h.firsts[k]) + h.counts[k] + (k == kKindPatch ? h.patch_far : 0u) <= n_rec;
    for (u32 l = 0; ok && l < 4; ++l)
        ok = h.wood_level[l] <= h.counts[kKindWood];
    for (u32 l = 0; ok && l < 3; ++l)
        ok = u64(h.stock_lod_first[l]) + h.stock_lod_count[l] <= h.counts[kKindStock];
    const float measures[] = {h.centre[0], h.centre[1], h.centre[2], h.radius, h.lo[0], h.lo[1], h.lo[2], h.hi[0], h.hi[1], h.hi[2],
        h.leaf_len, h.cluster_r, h.blob_r, h.stock_r, h.height, h.width, h.climb_top, h.leaf_area};
    for (const float m : measures)
        ok = ok && std::isfinite(m);
    for (u32 q = 0; ok && q < kRq; ++q)
    {
        for (u32 g = 0; g < 3; ++g)
            ok = ok && std::isfinite(h.cut.wood[g][q]) && std::isfinite(h.cut.stock[g][q]);
        ok = ok && std::isfinite(h.cut.patch[0][q]) && std::isfinite(h.cut.patch[1][q]);
    }
    for (const float r : h.cut.wood_r)
        ok = ok && std::isfinite(r);
    if (!ok)
        return false;
    for (u32 k = 0; k < kKinds; ++k)
    {
        T.count[k] = h.counts[k];
        T.first[k] = h.firsts[k];
    }
    std::copy(std::begin(h.wood_level), std::end(h.wood_level), std::begin(T.wood_level));
    std::copy(std::begin(h.stock_lod_first), std::end(h.stock_lod_first), std::begin(T.stock_lod_first));
    std::copy(std::begin(h.stock_lod_count), std::end(h.stock_lod_count), std::begin(T.stock_lod_count));
    T.centre.set(h.centre[0], h.centre[1], h.centre[2]);
    T.radius = h.radius;
    T.lo.set(h.lo[0], h.lo[1], h.lo[2]);
    T.hi.set(h.hi[0], h.hi[1], h.hi[2]);
    T.leaf_len = h.leaf_len;
    T.cluster_r = h.cluster_r;
    T.blob_r = h.blob_r;
    T.stock_r = h.stock_r;
    T.height = h.height;
    T.width = h.width;
    T.climb_top = h.climb_top;
    T.leaf_area = h.leaf_area;
    T.patch_far = h.patch_far;
    T.cut = h.cut;
    leaves = h.leaves;
    return true;
}

void cache_write(pcstr path, u64 hash, const Template& T, const xr_vector<u32>& elems, const xr_vector<u32>& axes, const xr_vector<u32>& aux,
    u32 leaves)
{
    CacheHeader h;
    h.hash = hash;
    for (u32 k = 0; k < kKinds; ++k)
    {
        h.counts[k] = T.count[k];
        h.firsts[k] = T.first[k];
    }
    std::copy(std::begin(T.wood_level), std::end(T.wood_level), std::begin(h.wood_level));
    std::copy(std::begin(T.stock_lod_first), std::end(T.stock_lod_first), std::begin(h.stock_lod_first));
    std::copy(std::begin(T.stock_lod_count), std::end(T.stock_lod_count), std::begin(h.stock_lod_count));
    h.centre[0] = T.centre.x;
    h.centre[1] = T.centre.y;
    h.centre[2] = T.centre.z;
    h.radius = T.radius;
    h.lo[0] = T.lo.x;
    h.lo[1] = T.lo.y;
    h.lo[2] = T.lo.z;
    h.hi[0] = T.hi.x;
    h.hi[1] = T.hi.y;
    h.hi[2] = T.hi.z;
    h.leaf_len = T.leaf_len;
    h.cluster_r = T.cluster_r;
    h.blob_r = T.blob_r;
    h.stock_r = T.stock_r;
    h.height = T.height;
    h.width = T.width;
    h.climb_top = T.climb_top;
    h.leaf_area = T.leaf_area;
    h.patch_far = T.patch_far;
    h.cut = T.cut;
    h.leaves = leaves;
    h.n_axes = u32(axes.size());
    h.n_elems = u32(elems.size());
    h.n_aux = u32(aux.size());
    // (Written beside and renamed over: a crash mid-write leaves no torn file under the real name.)
    string_path tmp;
    xr_sprintf(tmp, "%s.tmp", path);
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") || !f)
        return;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && (axes.empty() || fwrite(axes.data(), 4, axes.size(), f) == axes.size()) &&
        (elems.empty() || fwrite(elems.data(), 4, elems.size(), f) == elems.size()) &&
        (aux.empty() || fwrite(aux.data(), 4, aux.size(), f) == aux.size());
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

// ======================================================================================================
// The population
// ======================================================================================================
class TreePopulation final : public Population
{
public:
    TreePopulation() : Population(kPopTrees) {}

    u32 preset{2};
    PresetColumns cols{};
    float R{};      // the billboards from here this frame (m)
    float lod{1.f}; // r__trees_lod_scale (QA)
    float detail{1.f}; // the preset's crown detail times r__trees_detail (QA): the crowns' leaf and wood floors divided by it
    s32 tier{-1};   // r__trees_force_tier (QA): 0 hulls .. 4 blobs, 5 the billboard, -1 by distance
    bool prepared{};
    u64 kind_pin{}; // QA: only this kind is upgraded
    u32 species_pin{}; // QA: species + 1 every tree grows
    xr_vector<Placement> placements;
    xr_map<shared_str, Skel> skels;
    std::unordered_set<const dxRender_Visual*> replaced;
    xr_unordered_map<const dxRender_Visual*, u32> stand; // a stand-in -> its instance
    float elem_area[kTreeSpecies]{};
    xr_vector<u32> draws_view, draws_shadow;
    u64 t0{};
    // The billboards: the atlas pair, each template's cells, the facets' shader, the stock fill's shader and its cell
    // list, this frame's billboards.
    ref_rt bb_d, bb_nm;
    u32 bb_w{}, bb_h{}, bb_cell{};
    xr_vector<BbCell> bb;
    // The template of each cell set past the templates' own (their dead looks, in set order).
    xr_vector<u32> bb_dead;
    ref_shader s_bb, s_bb_copy;
    GpuBuffer bb_list_b;
    bool bb_ready{}, bb_cleared{};
    struct BbItem
    {
        float d;
        u32 inst;
        float alpha;
    };
    xr_vector<BbItem> bb_frame;
    u32 bb_drawn{};

    pcstr name() const override { return "trees"; }
    float reach() const override { return std::max(R, latch().far_plane); }
    // (Past R a tree with a billboard is its billboard alone: no record.)
    bool needs_record(u32 i, float d) const override { return tier >= 0 ? tier < 5 || !has_bb(i) : d < R || !has_bb(i); }
    // (A template's billboard once its cells hold its views: the first frame's trees keep their blobs. A tree standing
    // dead among living ones takes its template's dead look: none, or no room for it, and it keeps its blobs.)
    bool has_bb(u32 i) const
    {
        const Instance& I = instances[i];
        const u32 t = I.tmpl;
        if (!bb_ready || t >= bb.size() || !bb[t].placement)
            return false;
        const BbCell& B = bb[t];
        return I.dead && !B.dead_look ? B.dead_set != ~0u && B.dead_filled : B.filled;
    }
    // The atlas pair's levels with the bake: a cell's chain down to its texel.
    u32 bb_mips() const
    {
        u32 n = 1;
        while ((bb_cell >> (n - 1)) > 1)
            ++n;
        return n;
    }
    u32 frame_cascades() const override { return cols.cascades; }
    bool wanted() const override { return ps_r__trees != 0; }
    bool pending() const override { return prepared; }
    const xr_vector<u32>& view_draws() const override { return draws_view; }
    const xr_vector<u32>& shadow_draws() const override { return draws_shadow; }
    void apply_preset(u32 p) override { preset = std::min(p, 4u); }

    void frame_begin() override
    {
        cols = kPresets[preset];
        lod = ps_r__trees_lod_scale;
        detail = cols.crown * ps_r__trees_detail;
        cols.leaf_kpx = std::max(cols.kpx, cols.leaf_kpx / detail);
        // (QA r__trees_far_detail: the thinned leaves' and the clusters' tiers ending at a smaller leaf.)
        cols.leaf_px /= ps_r__trees_far_detail;
        cols.clu_px /= ps_r__trees_far_detail;
        tier = ps_r__trees_force_tier;
        // The billboard's distance: where the reference tree stands the preset's pixels tall on the render (the
        // upscaler's input) - a blob tier on to 360 m drew a hundred trees' trunks, patches and blobs for a few pixels.
        R = kBbTreeHeight * latch().px_per_m / cols.bb_px * lod;
        bb_frame.clear();
    }

    bool skip_draw(u32 d, u32 pass) const override
    {
        // QA (r__dbg_trees): 1 the stock tubes, 2 the stock patches, 4 the grafted wood, 8 the leaves, 32 the blooms, 64
        // the clusters, 128 the blobs, 512 every shadow map and the rain map (256 the billboards: render_extra; 1024 ..
        // 8192 the trees by their tier: view).
        static constexpr u32 kDbgBit[drCount] = {4, 8, 8, 8, 32, 64, 128, 8, 8, 8, 4, 8, 4, 1, 1, 1, 2, 4, 1, 2};
        return (ps_r__dbg_trees & kDbgBit[d]) || (pass && (ps_r__dbg_trees & 512));
    }

    // The near cascade's and the local lights' clusters (a far crown's past the leaves' 22 m) at two thirds, the middle
    // cascade's at two fifths, the far one's blobs at a half: a crown's clusters stack ten deep along the sun, the middle
    // map's texels each shaded as often - their cover made up, the light through them the same (QA: r__dbg_trees 16384
    // all).
    static float shadow_thin(u32 pass)
    {
        return (ps_r__dbg_trees & 16384) ? 1.f : (pass == 1 ? 0.67f : (pass == 2 ? 0.4f : (pass == 3 ? 0.5f : 1.f)));
    }

    // The view's clusters and blobs by their size on the screen: all from 12 px (a cluster) or 18 px (a blob), three
    // fifths at 4 and 6 px and under - a far crown's parts stood three and four deep, every pixel shaded as often; the
    // outer ones kept (they go by importance), their cover made up (QA: r__dbg_trees 16384 all).
    static float view_thin(float size_px, float full_px)
    {
        return (ps_r__dbg_trees & 16384) ? 1.f : std::clamp(0.6f + 0.4f * (size_px - full_px / 3.f) / (full_px * 2.f / 3.f), 0.6f, 1.f);
    }

    void fill_tables(float day, Fvector4* tables) override
    {
        for (u32 s = 0; s < kTreeSpecies; ++s)
        {
            if (!g_data.species[s].used)
                continue;
            fill_profile(g_data.species[s], elem_area[s], &tables[(kShrubSpecies + s) * kProfF4]);
            fill_season(g_data.species[s], day, &tables[kTabSeas + (kShrubSpecies + s) * kSeasF4], g_data.young_lead);
        }
    }

    void replaced_visuals(xr_vector<dxRender_Visual*>& out) const override
    {
        if (!detached)
            return;
        out.reserve(out.size() + replaced.size());
        for (const dxRender_Visual* v : replaced)
            out.push_back(const_cast<dxRender_Visual*>(v));
    }

    bool stand_in(const dxRender_Visual* visual, u32& species, u32& tmpl, bool& dead) const override
    {
        const auto it = stand.find(visual);
        if (it == stand.end())
            return false;
        const Instance& I = instances[it->second];
        species = I.species;
        tmpl = I.tmpl;
        // (A Red Forest pine's red stock cards are its red needles all year.)
        dead = I.dead || I.red;
        return true;
    }

    // ---- Level load ----
    Skel* skeleton(const shared_str& name)
    {
        Skel& e = skels[name];
        if (e.tried)
            return e.ok ? &e : nullptr;
        e.tried = true;
        string_path file;
        xr_sprintf(file, "da" DELIMITER "trees" DELIMITER "%s%s", name.c_str(), strext(name.c_str()) ? "" : ".dtsk");
        IReader* r = FS.r_open("$game_meshes$", file);
        if (!r)
        {
            Msg("! [trees] skeleton %s not found: the trees of its kinds stand stock", file);
            return nullptr;
        }
        pcstr why = nullptr;
        e.ok = da_shrub::read_skeleton(r->pointer(), size_t(r->length()), e.S, &why);
        e.hash = fnv(r->pointer(), size_t(r->length()));
        FS.r_close(r);
        if (!e.ok)
            Msg("! [trees] skeleton %s: %s, the trees of its kinds stand stock", file, why ? why : "unreadable");
        else if (e.S.rings_tamed)
            Msg("~ [trees] skeleton %s: %u misfit rings held (profiles past 0.6 of the radius, radii past their axis's bound)", file, e.S.rings_tamed);
        return e.ok ? &e : nullptr;
    }

    // Every stock tree of the level the shrubs did not take: the census's FLODs whose content the data names a tree with
    // a skeleton; each one placement on its exact transform (rotation, the uneven scale, the tilt, the translation), its
    // wind root (a part with foliage), its baked light, its tilt bucket.
    void collect_placements()
    {
        Census& C = census();
        u32 known = 0, stock = 0, no_skel = 0, tiny = 0;
        const float sbc = ps_r__Tree_SBC * 1.3333f;
        for (u32 ci = 0; ci < C.candidates.size(); ++ci)
        {
            const Candidate& c = C.candidates[ci];
            if (c.taken != ~0u)
                continue;
            const u64 key = kind_of(C, c, g_data.kinds, g_data.families);
            if (!key)
                continue;
            ++known;
            const Kind& k = g_data.kinds.find(key)->second;
            if (k.cls != tcTree && k.cls != tcYoung)
            {
                ++stock;
                continue;
            }
            if (kind_pin && key != kind_pin)
                continue;
            Skel* S = k.skel.size() ? skeleton(k.skel) : nullptr;
            if (!S)
            {
                ++no_skel;
                continue;
            }
            // (Every part of a stock tree shares its wind root: the first stands for them.)
            const FTreeVisual* first = C.parts[c.parts[0]].visual;
            const Fmatrix& xf = first->world();
            const float det = det3(xf);
            if (std::cbrt(std::abs(det)) < g_data.min_scale)
            {
                ++tiny;
                continue;
            }
            Placement P;
            P.candidate = ci;
            P.parent = c.parent;
            P.flod = c.flod;
            P.kind = key;
            P.k = &k;
            P.skel = S;
            P.xf = xf;
            P.wind = first;
            P.top = std::max(first->wind_height(), 1.f);
            // The static lighting the stock tree shader gives a vertex at mid height (hemi x H_MODELS), as the shrubs'.
            const FTreeVisualColors col = P.wind->colors();
            P.hemi = std::clamp(sbc * (col.scale.w * 0.7f + col.bias.w) * 1.4f, 0.f, 1.f);
            P.sun = std::clamp(sbc * (col.sun_scale * 0.7f + col.sun_bias), 0.f, 1.f);
            // The world's vertical in the model: past max_tilt_graft the grafted growth takes its bucket's (eight
            // headings by fifteen degrees of tilt), so a leaning tree's new branches still grow up toward the light.
            Fvector up = model_dir(xf, Fvector{0.f, 1.f, 0.f});
            up.normalize_safe();
            const float tilt = rad2deg(std::acos(std::clamp(up.y, -1.f, 1.f)));
            if (tilt > g_data.max_tilt_graft)
            {
                const u32 t = std::min(u32(std::lround(tilt / 15.f)), 6u);
                const u32 a = u32(std::lround(std::atan2(up.z, up.x) / (PI / 4.f)) + 8) & 7u;
                P.bucket = 1 + t * 8 + a;
                const float T = deg2rad(15.f * float(t)), A = float(a) * (PI / 4.f);
                P.up = da_shrub::v3(std::sin(T) * std::cos(A), std::cos(T), std::sin(T) * std::sin(A));
            }
            // A dead tree stands bare and grey; the Red Forest's dead pines keep their rust-red needles all year. A sapling
            // (a YOUNG model, a sapling kind) is wholly young; a tree by its height between the policy's young heights.
            P.red = k.dead && k.red;
            P.dead = k.dead && !k.red;
            P.youth = (k.sapling || k.cls == tcYoung) ? 1.f
                                                       : std::clamp((g_data.young_h[1] - P.top) / (g_data.young_h[1] - g_data.young_h[0]), 0.f, 1.f);
            placements.push_back(P);
        }
        Msg("* [trees] %u FLODs of tree parts, %u stock trees of the data: %u placements, %u kept stock by class, %u without a "
            "skeleton, %u too small",
            u32(C.candidates.size()), known, u32(placements.size()), stock, no_skel, tiny);
    }

    // The placements' species: the kind's own, else its community's (the Red Forest's for the red stock, else the grass
    // community under the root, else the class's), a conifer kind among its conifers, by share; a share of them standing
    // dead; at most kinds_per_level_species species a kind on the level (the rest take the kind's most frequent of the
    // same look).
    void pick_species()
    {
        for (Placement& P : placements)
        {
            const Kind& k = *P.k;
            P.species = species_pin ? s32(species_pin - 1) : k.species;
            if (P.species >= 0)
                continue;
            const u32 h = pos_hash(P.xf.c);
            s32 comm = k.community;
            if (comm < 0 && k.red)
                comm = g_data.red_comm;
            if (comm < 0)
            {
                const s32 gc = da_grass_community_at(P.xf.c.x, P.xf.c.z);
                comm = gc >= 0 && gc < 16 ? g_data.grass_comm[gc] : -1;
            }
            if (comm < 0)
                comm = g_data.class_comm[k.cls];
            if (comm < 0 || !g_data.communities[comm].used)
                continue;
            const Community& C = g_data.communities[comm];
            for (u32 pass = 0; pass < 2 && P.species < 0; ++pass)
            {
                float total = 0.f, w[16]{};
                for (u32 i = 0; i < C.count; ++i)
                {
                    const Species& S = g_data.species[C.species[i]];
                    if (pass == 0 && conifer_species(S) != k.conifer)
                        continue;
                    w[i] = C.share[i];
                    total += w[i];
                }
                float x = hash01(h) * total;
                for (u32 i = 0; i < C.count && total > 0.f; ++i)
                {
                    x -= w[i];
                    if (w[i] > 0.f && x <= 0.f)
                    {
                        P.species = s32(C.species[i]);
                        break;
                    }
                }
            }
        }
        // At most kinds_per_level_species species templates a kind on the level.
        std::map<u64, std::map<s32, u32>> per_kind;
        for (const Placement& P : placements)
            if (P.species >= 0)
                ++per_kind[P.kind][P.species];
        for (auto& [kind, counts] : per_kind)
        {
            if (counts.size() <= g_data.kinds_per_level_species)
                continue;
            xr_vector<std::pair<u32, s32>> ranked;
            for (const auto& [s, n] : counts)
                ranked.emplace_back(n, s);
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            ranked.resize(g_data.kinds_per_level_species);
            for (Placement& P : placements)
            {
                if (P.kind != kind || P.species < 0)
                    continue;
                if (std::any_of(ranked.begin(), ranked.end(), [&P](const auto& r) { return r.second == P.species; }))
                    continue;
                const bool conifer = conifer_species(g_data.species[P.species]);
                s32 to = ranked.front().second;
                for (const auto& r : ranked)
                    if (conifer_species(g_data.species[r.second]) == conifer)
                    {
                        to = r.second;
                        break;
                    }
                P.species = to;
            }
        }
        for (Placement& P : placements)
            if (P.species >= 0 && !P.red)
                P.dead = P.dead || hash01(pos_hash(P.xf.c) ^ 0x5bd1e995u) < g_data.species[P.species].dead;
    }

    // The templates the placements need (a kind, a species, a tilt bucket), grown on the task pool or read back from the
    // disk cache, packed; the instances on the placements' exact transforms.
    bool build()
    {
        using TKey = std::tuple<u64, s32, u32>;
        std::map<TKey, u32> tmpl_of;
        struct Job
        {
            u64 kind;
            u32 species, bucket;
            da_shrub::V3 up;
            Skel* skel;
            u32 seed, leaf_cap;
            u64 hash;
            string_path path;
            Template meta;
            xr_vector<u32> elems, axes, aux;
            u32 leaves{};
            bool ok{}, cached{};
        };
        xr_vector<Job> jobs;
        for (Placement& P : placements)
        {
            if (P.species < 0)
                continue;
            const TKey key{P.kind, P.species, P.bucket};
            const auto [it, fresh] = tmpl_of.try_emplace(key, u32(jobs.size()));
            if (fresh)
            {
                Job j{};
                j.kind = P.kind;
                j.species = u32(P.species);
                j.bucket = P.bucket;
                j.up = P.up;
                j.skel = P.skel;
                j.seed = u32(P.kind ^ (P.kind >> 32)) * 2654435761u + u32(P.species) * 7919u + P.bucket * 104729u;
                jobs.push_back(std::move(j));
            }
            P.tmpl = it->second;
        }
        if (jobs.empty())
            return true;
        // The level's leaf budget shared among its templates by what each asks (its crown's leaf area over its species'
        // element): the small ones take theirs, the large ones share the rest alike - one cap for all left a forty-metre
        // spruce a twentieth of its needles while a sapling's share went unused; a dead tree's species, its leaves never
        // drawn, its far parts' frame at 16k. Each cap in steps (a quarter of its power of two: a template keeps its disk
        // cache while the others move a little); the lower presets a share of the budget, their GPUs' memory and vertices
        // (from the next level load).
        constexpr double kBudgetShare[] = {0.35, 0.5, 0.7, 0.85, 1.0};
        const double budget = kLeafBudget * kBudgetShare[std::min(preset, 4u)];
        xr_vector<u32> need(jobs.size());
        g_pGamePersistent->LoadInBackground([&jobs, &need] {
            xr_parallel_for(TaskRange<size_t>(0, jobs.size(), 1), [&jobs, &need](const TaskRange<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                {
                    const Species& S = g_data.species[jobs[i].species];
                    const bool leafless = S.name.size() > 5 && (strstr(S.name.c_str(), "_dead") || strstr(S.name.c_str(), "_char"));
                    const u32 n = da_shrub::tree_leaf_need(S.arch, jobs[i].skel->S, jobs[i].up);
                    need[i] = leafless ? std::min(n, 16000u) : n;
                }
            });
        });
        {
            xr_vector<u32> order(jobs.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&need](u32 a, u32 b) { return need[a] < need[b]; });
            double rest = budget;
            for (u32 k = 0; k < order.size(); ++k)
            {
                const double fair = rest / double(order.size() - k);
                double c = std::clamp(std::min(double(need[order[k]]), fair), 4000.0, 640000.0);
                // (Down to a quarter step of its power of two.)
                const double step = std::exp2(std::floor(std::log2(c))) / 4.0;
                c = std::max(std::floor(c / step) * step, 4000.0);
                jobs[order[k]].leaf_cap = u32(c);
                rest = std::max(rest - c, 0.0);
            }
        }
        string_path dir;
        FS.update_path(dir, "$app_data_root$", "cache" DELIMITER "trees" DELIMITER);
        VerifyPath(dir);
        // (The cache of the generator's other versions: no template asks for it again.)
        {
            string_path mask;
            xr_strcpy(mask, dir);
            xr_strcat(mask, "*.bin");
            char keep[16];
            xr_sprintf(keep, "_%u.bin", kGenVer);
            WIN32_FIND_DATAA fd;
            if (const HANDLE h = FindFirstFileA(mask, &fd); h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    const size_t ln = xr_strlen(fd.cFileName), lk = xr_strlen(keep);
                    if (ln < lk || xr_strcmp(fd.cFileName + ln - lk, keep))
                    {
                        string_path p;
                        xr_strcpy(p, dir);
                        xr_strcat(p, fd.cFileName);
                        DeleteFileA(p);
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
        for (Job& j : jobs)
        {
            const u64 parts[] = {g_data.species_hash[j.species], j.skel->hash, j.kind, u64(j.bucket), u64(j.leaf_cap), u64(kGenVer),
                u64(sizeof(CacheHeader))};
            j.hash = fnv(parts, sizeof(parts));
            cache_path(j.path, j.kind, g_data.species[j.species].name.c_str(), j.hash);
        }
        // (Files and arithmetic only: the loading screen keeps being drawn meanwhile.)
        g_pGamePersistent->LoadInBackground([&jobs] {
            xr_parallel_for(TaskRange<size_t>(0, jobs.size(), 1), [&jobs](const TaskRange<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                {
                    Job& j = jobs[i];
                    if ((j.cached = cache_read(j.path, j.hash, j.meta, j.elems, j.axes, j.aux, j.leaves)))
                    {
                        j.ok = true;
                        continue;
                    }
                    // (A torn or stale file may have filled part of them.)
                    j.elems.clear();
                    j.axes.clear();
                    j.aux.clear();
                    da_shrub::Template T;
                    da_shrub::generate_on_skeleton(g_data.species[j.species].arch, j.skel->S, j.seed, j.up, j.leaf_cap, T);
                    pack_tree(T, j.meta, j.elems, j.axes, j.aux, j.seed);
                    j.leaves = u32(T.leaves.size());
                    j.ok = true;
                    cache_write(j.path, j.hash, j.meta, j.elems, j.axes, j.aux, j.leaves);
                }
            });
        });
        // One element, axis and aux buffer for the level's trees.
        xr_vector<u32> elems, axes, aux;
        templates.assign(jobs.size(), Template{});
        u32 cached = 0;
        double e_area[kTreeSpecies]{}, e_count[kTreeSpecies]{};
        for (u32 i = 0; i < jobs.size(); ++i)
        {
            const Job& j = jobs[i];
            Template& T = templates[i];
            T = j.meta;
            T.species = kShrubSpecies + j.species;
            T.kind = j.kind;
            T.bucket = j.bucket;
            T.axis_first = u32(axes.size() / 8);
            T.axis_count = u32(j.axes.size() / 8);
            T.aux_first = u32(aux.size() / 8);
            const u32 base = u32(elems.size() / 8);
            for (u32 k = 0; k < kKinds; ++k)
                T.first[k] += base;
            elems.insert(elems.end(), j.elems.begin(), j.elems.end());
            axes.insert(axes.end(), j.axes.begin(), j.axes.end());
            aux.insert(aux.end(), j.aux.begin(), j.aux.end());
            cached += j.cached ? 1u : 0u;
            e_area[j.species] += T.leaf_area;
            e_count[j.species] += double(j.leaves);
        }
        for (u32 s = 0; s < kTreeSpecies; ++s)
            elem_area[s] = e_count[s] > 0.0 ? float(e_area[s] / e_count[s]) : 0.f;
        // (Every stock ring and patch vertex a tube or a patch names lies inside its template's aux range, finite and
        // within reach of its box: a stale record would throw a vertex across the sky. A template that fails leaves its
        // trees stock.)
        {
            u32 checked = 0, bad = 0, bad_patch = 0;
            xr_vector<u8> rejected(jobs.size(), 0);
            const auto finite_in = [](const u32* w, u32 n, float lim) {
                for (u32 k = 0; k < n; ++k)
                    if (const float v = std::bit_cast<float>(w[k]); !std::isfinite(v) || std::abs(v) > lim)
                        return false;
                return true;
            };
            for (u32 i = 0; i < jobs.size(); ++i)
            {
                const Template& T = templates[i];
                const u32 slots = u32(jobs[i].aux.size() / 8);
                const float lim = 20.f + 4.f * std::max({std::abs(T.lo.x), std::abs(T.lo.y), std::abs(T.lo.z), std::abs(T.hi.x),
                    std::abs(T.hi.y), std::abs(T.hi.z)});
                const auto report = [&](const char* what, u32 e, u32 slot) {
                    rejected[i] = 1;
                    if (bad++ >= 8)
                        return;
                    const auto f = [&](u32 k) {
                        return size_t(slot) * 8 + k < size_t(slots) * 8 ? std::bit_cast<float>(aux[size_t(T.aux_first + slot) * 8 + k]) : 0.f;
                    };
                    Msg("! [trees] check: template %u (kind %016llx, %s) %s element %u names aux slot %u of %u: %g %g %g | %g | %g %g %g %g %g %g",
                        i, T.kind, g_data.species[jobs[i].species].name.c_str(), what, e, slot, slots, f(0), f(1), f(2), f(6), f(8), f(9),
                        f(10), f(11), f(12), f(13));
                };
                // (The header's parts inside the template's slots, the junction table's records among the records.)
                const u32* H = slots ? &aux[size_t(T.aux_first) * 8] : nullptr;
                const auto within = [slots](u32 first, u64 n) { return first <= slots && u64(first) + n <= slots; };
                if (!H || !within(H[0], H[1]) || !within(H[2], (u64(H[1]) + 7) / 8 + (u64(H[1]) * 2 + 7) / 8) || !within(H[3], 0) || !within(H[4], (u64(H[5]) + 15) / 16) ||
                    !within(H[6], u64(H[7]) * 2))
                    report("header", 0, 0);
                else
                    for (u32 k = 0; k < (H[5] + 1) / 2; ++k)
                        if (const u32 w = aux[size_t(T.aux_first + H[4]) * 8 + k]; (w & 0xFFFFu) > H[7] || (w >> 16) > H[7])
                        {
                            report("junction table", k, H[4]);
                            break;
                        }
                for (u32 e = T.first[kKindStock]; e < T.first[kKindStock] + T.count[kKindStock]; ++e, ++checked)
                {
                    for (const u32 k : {4u, 5u})
                    {
                        const u32 slot = elems[size_t(e) * 8 + k];
                        const u32* r = slot + 1 < slots ? &aux[size_t(T.aux_first + slot) * 8] : nullptr;
                        if (!r || !finite_in(r, 3, lim) || !finite_in(r + 6, 1, 50.f) || !finite_in(r + 8, 6, 8.f))
                            report("stock", e - T.first[kKindStock], slot);
                    }
                    if (const u32 js = elems[size_t(e) * 8 + 7] >> 8; js && (!H || js < H[6] || js + 2 > H[6] + 2 * H[7] || (js - H[6]) % 2))
                        report("stock (junction)", e - T.first[kKindStock], js);
                }
                for (u32 e = T.first[kKindPatch]; e < T.first[kKindPatch] + T.count[kKindPatch] + T.patch_far; ++e, ++checked)
                {
                    for (const u32 k : {4u, 5u, 6u})
                    {
                        const u32 slot = elems[size_t(e) * 8 + k];
                        const u32* v = slot < slots ? &aux[size_t(T.aux_first + slot) * 8] : nullptr;
                        if (!v || !finite_in(v, 3, lim))
                        {
                            ++bad_patch;
                            report("patch", e - T.first[kKindPatch], slot);
                        }
                    }
                    // (The element's own position is its first vertex's: a slot off by any amount shows here.)
                    const u32 s0 = elems[size_t(e) * 8 + 4];
                    if (s0 < slots)
                        for (u32 k = 0; k < 3; ++k)
                            if (std::abs(std::bit_cast<float>(aux[size_t(T.aux_first + s0) * 8 + k]) -
                                    std::bit_cast<float>(elems[size_t(e) * 8 + k])) > 1e-4f)
                            {
                                ++bad_patch;
                                report("patch (first vertex moved)", e - T.first[kKindPatch], s0);
                                break;
                            }
                }
            }
            u32 left_stock = 0;
            for (Placement& P : placements)
                if (P.tmpl != ~0u && P.tmpl < rejected.size() && rejected[P.tmpl])
                {
                    P.tmpl = ~0u;
                    ++left_stock;
                }
            Msg("%c [trees] check: %u stock and patch elements, %u bad references (%u of patches); %u templates rejected, %u trees left "
                "stock",
                bad ? '!' : '*', checked, bad, bad_patch, u32(std::count(rejected.begin(), rejected.end(), u8(1))), left_stock);
        }
        tables_dirty();
        // (A record carries its template's first aux slot in 24 bits.)
        if (aux.size() / 8 >= (1u << 24))
            return false;
        if (!make_buffer(elems_b, u32(elems.size() * 4), elems.data()) || !make_buffer(axes_b, u32(axes.size() * 4), axes.data()) ||
            !make_buffer(aux_b, u32(std::max<size_t>(aux.size(), 8) * 4), aux.empty() ? nullptr : aux.data()))
            return false;
        Msg("* [trees] %u templates (%u from the disk cache), %u KB of elements, %u KB of axes, %u KB of rings and patches",
            u32(templates.size()), cached, u32(elems.size() * 4) >> 10, u32(axes.size() * 4) >> 10, u32(aux.size() * 4) >> 10);
        g_pGamePersistent->LoadPump();
        // The instances: the stock placement's transform as it is.
        instances.clear();
        max_height = 0.f;
        for (u32 pi = 0; pi < placements.size(); ++pi)
        {
            const Placement& P = placements[pi];
            if (P.tmpl == ~0u)
                continue;
            const Template& T = templates[P.tmpl];
            Instance I;
            I.tmpl = P.tmpl;
            I.species = kShrubSpecies + u32(P.species);
            I.placement = pi;
            const Fmatrix& m = P.xf;
            const float rows[3][4] = {{m.i.x, m.j.x, m.k.x, m.c.x}, {m.i.y, m.j.y, m.k.y, m.c.y}, {m.i.z, m.j.z, m.k.z, m.c.z}};
            CopyMemory(I.row, rows, sizeof(rows));
            I.scale = std::cbrt(std::abs(det3(m)));
            I.scale_h = I.scale_w = I.scale;
            I.hemi = P.hemi;
            I.sun = P.sun;
            const u32 hp = pos_hash(m.c);
            I.seed = u16(hp >> 16);
            I.pheno = s8(std::lround((hash01(hp ^ 0x9e3779b9u) * 2.f - 1.f) * 127.f));
            I.dead = P.dead;
            I.red = P.red;
            // A young tree's spring lead: its youth's share of the policy's days, each tree its own share of it.
            I.youth = P.youth;
            I.lead = u8(std::lround(255.f * P.youth * (0.6f + 0.4f * hash01(hp ^ 0x68e31da4u))));
            I.wind = P.wind;
            I.tree_h = P.top;
            I.climb_top = T.climb_top > 0.f ? T.climb_top * m.j.y : 0.f;
            // The ground round the trunk: the median of the level's static ground at four points a hand past its widest
            // ring, within three metres of its root, past the trees' own collision (a low limb's over the ground).
            {
                const float rr = T.stock_r * I.scale + 0.3f;
                float hit[4];
                u32 hits = 0;
                for (u32 k = 0; k < 4; ++k)
                {
                    const float a = 1.5707963f * float(k) + 0.785398f;
                    if (const float y = ground_at(m.c.x + rr * std::cos(a), m.c.z + rr * std::sin(a), m.c.y + 3.f, 6.f, flt_max, true); y < flt_max)
                        hit[hits++] = y;
                }
                std::sort(hit, hit + hits);
                const float mid = hits ? 0.5f * (hit[(hits - 1) / 2] + hit[hits / 2]) : m.c.y;
                I.ground = std::clamp(mid - m.c.y, -2.f, 3.f);
            }
            // The world box of the template's box.
            I.box.invalidate();
            for (u32 c = 0; c < 8; ++c)
            {
                const Fvector q{(c & 1) ? T.hi.x : T.lo.x, (c & 2) ? T.hi.y : T.lo.y, (c & 4) ? T.hi.z : T.lo.z};
                Fvector w;
                m.transform_tiny(w, q);
                I.box.modify(w);
            }
            Fvector centre;
            I.box.getcenter(centre);
            I.bound.set(centre, I.box.vMin.distance_to(I.box.vMax) * 0.5f);
            max_height = std::max(max_height, I.box.vMax.y - m.c.y);
            instances.push_back(I);
        }
        return true;
    }

    // The billboards: each template's eight cells in the module's atlas (a row of eight a template), its facets' stock
    // cells and their quads in the template's model space (from its first placement's FLOD), the atlas pair made.
    bool build_billboards()
    {
        bb.assign(templates.size(), BbCell{});
        bb_dead.clear();
        if (templates.empty())
            return true;
        // The look the bake gives each template (da_trees_bake.cpp): its living trees' mean youth, the Red Forest's
        // needles, and the mean of their records' hemisphere light over their facets' vertex light (lod.vs scales the
        // atlas's by the latter); its dead trees' the same - the template's own look when every one stands dead, else
        // its second cell set's.
        xr_vector<u32> n_tree(templates.size(), 0), n_dead(templates.size(), 0);
        for (const Placement& P : placements)
        {
            if (P.tmpl == ~0u)
                continue;
            BbCell& B = bb[P.tmpl];
            float vh = 0.f;
            for (const FLOD::_face& F : static_cast<const FLOD*>(P.flod)->facets)
                for (const FLOD::_vertex& v : F.v)
                    vh += float(color_get_A(v.c_rgb_hemi)) / (255.f * 32.f);
            const float h = P.hemi / std::max(vh, 0.05f);
            ++n_tree[P.tmpl];
            if (P.dead)
            {
                B.dead_hemi += h;
                B.dead_youth += P.youth;
                ++n_dead[P.tmpl];
            }
            else
            {
                B.hemi += h;
                B.youth += P.youth;
                B.red = B.red || P.red;
            }
        }
        for (u32 t = 0; t < bb.size(); ++t)
        {
            BbCell& B = bb[t];
            const u32 live = n_tree[t] - n_dead[t];
            B.dead_hemi /= float(std::max(n_dead[t], 1u));
            B.dead_youth /= float(std::max(n_dead[t], 1u));
            B.dead_look = n_tree[t] && !live;
            B.hemi = B.dead_look ? B.dead_hemi : B.hemi / float(std::max(live, 1u));
            B.youth = B.dead_look ? B.dead_youth : B.youth / float(std::max(live, 1u));
        }
        // The cell sets, a row of eight cells each: the templates' own, then the dead looks (the halved cell for every row
        // when they do not fit 4096 lines; the dead looks the first to go past the room).
        u32 sets = u32(templates.size());
        for (u32 t = 0; t < bb.size(); ++t)
            sets += n_dead[t] && !bb[t].dead_look ? 1u : 0u;
        bb_cell = kBbCell;
        u32 per_row = kBbWidth / (8 * bb_cell);
        u32 rows = (sets + per_row - 1) / per_row;
        if (rows * bb_cell > 4096)
        {
            bb_cell = kBbCell / 2;
            per_row = kBbWidth / (8 * bb_cell);
            rows = std::min((sets + per_row - 1) / per_row, 4096u / bb_cell);
        }
        const u32 room = per_row * (4096 / bb_cell);
        for (u32 t = 0; t < bb.size(); ++t)
            if (n_dead[t] && !bb[t].dead_look && u32(templates.size() + bb_dead.size()) < room)
            {
                bb[t].dead_set = u32(templates.size() + bb_dead.size());
                bb_dead.push_back(t);
            }
        bb_w = kBbWidth;
        bb_h = std::max(rows * bb_cell, bb_cell);
        // (The gutter round a facet's rect: the bake draws the trees beyond the facet's edges into it, so the filtering and
        // the mips never reach the next cell.)
        const float gutter = float(bb_cell / 16);
        for (u32 pi = 0; pi < placements.size(); ++pi)
        {
            const Placement& P = placements[pi];
            if (P.tmpl == ~0u || bb[P.tmpl].placement || P.tmpl >= room)
                continue;
            BbCell& B = bb[P.tmpl];
            B.placement = pi + 1;
            const FLOD* flod = static_cast<const FLOD*>(P.flod);
            const u32 row = P.tmpl / per_row, col = (P.tmpl % per_row) * 8;
            for (u32 f = 0; f < 8; ++f)
            {
                BbView& V = B.view[f];
                // (The facet's uv maps edge to edge onto its rect; the copy fills every texel of it.)
                const float x0 = float((col + f) * bb_cell) + gutter, y0 = float(row * bb_cell) + gutter, s = float(bb_cell) - 2.f * gutter;
                V.rect.set(x0 / float(bb_w), y0 / float(bb_h), (x0 + s) / float(bb_w), (y0 + s) / float(bb_h));
                const FLOD::_face& F = flod->facets[f];
                V.stock.set(1e9f, 1e9f, -1e9f, -1e9f);
                for (u32 k = 0; k < 4; ++k)
                {
                    V.stock.set(std::min(V.stock.x, F.v[k].t.x), std::min(V.stock.y, F.v[k].t.y), std::max(V.stock.z, F.v[k].t.x),
                        std::max(V.stock.w, F.v[k].t.y));
                    V.corner[k] = model_point(P.xf, F.v[k].v);
                }
                // (Each corner's uv as render_extra maps it.)
                for (u32 k = 0; k < 4; ++k)
                    V.uv[k].set(V.rect.x + (F.v[k].t.x - V.stock.x) / std::max(V.stock.z - V.stock.x, 1e-6f) * (V.rect.z - V.rect.x),
                        V.rect.y + (F.v[k].t.y - V.stock.y) / std::max(V.stock.w - V.stock.y, 1e-6f) * (V.rect.w - V.rect.y));
                V.normal = model_dir(P.xf, F.N);
                V.normal.normalize_safe();
            }
            // (The dead look's cells: the same facets, in their own row.)
            if (B.dead_set != ~0u)
            {
                const float du = (float((B.dead_set % per_row) * 8) - float(col)) * float(bb_cell) / float(bb_w);
                const float dv = (float(B.dead_set / per_row) - float(row)) * float(bb_cell) / float(bb_h);
                for (u32 f = 0; f < 8; ++f)
                {
                    BbView& D = B.dead_view[f];
                    D = B.view[f];
                    D.rect.set(D.rect.x + du, D.rect.y + dv, D.rect.z + du, D.rect.w + dv);
                    for (Fvector2& t : D.uv)
                        t.set(t.x + du, t.y + dv);
                }
            }
            // The stock atlas pair the facets drew with (the level folder's level_lods and its _nm).
            if (flod->shader && flod->shader->E[SE_R1_LMODELS])
                for (const ref_pass& pass : flod->shader->E[SE_R1_LMODELS]->passes)
                {
                    if (!pass || !pass->T)
                        continue;
                    for (const auto& [stage, tex] : *pass->T)
                    {
                        if (!tex)
                            continue;
                        pcstr n = tex->cName.c_str();
                        const size_t len = n ? xr_strlen(n) : 0;
                        if (len > 3 && !xr_stricmp(n + len - 3, "_nm"))
                            B.stock_nm = tex;
                        else if (len)
                            B.stock_d = tex;
                    }
                    break;
                }
        }
        // (With the bake the pair has a cell's chain of levels, down to its texel: the bake writes them.)
        Flags32 mips{};
        if (da_trees_bake_available())
            mips.assign(CRT::CreateMips | (bb_mips() << CRT::MipsShift));
        bb_d.create(kBbAlbedo, bb_w, bb_h, D3DFMT_A8R8G8B8, 1, 1, mips);
        bb_nm.create(kBbNormal, bb_w, bb_h, D3DFMT_A8R8G8B8, 1, 1, mips);
        if (!bb_d || !bb_d->valid() || !bb_nm || !bb_nm->valid())
            return false;
        // The facets draw with the stock billboard shaders (lod.vs; da_tree_lod.ps: lod.ps writing the foliage material of
        // the near tiers from the bake's foliage atlas) and states (r3\details_lod.s l_special) on the module's atlas pair;
        // the stock cells are copied into it by da_tree_bb_copy.
        static class CBlender_da_tree_bb : public IBlender
        {
        public:
            LPCSTR getComment() override { return "INTERNAL: trees, billboards"; }
            BOOL canBeDetailed() override { return FALSE; }
            BOOL canBeLMAPped() override { return FALSE; }
            void Compile(CBlender_Compile& C) override
            {
                IBlender::Compile(C);
                if (C.iElement > 0)
                    return;
                C.r_Pass("lod", "da_tree_lod", FALSE, TRUE, TRUE);
                C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
                C.r_StencilRef(0x01);
                C.r_dx11Texture("s_base", kBbAlbedo);
                C.r_dx11Texture("s_hemi", kBbNormal);
                C.r_dx11Texture("s_tree_mtl", kBbMaterial);
                C.r_dx11Sampler("smp_base");
                C.r_dx11Sampler("smp_linear");
                C.r_End();
            }
        } b_bb;
        static class CBlender_da_tree_bb_copy : public IBlender
        {
        public:
            LPCSTR getComment() override { return "INTERNAL: trees, billboard atlas"; }
            BOOL canBeDetailed() override { return FALSE; }
            BOOL canBeLMAPped() override { return FALSE; }
            void Compile(CBlender_Compile& C) override
            {
                IBlender::Compile(C);
                if (C.iElement > 0)
                    return;
                C.r_Pass("da_tree_bb_copy", "da_tree_bb_copy", FALSE, FALSE, FALSE);
                C.r_dx11Sampler("smp_rtlinear");
                C.r_CullMode(D3DCULL_NONE);
                C.r_End();
            }
        } b_copy;
        s_bb.create(&b_bb, "r2" DELIMITER "da_tree_bb");
        s_bb_copy.create(&b_copy, "r2" DELIMITER "da_tree_bb_copy");
        bb_ready = s_bb && s_bb->E[0] && s_bb_copy && s_bb_copy->E[0];
        return bb_ready;
    }

    // The stock atlas a facet drew with, as the stock billboards would bind it (loaded, its DirectStorage upload done).
    static ID3DShaderResourceView* stock_srv(const ref_texture& t)
    {
        if (!t)
            return nullptr;
        if (!t->flags.bLoaded)
            t->Load();
        else if (t->ds_pending())
            t->ds_finish();
        return t->get_SRView();
    }

    // The stock cells copied into the templates' cells (at the level's first frame, again after a device reset lost the
    // atlas) until WP5's bake fills them from the trees themselves: on the immediate context before the G-buffer, the
    // scene's targets and viewport set back after.
    void pre_render(CBackend& cmd) override
    {
        // (The bake draws the cells from the trees themselves, after the bends: da_trees_bake.cpp.)
        if (!bb_ready || !bb_d || !bb_d->valid() || da_trees_bake_owns_atlas())
            return;
        // (A device reset asks for a history reset on its first frame drawn - the atlas's surfaces were made again -,
        // and so does a change of the season's texture folders: the stock cells again, every template to bake again.)
        if (da_history_reset())
            bb_cleared = false;
        // (The dead looks' cells stay empty without the bake: those trees keep their blobs.)
        if (!bb_cleared)
            for (BbCell& B : bb)
            {
                B.filled = B.dead_filled = false;
                B.dirty = B.dead_dirty = true;
            }
        xr_vector<u32> todo;
        for (u32 t = 0; t < bb.size(); ++t)
        {
            const BbCell& B = bb[t];
            if (!B.filled && B.placement && stock_srv(B.stock_d) && stock_srv(B.stock_nm))
                todo.push_back(t);
        }
        if (todo.empty())
            return;
        RImplementation.Target->u_setrt(cmd, bb_d, bb_nm, (ID3DDepthStencilView*)nullptr);
        cmd.SetViewport({0.f, 0.f, float(bb_w), float(bb_h), 0.f, 1.f});
        if (!bb_cleared)
        {
            cmd.ClearRT(bb_d, Fcolor{0.f, 0.f, 0.f, 0.f});
            cmd.ClearRT(bb_nm, Fcolor{0.5f, 0.5f, 1.f, 0.f});
            bb_cleared = true;
        }
        // One draw per stock atlas pair (a level has one): each cell a quad, its destination in clip space and its source.
        while (!todo.empty())
        {
            const ref_texture d = bb[todo.front()].stock_d, nm = bb[todo.front()].stock_nm;
            xr_vector<float> list;
            xr_vector<u32> rest;
            for (const u32 t : todo)
            {
                BbCell& B = bb[t];
                if (B.stock_d != d || B.stock_nm != nm)
                {
                    rest.push_back(t);
                    continue;
                }
                for (const BbView& V : B.view)
                    list.insert(list.end(), {V.rect.x * 2.f - 1.f, 1.f - V.rect.y * 2.f, V.rect.z * 2.f - 1.f, 1.f - V.rect.w * 2.f,
                                                V.stock.x, V.stock.y, V.stock.z, V.stock.w});
                B.filled = true;
            }
            const u32 n = u32(list.size() / 8);
            if (n && make_buffer(bb_list_b, u32(list.size() * 4), list.data()))
            {
                cmd.set_Element(s_bb_copy->E[0]);
                cmd.set_c("da_tree_bb0", float(n), 0.f, 0.f, 0.f);
                cmd.SRVSManager.SetVSResource(0, bb_list_b.srv);
                cmd.SRVSManager.SetPSResource(0, stock_srv(d));
                cmd.SRVSManager.SetPSResource(1, stock_srv(nm));
                draw_quads(cmd, n);
                cmd.SRVSManager.SetVSResource(0, nullptr);
                cmd.SRVSManager.SetPSResource(0, nullptr);
                cmd.SRVSManager.SetPSResource(1, nullptr);
                cmd.SRVSManager.Apply(cmd.context_id);
            }
            todo.swap(rest);
        }
        RImplementation.Target->u_setrt_scene_base(cmd);
        RImplementation.rmNormal(cmd);
    }

    // The far billboards in the G-buffer: each tree's two stock facets nearest the view, blended as the stock billboards
    // are (r__dsgraph_render_lods.cpp) - shifted half a radius toward the eye, the billboard's alpha its band's and the
    // far plane's dissolve, its uv on its template's cells -, near to far.
    void render_extra(CBackend& cmd) override
    {
        bb_drawn = 0;
        if (bb_frame.empty() || !bb_ready || (ps_r__dbg_trees & 256))
            return;
        dx11GpuTimerScope gpu(dx11GpuTimers::TreesBb);
        std::sort(bb_frame.begin(), bb_frame.end(), [](const BbItem& a, const BbItem& b) { return a.d < b.d; });
        const Latch& L = latch();
        const FLOD* first = static_cast<const FLOD*>(placements[instances[bb_frame.front().inst].placement].flod);
        const u32 stride = first->geom->vb_stride;
        const u32 fit = RImplementation.Vertex.GetSize() / (stride * 4);
        cmd.set_xform_world(Fidentity);
        for (size_t at = 0; at < bb_frame.size();)
        {
            const u32 n = u32(std::min<size_t>(bb_frame.size() - at, fit));
            u32 offset = 0;
            auto* V = static_cast<FLOD::_hw*>(RImplementation.Vertex.Lock(n * 4, stride, offset));
            for (u32 j = 0; j < n; ++j, ++at)
            {
                const BbItem& item = bb_frame[at];
                const Instance& I = instances[item.inst];
                const FLOD* lod = static_cast<const FLOD*>(placements[I.placement].flod);
                const BbCell& B = bb[I.tmpl];
                // (A tree standing dead among living ones on its template's dead look.)
                const BbView* views = I.dead && !B.dead_look ? B.dead_view : B.view;
                Fvector dir, shift;
                dir.sub(lod->vis.sphere.P, L.cam);
                const float dist = dir.magnitude();
                dir.div(std::max(dist, EPS_S));
                shift.mul(dir, -0.5f * lod->vis.sphere.R);
                float alpha = item.alpha;
                if (L.far_plane > 1.f)
                    alpha *= std::clamp((L.far_plane * 0.97f - dist) / (L.far_plane * 0.12f), 0.f, 1.f);
                const u32 uA = u32(std::clamp(iFloor(alpha * 255.f), 0, 255));
                u32 best = 0, next = 1;
                float d_best = -2.f, d_next = -2.f, d_third = -2.f;
                for (u32 f = 0; f < 8; ++f)
                {
                    const float dd = dir.dotproduct(lod->facets[f].N);
                    if (dd > d_best)
                    {
                        d_third = d_next;
                        next = best;
                        d_next = d_best;
                        best = f;
                        d_best = dd;
                    }
                    else if (dd > d_next)
                    {
                        d_third = d_next;
                        next = f;
                        d_next = dd;
                    }
                    else if (dd > d_third)
                        d_third = dd;
                }
                const float factor = 0.5f + 0.5f * (1.f - (d_next - d_third) / std::max(d_best - d_third, 1e-5f));
                const u32 uF = u32(std::clamp(iFloor(factor * 255.5f), 0, 255));
                const FLOD::_face& FA = lod->facets[best];
                const FLOD::_face& FB = lod->facets[next];
                // (A facet's uv from its own stock cell onto its template's: another compile of the kind may have its
                // cells elsewhere in the level's atlas.)
                const auto remap = [views](u32 f, const FLOD::_face& F, const Fvector2& t) {
                    float u0 = F.v[0].t.x, u1 = u0, v0 = F.v[0].t.y, v1 = v0;
                    for (const FLOD::_vertex& c : F.v)
                    {
                        u0 = std::min(u0, c.t.x);
                        u1 = std::max(u1, c.t.x);
                        v0 = std::min(v0, c.t.y);
                        v1 = std::max(v1, c.t.y);
                    }
                    const BbView& Vw = views[f];
                    return Fvector2{Vw.rect.x + (t.x - u0) / std::max(u1 - u0, 1e-6f) * (Vw.rect.z - Vw.rect.x),
                        Vw.rect.y + (t.y - v0) / std::max(v1 - v0, 1e-6f) * (Vw.rect.w - Vw.rect.y)};
                };
                for (const int id : {3, 0, 2, 1})
                {
                    V->p0.add(FB.v[id].v, shift);
                    V->p1.add(FA.v[id].v, shift);
                    V->n0 = FB.N;
                    V->n1 = FA.N;
                    V->sun_af = color_rgba(FB.v[id].c_sun, FA.v[id].c_sun, uA, uF);
                    V->t0 = remap(next, FB, FB.v[id].t);
                    V->t1 = remap(best, FA, FA.v[id].t);
                    V->rgbh0 = FB.v[id].c_rgb_hemi;
                    V->rgbh1 = FA.v[id].c_rgb_hemi;
                    ++V;
                }
            }
            RImplementation.Vertex.Unlock(n * 4, stride);
            cmd.set_Element(s_bb->E[0]);
            // (The foliage codes' dither turns with the frame as the near tiers' does: da_tree_lod.ps.)
            cmd.set_c("da_tree_lod", float(L.motion ? Device.dwFrame & 63u : 0u), 0.f, 0.f, 0.f);
            // (The billboards' sway takes the trees' frame-wide wave and wind: no stock tree may have set them.)
            FTreeVisual::SetupInstancedGlobals(cmd);
            cmd.set_Geometry(&*first->geom);
            cmd.Render(D3DPT_TRIANGLELIST, offset, 0, n * 4, 0, n * 2);
            bb_drawn += n;
        }
    }

    // ---- Lifecycle ----
    bool prepare(bool shaders, bool own) override
    {
        prepared = false;
        data_reload();
        if (!g_data.file)
        {
            Msg("* [trees] dead_air_x64_trees.ltx not found: the stock trees stand");
            return false;
        }
        if (!shaders || !own)
        {
            Msg("! [trees] the upgraded trees are off on this level (the shaders or the textures are missing): the stock trees "
                "stand");
            failed = true;
            return false;
        }
        dx11GpuTimers::RegisterField("trees_cpu", [] {
            Population* P = population(kPopTrees);
            const u64 ticks = P ? P->cpu_ticks.exchange(0, std::memory_order_relaxed) : 0;
            const u32 frames = P ? P->cpu_frames.exchange(0, std::memory_order_relaxed) : 0;
            return frames ? float(double(ticks) * 1000.0 / double(CPU::qpc_freq) / frames) : 0.f;
        });
        t0 = CPU::QPC();
        collect_placements();
        pick_species();
        if (std::none_of(placements.begin(), placements.end(), [](const Placement& P) { return P.species >= 0; }))
        {
            Msg("* [trees] no stock tree of this level grows a tree of the data (%u placements)", u32(placements.size()));
            placements.clear();
            return false;
        }
        for (const Placement& P : placements)
            if (P.species >= 0)
                census().candidates[P.candidate].taken = kPopTrees;
        prepared = true;
        return true;
    }

    bool rebuild()
    {
        loaded = active = false;
        if (!build())
            return false;
        if (!build_billboards())
            Msg("! [trees] the billboard atlas could not be made: the far trees end at their blobs");
        build_grid(*this);
        tables_dirty();
        loaded = active = !instances.empty();
        return true;
    }

    void finish_load() override
    {
        prepared = false;
        if (!rebuild())
        {
            Msg("! [trees] the upgraded trees are off on this level (the template buffers could not be made): the stock trees "
                "stand");
            release();
            failed = true;
            return;
        }
        if (wanted())
            set_detached(true, false);
        u32 elems = 0;
        for (const Template& T : templates)
            for (u32 k = 0; k < kKinds; ++k)
                elems += T.count[k];
        Msg("* [trees] %u trees of %u placements, %u templates, %u elements (%u KB), billboard atlas %ux%u (%u px cells), %.0f ms",
            u32(instances.size()), u32(placements.size()), u32(templates.size()), elems,
            (elems_b.bytes + axes_b.bytes + aux_b.bytes) >> 10, bb_w, bb_h, bb_cell, float(CPU::QPC() - t0) * 1000.f / float(CPU::qpc_freq));
    }

    // The stock trees the trees replace leave (or rejoin) their parent hierarchy, whole: every consumer of the sector
    // hierarchies is rid of them; their parts stay alive (the wind roots, the ray tracing stand-ins).
    void set_detached(bool detach, bool announce) override
    {
        if (detached == detach)
            return;
        replaced.clear();
        stand.clear();
        for (u32 i = 0; i < instances.size(); ++i)
        {
            const Placement& P = placements[instances[i].placement];
            if (!P.flod || !P.parent)
                continue;
            auto& children = P.parent->children;
            const auto it = std::find(children.begin(), children.end(), P.flod);
            if (detach)
            {
                if (it != children.end())
                    children.erase(it);
                replaced.insert(P.flod);
                stand.emplace(P.flod, i);
                for (const dxRender_Visual* part : static_cast<FHierrarhyVisual*>(P.flod)->children)
                    stand.emplace(part, i);
            }
            else if (it == children.end())
                children.push_back(P.flod);
        }
        detached = detach;
        if (announce)
            Msg("* [trees] %s", detached ? "the upgraded trees stand in for the stock trees" : "the stock trees are back (QA)");
    }

    void release() override
    {
        set_detached(false, false);
        elems_b.release();
        axes_b.release();
        aux_b.release();
        bb_list_b.release();
        s_bb.destroy();
        s_bb_copy.destroy();
        bb_d.destroy();
        bb_nm.destroy();
        bb.clear();
        bb_dead.clear();
        bb_frame.clear();
        bb_ready = bb_cleared = false;
        bb_w = bb_h = bb_cell = 0;
        templates.clear();
        instances.clear();
        cells.clear();
        cell_items.clear();
        placements.clear();
        skels.clear();
        replaced.clear();
        stand.clear();
        frame_slot.clear();
        for (auto& pass : pass_chunks)
            for (xr_vector<ChunkRec>& list : pass)
                list.clear();
        for (auto& pass : draw_count)
            std::fill(std::begin(pass), std::end(pass), 0u);
        std::fill(std::begin(elem_area), std::end(elem_area), 0.f);
        loaded = active = detached = failed = prepared = false;
        max_height = 0.f;
    }

    // ---- The frame ----
    // The trunk's sway on the stock root's oscillator now and a frame back, the tree's height and climbable top, the
    // template's first aux slot and the tree's youth (record words 20..23).
    void record_tail(u32 i, u32* w) const override
    {
        const Instance& I = instances[i];
        const Latch& L = latch();
        Fvector2 now{}, prev{};
        if (I.wind)
        {
            const FTreeVisualWind s = I.wind->wind_state();
            const Fvector root{I.row[0][3], I.row[1][3], I.row[2][3]};
            now = trunk_push(s.now, L.tree_wave, L.tree_wind, L.wind_field, root, s.gate);
            prev = trunk_push(s.previous, L.tree_wave_prev, L.tree_wind_prev, L.wind_field_prev, root, s.gate);
        }
        w[0] = h2(now.x, now.y);
        w[1] = h2(prev.x, prev.y);
        w[2] = h2(I.tree_h, I.climb_top);
        w[3] = templates[I.tmpl].aux_first | (u8f(I.youth) << 24);
    }

    // The tier distances of instance i (its species' leaf on the screen): the hulls end at d[0], the quads at d[1], the
    // thinned quads at d[2], the clusters at d[3]; a bare tree keeps its finer tiers a third longer (its twigs).
    void tiers(const Instance& I, const Template& T, bool leaves, float (&d)[4]) const
    {
        const Latch& L = latch();
        const Species& S = g_data.species[I.species - kShrubSpecies];
        const bool spray = S.arch.leaf_kind == da_shrub::lkSpray;
        const float leaf_m = (spray ? 0.5f * (S.arch.leaf_len[0] + S.arch.leaf_len[1]) : T.leaf_len) * I.scale;
        const float k = leaf_m * L.px_per_m * lod * (leaves ? 1.f : 1.35f);
        d[0] = std::clamp(k / cols.hull_px, 0.5f, std::max(R * 0.05f, 1.f));
        d[1] = std::clamp(k / cols.quad_px, d[0] * 1.2f, std::max(R * 0.15f, d[0] * 1.2f));
        d[2] = std::clamp(k / cols.leaf_px, d[1] * 1.2f, std::max(R * 0.35f, d[1] * 1.2f));
        d[3] = std::clamp(k / cols.clu_px, d[2] * 1.2f, std::max(R * 0.8f, d[2] * 1.2f));
    }

    // The elements [b, e) of a wood set of n (its keys falling, rq its table: WoodCut) by the sides each needs: the part
    // over the radius r[0] into d[0], the next ones over r[1], r[2] into d[1], d[2], the rest into d[3] (template metres; a
    // key is no less than its element's radius, so the elements past a key are thinner for sure). r all 0: all into d[0].
    // (The table's positions are shares of the whole set: read against a prefix's count, the splits fell short and thick
    // limbs took the sides of thin ones.)
    void emit_sides(u32 pass, const Draw (&d)[4], const float (&r)[3], u32 rec, u32 first, u32 n, u32 b, u32 e, const float (&rq)[kRq],
        float lo, float hi, u32 flags = 0, float keep = 1.f, float aux = 0.f)
    {
        u32 at = b;
        for (u32 k = 0; k < 4; ++k)
        {
            const u32 end = k < 3 ? std::clamp(key_prefix(rq, n, r[k]), at, e) : e;
            emit(*this, pass, d[k], rec, first + at, end - at, keep, lo, hi, flags, aux);
            at = end;
        }
    }

    // The stock wood of a level of detail and its patches into pass `pass`, each set's prefix over its floor (WoodCut;
    // half_px: the pass's half pixel or texel over the tree's scale - the tubes' wire rule, the patches under half of it
    // none: a limb under half a pixel across) (hi: the crossfade's upper end; tess: the patches tessellated - near the
    // trunk -, else as the stock has them), the tubes by their sides' radii (d, r: emit_sides), the patches under r[2] the
    // far wood's; morph: the tubes' share of the way onto the next level's (their chunks' aux), flat: the tessellation's
    // lift eased out (1 the stock's facets). The far level (2) is its tubes alone, whole along the axes and round them
    // (kChunkFar: no step back under a patch) - a far trunk's patches were a thousand triangles a tree for a few pixels.
    void emit_stock(u32 pass, u32 rec, const Template& T, u32 level, const Draw (&d)[4], const float (&r)[3], float half_px, float hi = 1.f,
        bool tess = false, float morph = 0.f, float flat = 0.f)
    {
        const u32 ns = T.stock_lod_count[level];
        emit_sides(pass, d, r, rec, T.first[kKindStock] + T.stock_lod_first[level], ns, 0, key_prefix(T.cut.stock[level], ns, 0.5f * half_px),
            T.cut.stock[level], 0.f, hi, kChunkRigid | (level == 2 ? kChunkFar : 0u), 1.f, level < 2 ? morph : 0.f);
        if (level == 2)
            return;
        constexpr Draw pd[4] = {drPatch, drPatch, drPatch, drPatchFar};
        const float pr[3] = {r[2], r[2], r[2]};
        if (tess || !T.patch_far)
        {
            const u32 np = T.count[kKindPatch];
            emit_sides(pass, pd, pr, rec, T.first[kKindPatch], np, 0, key_prefix(T.cut.patch[0], np, 0.5f * half_px), T.cut.patch[0], 0.f,
                hi, kChunkRigid, 1.f, tess ? flat : 0.f);
        }
        else
            emit_sides(pass, pd, pr, rec, T.first[kKindPatch] + T.count[kKindPatch], T.patch_far, 0,
                key_prefix(T.cut.patch[1], T.patch_far, 0.5f * half_px), T.cut.patch[1], 0.f, hi, kChunkRigid);
    }

    // The view's stock wood of a level of detail through the GPU cut (r3\da_tree_cut.cs): its tubes' prefix over the
    // floor, each by the sides it needs at its own depth (morph: their way onto the next level's); its patches near
    // (tessellated near the trunk: tess, their lift eased out by flat) or far by their bark's tile there, the ones under the
    // far wood's radius (r_far, template metres) far; bark: the species' bark scale (its tile's).
    void cut_stock(u32 rec, const Template& T, u32 level, float half_px, float hi, bool tess, float r_far, float bark, float morph, float flat)
    {
        constexpr Draw ds[4] = {drStock32, drStock16, drStock8, drStockFar};
        cut(*this, ccStock, ds, rec, T.first[kKindStock] + T.stock_lod_first[level],
            key_prefix(T.cut.stock[level], T.stock_lod_count[level], 0.5f * half_px), 1.f, 0.f, hi,
            kChunkRigid | (level == 2 ? kChunkFar : 0u), bark, 0.f, 0.f, level < 2 ? morph : 0.f);
        if (level == 2)
            return;
        constexpr Draw dp[4] = {drPatch, drPatchFar, drPatchFar, drPatchFar}, df[4] = {drPatchFar, drPatchFar, drPatchFar, drPatchFar};
        const bool near_set = tess || !T.patch_far;
        const float(&rq)[kRq] = T.cut.patch[near_set ? 0 : 1];
        const u32 first = T.first[kKindPatch] + (near_set ? 0u : T.count[kKindPatch]);
        const u32 all = near_set ? T.count[kKindPatch] : T.patch_far;
        const u32 n = key_prefix(rq, all, 0.5f * half_px);
        const u32 n_near = std::min(key_prefix(rq, all, r_far), n);
        const float aux = tess ? flat : 0.f;
        cut(*this, ccPatch, dp, rec, first, n_near, 1.f, 0.f, hi, kChunkRigid, bark, 0.f, 0.f, aux);
        cut(*this, ccPatch, df, rec, first + n_near, n - n_near, 1.f, 0.f, hi, kChunkRigid, bark, 0.f, 0.f, aux);
    }

    // The same in one draw (the shadow passes: no far wood there).
    void emit_stock(u32 pass, u32 rec, const Template& T, u32 level, Draw d, float half_px, float hi = 1.f, bool tess = false, float morph = 0.f,
        float flat = 0.f)
    {
        const Draw ds[4] = {d, d, d, d};
        constexpr float none[3] = {0.f, 0.f, 0.f};
        emit_stock(pass, rec, T, level, ds, none, half_px, hi, tess, morph, flat);
    }

    // The stock wood's level of detail at view distance d (the tier distances dt): every ring to the quads' end, the
    // middle level to the clusters', the far level past them - and its morph onto the next level over the last kRingBand
    // of its range (the view and the near cascade alike: a coarser caster stood out of the view's tube).
    static void stock_level(float d, const float (&dt)[4], u32& level, float& morph)
    {
        level = d < dt[1] ? 0u : (d < dt[3] ? 1u : 2u);
        morph = level == 0 ? smoothstep(dt[1] * (1.f - kRingBand), dt[1], d) : (level == 1 ? smoothstep(dt[3] * (1.f - kRingBand), dt[3], d) : 0.f);
        // (QA r__veg_soft without bit 4: the levels switch as they stand.)
        if (!(ps_r__veg_soft & 4))
            morph = 0.f;
    }

    // The stock patches tessellated (the near set) where the camera is within kTessFar of the trunk's lower three fifths,
    // its flare and its forks - at touching distance, where the 4 cm of its bulge are ten pixels of a silhouette -, the
    // lift eased in from kTessFar to kTessNear (flat: 1 the stock's own facets, where the far set takes over unseen); past
    // it the stock's own triangles (4^2 .. 4^4 fewer). By the hull tier, a big crown took its tessellated patches with its
    // trunk 9 m off: tens of thousands of triangles a tree, a vertex shader each corner - a millisecond a tree.
    bool near_patches(const Instance& I, const Template& T, float& flat) const
    {
        flat = 1.f;
        // (QA, r__dbg_trees 131072: the stock's own triangles everywhere - the tessellation's look against them.)
        if (ps_r__dbg_trees & 131072)
            return false;
        const Fvector& cam = latch().cam;
        const Fvector root{I.row[0][3], I.row[1][3], I.row[2][3]};
        Fvector seg{I.row[0][1], I.row[1][1], I.row[2][1]}, rc;
        seg.mul(0.6f * T.height);
        rc.sub(cam, root);
        Fvector at;
        at.mad(root, seg, std::clamp(rc.dotproduct(seg) / std::max(seg.square_magnitude(), 1e-6f), 0.f, 1.f));
        const float gap = at.distance_to(cam) - T.stock_r * I.scale;
        // (QA r__veg_soft without bit 4: the tessellation whole within 2.5 m, as it switched before.)
        if (!(ps_r__veg_soft & 4))
        {
            flat = 0.f;
            return gap < 2.5f;
        }
        flat = smoothstep(kTessNear, kTessFar, gap);
        return gap < kTessFar;
    }

    // A pass's floor: half its pixel or texel (m) over the tree's scale, a hair under the shaders' (their radii are half
    // floats).
    static float floor_of(const Instance& I, float px_m) { return 0.495f * px_m / std::max(I.scale, 1e-3f); }

    // A species' leaf on the screen: a spray's leaflet, a tree's mean leaf (m).
    float leaf_size(const Instance& I, const Template& T, const Species& S) const
    {
        return (S.arch.leaf_kind == da_shrub::lkSpray ? 0.5f * (S.arch.leaf_len[0] + S.arch.leaf_len[1]) : T.leaf_len) * I.scale;
    }

    // The leaf draws of a species: near (hulls; a spray's leaves, a needle sprig's crossed quads), its quads, its far quad
    // (a spray's card, a needle sprig's quad turned to the eye).
    void leaf_draws(const Species& S, u32& near_d, u32& quad_d, u32& far_d) const
    {
        const auto kind = S.arch.leaf_kind;
        const bool cut = kind != da_shrub::lkSimple || S.lobes > 0.5f;
        near_d = kind == da_shrub::lkNeedles ? drNeedle2 : (kind == da_shrub::lkSpray ? drSpray : drLeaf8);
        quad_d = kind == da_shrub::lkSpray ? drSpray4 : (cut ? drLeafCut : drLeaf4);
        far_d = cut ? drLeafCut : drLeaf4;
    }

    // One tree in the view at distance d: the billboard over the last 15 % before R and on to the far plane; nearer, the
    // stock wood (its tubes by the silhouette's need, its rings by the tier, its patches), the grafted wood by group (the
    // shoots past the hulls, the twigs past the quads - a bare crown's a tier later -, the branches past the clusters and
    // a half), every wood's prefix over the floor at the tree's nearest depth (WoodCut: the shaders' wire rules - a leafy
    // crown's branches at three times it, its twigs at four: under the leaves), the leaves - hulls, then quads, on to the
    // clusters thinned to the leaf floor, their survivors grown (a crown keeps its leaves' look: the spray cards between
    // read as fern fronds) -, the blooms, the clusters, the blobs, each tier crossfading into the next over the last kBand
    // of its distance by the elements' hashes.
    void view(u32 i, float d) override
    {
        const Latch& L = latch();
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const Species& S = g_data.species[I.species - kShrubSpecies];
        // The billboard's alpha rises over the band the tree dissolves in (a template the atlas had no room for keeps its
        // blobs out to the far plane).
        const bool bbs = has_bb(i);
        float fbb = bbs ? smoothstep(R * (1.f - kBand), R, d) : 0.f;
        if (tier >= 0)
            fbb = tier >= 5 && bbs ? 1.f : 0.f;
        if (fbb > 0.f)
            bb_frame.push_back(BbItem{d, i, fbb});
        if (fbb >= 1.f)
            return;
        const float fR = 1.f - fbb;
        const bool leaves = !I.dead && (I.red || leafy(I.species));
        const bool blooms = !I.dead && !I.red && blooming(I.species);
        float dt[4];
        tiers(I, T, leaves, dt);
        // (A tree with its billboard keeps its clusters on to it - the billboard is baked from them: from blobs a far
        // crown stood as flat daubs -, its blobs only in the shadows and the rain map; its stock wood takes its far level
        // where the clusters ended.)
        const float dt_stock[4] = {dt[0], dt[1], dt[2], dt[3]};
        if (bbs)
            dt[3] = std::max(dt[3], R * 1.05f);
        float fh = smoothstep(dt[0] * (1.f - kBand), dt[0], d), fq = smoothstep(dt[1] * (1.f - kBand), dt[1], d);
        float fc = smoothstep(dt[2] * (1.f - kBand), dt[2], d), fl = smoothstep(dt[3] * (1.f - kBand), dt[3], d);
        if (tier >= 0)
        {
            fh = tier >= 1 ? 1.f : 0.f;
            fq = tier >= 2 ? 1.f : 0.f;
            fc = tier >= 3 ? 1.f : 0.f;
            fl = tier >= 4 ? 1.f : 0.f;
        }
        const float px = L.px_per_m / std::max(d, 0.1f);
        const u32 seen_tier = fq < 1.f ? 0 : (fc < 1.f ? 1 : (fl < 1.f ? 2 : 3));
        // QA (r__dbg_trees 1024, 2048, 4096, 8192): the trees in the leaves', the thinned leaves', the clusters' or the
        // blobs' tier left out of the view.
        if (ps_r__dbg_trees & (1024 << seen_tier))
            return;
        const u32 rec = frame_record(*this, i);
        ++seen[seen_tier];
        // The floor at the tree's nearest depth (the shaders' at every segment's own is no finer). The far wood
        // (r3\da_shrub_wood_far.ps: the bark's mean worked out per vertex, five interpolants for seventeen): the whole
        // tree where its bark tile is under four pixels even there, else every element under a pixel and a half's radius
        // there (a near crown's twigs and shoots, its thin limbs).
        Fvector to;
        to.sub(I.bound.P, L.cam);
        const float zmin = std::max(to.dotproduct(L.fwd) - I.bound.R, 0.05f);
        const float half_px = floor_of(I, zmin / std::max(L.px_per_m, 1.f));
        const bool far_tree = 0.06f * std::max(S.bark_scale, 0.25f) * L.px_per_m < 4.f * zmin;
        // The sides by the silhouette's need there (half a pixel of error: 32 sides from a 26 px radius, 16 from 6.6, 8
        // from 2.6, five under it; the depth a metre and a half at the least - inside a crown its nearest point is the
        // eye's own), a group's twigs eight at the most, its shoots five; the far wood under a pixel and a half (template
        // metres; all of a far tree's).
        const float px1 = 2.f * half_px * std::max(1.5f / zmin, 1.f), rf = far_tree ? 1e9f : 1.5f * px1;
        const float rs[3] = {far_tree ? 1e9f : 26.f * px1, far_tree ? 1e9f : 6.57f * px1, rf};
        const float rw[3][3] = {{far_tree ? 1e9f : 6.57f * px1, far_tree ? 1e9f : 2.62f * px1, rf}, {1e9f, far_tree ? 1e9f : 2.62f * px1, rf},
            {1e9f, 1e9f, rf}};
        constexpr Draw ds[4] = {drStock32, drStock16, drStock8, drStockFar}, dw[4] = {drWood16, drWood8, drWood, drWoodFar};
        // The GPU cut (r3\da_tree_cut.cs) takes the wood and the near leaves past the CPU's prefixes: each element out of
        // the view by its own bent place, its sides, its far wood and its tier by its own depth - a near tree's far side
        // drew at its near side's detail, and its crown out of the view ran the vertex shaders (QA's forced tiers: the
        // CPU's cut alone).
        const bool gpu = tier < 0 && cut_on();
        const float bark = std::max(S.bark_scale, 0.25f);
        // The stock wood (dissolving into the billboard over its band); its patches tessellated near its trunk.
        // (A leafy crown's limbs past its leaves at twice the floor - under the clusters' foliage only the thick ones
        // show -, eased over the clusters' band.)
        const float ks = leaves ? std::max(1.f, (1.f + fc) / detail) : 1.f;
        u32 level;
        float morph, flat;
        stock_level(d, dt_stock, level, morph);
        const bool tess = near_patches(I, T, flat);
        if (gpu)
            cut_stock(rec, T, level, ks * half_px, fR, tess, rf, bark, morph, flat);
        else
            emit_stock(0, rec, T, level, ds, rs, ks * half_px, fR, tess, morph, flat);
        // The grafted wood by group: a leafy crown's twigs end with its quads and its shoots with its hulls (the leaves
        // cover them past there); a bare crown keeps them a tier longer, its twigs are its silhouette. A leafy crown's
        // branches at twice the floor among its leaves, four times under its clusters (eased over their band), its twigs
        // at two and a half - each group's prefix ending softly (wood_runs: the limbs at its end shrink away by their
        // radius instead of blinking).
        const u32 w0 = T.first[kKindWood], n1 = T.wood_level[1], n2 = T.wood_level[2] - n1, n3 = T.wood_level[3] - T.wood_level[2];
        const float f1 = smoothstep(dt[3] * 1.5f * (1.f - kBand), dt[3] * 1.5f, d);
        const float kb = leaves ? std::max(1.f, (2.f + 2.f * fc) / detail) : 1.f, kt = leaves ? std::max(1.f, 2.5f / detail) : 1.f, qpx = 0.5f * half_px;
        const u32 g_first[3] = {w0, w0 + n1, w0 + T.wood_level[2]}, g_n[3] = {n1, n2, n3};
        // (QA r__trees_detail 2 and more: a leafy crown's twigs and shoots a tier later too.)
        const bool late = !leaves || detail >= 2.f;
        const float g_cut[3] = {kb * qpx, kt * qpx, kt * qpx}, g_lo[3] = {f1, late ? fc : fq, late ? fq : fh};
        for (u32 g = 0; g < 3; ++g)
        {
            const float(&rq)[kRq] = T.cut.wood[g];
            if (gpu)
            {
                // (A group's cap: the twigs eight sides at the most, the shoots five.)
                constexpr Draw dw1[4] = {drWood8, drWood8, drWood, drWoodFar}, dw2[4] = {drWood, drWood, drWood, drWoodFar};
                const Draw(&dg)[4] = g == 0 ? dw : (g == 1 ? dw1 : dw2);
                wood_runs(rq, g_n[g], g_cut[g], [&](u32 a, u32 c, float keep) {
                    cut(*this, ccWood, dg, rec, g_first[g] + a, c, keep, g_lo[g], fR, 0u, bark);
                });
            }
            else
                wood_runs(rq, g_n[g], g_cut[g], [&](u32 a, u32 c, float keep) {
                    emit_sides(0, dw, rw[g], rec, g_first[g], g_n[g], a, a + c, rq, g_lo[g], fR, 0u, keep);
                });
        }
        // The foliage: a leafy crown's, or a bare one's withered leaves hanging on through the winter (a marcescent oak's,
        // hornbeam's: the vertex shader keeps those alone; the twigs stay its silhouette's tiers). The hulls by the pixel
        // floor; the quads past them by the leaf floor - a crown's leaves stand at that share and grow to hold its cover -,
        // a spray's leaflets (its element's own quads) on to its far quad, the whole spray.
        if ((leaves || (!I.dead && S.marcescent > 0.f)) && fc < 1.f)
        {
            u32 near_d, quad_d, far_d;
            leaf_draws(S, near_d, quad_d, far_d);
            const float size = leaf_size(I, T, S);
            const float keep = floor_keep(size, px, cols.kpx), qkeep = floor_keep(size, px, cols.leaf_kpx);
            const u32 all = T.count[kKindLeaf], n = kept(all, keep), nq = kept(all, qkeep);
            // A spray's leaflets as hulls only over 27 px (six hulls a spray drew 48 vertices), as quads on to ten pixels,
            // its whole spray's quad past them (six quads drew 24 vertices for leaflets of six).
            const bool spray = S.arch.leaf_kind == da_shrub::lkSpray;
            const float fhs = spray ? smoothstep(0.6f * dt[0] * (1.f - kBand), 0.6f * dt[0], d) : fh;
            // (Through the GPU cut each leaf's tier by its own distance, the clusters' end the tree's; a crown thinned apart
            // for its hulls and its quads keeps the CPU's tiers.)
            if (gpu && n == nq && keep == qkeep)
            {
                const Draw dl[4] = {Draw(near_d), Draw(quad_d), Draw(far_d), Draw(far_d)};
                cut(*this, ccLeaf, dl, rec, T.first[kKindLeaf], n, keep, fc, 1.f, 0u, spray ? 0.6f * dt[0] : dt[0], spray ? 0.6f * dt[1] : dt[1],
                    fc);
            }
            else
            {
                // (Every thinned prefix ends softly: emit_thinned.)
                emit_thinned(*this, 0, near_d, rec, T.first[kKindLeaf], all, keep, keep, fhs, 1.f);
                if (quad_d == far_d)
                    emit_thinned(*this, 0, quad_d, rec, T.first[kKindLeaf], all, qkeep, qkeep, fc, fhs);
                else
                {
                    const float fs = std::min(spray ? smoothstep(0.6f * dt[1] * (1.f - kBand), 0.6f * dt[1], d) : fq, fhs);
                    emit_thinned(*this, 0, quad_d, rec, T.first[kKindLeaf], all, qkeep, qkeep, fs, fhs);
                    emit_thinned(*this, 0, far_d, rec, T.first[kKindLeaf], all, qkeep, qkeep, fc, fs);
                }
            }
        }
        // (The flowers and fruit by the pixel floor at twice its pixels: a cherry's fruit were tens of thousands of
        // elements a tree.)
        if (blooms && fc < 1.f)
        {
            const float bkeep = floor_keep(std::max(S.arch.bloom_size, S.arch.fruit_size) * I.scale, px, 2.f * cols.kpx);
            emit_thinned(*this, 0, drBloom, rec, T.first[kKindBloom], T.count[kKindBloom], bkeep, bkeep, fc, 1.f);
        }
        if (fc > 0.f && fl < 1.f)
        {
            const float keep = floor_keep(2.f * T.cluster_r * I.scale, px, cols.kpx * 2.f);
            const float thin = view_thin(2.f * T.cluster_r * I.scale * px, 12.f);
            emit_thinned(*this, 0, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep * thin, keep, fl, std::min(fc, fR),
                thin_flags(thin));
        }
        if (fl > 0.f)
        {
            const float keep = floor_keep(2.f * T.blob_r * I.scale, px, cols.kpx * 3.f);
            const float thin = view_thin(2.f * T.blob_r * I.scale * px, 18.f);
            emit_thinned(*this, 0, drBlob, rec, T.first[kKindBlob], T.count[kKindBlob], keep * thin, keep, 0.f, std::min(fl, fR),
                kChunkRigid | thin_flags(thin));
        }
    }

    // One tree in sun cascade c, every wood's prefix over the texel's floor (WoodCut): the near cascade its branches - a
    // bare crown's twigs and shoots too, its shadow's lace - and its leaves thinned to the shadow floor (the view's leaf
    // tiers: its own leaves' shadow; past them the clusters), the middle the main wood and the clusters (their leaves
    // gathered into dappling clumps, r3\da_shrub_cluster_shadow.ps), the far the trunk and the blobs. No caster switches
    // by the camera's distance near the eye: the twigs at the hulls' end and the cards at the quads' stepped the shadows
    // round the player as one walked.
    void cascade(u32 c, u32 i, float texel) override
    {
        const Latch& L = latch();
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const Species& S = g_data.species[I.species - kShrubSpecies];
        const u32 rec = frame_record(*this, i);
        const bool leaves = !I.dead && (I.red || leafy(I.species));
        const float px = 1.f / std::max(texel, 1e-3f), half_tx = floor_of(I, texel);
        const float d = std::max(I.bound.P.distance_to(L.cam) - 0.5f * I.bound.R, 0.f);
        float dt[4];
        tiers(I, T, leaves, dt);
        const u32 w0 = T.first[kKindWood], n1 = T.wood_level[1];
        // (The near cascade takes the stock rings, their morph and the patches the view draws - tessellated near the trunk:
        // the stock's flat facets over a concave stretch stood in front of the view's rounded surface and shadowed it, a
        // dark band down a pine's trunk; the two differ by a centimetre or two, no step in a shadow.)
        if (c == 0)
        {
            u32 level;
            float morph, flat;
            stock_level(d, dt, level, morph);
            const bool tess = near_patches(I, T, flat);
            emit_stock(1, rec, T, level, drStock16, half_tx, 1.f, tess, morph, flat);
            // (A leafy crown's branches at three times the floor: inside the crown the leaves' shadow covers the thin ones.)
            emit(*this, 1, drWood, rec, w0, key_prefix(T.cut.wood[0], n1, (leaves ? 3.f : 1.f) * half_tx), 1.f, 0.f, 1.f);
            if (!leaves)
            {
                emit(*this, 1, drWood, rec, w0 + n1, key_prefix(T.cut.wood[1], T.wood_level[2] - n1, half_tx), 1.f, 0.f, 1.f);
                emit(*this, 1, drWood, rec, w0 + T.wood_level[2], key_prefix(T.cut.wood[2], T.wood_level[3] - T.wood_level[2], half_tx), 1.f,
                    0.f, 1.f);
            }
            // (The leaves within 22 m, the clusters past them, crossfading over the outer half of it: a tree farther off
            // casts into the twenty-metre map only at a low sun, its shadow there long and soft - over the stock band's 3 m
            // its shadow on the ground at one's feet turned from blots to leaves at once as one walked toward it.)
            const float l0 = std::min(dt[2], 22.f), fc = smoothstep(l0 * 0.5f, l0, d);
            if ((leaves || (!I.dead && S.marcescent > 0.f)) && fc < 1.f)
            {
                u32 near_d, quad_d, far_d;
                leaf_draws(S, near_d, quad_d, far_d);
                const float keep = floor_keep(leaf_size(I, T, S), px, cols.shadow_kpx);
                emit_thinned(*this, 1, far_d, rec, T.first[kKindLeaf], T.count[kKindLeaf], keep, keep, fc, 1.f);
            }
            if (leaves && fc > 0.f)
            {
                const float keep = floor_keep(2.f * T.cluster_r * I.scale, px, 2.f);
                emit_thinned(*this, 1, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep * shadow_thin(1), keep, 0.f, fc,
                    thin_flags(shadow_thin(1)));
            }
        }
        else if (c == 1)
        {
            // (The far level: its tubes whole, no patches - a forest's patches were tens of thousands of triangles in the
            // middle map for a texel or two of difference.)
            emit_stock(1 + c, rec, T, 2u, drStock8, half_tx);
            // (A leafy crown's shadow there is its clusters'; its grafted branches only when bare.)
            if (!leaves)
                emit(*this, 1 + c, drWood, rec, w0, key_prefix(T.cut.wood[0], n1, half_tx), 1.f, 0.f, 1.f);
            const float keep = floor_keep(2.f * T.cluster_r * I.scale, px, 2.f);
            emit_thinned(*this, 1 + c, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep * shadow_thin(1 + c), keep, 0.f, 1.f,
                thin_flags(shadow_thin(1 + c)));
        }
        else
        {
            emit(*this, 1 + c, drStock8, rec, T.first[kKindStock] + T.stock_lod_first[2], key_prefix(T.cut.stock[2], T.stock_lod_count[2], half_tx),
                1.f, 0.f, 1.f, kChunkRigid | kChunkFar);
            // (A blob a dozen texels across at the least, the survivors grown: the far map's tens of thousands of blobs cast
            // the same crowns' shadows - a half of them past that, their cover made up.)
            const float keep = floor_keep(2.f * T.blob_r * I.scale, px, 12.f);
            emit_thinned(*this, 1 + c, drBlob, rec, T.first[kKindBlob], T.count[kKindBlob], keep * shadow_thin(1 + c), keep, 0.f, 1.f,
                kChunkRigid | thin_flags(shadow_thin(1 + c)));
        }
    }

    // The rain's occlusion map: the crown's blobs from above (a bare or dead tree shelters little).
    void rain(u32 i) override
    {
        const Instance& I = instances[i];
        if (I.dead)
            return;
        const Template& T = templates[I.tmpl];
        emit(*this, kRainPass, drBlob, frame_record(*this, i), T.first[kKindBlob], T.count[kKindBlob], 1.f, 0.f, 1.f, kChunkRigid);
    }

    // A shadowed local light: the stock wood's far level (its tubes whole, no patches: a lamp's map is a few centimetres a
    // texel at the least, and a fork's patches were most of the trees' cost there - three hundred thousand triangles in
    // a village's lamps), a bare crown's branches and a leafy one's clusters, kept by the light's texel.
    void light(u32 i, float texel) override
    {
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const u32 rec = frame_record(*this, i);
        const float half_tx = floor_of(I, texel);
        emit(*this, 0, drStock8, rec, T.first[kKindStock] + T.stock_lod_first[2], key_prefix(T.cut.stock[2], T.stock_lod_count[2], 0.5f * half_tx),
            1.f, 0.f, 1.f, kChunkRigid | kChunkFar);
        if (!I.dead && (I.red || leafy(I.species)))
        {
            // (Drawn as the near cascade's pass: its thinning.)
            const float keep = floor_keep(2.f * T.cluster_r * I.scale, 1.f / std::max(texel, 1e-3f), 2.f);
            emit_thinned(*this, 0, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep * shadow_thin(1), keep, 0.f, 1.f,
                thin_flags(shadow_thin(1)));
        }
        else
            emit(*this, 0, drWood, rec, T.first[kKindWood], key_prefix(T.cut.wood[0], T.wood_level[1], 0.5f * half_tx), 1.f, 0.f, 1.f);
    }
};

TreePopulation* g_trees{};

TreePopulation& trees()
{
    static TreePopulation P;
    if (!g_trees)
    {
        g_trees = &P;
        // The G-buffer's order, near to far for early-Z: the stock wood, the grafted wood, the leaves, the cards, the
        // blooms, the clusters and blobs; the shadow maps' draws.
        P.draws_view = {drStock32, drStock16, drStock8, drStockFar, drPatch, drPatchFar, drWood16, drWood8, drWood, drWoodFar, drLeaf8,
            drNeedle2, drSpray, drSprayCut, drSpray4, drLeaf4, drLeafCut, drBloom, drCluster, drBlob};
        P.draws_shadow = {drStock32, drStock16, drStock8, drPatch, drWood, drLeaf4, drLeafCut, drCluster, drBlob};
    }
    return P;
}
} // namespace

namespace da_veg
{
Population* trees_population() { return &trees(); }
} // namespace da_veg

// ======================================================================================================
// QA and the billboard bake's entry points
// ======================================================================================================
void da_trees_stats()
{
    TreePopulation& P = trees();
    if (!P.loaded)
    {
        Msg("* [trees] off%s", P.failed ? " (failed on this level)" : "");
        da_trees_bake_stats();
        return;
    }
    const PresetColumns& c = kPresets[P.preset];
    Msg("* [trees] preset %u: leaf tiers %.0f/%.0f/%.1f/%.1f px, billboards from %.0f m (this frame: %.0f px), pixel floor %.2f px, "
        "leaf floor %.1f px over a crown detail of %.1f, shadow leaf floor %.0f texels, %u cascades; %s",
        P.preset, c.hull_px, c.quad_px, c.leaf_px, c.clu_px, P.R, c.bb_px, c.kpx, c.leaf_kpx, c.crown * ps_r__trees_detail, c.shadow_kpx,
        c.cascades, P.detached ? "standing" : "the stock trees stand (r__trees 0)");
    Msg("* [trees] %u trees on %u placements, %u templates; this frame %u leaves, %u thinned, %u clusters, %u blobs, %u billboards; %u "
        "records, %u chunks, %u of them bending (bend slots for %u chunks, %u a frame back; both populations)",
        u32(P.instances.size()), u32(P.placements.size()), u32(P.templates.size()), P.seen[0], P.seen[1], P.seen[2], P.seen[3], P.bb_drawn,
        frame_records(), frame_chunks(), frame_bends(), bend_capacity(), bend_view_capacity());
    static constexpr pcstr names[drCount] = {"wood", "leaf 8", "leaf 4", "leaf cut", "bloom", "cluster", "blob", "spray",
        "spray cut", "spray 4", "wood 8", "needle 2", "wood 16", "stock 32", "stock 16", "stock 8", "patch", "wood far", "stock far",
        "patch far"};
    for (u32 d = 0; d < drCount; ++d)
        if (P.elements[d])
            Msg("* [trees]   %-9s %8u elements, view %5u chunks, cascades %5u / %5u / %5u, rain %5u", names[d], P.elements[d],
                P.draw_count[0][d], P.draw_count[1][d], P.draw_count[2][d], P.draw_count[3][d], P.draw_count[kRainPass][d]);
    Msg("* [trees]   %u chunks in the shadowed local lights", light_chunks(kPopTrees));
    Msg("* [trees]   the GPU cut %s: %u candidate elements this frame, %u with no room (drawn uncut)", cut_on() ? "on" : "off",
        P.cut_elements, P.cut_fallback);
    cut_readback_request();
    u32 per_species[kTreeSpecies]{}, dead = 0, red = 0, saplings = 0, growing = 0;
    for (const Instance& I : P.instances)
    {
        ++per_species[I.species - kShrubSpecies];
        dead += I.dead ? 1u : 0u;
        red += I.red ? 1u : 0u;
        saplings += I.youth >= 1.f ? 1u : 0u;
        growing += I.youth > 0.f && I.youth < 1.f ? 1u : 0u;
    }
    for (u32 s = 0; s < kTreeSpecies; ++s)
        if (per_species[s])
            Msg("* [trees]   %-10s %5u trees", g_data.species[s].name.c_str(), per_species[s]);
    Msg("* [trees]   %u standing dead, %u Red Forest pines with their red needles, %u saplings, %u growing up (youth 0..1)", dead,
        red, saplings, growing);
    std::map<u64, u32> per_kind;
    for (const Instance& I : P.instances)
        ++per_kind[P.templates[I.tmpl].kind];
    for (const auto& [kind, n] : per_kind)
        Msg("* [trees]   kind %016llx: %u trees", (unsigned long long)kind, n);
    da_trees_bake_stats();
}

void da_trees_probe(pcstr args)
{
    TreePopulation& P = trees();
    if (!P.loaded)
    {
        Msg("* [trees] probe: off");
        return;
    }
    // (`templates`: every template's species, height, leaf area and parts, and how many trees wear it.)
    if (args && strstr(args, "templates"))
    {
        xr_vector<u32> wearers(P.templates.size(), 0), first(P.templates.size(), ~0u);
        for (u32 i = 0; i < P.instances.size(); ++i)
            if (!wearers[P.instances[i].tmpl]++)
                first[P.instances[i].tmpl] = i;
        for (u32 t = 0; t < P.templates.size(); ++t)
        {
            const Template& T = P.templates[t];
            Msg("* [trees] template %2u: kind %016llx %-12s bucket %u, %.1f m tall, %.1f m wide, leaf area %.0f m2, %u leaves, %u "
                "clusters (r %.2f), %u blobs (r %.2f), %u stock and %u patch elements, %u trees (one at %.0f %.0f %.0f)",
                t, (unsigned long long)T.kind, g_data.species[T.species - kShrubSpecies].name.c_str(), T.bucket, T.height, T.width,
                T.leaf_area, T.count[kKindLeaf], T.count[kKindCluster], T.cluster_r, T.count[kKindBlob], T.blob_r,
                T.count[kKindStock], T.count[kKindPatch], wearers[t], first[t] != ~0u ? P.instances[first[t]].bound.P.x : 0.f,
                first[t] != ~0u ? P.instances[first[t]].box.vMin.y : 0.f, first[t] != ~0u ? P.instances[first[t]].bound.P.z : 0.f);
        }
        return;
    }
    const Fvector cam = Device.vCameraPosition, dir = Device.vCameraDirection;
    // (`ray`: the trees whose bounds the view's ray passes through, nearest first.)
    const bool ray = args && strstr(args, "ray");
    xr_vector<std::pair<float, u32>> closest;
    for (u32 i = 0; i < P.instances.size(); ++i)
    {
        const Fvector c = P.instances[i].bound.P;
        const float d = c.distance_to(cam);
        if (ray)
        {
            Fvector to;
            to.sub(c, cam);
            const float along = to.dotproduct(dir), R = P.instances[i].bound.R;
            if (along <= 0.f || d * d - along * along > R * R)
                continue;
        }
        closest.emplace_back(d, i);
    }
    const size_t n = std::min<size_t>(closest.size(), 10);
    std::partial_sort(closest.begin(), closest.begin() + n, closest.end());
    for (size_t k = 0; k < n; ++k)
    {
        const Instance& I = P.instances[closest[k].second];
        const Template& T = P.templates[I.tmpl];
        const float d = std::max(closest[k].first - 0.5f * I.bound.R, 0.f);
        float dt[4];
        P.tiers(I, T, !I.dead, dt);
        pcstr tier = d < dt[0] ? "hulls" : d < dt[1] ? "quads" : d < dt[2] ? "thinned quads" : d < dt[3] ? "clusters" : d < P.R ? "blobs" : "billboard";
        Msg("* [trees] probe %2u: kind %016llx %-10s at %5.1f m (%6.1f %5.1f %6.1f), %.1f m tall, youth %.2f, template %u (bucket %u, "
            "%u leaves), %s%s%s",
            u32(k), (unsigned long long)T.kind, g_data.species[I.species - kShrubSpecies].name.c_str(), d, I.bound.P.x, I.box.vMin.y,
            I.bound.P.z, I.tree_h, I.youth, I.tmpl, T.bucket, T.count[kKindLeaf], tier, I.dead ? ", dead" : "", I.red ? ", red needles" : "");
        // The trunk's sway this frame: the stock root's oscillator and the push per metre the shader takes.
        if (I.wind)
        {
            const FTreeVisualWind s = I.wind->wind_state();
            const Latch& L = latch();
            const Fvector2 push = trunk_push(s.now, L.tree_wave, L.tree_wind, L.wind_field, Fvector{I.row[0][3], I.row[1][3], I.row[2][3]}, s.gate);
            Msg("* [trees] probe %2u: wind root, gate %.0f, q %.2f, freq %.2f, field %.2f, push %.3f per m, climbable to %.1f m", u32(k),
                s.gate, s.now.x, s.now.y, L.wind_field.z, std::sqrt(push.x * push.x + push.y * push.y), I.climb_top);
        }
        else
            Msg("* [trees] probe %2u: no wind root - the trunk stands still", u32(k));
        if (ray)
            Msg("* [trees] probe %2u: %u clusters (r %.2f m), %u blobs (r %.2f m), leaf area %.0f m2, scale %.2f, tiers "
                "%.0f %.0f %.0f %.0f m, R %.0f m",
                u32(k), T.count[kKindCluster], T.cluster_r, T.count[kKindBlob], T.blob_r, T.leaf_area,
                I.scale, dt[0], dt[1], dt[2], dt[3], P.R);
    }
}

void da_trees_pin_species(pcstr name)
{
    TreePopulation& P = trees();
    const pcstr arg = name ? skip_space(name) : "";
    u32 pin = 0;
    if (*arg && xr_stricmp(arg, "off") && xr_stricmp(arg, "0"))
    {
        const s32 sp = species_by_name(arg);
        if (sp < 0)
        {
            Msg("! [trees] species pin: no species '%s'", arg);
            return;
        }
        pin = u32(sp) + 1;
    }
    P.species_pin = pin;
    Msg("* [trees] species pin: %s (from the next level load)", pin ? g_data.species[pin - 1].name.c_str() : "off (the data decides)");
}

void da_trees_pin_kind(pcstr key)
{
    TreePopulation& P = trees();
    const pcstr arg = key ? skip_space(key) : "";
    P.kind_pin = *arg && xr_stricmp(arg, "off") && xr_stricmp(arg, "0") ? u64(strtoull(arg, nullptr, 16)) : 0;
    if (P.kind_pin)
        Msg("* [trees] kind pin: only kind %016llx is upgraded (from the next level load)", (unsigned long long)P.kind_pin);
    else
        Msg("* [trees] kind pin: off (from the next level load)");
}

bool da_trees_bb_atlas(DaTreeBbAtlas& out)
{
    const TreePopulation& P = trees();
    if (!P.bb_ready)
        return false;
    out.albedo = kBbAlbedo;
    out.normal = kBbNormal;
    out.material = kBbMaterial;
    out.width = P.bb_w;
    out.height = P.bb_h;
    out.cell = P.bb_cell;
    out.templates = u32(P.templates.size());
    out.sets = u32(P.templates.size() + P.bb_dead.size());
    out.gutter = P.bb_cell / 16;
    out.mips = P.bb_d && P.bb_d->pSurface ? P.bb_mips() : 1u;
    out.R = P.R;
    out.kpx = P.cols.kpx;
    return true;
}

// A cell set's template, and whether the set is its dead look (false: no such set).
static bool bb_set(const TreePopulation& P, u32 set, u32& tmpl, bool& dead)
{
    const u32 n = u32(P.bb.size());
    dead = set >= n;
    tmpl = dead ? (set - n < P.bb_dead.size() ? P.bb_dead[set - n] : ~0u) : set;
    return tmpl < n;
}

bool da_trees_bb_template(u32 set, DaTreeBbTemplate& out)
{
    const TreePopulation& P = trees();
    u32 tmpl;
    bool dead;
    if (!bb_set(P, set, tmpl, dead) || !P.bb[tmpl].placement)
        return false;
    const BbCell& B = P.bb[tmpl];
    const BbView* views = dead ? B.dead_view : B.view;
    const Template& T = P.templates[tmpl];
    out.tmpl = tmpl;
    out.kind = T.kind;
    out.species = T.species;
    out.placement_xform = P.placements[B.placement - 1].xf;
    for (u32 f = 0; f < 8; ++f)
    {
        out.view[f].rect = views[f].rect;
        for (u32 k = 0; k < 4; ++k)
        {
            out.view[f].corner[k] = views[f].corner[k];
            out.view[f].uv[k] = views[f].uv[k];
        }
        out.view[f].normal = views[f].normal;
    }
    out.dirty = dead ? B.dead_dirty : B.dirty;
    out.filled = dead ? B.dead_filled : B.filled;
    out.youth = dead ? B.dead_youth : B.youth;
    out.hemi = dead ? B.dead_hemi : B.hemi;
    out.dead = dead || B.dead_look;
    out.red = !dead && B.red;
    return true;
}

void da_trees_bb_mark(u32 set, bool dirty)
{
    TreePopulation& P = trees();
    u32 tmpl;
    bool dead;
    if (!bb_set(P, set, tmpl, dead))
        return;
    BbCell& B = P.bb[tmpl];
    (dead ? B.dead_dirty : B.dirty) = dirty;
    // (Cleared by the bake: its cells hold its picture now.)
    bool& filled = dead ? B.dead_filled : B.filled;
    filled = filled || !dirty;
}
} // namespace xray::render::RENDER_NAMESPACE
