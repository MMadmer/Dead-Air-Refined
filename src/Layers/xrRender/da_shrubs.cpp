// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural shrubs (docs/dead-air/SHRUBS.md): the shrubs' population of the vegetation core (da_veg.cpp).
//
// Level load: the species, communities and stock kinds of dead_air_x64_shrubs.ltx; every stock bush of the level (an
// FLOD of tree parts whose content the data names a shrub class, from the core's census) becomes one or more
// placements - its root, heading, size, its static lighting and the plant community of its spot pick a species - and
// its FLOD leaves the sector hierarchy, so no view, shadow, billboard, GPU-driven registry or ray tracing scene meets it
// again. The bushes of each species grow from a few templates (three heights, three variants) generated on the task
// pool and packed into one element buffer and one axis buffer. Per frame the core walks the bushes; each in the view
// takes its tier, crossfade and the share of its elements the pixel floor keeps, each in a sun cascade's reach that
// cascade's draws; the G-buffer and each cascade draw each element kind once, instanced over its chunks; the vertex
// shaders read the records and bend every point through its chain of axes.

#include "stdafx.h"
#include "da_shrubs.h"
#include "da_veg.h"
#include "da_veg_internal.h"
#include "da_shrub_gen.h"
#include "da_grass_data.h"
#include "FTreeVisual.h"
#include "FHierrarhyVisual.h"
#include "xrRender_console.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCommon/xr_unordered_map.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

#include <unordered_map>
#include <unordered_set>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using namespace da_veg;

// Templates of a species: three heights over its range, three variants each (the instance's mirror and seed make more
// of them).
constexpr u32 kHeights = 3, kVariants = 3;
constexpr u32 kCommunities = 24, kCommSpecies = 12;

// The stock classes the shrubs replace (BUSHES_ENGINE.md 1.2).
enum StockClass : u8
{
    clNone,
    clBush,      // one shrub
    clBushGroup, // two to five shrubs on one root
    clThicket,   // a 21 m mass of shrubs
    clConifer,   // a young spruce or juniper
    clCount
};
constexpr pcstr kClassNames[clCount] = {"NONE", "BUSH", "BUSH_GROUP", "THICKET", "CONIFER_BUSH"};

StockClass class_by_name(pcstr s)
{
    s = skip_space(s);
    for (u32 c = clCount - 1; c > clNone; --c)
    {
        const size_t n = xr_strlen(kClassNames[c]);
        if (!_strnicmp(s, kClassNames[c], n) && !isalnum(u8(s[n])) && s[n] != '_')
            return StockClass(c);
    }
    return clNone;
}

u32 class_bits(pcstr v)
{
    u32 bits = 0;
    string64 item;
    for (u32 i = 0, n = _GetItemCount(v); i < n; ++i)
        if (const StockClass c = class_by_name(_GetItem(v, i, item)); c != clNone)
            bits |= 1u << c;
    return bits;
}

// A stock model the data names: its class, its look (dry cards, the Red Forest's red ones) and, for a group or a
// thicket, its clumps (model-space x, z, radius, top).
struct Kind
{
    StockClass cls{clNone};
    bool dry{}, red{};
    u32 clumps{};
    Fvector4 clump[8]{};
};

struct Data
{
    bool file{};
    Species species[kShrubSpecies];
    Community communities[kCommunities];
    // grass community (0..14) -> shrub community, stock class -> shrub community where no grass grows, the Red Forest's
    // community for the red stock.
    s8 grass_comm[16]{};
    s8 class_comm[clCount]{};
    s8 red_comm{-1};
    xr_unordered_map<u64, Kind> kinds;
    std::unordered_multimap<u64, FamilyRow> families;
    // The size policy (BUSHES_ENGINE.md 1.2) and the stand of a group's or a thicket's clump.
    float max_top[clCount]{0.f, 8.5f, 8.5f, 10.5f, 8.5f};
    float min_scale{0.05f};
    float spacing{2.6f};
    float dry_dead{0.12f}; // the share of the dry stock that stands dead
};
Data g_data;

s32 species_by_name(pcstr name)
{
    for (u32 i = 0; i < kShrubSpecies; ++i)
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

void data_reload()
{
    g_data.~Data();
    new (&g_data) Data();
    for (s8& c : g_data.grass_comm)
        c = -1;
    for (s8& c : g_data.class_comm)
        c = -1;
    if (!FS.exist("$game_config$", "dead_air_x64_shrubs.ltx"))
        return;
    string_path fn;
    FS.update_path(fn, "$game_config$", "dead_air_x64_shrubs.ltx");
    CInifile ini(fn, TRUE);
    g_data.file = true;
    for (u32 i = 0; i < kShrubSpecies; ++i)
    {
        string64 name;
        xr_sprintf(name, "shrub_species_%u", i + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        parse_species(s, g_data.species[i], &class_bits);
        pcstr id = da_ini_value(s, "name");
        g_data.species[i].name = id ? skip_space(id) : name;
    }
    for (u32 i = 0; i < kCommunities; ++i)
    {
        string64 name;
        xr_sprintf(name, "shrub_community_%u", i + 1);
        if (!ini.section_exist(name))
            continue;
        const CInifile::Sect& s = ini.r_section(name);
        Community& c = g_data.communities[i];
        pcstr id = da_ini_value(s, "name");
        c.name = id ? skip_space(id) : name;
        parse_community(s, c, kCommSpecies, &species_by_name);
    }
    // The grass communities (by their names in dead_air_x64_seasons.ltx) and the stock classes.
    if (ini.section_exist("shrub_grass_community"))
    {
        const DaSeasonData& grass = da_season_data();
        for (const CInifile::Item& item : ini.r_section("shrub_grass_community").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const s32 sc = community_by_name(skip_space(item.second.c_str()));
            for (u32 gc = 0; gc < da_grass_communities && sc >= 0; ++gc)
                if (grass.communities[gc].used && !xr_stricmp(da_grass_community_name(gc), item.first.c_str()))
                    g_data.grass_comm[gc] = s8(sc);
        }
    }
    if (ini.section_exist("shrub_class_community"))
        for (const CInifile::Item& item : ini.r_section("shrub_class_community").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            const StockClass cls = class_by_name(item.first.c_str());
            const s32 sc = community_by_name(skip_space(item.second.c_str()));
            if (cls != clNone && sc >= 0)
                g_data.class_comm[cls] = s8(sc);
        }
    if (ini.section_exist("shrub_policy"))
    {
        const CInifile::Sect& s = ini.r_section("shrub_policy");
        if (pcstr v = da_ini_value(s, "max_top"))
        {
            string64 item;
            for (u32 i = 0, n = _GetItemCount(v); i < n; ++i)
            {
                _GetItem(v, i, item);
                pstr colon = strchr(item, ':');
                if (!colon)
                    continue;
                *colon = 0;
                if (const StockClass c = class_by_name(item); c != clNone)
                    g_data.max_top[c] = float(atof(colon + 1));
            }
        }
        read_float(s, "min_scale", g_data.min_scale);
        read_float(s, "spacing", g_data.spacing);
        read_float(s, "dry_dead", g_data.dry_dead);
        if (pcstr v = da_ini_value(s, "red_community"))
            g_data.red_comm = s8(community_by_name(skip_space(v)));
    }
    // The stock models: "<kind key> = CLASS[, dry][, red][, x:z:radius:top ...]".
    if (ini.section_exist("shrub_kinds"))
        for (const CInifile::Item& item : ini.r_section("shrub_kinds").Data)
        {
            if (!item.first.c_str() || !item.second.c_str())
                continue;
            Kind k;
            string256 field;
            pcstr v = item.second.c_str();
            const u32 n = _GetItemCount(v);
            k.cls = class_by_name(_GetItem(v, 0, field));
            if (k.cls == clNone)
                continue;
            for (u32 i = 1; i < n; ++i)
            {
                _GetItem(v, i, field);
                pcstr f = skip_space(field);
                if (!xr_stricmp(f, "dry"))
                    k.dry = true;
                else if (!xr_stricmp(f, "red"))
                    k.red = true;
                else if (k.clumps < 8)
                {
                    Fvector4& c = k.clump[k.clumps];
                    if (sscanf_s(f, "%f:%f:%f:%f", &c.x, &c.y, &c.z, &c.w) == 4)
                        ++k.clumps;
                }
            }
            g_data.kinds[u64(strtoull(item.first.c_str(), nullptr, 16))] = k;
        }
    // Other compiles of the known models: "<signature> = <kind key>:x0:y0:z0:x1:y1:z1:area, ..." (box in mm).
    if (ini.section_exist("shrub_families"))
        parse_families(ini.r_section("shrub_families"), g_data.families);
    u32 species = 0, communities = 0;
    for (const Species& s : g_data.species)
        species += s.used ? 1 : 0;
    for (const Community& c : g_data.communities)
        communities += c.used ? 1 : 0;
    Msg("* [shrubs] data: %u species in %u communities, %u stock models (%u other compiles)", species, communities,
        u32(g_data.kinds.size()), u32(g_data.families.size()));
}

// The columns of preset 0..4 (minimum..extreme): the eight-vertex leaves to d0, the leaves to d1, the clusters to d2,
// the blobs to R (a stock bush went at about 150 m at the default settings, a group at 300, a thicket at 400: the bush's
// size stretches all of them), the pixel floor of a leaf, the cascades that take shrubs, and the pixel floors under
// which a bush's mean leaf gives way to its clusters (at two on every preset a small-leaved bush stood in clusters from
// ten metres on: the clusters' cells read as a mosaic at 25 m).
struct PresetColumns
{
    float d0, d1, d2, R, kpx;
    u32 cascades;
    float leaf_floors;
};
constexpr PresetColumns kPresets[5] = {{4.f, 14.f, 40.f, 150.f, 3.f, 1, 2.f}, {5.f, 17.f, 50.f, 180.f, 2.5f, 1, 2.f},
    {6.f, 21.f, 60.f, 210.f, 2.f, 2, 1.6f}, {8.f, 26.f, 75.f, 250.f, 1.75f, 2, 1.3f}, {10.f, 32.f, 90.f, 300.f, 1.5f, 3, 1.f}};

// A stock bush's share: where a bush stands, how big, its light. A group's or a thicket's FLOD gives several, the
// first of them its head (`group`).
struct Placement
{
    StockClass cls{};
    bool dry{}, red{};
    FHierrarhyVisual* parent{};
    dxRender_Visual* flod{};
    u32 group{};
    Fvector root{};
    float yaw{}, height{}, width{};
    float hemi{1.f}, sun{1.f};
    s32 species{-1}; // the communities' pick (-1: the stock bush stands)
    bool dead{};
    u32 candidate{}; // its FLOD in the core's census
};

// One generated bush into the element and axis buffers: the wood ordered by level (a far tier draws a prefix of it),
// the leaves and clusters by importance (the lit outer ones first: the pixel floor draws a prefix and the interior goes
// first), the blooms from the top, the blobs by area.
void pack_template(const da_shrub::Template& T, Template& out, xr_vector<u32>& elems, xr_vector<u32>& axes)
{
    using namespace da_shrub;
    out.axis_first = u32(axes.size() / 8);
    out.axis_count = u32(T.axes.size());
    for (u32 i = 0; i < T.axes.size(); ++i)
    {
        const Axis& a = T.axes[i];
        // (The parent's 24 bits, 0xFFFFFF none; the level byte on top.)
        const u32 parent = a.parent < 0 ? 0xFFFFFFu : u32(a.parent);
        put(axes, a.base, parent | (std::min(a.level, 255u) << 24), oct(a.dir), h2(a.length, a.attach), h2(a.flex, a.freq),
            f16(a.r_base) | ((hash32(i * 2654435761u + out.variant) >> 16) << 16));
    }
    xr_vector<u32> order;
    xr_vector<float> imp;
    // Wood.
    out.first[kKindWood] = u32(elems.size() / 8);
    order.resize(T.segments.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&T](u32 a, u32 b) { return T.axes[T.segments[a].axis].level < T.axes[T.segments[b].axis].level; });
    for (u32 i : order)
    {
        const Segment& sg = T.segments[i];
        const u32 level = std::min(T.axes[sg.axis].level, 3u);
        for (u32 l = level; l < 4; ++l)
            ++out.wood_level[l];
        const V3 d = sg.b - sg.a;
        // (The level byte's top bit: a dead stub, grey weathered wood; the next: a prickle or thorn, its own colour;
        // r3\da_shrub_wood_vs.h.)
        const Axis& ax = T.axes[sg.axis];
        put(elems, sg.a, sg.axis, oct(normalize(d)), h2(length(d), sg.ra), h2(sg.rb, sg.s0),
            level | (ax.dead ? 0x80u : 0u) | (ax.leafless && !ax.dead ? 0x40u : 0u) | (u8f(sg.vis) << 8) | ((hash32(i) >> 16) << 16));
    }
    out.count[kKindWood] = u32(T.segments.size());
    // Leaves.
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
    // A leaf's side is its roll about the midrib (eight bits); the rest of the word carries the crown's normal, the
    // gradient of the ellipsoid of the bush's bounds, which the vertex shader blends into the leaf's shading normal
    // (da_shrub_leaf_vs.h: the bush is lit as one volume, a fluttering leaf does not flash). The clusters and blobs carry
    // the same crown normal (da_shrub_cluster.ps): every tier is lit alike.
    const V3 cc = (T.bmin + T.bmax) * 0.5f, ce = (T.bmax - T.bmin) * 0.5f;
    const V3 inv_e2{1.f / std::max(ce.x * ce.x, 1e-4f), 1.f / std::max(ce.y * ce.y, 1e-4f), 1.f / std::max(ce.z * ce.z, 1e-4f)};
    const auto crown_at = [&cc, &inv_e2](V3 p) {
        const V3 m = p - cc;
        return normalize(v3(m.x * inv_e2.x, m.y * inv_e2.y, m.z * inv_e2.z), v3(0.f, 1.f, 0.f));
    };
    for (u32 i : order)
    {
        const Leaf& f = T.leaves[i];
        const V3 crown = crown_at(f.p + f.dir * (0.5f * f.len));
        put(elems, f.p, f.axis | (u8f(f.rank) << 16) | (u8f(f.fall) << 24), oct(f.dir), leaf_roll8(f.dir, f.side) | (oct12(crown) << 8),
            h2(f.len, f.wid), f16(f.s) | (u8f(f.light) << 16) | ((hash32(i * 3u + 1u) & 0xFFu) << 24));
    }
    out.count[kKindLeaf] = u32(T.leaves.size());
    out.leaf_len = T.leaves.empty() ? 0.05f : leaf_len / float(T.leaves.size());
    // Blooms.
    out.first[kKindBloom] = u32(elems.size() / 8);
    order.resize(T.blooms.size());
    for (u32 i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&T](u32 a, u32 b) { return T.blooms[a].p.y > T.blooms[b].p.y; });
    for (u32 i : order)
    {
        const Bloom& b = T.blooms[i];
        put(elems, b.p, b.axis | (u8f(b.rank) << 16), oct(b.dir), h2(b.size, b.s), u32(b.kind) | ((hash32(i * 5u + 3u) >> 8) << 8),
            u8f(b.vis));
    }
    out.count[kKindBloom] = u32(T.blooms.size());
    // Clusters.
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
        put(elems, c.c, c.axis | (u8f(c.rank) << 16) | (u8f(c.fall) << 24), oct(crown_at(c.c)), h2(c.r, c.area),
            f16(c.s) | (u8f(c.light) << 16) | (std::min(c.blooms, 255u) << 24), std::min(c.leaves, 65535u) | ((hash32(i * 7u) >> 16) << 16));
    }
    out.count[kKindCluster] = u32(T.clusters.size());
    out.cluster_r = T.clusters.empty() ? 0.1f : cr / float(T.clusters.size());
    // Blobs: they bend with the first stem at their height.
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
        // (w's high half: how far the blob was drawn in, its shadow cast from where its clusters are.)
        put(elems, b.c, 0u | (u8f(b.rank) << 16) | (u8f(b.fall) << 24), oct(crown_at(b.c)), h2(b.r, b.area),
            u8f(b.bloom_share) | (u8f(b.light) << 8) | ((hash32(i * 13u) >> 16) << 16), f16(std::max(b.c.y, 0.f)) | (f16(b.pull) << 16));
    }
    out.count[kKindBlob] = u32(T.blobs.size());
    out.blob_r = T.blobs.empty() ? 0.3f : br / float(T.blobs.size());
    const V3 c = (T.bmin + T.bmax) * 0.5f;
    out.centre.set(c.x, c.y, c.z);
    out.radius = 0.5f * length(T.bmax - T.bmin);
    out.height = T.height;
    out.width = T.width;
}

// The heights of a species' templates: the middles of the thirds of its range.
float template_height(const Species& S, u32 k) { return S.height[0] + (S.height[1] - S.height[0]) * (float(k) + 0.5f) / float(kHeights); }

class ShrubPopulation final : public Population
{
public:
    ShrubPopulation() : Population(kPopShrubs) {}

    u32 preset{2};
    PresetColumns cols{};
    float lod{1.f}; // r__shrubs_lod_scale (QA): the view's tier distances scaled
    s32 tier{-1};   // r__shrubs_force_tier (QA): 0 near .. 3 far for every bush in the view, -1 by distance
    bool prepared{};
    xr_vector<Placement> placements;
    std::unordered_set<const dxRender_Visual*> replaced;
    // The stand-ins of the ray tracing scene (a replaced FLOD and its parts): the species, template and death of its
    // first bush.
    struct StandIn
    {
        u32 species, tmpl;
        bool dead;
    };
    xr_unordered_map<const dxRender_Visual*, StandIn> stand;
    // The species' mean leaf element (a leaf, a compound leaf, a spray, a needle tuft) by area over its templates (m2):
    // a blob draws its cells that size (r3\da_shrub_cluster_vs.h), a cluster its own leaves'.
    float elem_area[kShrubSpecies]{};
    u32 pin{}; // QA: species + 1 every bush grows, 0 = the communities decide
    xr_vector<u32> draws_view, draws_shadow;

    pcstr name() const override { return "shrubs"; }
    float reach() const override { return cols.R * 1.4f; }
    u32 frame_cascades() const override { return cols.cascades; }
    bool wanted() const override { return ps_r__shrubs != 0; }
    bool pending() const override { return prepared; }
    const xr_vector<u32>& view_draws() const override { return draws_view; }
    const xr_vector<u32>& shadow_draws() const override { return draws_shadow; }

    void apply_preset(u32 p) override { preset = std::min(p, 4u); }

    void frame_begin() override
    {
        cols = kPresets[preset];
        lod = ps_r__shrubs_lod_scale;
        tier = ps_r__shrubs_force_tier;
    }

    bool skip_draw(u32 d, u32 pass) const override
    {
        // QA (r__dbg_shrubs): a kind of element or every shadow map left out.
        static constexpr u32 kDbgBit[drCount] = {1, 2 | 64, 2 | 64, 2 | 128, 4, 8, 16, 2 | 256, 2 | 256, 2 | 256, 1, 2 | 64, 1, 1, 1, 1, 1, 1, 1, 1};
        return (ps_r__dbg_shrubs & kDbgBit[d]) || (pass && (ps_r__dbg_shrubs & 32));
    }

    void fill_tables(float day, Fvector4* tables) override
    {
        for (u32 s = 0; s < kShrubSpecies; ++s)
        {
            if (!g_data.species[s].used)
                continue;
            fill_profile(g_data.species[s], elem_area[s], &tables[s * kProfF4]);
            fill_season(g_data.species[s], day, &tables[kTabSeas + s * kSeasF4]);
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

    // Every stock bush of the level: the census's FLODs whose content the data names; the size policy keeps the ones
    // scaled into tree crowns stock. A bush of one clump is one placement on its root, as tall as the stock top and as
    // wide as its box (the passable hull the AI graph and the cover points were compiled around is about 0.7 of both); a
    // group's and a thicket's clumps are filled with bushes about `spacing` apart, each on the ground under it.
    void collect_placements()
    {
        Census& C = census();
        u32 known = 0, oversize = 0;
        const float sbc = ps_r__Tree_SBC * 1.3333f;
        for (u32 ci = 0; ci < C.candidates.size(); ++ci)
        {
            const Candidate& c = C.candidates[ci];
            const u64 key = kind_of(C, c, g_data.kinds, g_data.families);
            if (!key)
                continue;
            const Kind* kind = &g_data.kinds.find(key)->second;
            ++known;
            const FTreeVisual* first = C.parts[c.parts[0]].visual;
            const Fmatrix& xf = first->world();
            const float scale_y = xf.j.magnitude(), scale_x = xf.i.magnitude();
            Fbox box;
            box.invalidate();
            for (u32 p : c.parts)
                box.merge(C.parts[p].visual->vis.box);
            const float top = box.vMax.y - xf.c.y;
            if (scale_y < g_data.min_scale || top > g_data.max_top[kind->cls])
            {
                oversize += top > g_data.max_top[kind->cls] ? 1u : 0u;
                continue;
            }
            // The static lighting the stock tree shader gives a vertex at mid height (hemi x H_MODELS).
            const FTreeVisualColors col = first->colors();
            Placement base;
            base.cls = kind->cls;
            base.dry = kind->dry;
            base.red = kind->red;
            base.flod = c.flod;
            base.parent = c.parent;
            base.group = u32(placements.size());
            base.yaw = std::atan2(-xf.i.z, xf.i.x);
            base.hemi = std::clamp(sbc * (col.scale.w * 0.7f + col.bias.w) * 1.4f, 0.f, 1.f);
            base.sun = std::clamp(sbc * (col.sun_scale * 0.7f + col.sun_bias), 0.f, 1.f);
            base.candidate = ci;
            if (kind->clumps == 0 || (kind->clumps == 1 && kind->cls != clThicket))
            {
                Placement P = base;
                P.root = xf.c;
                P.height = top * 0.96f;
                P.width = std::max(box.vMax.x - box.vMin.x, box.vMax.z - box.vMin.z) * 0.86f;
                placements.push_back(P);
                continue;
            }
            for (u32 k = 0; k < kind->clumps; ++k)
            {
                // A clump: its centre and radius in the model, its top; a wide one holds several bushes on a jittered
                // sunflower spiral, their heights varying under its top, each on the ground under it.
                const Fvector4& cl = kind->clump[k];
                Fvector centre;
                xf.transform_tiny(centre, Fvector{cl.x, 0.f, cl.y});
                const float r = cl.z * scale_x, h = cl.w * scale_y;
                const u32 n = std::clamp(u32(std::lround(r * r / (g_data.spacing * g_data.spacing))), 1u, 14u);
                const u32 hsh = pos_hash(centre);
                for (u32 i = 0; i < n; ++i)
                {
                    Placement P = base;
                    const float rr = n > 1 ? r * 0.8f * std::sqrt((float(i) + 0.5f) / float(n)) : 0.f;
                    const float az = float(i) * 2.39996323f + hash01(hsh + i) * 0.8f;
                    const float x = centre.x + std::cos(az) * rr, z = centre.z + std::sin(az) * rr;
                    P.root.set(x, ground_at(x, z, xf.c.y + h, h + 4.f, centre.y), z);
                    P.height = h * (n > 1 ? 0.6f + 0.35f * hash01(hsh * 7u + i) : 0.96f);
                    P.width = n > 1 ? std::min(2.f * r / std::sqrt(float(n)) * 1.3f, P.height * 1.6f) : 2.f * r * 0.86f;
                    P.yaw = base.yaw + hash01(hsh * 13u + i) * 6.2832f;
                    placements.push_back(P);
                }
            }
        }
        Msg("* [shrubs] %u FLODs of tree parts, %u stock bushes of the data (%u placements), %u kept stock (oversize)",
            u32(C.candidates.size()), known - oversize, u32(placements.size()), oversize);
    }

    // The placements' species: the community of the spot (the Red Forest's for the red stock, the grass under it, else
    // its class's), the species of it that stand in for the class - within their height range (a quarter beyond) first,
    // then of any height, then the class's own community - by share; a share of them standing dead.
    void pick_species()
    {
        for (Placement& P : placements)
        {
            P.species = -1;
            const u32 h = pos_hash(P.root);
            s32 comm = P.red ? g_data.red_comm : -1;
            if (comm < 0)
            {
                const s32 gc = da_grass_community_at(P.root.x, P.root.z);
                comm = gc >= 0 && gc < 16 ? g_data.grass_comm[gc] : -1;
            }
            if (comm < 0 || !g_data.communities[comm].used)
                comm = g_data.class_comm[P.cls];
            for (u32 pass = 0; pass < 4 && P.species < 0; ++pass)
            {
                if (pass == 2)
                    comm = g_data.class_comm[P.cls];
                if (comm < 0 || !g_data.communities[comm].used)
                    break;
                const Community& C = g_data.communities[comm];
                float total = 0.f, w[kCommSpecies]{};
                for (u32 i = 0; i < C.count; ++i)
                {
                    const Species& S = g_data.species[C.species[i]];
                    if (!(S.classes & (1u << P.cls)))
                        continue;
                    const bool fits = P.height >= S.height[0] * 0.75f && P.height <= S.height[1] * 1.25f;
                    if ((pass & 1u) == 0 && !fits)
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
            if (P.species >= 0)
                P.dead = hash01(h ^ 0x5bd1e995u) < g_data.species[P.species].dead + (P.dry ? g_data.dry_dead : 0.f);
        }
    }

    // The templates the placements need, generated on the task pool and packed; the instances on the placements.
    bool build()
    {
        xr_vector<u32> tmpl_of(kShrubSpecies * kHeights * kVariants, ~0u);
        struct Job
        {
            u32 species, height_class, variant;
            da_shrub::Template T;
        };
        xr_vector<Job> jobs;
        struct Use
        {
            u32 placement, species, height_class, variant;
            float height, width;
        };
        xr_vector<Use> uses;
        for (u32 i = 0; i < placements.size(); ++i)
        {
            const Placement& P = placements[i];
            if (P.species < 0)
                continue;
            const u32 sp = pin ? pin - 1 : u32(P.species);
            const Species& S = g_data.species[sp];
            Use u{i, sp, 0, (pos_hash(P.root) >> 9) % kVariants, std::clamp(P.height, S.height[0] * 0.75f, S.height[1] * 1.25f), 0.f};
            u.width = P.width * u.height / std::max(P.height, 0.1f);
            float best = 1e9f;
            for (u32 k = 0; k < kHeights; ++k)
                if (const float e = std::abs(template_height(S, k) - u.height); e < best)
                {
                    best = e;
                    u.height_class = k;
                }
            u32& t = tmpl_of[(sp * kHeights + u.height_class) * kVariants + u.variant];
            if (t == ~0u)
            {
                t = u32(jobs.size());
                jobs.push_back(Job{sp, u.height_class, u.variant, {}});
            }
            uses.push_back(u);
        }
        xr_parallel_for(TaskRange<size_t>(0, jobs.size(), 1), [&jobs](const TaskRange<size_t>& range) {
            for (size_t i = range.begin(); i != range.end(); ++i)
            {
                Job& j = jobs[i];
                const Species& S = g_data.species[j.species];
                const float h = template_height(S, j.height_class);
                da_shrub::generate(S.arch, h, h * 0.5f * (S.width[0] + S.width[1]),
                    1u + j.species * 7919u + j.height_class * 104729u + j.variant * 1299709u, j.T);
            }
        });
        xr_vector<u32> elems, axes;
        templates.assign(jobs.size(), Template{});
        for (u32 i = 0; i < jobs.size(); ++i)
        {
            Template& T = templates[i];
            T.species = jobs[i].species;
            T.height_class = jobs[i].height_class;
            T.variant = jobs[i].variant;
            pack_template(jobs[i].T, T, elems, axes);
        }
        double e_area[kShrubSpecies]{}, e_count[kShrubSpecies]{};
        for (const Job& j : jobs)
        {
            e_area[j.species] += j.T.leaf_area;
            e_count[j.species] += double(j.T.leaves.size());
        }
        for (u32 s = 0; s < kShrubSpecies; ++s)
            elem_area[s] = e_count[s] > 0.0 ? float(e_area[s] / e_count[s]) : 0.f;
        // (The profile rows carry them: the tables are filled again.)
        tables_dirty();
        if (!make_buffer(elems_b, u32(elems.size() * 4), elems.data()) || !make_buffer(axes_b, u32(axes.size() * 4), axes.data()))
            return false;
        // The template scaled to the placement's height, its width within a fifth of the template's; half of them
        // mirrored (a template's every other bush is its mirror image).
        instances.clear();
        instances.reserve(uses.size());
        max_height = 0.f;
        for (const Use& u : uses)
        {
            const Placement& P = placements[u.placement];
            Instance I;
            I.tmpl = tmpl_of[(u.species * kHeights + u.height_class) * kVariants + u.variant];
            I.species = u.species;
            I.placement = u.placement;
            const Template& T = templates[I.tmpl];
            I.scale_h = u.height / std::max(T.height, 0.1f);
            I.scale_w = I.scale_h * std::clamp(u.width / std::max(T.width * I.scale_h, 0.1f), 0.8f, 1.25f);
            I.scale = I.scale_h;
            I.hemi = P.hemi;
            I.sun = P.sun;
            const u32 hp = pos_hash(P.root);
            I.seed = u16(hp >> 16);
            I.pheno = s8(std::lround((hash01(hp ^ 0x9e3779b9u) * 2.f - 1.f) * 127.f));
            I.dead = P.dead;
            const float c = std::cos(P.yaw), s = std::sin(P.yaw);
            const float mx = (hp & 1u) ? -1.f : 1.f;
            const float rows[3][4] = {{c * I.scale_w * mx, 0.f, s * I.scale_w, P.root.x}, {0.f, I.scale_h, 0.f, P.root.y},
                {-s * I.scale_w * mx, 0.f, c * I.scale_w, P.root.z}};
            CopyMemory(I.row, rows, sizeof(rows));
            Fvector centre{rows[0][0] * T.centre.x + rows[0][2] * T.centre.z, T.centre.y * I.scale_h,
                rows[2][0] * T.centre.x + rows[2][2] * T.centre.z};
            centre.add(P.root);
            I.bound.set(centre, T.radius * std::max(I.scale_h, I.scale_w));
            I.box.set(centre, centre);
            I.box.grow(I.bound.R);
            max_height = std::max(max_height, T.height * I.scale_h);
            instances.push_back(I);
        }
        return true;
    }

    // The species, templates, instances and grid of the placements (a level load, a species pin).
    bool rebuild()
    {
        loaded = active = false;
        if (!build())
            return false;
        build_grid(*this);
        tables_dirty();
        loaded = active = !instances.empty();
        return true;
    }

    // The stock bushes the shrubs replace leave (or rejoin) their parent hierarchy: every view, shadow, billboard, the
    // GPU-driven registry and the ray tracing scene walk the sector hierarchies, so a detached FLOD is gone from all of
    // them at no cost per frame (HideOverlayGeometry's precedent). A group goes when any of its bushes stands.
    void set_detached(bool detach, bool announce) override
    {
        if (detached == detach)
            return;
        xr_vector<u8> group_on(placements.size(), 0);
        for (const Placement& P : placements)
            if (P.species >= 0)
                group_on[P.group] = 1;
        replaced.clear();
        for (u32 i = 0; i < placements.size(); ++i)
        {
            const Placement& P = placements[i];
            if (P.group != i || !group_on[i] || !P.flod || !P.parent)
                continue;
            auto& children = P.parent->children;
            const auto it = std::find(children.begin(), children.end(), P.flod);
            if (detach)
            {
                if (it != children.end())
                    children.erase(it);
                replaced.insert(P.flod);
            }
            else if (it == children.end())
                children.push_back(P.flod);
        }
        detached = detach;
        stand.clear();
        if (detach)
            for (const Instance& I : instances)
            {
                const Placement& head = placements[placements[I.placement].group];
                if (!head.flod || !stand.emplace(head.flod, StandIn{I.species, I.tmpl, I.dead}).second)
                    continue;
                for (const dxRender_Visual* part : static_cast<FHierrarhyVisual*>(head.flod)->children)
                    stand.emplace(part, StandIn{I.species, I.tmpl, I.dead});
            }
        if (announce)
            Msg("* [shrubs] %s", detached ? "the procedural shrubs stand in for the stock bushes" : "the stock bushes are back (QA)");
    }

    bool stand_in(const dxRender_Visual* visual, u32& species, u32& tmpl, bool& dead) const override
    {
        const auto it = stand.find(visual);
        if (it == stand.end())
            return false;
        species = it->second.species;
        tmpl = it->second.tmpl;
        dead = it->second.dead;
        return true;
    }

    bool prepare(bool shaders, bool) override
    {
        prepared = false;
        const auto fail = [this](pcstr why) {
            Msg("! [shrubs] the procedural shrubs are off on this level (%s): the stock bushes stand", why);
            release();
            failed = true;
            return false;
        };
        data_reload();
        if (!g_data.file)
            return fail("dead_air_x64_shrubs.ltx not found");
        if (!shaders)
            return fail("the shaders or the textures are missing");
        dx11GpuTimers::RegisterField("shrubs_cpu", [] {
            Population* P = population(kPopShrubs);
            const u64 ticks = P ? P->cpu_ticks.exchange(0, std::memory_order_relaxed) : 0;
            const u32 frames = P ? P->cpu_frames.exchange(0, std::memory_order_relaxed) : 0;
            return frames ? float(double(ticks) * 1000.0 / double(CPU::qpc_freq) / frames) : 0.f;
        });
        t0 = CPU::QPC();
        collect_placements();
        pick_species();
        if (std::none_of(placements.begin(), placements.end(), [](const Placement& P) { return P.species >= 0; }))
        {
            Msg("* [shrubs] no stock bush of this level grows a shrub of the data (%u placements)", u32(placements.size()));
            placements.clear();
            return false;
        }
        // The census's models a bush stands on are the shrubs'; the rest stay free for the trees.
        for (const Placement& P : placements)
            if (P.species >= 0)
                census().candidates[P.candidate].taken = kPopShrubs;
        prepared = true;
        return true;
    }

    void finish_load() override
    {
        prepared = false;
        if (!rebuild())
        {
            Msg("! [shrubs] the procedural shrubs are off on this level (the template buffers could not be created): the "
                "stock bushes stand");
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
        Msg("* [shrubs] %u bushes of %u placements, %u templates, %u elements (%u KB), %.0f ms", u32(instances.size()),
            u32(placements.size()), u32(templates.size()), elems, (elems_b.bytes + axes_b.bytes) >> 10,
            float(CPU::QPC() - t0) * 1000.f / float(CPU::qpc_freq));
    }

    void release() override
    {
        set_detached(false, false);
        elems_b.release();
        axes_b.release();
        aux_b.release();
        templates.clear();
        instances.clear();
        cells.clear();
        cell_items.clear();
        placements.clear();
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

    // One bush in the view at distance d: the near tier (wood, leaves, blooms), the middle (the main wood, clusters), the
    // far (stems, blobs), each crossfading into the next over the last kBand of its distance by the elements' hashes;
    // bare, a bush keeps its twigs a tier longer (Weber and Penn's leafless rule).
    void view(u32 i, float d) override
    {
        const Latch& L = latch();
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const Species& S = g_data.species[I.species];
        const float scale = I.scale_h;
        const bool leaves = !I.dead && leafy(I.species);
        const bool blooms = !I.dead && blooming(I.species);
        const float size_k = std::clamp(std::sqrt(T.height * scale / 2.5f), 0.7f, 1.6f) * (leaves ? 1.f : 1.35f);
        // The leaves end where the mean leaf (a spray, a compound leaf, a needle tuft) is under two pixel floors on the
        // screen, the preset's distance at the most: the clusters take over where their leaf cells are a few pixels and
        // their pattern no longer reads against the leaves (from four floors they came in at six pixels a cell, and a
        // bush visibly drew in as one walked up to it). Small leaves are alpha tested, not dithered: no haze.
        const float d0 = cols.d0 * size_k * lod, d2 = cols.d2 * size_k * lod, R = cols.R * std::min(size_k, 1.4f);
        const float d1 =
            std::max(std::min(cols.d1 * size_k, T.leaf_len * scale * L.px_per_m / (cols.leaf_floors / ps_r__shrubs_far_detail * cols.kpx)) * lod,
                d0 * 1.2f);
        // A leaf's hull (eight vertices fitted to its texture's outline, a spray's six leaves forty-eight) pays for itself
        // while the leaf is two dozen pixels and more: past that its texture's quad (four vertices) costs less than the
        // fragments the hull saves (the spray's leaf, not the spray, sizes it; the vertex work of a near willow's sprays was
        // a millisecond of its own).
        const bool spray = S.arch.leaf_kind == da_shrub::lkSpray;
        const float leaf_size = (spray ? 0.5f * (S.arch.leaf_len[0] + S.arch.leaf_len[1]) : T.leaf_len) * scale;
        const float d_blade = std::clamp(leaf_size * L.px_per_m / 24.f * lod, std::min(1.f, d0), d0);
        float f0 = smoothstep(d0 * (1.f - kBand), d0, d), f1 = smoothstep(d1 * (1.f - kBand), d1, d);
        float f2 = smoothstep(d2 * (1.f - kBand), d2, d), fb = smoothstep(d_blade * (1.f - kBand), d_blade, d);
        // QA: every bush in one tier, its pixel floor still the distance's.
        if (tier >= 0)
        {
            f0 = fb = tier >= 1 ? 1.f : 0.f;
            f1 = tier >= 2 ? 1.f : 0.f;
            f2 = tier >= 3 ? 1.f : 0.f;
        }
        const float fR = 1.f - smoothstep(R * 0.9f, R, d);
        if (fR <= 0.f)
            return;
        const float px = L.px_per_m / std::max(d, 0.1f);
        const u32 rec = frame_record(*this, i);
        ++seen[f1 < 1.f ? 0 : (f2 < 1.f ? 1 : 2)];
        // The wood by level, each finer level dissolving out by its segments' hashes over the band where its tier ends:
        // the finest twigs past d0, the twigs past d1 (a bare bush keeps them to d2), the branches past d2, the stems at d2
        // and a half. The wire rule thins what is under a pixel within a tier (da_shrub_wood_vs.h). Within d0 the stems
        // and the branches are eight-sided tubes (a five-sided stem's corners show at arm's length); the twigs, a few
        // pixels across, stay five-sided.
        const u32 w0 = T.first[kKindWood];
        const Draw wood = d < d0 ? drWood8 : drWood;
        if (f2 < 1.f || d < d2 * 1.5f)
            emit(*this, 0, wood, rec, w0, T.wood_level[0], 1.f, 0.f, 1.f);
        emit(*this, 0, wood, rec, w0 + T.wood_level[0], T.wood_level[1] - T.wood_level[0], 1.f, f2, 1.f);
        emit(*this, 0, drWood, rec, w0 + T.wood_level[1], T.wood_level[2] - T.wood_level[1], 1.f, leaves ? f1 : f2, 1.f);
        emit(*this, 0, drWood, rec, w0 + T.wood_level[2], T.wood_level[3] - T.wood_level[2], 1.f, std::max(f0, f1), 1.f);
        if (f1 < 1.f)
        {
            if (leaves)
            {
                // The blades (a spray's own leaves) give way to diamonds (a spray's leaves as diamonds) over the band past
                // the blade distance, element by element, as every tier does. A spray stands as its leaves in both: a quad
                // turned to the eye showed all its leaves face on where the near ones turn every way, and a bush read
                // twice as dense and in flakes until one walked up to it. Its pixel floor is its leaves'.
                const float keep = floor_keep(leaf_size, px, cols.kpx);
                const u32 n = T.count[kKindLeaf];
                // Near, a compound or a lobed leaf is folded along its midrib (its rachis) and droops like a blade: its
                // texture's square on the eight-vertex strip (a flat quad read as paper); a needle sprig is two crossed
                // quads near (one turned to the eye showed a flat frond, a fern), one farther.
                const bool needles = S.arch.leaf_kind == da_shrub::lkNeedles;
                const bool cut = S.arch.leaf_kind != da_shrub::lkSimple || S.lobes > 0.5f;
                u32 near_draw = needles ? drNeedle2 : drLeaf8, far_draw = cut ? drLeafCut : drLeaf4;
                if (spray)
                {
                    near_draw = drSpray;
                    far_draw = drSpray4;
                }
                // (A needle tuft is a few centimetres: by its own size it would be crossed quads for a metre or two; they
                // hold to half of d0, where the brush still reads as one.)
                const float fn = tier >= 0 ? fb : smoothstep(0.5f * d0 * (1.f - kBand), 0.5f * d0, d);
                const float lo = std::max(needles ? fn : fb, f1);
                if (near_draw == far_draw)
                    emit_thinned(*this, 0, far_draw, rec, T.first[kKindLeaf], n, keep, keep, f1, 1.f);
                else
                {
                    emit_thinned(*this, 0, near_draw, rec, T.first[kKindLeaf], n, keep, keep, lo, 1.f);
                    emit_thinned(*this, 0, far_draw, rec, T.first[kKindLeaf], n, keep, keep, f1, lo);
                }
            }
            if (blooms)
                emit(*this, 0, drBloom, rec, T.first[kKindBloom], T.count[kKindBloom], 1.f, f1, 1.f);
        }
        // (The middle and far parts' prefixes end softly as the leaves' do: emit_thinned.)
        if (f1 > 0.f && f2 < 1.f)
        {
            const float keep = floor_keep(2.f * T.cluster_r * scale, px, cols.kpx * 2.f);
            emit_thinned(*this, 0, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep, keep, f2, f1);
            if (blooms)
                emit_thinned(*this, 0, drBloom, rec, T.first[kKindBloom], T.count[kKindBloom], keep * 0.5f, 1.f, f2, f1);
        }
        if (f2 > 0.f)
        {
            const float keep = floor_keep(2.f * T.blob_r * scale, px, cols.kpx * 3.f);
            emit_thinned(*this, 0, drBlob, rec, T.first[kKindBlob], T.count[kKindBlob], keep, keep, 0.f, std::min(f2, fR));
        }
    }

    // One bush in sun cascade c: the near cascade takes the wood and the leaves (at its texel), the middle the main wood
    // and the clusters, the far the blobs.
    void cascade(u32 c, u32 i, float texel) override
    {
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const Species& S = g_data.species[I.species];
        const float scale = I.scale_h;
        const u32 rec = frame_record(*this, i);
        const bool leaves = !I.dead && leafy(I.species);
        const float px = 1.f / std::max(texel, 1e-3f);
        if (c == 0)
        {
            emit(*this, 1 + c, drWood, rec, T.first[kKindWood], T.wood_level[2], 1.f, 0.f, 1.f);
            if (leaves)
            {
                const float keep = floor_keep(T.leaf_len * scale, px, 1.5f);
                const u32 draw = S.arch.leaf_kind != da_shrub::lkSimple || S.lobes > 0.5f ? drLeafCut : drLeaf4;
                emit_thinned(*this, 1 + c, draw, rec, T.first[kKindLeaf], T.count[kKindLeaf], keep, keep, 0.f, 1.f);
            }
        }
        else if (c == 1)
        {
            emit(*this, 1 + c, drWood, rec, T.first[kKindWood], T.wood_level[1], 1.f, 0.f, 1.f);
            const float keep = floor_keep(2.f * T.cluster_r * scale, px, 2.f);
            emit_thinned(*this, 1 + c, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep, keep, 0.f, 1.f);
        }
        else
        {
            const float keep = floor_keep(2.f * T.blob_r * scale, px, 2.f);
            emit_thinned(*this, 1 + c, drBlob, rec, T.first[kKindBlob], T.count[kKindBlob], keep, keep, 0.f, 1.f);
        }
    }

    // The rain's occlusion map: a crown's blobs from above (a bare bush shelters little, its blobs' leaves are off).
    void rain(u32 i) override
    {
        const Instance& I = instances[i];
        if (I.dead)
            return;
        const Template& T = templates[I.tmpl];
        emit(*this, kRainPass, drBlob, frame_record(*this, i), T.first[kKindBlob], T.count[kKindBlob], 1.f, 0.f, 1.f);
    }

    // A shadowed local light: the main wood and the clusters, kept by the light's texel (a flashlight's map is coarse).
    void light(u32 i, float texel) override
    {
        const Instance& I = instances[i];
        const Template& T = templates[I.tmpl];
        const u32 rec = frame_record(*this, i);
        emit(*this, 0, drWood, rec, T.first[kKindWood], T.wood_level[1], 1.f, 0.f, 1.f);
        if (!I.dead && leafy(I.species))
        {
            const float keep = floor_keep(2.f * T.cluster_r * I.scale_h, 1.f / std::max(texel, 1e-3f), 2.f);
            emit_thinned(*this, 0, drCluster, rec, T.first[kKindCluster], T.count[kKindCluster], keep, keep, 0.f, 1.f);
        }
    }

private:
    u64 t0{};
};

ShrubPopulation* g_shrubs{};

ShrubPopulation& shrubs()
{
    static ShrubPopulation P;
    if (!g_shrubs)
    {
        g_shrubs = &P;
        // The G-buffer's order, near to far for early-Z: the wood and the leaves, the blooms, then the middle and far
        // tiers; the shadow maps' draws.
        P.draws_view = {drWood8, drWood, drLeaf8, drNeedle2, drSpray, drSprayCut, drSpray4, drLeaf4, drLeafCut, drBloom, drCluster,
            drBlob};
        P.draws_shadow = {drWood, drLeaf4, drLeafCut, drCluster, drBlob};
    }
    return P;
}
} // namespace

namespace da_veg
{
Population* shrubs_population() { return &shrubs(); }
} // namespace da_veg

void da_shrubs_stats()
{
    ShrubPopulation& P = shrubs();
    if (!P.loaded)
    {
        Msg("* [shrubs] off%s", P.failed ? " (failed on this level)" : "");
        return;
    }
    const PresetColumns& c = kPresets[P.preset];
    Msg("* [shrubs] preset %u: d0/d1/d2 %.0f/%.0f/%.0f m, R %.0f m, pixel floor %.2f px, %u cascades; %s", P.preset, c.d0, c.d1,
        c.d2, c.R, c.kpx, c.cascades, P.detached ? "standing" : "the stock bushes stand (r__shrubs 0)");
    Msg("* [shrubs] %u bushes on %u placements, %u templates; this frame %u near, %u middle, %u far, %u instance records, "
        "%u chunks (both populations)",
        u32(P.instances.size()), u32(P.placements.size()), u32(P.templates.size()), P.seen[0], P.seen[1], P.seen[2],
        frame_records(), frame_chunks());
    static constexpr pcstr names[drCount] = {"wood", "leaf 8", "leaf 4", "leaf cut", "bloom", "cluster", "blob", "spray",
        "spray cut", "spray 4", "wood 8", "needle 2", "wood 16", "stock 32", "stock 16", "stock 8", "patch", "wood far", "stock far",
        "patch far"};
    for (u32 d = 0; d < drCount; ++d)
    {
        if (d > drNeedle2)
            break;
        Msg("* [shrubs]   %-9s %7u elements, view %5u chunks, cascades %5u / %5u / %5u, rain %5u", names[d], P.elements[d],
            P.draw_count[0][d], P.draw_count[1][d], P.draw_count[2][d], P.draw_count[3][d], P.draw_count[kRainPass][d]);
    }
    Msg("* [shrubs]   %u shadowed local lights cast vegetation this frame (%u offered, %u within %.0f m), %u chunks of shrubs",
        lights_cast(), lights_offered(), lights_near(), kLightReach, light_chunks(kPopShrubs));
    Msg("* [shrubs]   %u sun cascades asked before their frame was built", stale_passes());
    u32 per_species[kShrubSpecies]{}, dead = 0;
    for (const Instance& I : P.instances)
    {
        ++per_species[I.species];
        dead += I.dead ? 1u : 0u;
    }
    for (u32 s = 0; s < kShrubSpecies; ++s)
        if (per_species[s])
            Msg("* [shrubs]   %-8s %5u bushes", g_data.species[s].name.c_str(), per_species[s]);
    Msg("* [shrubs]   %u of them standing dead", dead);
}

void da_shrubs_probe(pcstr args)
{
    ShrubPopulation& P = shrubs();
    if (!P.loaded)
    {
        Msg("* [shrubs] probe: off");
        return;
    }
    // (`ray`: the bushes whose bounds the view's ray passes through, nearest first.)
    const Fvector cam = Device.vCameraPosition, dir = Device.vCameraDirection;
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
    const PresetColumns& c = kPresets[P.preset];
    for (size_t k = 0; k < n; ++k)
    {
        const Instance& I = P.instances[closest[k].second];
        const Template& T = P.templates[I.tmpl];
        const Species& S = g_data.species[I.species];
        const float size_k = std::clamp(std::sqrt(T.height * I.scale_h / 2.5f), 0.7f, 1.6f);
        const float d = std::max(closest[k].first - 0.5f * I.bound.R, 0.f);
        pcstr tier = d < c.d0 * size_k ? "near" : d < c.d1 * size_k ? "leaves" : d < c.d2 * size_k ? "clusters" : "blobs";
        Msg("* [shrubs] probe %2u: %-8s at %5.1f m (%6.1f %5.1f %6.1f), %.1f m tall, template %u (h %.1f, class %u, variant %u), %s%s",
            u32(k), S.name.c_str(), d, I.bound.P.x, I.box.vMin.y, I.bound.P.z, T.height * I.scale_h, I.tmpl, T.height,
            T.height_class, T.variant, tier, I.dead ? ", dead" : "");
    }
}

void da_shrubs_pin_species(pcstr name)
{
    ShrubPopulation& P = shrubs();
    const pcstr arg = name ? skip_space(name) : "";
    u32 pin = 0;
    if (*arg && xr_stricmp(arg, "off") && xr_stricmp(arg, "0"))
    {
        const s32 sp = species_by_name(arg);
        if (sp < 0)
        {
            Msg("! [shrubs] species pin: no species '%s'", arg);
            return;
        }
        pin = u32(sp) + 1;
    }
    P.pin = pin;
    Msg("* [shrubs] species pin: %s", P.pin ? g_data.species[P.pin - 1].name.c_str() : "off (the communities decide)");
    if (P.loaded && !P.rebuild())
        Msg("! [shrubs] the pinned species' templates could not be built");
}
} // namespace xray::render::RENDER_NAMESPACE
