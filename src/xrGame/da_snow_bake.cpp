// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The level's snow fields at 1 m (docs/dead-air/WINTER.md), baked once per level load from the
// collision mesh and level.details:
//   ground  - the terrain, hole-filled (roofs and decks do not count as ground);
//   top     - the topmost non-passable surface, where snow lies (roofs and decks get snow, nothing
//             under them); passable things - tree crowns, bushes, water, occluders - are skipped;
//   classes - open, under trees (passable crowns and tree visuals), road (paved collision materials
//             minus railway ballast, wide paved yards excepted), drift (the lee of walls, fences,
//             buildings and forest edges for the westerly winter wind, windward toes, hollows, yards);
//   factors - accumulation A (slope, lee, hollows, canopy), melt M (sun aspect, tree wells, lee,
//             paving), a two-octave noise and the neglect N;
//   LUT     - this level's 365 days of (S_c, Mc_c) per class, solved from histograms of its own
//             A_c / M_c so that every class hits the cover and depth keys of [snow_keys].
// Thresholds and material names are data ([snow_bake] of dead_air_x64_seasons.ltx).

#include "StdAfx.h"
#include "da_snow_bake.h"
#include "da_snow_deformers.h"
#include "Level.h"
#include "GamePersistent.h"
#include "xrEngine/da_snow_field.h"
#include "xrEngine/Environment.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"

namespace
{
constexpr float none_y = -1e30f;
constexpr u32 max_dim = 4096;     // texels per axis; the largest shipped level is 1.6 km

// ---- Data ---------------------------------------------------------------------------------------------

// [snow_bake] of dead_air_x64_seasons.ltx (docs/dead-air/SEASON_DATA.md has every field); the defaults
// below are the same values, so a missing file or line bakes the shipped level.
struct SBakeParams
{
    float texel = 1.f;
    float ground_band = 1.f;                  // ground_dy
    float receiver_ny = 0.5f;
    float slope_lo = 0.35f, slope_hi = 0.75f; // slope = a, b: saturate((ny - a) / b)
    float canopy_height = 2.f;                // crown_h
    float tree_disc_radius = 2.4f, tree_disc_weight = 0.7f; // tree_disc = a x the half-width, weight
    float canopy_blur = 3.f, canopy_class = 0.5f, canopy_lee_cut = 0.3f, canopy_snow = 0.45f;
    float well_lo = 0.2f, well_hi = 0.6f;     // tree_well = a, b (the melt weight is melt's b)
    float forest_area = 300.f;
    float obstacle = 0.8f, lee_cap = 2.5f, blob_area = 4.f, wall_ny = 0.3f;
    float wind_from[8]{270.f, 247.5f, 292.5f}, wind_weight[8]{1.f, 0.7f, 0.7f};
    u32 wind_dirs = 3;
    float lee_reach = 15.f, lee_flat = 6.f, lee_end = 12.f;
    float toe_reach = 1.5f, toe_weight = 0.45f;
    float hollow_blur = 9.f, hollow_lo = 0.2f, hollow_range = 0.4f;
    float building_height = 2.5f, building_area = 25.f;
    float yard_weight = 0.7f, yard_decay = 10.f, yard_reach = 20.f;
    float fence_height = 0.6f, fence_weight = 0.5f, fence_reach = 5.f;
    float drift_class = 0.5f, class_blur = 3.f;
    float lee_gain = 0.6f, drift_min = 0.3f, drift_max = 1.6f; // drift_factor
    float depression_blur = 7.f, depression_gain = 2.5f, depression_min = 0.7f, depression_max = 1.4f;
    float sun_elevation = 15.f;
    float melt_sun = 0.35f, melt_treewell = 0.4f, melt_lee = 0.3f, melt_road = 0.4f;
    float noise_len[4]{3.f, 12.f}, noise_weight[4]{0.6f, 0.4f};
    u32 noise_octaves = 2;
    float floor_a = 0.001f, floor_m = 0.2f;
    float cover_eps = 0.01f;
    u32 lut_bins = 256;
    float rail_dilation = 8.f, wide_paved = 25.f;
    xr_string road{"materials\\asphalt, materials\\gravel, materials\\concrete"};
    xr_string ballast{"materials\\gravel"};
    xr_string rail{"materials\\metal, materials\\metal_pipe"};
    xr_string skip{"materials\\bush, materials\\bush_sux, materials\\death, materials\\fake, materials\\occ, "
                   "materials\\water, materials\\water_radiation"};
    xr_string crown{"materials\\bush, materials\\bush_sux"};
    xr_string water{"materials\\water, materials\\water_radiation"};
    xr_string trunk{"materials\\tree_trunk"};
    xr_string soil{"default, materials\\earth, materials\\grass, materials\\earth_slide, materials\\dirt, materials\\sand, "
                   "materials\\earth_death"};
    xr_string fence{"materials\\setka_rabica, materials\\wooden_board, materials\\wood, materials\\concrete"};
};

// A tree visual's crown half-width the tree_disc factor scales (the game layer sees the root, not the
// bounding box; the shipped trees run 5-7 m across).
constexpr float tree_half_width = 3.f;

SBakeParams read_params()
{
    SBakeParams p;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_seasons.ltx");
    if (!FS.exist(path))
        return p;
    const CInifile ini(path, TRUE);
    constexpr pcstr S = "snow_bake";
    if (!ini.section_exist(S))
        return p;
    // Up to n comma-separated numbers: the ones present replace the defaults in order.
    const auto v = [&](pcstr line, std::initializer_list<float*> out) {
        if (!ini.line_exist(S, line))
            return;
        pcstr src = ini.r_string(S, line);
        const int n = std::min(_GetItemCount(src, ','), int(out.size()));
        auto it = out.begin();
        for (int i = 0; i < n; ++i, ++it)
        {
            string64 item;
            _GetItem(src, i, item, ',');
            float x;
            if (sscanf(item, "%f", &x) == 1 && _valid(x))
                **it = x;
        }
    };
    const auto list = [&](pcstr line, float* out, u32 cap) {
        if (!ini.line_exist(S, line))
            return 0u;
        pcstr src = ini.r_string(S, line);
        const u32 n = std::min(u32(_GetItemCount(src, ',')), cap);
        u32 got = 0;
        for (u32 i = 0; i < n; ++i)
        {
            string64 item;
            _GetItem(src, int(i), item, ',');
            float x;
            if (sscanf(item, "%f", &x) == 1 && _valid(x))
                out[got++] = x;
        }
        return got;
    };
    const auto l = [&](pcstr line, xr_string& out) {
        if (ini.line_exist(S, line))
            out = ini.r_string(S, line);
    };

    v("texel", {&p.texel});
    v("receiver_ny", {&p.receiver_ny});
    v("ground_dy", {&p.ground_band});
    float slope_b = p.slope_hi - p.slope_lo;
    v("slope", {&p.slope_lo, &slope_b});
    p.slope_hi = p.slope_lo + std::max(slope_b, 0.01f);
    v("crown_h", {&p.canopy_height});
    float disc_factor = p.tree_disc_radius / tree_half_width;
    v("tree_disc", {&disc_factor, &p.tree_disc_weight});
    p.tree_disc_radius = disc_factor * tree_half_width;
    v("canopy_blur", {&p.canopy_blur});
    v("canopy_class", {&p.canopy_class});
    v("canopy_lee", {&p.canopy_lee_cut});
    v("canopy_shield", {&p.canopy_snow});
    v("tree_well", {&p.well_lo, &p.well_hi});
    v("forest_area", {&p.forest_area});
    v("obstacle_h", {&p.obstacle});
    v("obstacle_cap", {&p.lee_cap});
    v("obstacle_area", {&p.blob_area});
    v("wall_ny", {&p.wall_ny});
    {
        float from[8], weight[8];
        const u32 nf = list("wind_from", from, 8);
        if (nf)
        {
            const u32 nw = list("wind_weight", weight, 8);
            p.wind_dirs = nf;
            for (u32 i = 0; i < nf; ++i)
            {
                p.wind_from[i] = from[i];
                p.wind_weight[i] = i < nw ? weight[i] : 1.f;
            }
        }
    }
    v("lee_reach", {&p.lee_reach});
    v("lee_flat", {&p.lee_flat});
    v("lee_end", {&p.lee_end});
    v("toe_reach", {&p.toe_reach});
    v("toe_weight", {&p.toe_weight});
    v("hollow", {&p.hollow_blur, &p.hollow_lo, &p.hollow_range});
    v("building", {&p.building_height, &p.building_area});
    v("yard", {&p.yard_weight, &p.yard_decay, &p.yard_reach});
    v("fence", {&p.fence_height, &p.fence_weight, &p.fence_reach});
    v("drift_class", {&p.drift_class});
    v("class_blur", {&p.class_blur});
    v("drift_factor", {&p.lee_gain, &p.drift_min, &p.drift_max});
    v("depression", {&p.depression_blur, &p.depression_gain, &p.depression_min, &p.depression_max});
    v("sun_elevation", {&p.sun_elevation});
    v("melt", {&p.melt_sun, &p.melt_treewell, &p.melt_lee, &p.melt_road});
    {
        float pairs[8];
        const u32 n = list("noise", pairs, 8) / 2;
        if (n)
        {
            p.noise_octaves = n;
            for (u32 i = 0; i < n; ++i)
            {
                p.noise_len[i] = pairs[2 * i];
                p.noise_weight[i] = pairs[2 * i + 1];
            }
        }
    }
    // The noise amplitudes and the drift shape belong to the field's depth formula (SSnowField), which
    // the renderer evaluates too: the data can only confirm them.
    {
        float amp_open = 0.35f, amp_road = 0.15f, shape_a = 0.5f, shape_b = 0.3f;
        v("noise_amp", {&amp_open, &amp_road});
        v("drift_shape", {&shape_a, &shape_b});
        if (_abs(amp_open - 0.35f) > 1e-4f || _abs(amp_road - 0.15f) > 1e-4f || _abs(shape_a - 0.5f) > 1e-4f ||
            _abs(shape_b - 0.3f) > 1e-4f)
            Msg("! [snow] [snow_bake] noise_amp / drift_shape differ from the snow field's fixed formula "
                "(0.35, 0.15 / 0.5, 0.3): the formula's values are used");
    }
    v("floor", {&p.floor_a, &p.floor_m});
    v("cover_eps", {&p.cover_eps});
    float bins = float(p.lut_bins);
    v("lut_bins", {&bins});
    p.lut_bins = u32(clampr(bins, 32.f, 1024.f));
    v("rail_dilation", {&p.rail_dilation});
    v("wide_paved", {&p.wide_paved});
    l("road", p.road);
    l("ballast", p.ballast);
    l("rail", p.rail);
    l("skip", p.skip);
    l("crown", p.crown);
    l("water", p.water);
    l("trunk", p.trunk);
    l("soil", p.soil);
    l("fence_materials", p.fence);

    p.texel = clampr(p.texel, 0.5f, 4.f);
    p.hollow_range = std::max(p.hollow_range, 0.01f);
    p.lee_flat = clampr(p.lee_flat, 0.f, p.lee_end - 0.01f);
    p.floor_m = clampr(p.floor_m, 0.05f, 1.f);
    p.floor_a = clampr(p.floor_a, 1e-5f, 0.1f);
    p.cover_eps = clampr(p.cover_eps, 0.001f, 0.05f);
    p.wind_dirs = clampr(p.wind_dirs, 1u, 8u);
    return p;
}

// ---- Materials ------------------------------------------------------------------------------------------

enum : u16
{
    mc_road = 1 << 0,
    mc_ballast = 1 << 1,
    mc_rail = 1 << 2,
    mc_skip = 1 << 3,
    mc_crown = 1 << 4,
    mc_water = 1 << 5,
    mc_trunk = 1 << 6,
    mc_soil = 1 << 7,
    mc_fence = 1 << 8,
};

// A data name matches the full material name ("materials\asphalt") or its last part ("asphalt").
bool in_list(const xr_string& list, pcstr name)
{
    pcstr leaf = strrchr(name, '\\');
    leaf = leaf ? leaf + 1 : name;
    const int n = _GetItemCount(list.c_str(), ',');
    for (int i = 0; i < n; ++i)
    {
        string128 item;
        _GetItem(list.c_str(), i, item, ',');
        if (*item && (!xr_stricmp(item, name) || !xr_stricmp(item, leaf)))
            return true;
    }
    return false;
}

xr_vector<u16> material_classes(const SBakeParams& p)
{
    xr_vector<u16> cls(GMLib.CountMaterial(), 0);
    for (u32 i = 0; i < cls.size(); ++i)
    {
        const SGameMtl* m = GMLib.GetMaterialByIdx(u16(i));
        if (!m)
            continue;
        pcstr name = m->m_Name.c_str();
        u16 c = 0;
        const bool passable = m->Flags.test(SGameMtl::flPassable);
        const bool liquid = m->Flags.test(SGameMtl::flLiquid);
        if (in_list(p.road, name))
            c |= mc_road;
        if (in_list(p.ballast, name))
            c |= mc_ballast;
        if (in_list(p.rail, name))
            c |= mc_rail;
        if (passable || in_list(p.skip, name))
            c |= mc_skip;
        if (in_list(p.crown, name))
            c |= mc_crown | mc_skip;
        // Liquid AND passable: a tiled floor carries flLiquid too, and it is a floor.
        if (in_list(p.water, name) || (liquid && passable))
            c |= mc_water | mc_skip;
        if (in_list(p.trunk, name))
            c |= mc_trunk;
        if (in_list(p.soil, name))
            c |= mc_soil;
        if (in_list(p.fence, name))
            c |= mc_fence;
        cls[i] = c;
    }
    return cls;
}

// ---- Grid ---------------------------------------------------------------------------------------------

struct SGrid
{
    int nx{}, nz{};
    float x0{}, z0{}, texel{1.f};
    size_t size() const { return size_t(nx) * size_t(nz); }
    size_t at(int x, int z) const { return size_t(z) * size_t(nx) + size_t(x); }
    float cx(int x) const { return x0 + (float(x) + 0.5f) * texel; }
    float cz(int z) const { return z0 + (float(z) + 0.5f) * texel; }
    // World to texel-index space where index i sits exactly on the centre of texel i.
    float fx(float wx) const { return (wx - x0) / texel - 0.5f; }
    float fz(float wz) const { return (wz - z0) / texel - 0.5f; }
};

template <typename F>
void parallel_rows(int nz, const F& fn)
{
    xr_parallel_for(TaskRange<size_t>(0, size_t(nz)), [&](const TaskRange<size_t>& r) {
        for (size_t z = r.begin(); z != r.end(); ++z)
            fn(int(z));
    });
}

// Every texel CENTRE inside the triangle (XZ projection): fn(index, y on the triangle's plane there).
template <typename F>
void raster_centres(const SGrid& g, const Fvector& va, const Fvector& vb, const Fvector& vc, const F& fn)
{
    const float ax = g.fx(va.x), az = g.fz(va.z);
    const float bx = g.fx(vb.x), bz = g.fz(vb.z);
    const float cx = g.fx(vc.x), cz = g.fz(vc.z);
    const float area2 = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
    if (_abs(area2) < 1e-6f)
        return;
    const float inv = 1.f / area2;
    const int x0 = std::max(iCeil(std::min({ax, bx, cx})), 0), x1 = std::min(iFloor(std::max({ax, bx, cx})), g.nx - 1);
    const int z0 = std::max(iCeil(std::min({az, bz, cz})), 0), z1 = std::min(iFloor(std::max({az, bz, cz})), g.nz - 1);
    const float ylo = std::min({va.y, vb.y, vc.y}), yhi = std::max({va.y, vb.y, vc.y});
    for (int z = z0; z <= z1; ++z)
    {
        const float pz = float(z);
        for (int x = x0; x <= x1; ++x)
        {
            const float px = float(x);
            const float wa = (cx - bx) * (pz - bz) - (px - bx) * (cz - bz);
            const float wb = (ax - cx) * (pz - cz) - (px - cx) * (az - cz);
            const float wc = (bx - ax) * (pz - az) - (px - ax) * (bz - az);
            if (wa * area2 < 0.f || wb * area2 < 0.f || wc * area2 < 0.f)
                continue;
            fn(g.at(x, z), clampr((wa * va.y + wb * vb.y + wc * vc.y) * inv, ylo, yhi));
        }
    }
}

// Every texel the triangle's XZ projection touches at all (separating axes), slivers and walls
// included: fn(index).
template <typename F>
void raster_cover(const SGrid& g, const Fvector& va, const Fvector& vb, const Fvector& vc, const F& fn)
{
    const float px[3] = {g.fx(va.x), g.fx(vb.x), g.fx(vc.x)};
    const float pz[3] = {g.fz(va.z), g.fz(vb.z), g.fz(vc.z)};
    const int x0 = std::max(iFloor(std::min({px[0], px[1], px[2]}) + 0.5f), 0);
    const int x1 = std::min(iFloor(std::max({px[0], px[1], px[2]}) + 0.5f), g.nx - 1);
    const int z0 = std::max(iFloor(std::min({pz[0], pz[1], pz[2]}) + 0.5f), 0);
    const int z1 = std::min(iFloor(std::max({pz[0], pz[1], pz[2]}) + 0.5f), g.nz - 1);
    if (x0 > x1 || z0 > z1)
        return;
    float nxs[3], nzs[3], lo[3], hi[3];
    for (int e = 0; e < 3; ++e)
    {
        const int f = (e + 1) % 3;
        nxs[e] = -(pz[f] - pz[e]);
        nzs[e] = px[f] - px[e];
        lo[e] = flt_max;
        hi[e] = -flt_max;
        for (int k = 0; k < 3; ++k)
        {
            const float d = nxs[e] * px[k] + nzs[e] * pz[k];
            lo[e] = std::min(lo[e], d);
            hi[e] = std::max(hi[e], d);
        }
    }
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
        {
            bool overlap = true;
            for (int e = 0; e < 3 && overlap; ++e)
            {
                const float c = nxs[e] * float(x) + nzs[e] * float(z);
                const float r = 0.5f * (_abs(nxs[e]) + _abs(nzs[e]));
                overlap = c + r >= lo[e] && c - r <= hi[e];
            }
            if (overlap)
                fn(g.at(x, z));
        }
}

// Box blur of radius r (window 2r + 1), edges replicated, separable.
xr_vector<float> box_blur(const SGrid& g, const xr_vector<float>& src, int r)
{
    xr_vector<float> tmp(src.size()), out(src.size());
    if (r <= 0)
        return src;
    const float inv = 1.f / float(2 * r + 1);
    parallel_rows(g.nz, [&](int z) {
        const float* s = &src[g.at(0, z)];
        float* d = &tmp[g.at(0, z)];
        double acc = 0.0;
        for (int k = -r; k <= r; ++k)
            acc += s[clampr(k, 0, g.nx - 1)];
        for (int x = 0; x < g.nx; ++x)
        {
            d[x] = float(acc) * inv;
            acc += s[std::min(x + r + 1, g.nx - 1)] - s[std::max(x - r, 0)];
        }
    });
    xr_parallel_for(TaskRange<size_t>(0, size_t(g.nx)), [&](const TaskRange<size_t>& rng) {
        for (int x = int(rng.begin()); x != int(rng.end()); ++x)
        {
            double acc = 0.0;
            for (int k = -r; k <= r; ++k)
                acc += tmp[g.at(x, clampr(k, 0, g.nz - 1))];
            for (int z = 0; z < g.nz; ++z)
            {
                out[g.at(x, z)] = float(acc) * inv;
                acc += tmp[g.at(x, std::min(z + r + 1, g.nz - 1))] - tmp[g.at(x, std::max(z - r, 0))];
            }
        }
    });
    return out;
}

// Chamfer distance (m) to the nearest seed texel, two passes with 1 and sqrt(2) steps.
xr_vector<float> distance_to(const SGrid& g, const xr_vector<u8>& seed)
{
    const float inf = 1e9f, s1 = g.texel, s2 = g.texel * 1.41421356f;
    xr_vector<float> d(g.size());
    for (size_t i = 0; i < d.size(); ++i)
        d[i] = seed[i] ? 0.f : inf;
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
        {
            float v = d[g.at(x, z)];
            if (x > 0)
                v = std::min(v, d[g.at(x - 1, z)] + s1);
            if (z > 0)
            {
                v = std::min(v, d[g.at(x, z - 1)] + s1);
                if (x > 0)
                    v = std::min(v, d[g.at(x - 1, z - 1)] + s2);
                if (x < g.nx - 1)
                    v = std::min(v, d[g.at(x + 1, z - 1)] + s2);
            }
            d[g.at(x, z)] = v;
        }
    for (int z = g.nz - 1; z >= 0; --z)
        for (int x = g.nx - 1; x >= 0; --x)
        {
            float v = d[g.at(x, z)];
            if (x < g.nx - 1)
                v = std::min(v, d[g.at(x + 1, z)] + s1);
            if (z < g.nz - 1)
            {
                v = std::min(v, d[g.at(x, z + 1)] + s1);
                if (x < g.nx - 1)
                    v = std::min(v, d[g.at(x + 1, z + 1)] + s2);
                if (x > 0)
                    v = std::min(v, d[g.at(x - 1, z + 1)] + s2);
            }
            d[g.at(x, z)] = v;
        }
    return d;
}

// Keeps (or drops) the 8-connected components of `mask` by their area in texels.
void filter_components(const SGrid& g, xr_vector<u8>& mask, float min_area_texels, bool keep_large)
{
    xr_vector<int> label(mask.size(), -1);
    xr_vector<size_t> stack, members;
    for (size_t start = 0; start < mask.size(); ++start)
    {
        if (!mask[start] || label[start] >= 0)
            continue;
        members.clear();
        stack.clear();
        stack.push_back(start);
        label[start] = 1;
        while (!stack.empty())
        {
            const size_t i = stack.back();
            stack.pop_back();
            members.push_back(i);
            const int x = int(i % size_t(g.nx)), z = int(i / size_t(g.nx));
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int xx = x + dx, zz = z + dz;
                    if ((!dx && !dz) || xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz)
                        continue;
                    const size_t j = g.at(xx, zz);
                    if (mask[j] && label[j] < 0)
                    {
                        label[j] = 1;
                        stack.push_back(j);
                    }
                }
        }
        const bool large = float(members.size()) >= min_area_texels;
        if (large != keep_large)
            for (const size_t i : members)
                mask[i] = 0;
    }
}

// Pyramid hole fill: texels without a value take the mean of the valid ones around them, coarser
// and coarser until every hole is covered (buildings, the parts of the level no slot describes).
void fill_holes(int nx, int nz, xr_vector<float>& v, const xr_vector<u8>& valid)
{
    struct SLevel
    {
        int nx, nz;
        xr_vector<float> v;
        xr_vector<u8> ok;
    };
    xr_vector<SLevel> levels;
    levels.push_back({nx, nz, v, valid});
    for (;;)
    {
        const SLevel& L = levels.back();
        bool holes = false;
        for (const u8 o : L.ok)
            if (!o)
            {
                holes = true;
                break;
            }
        if (!holes || (L.nx <= 1 && L.nz <= 1))
            break;
        SLevel N;
        N.nx = (L.nx + 1) / 2;
        N.nz = (L.nz + 1) / 2;
        N.v.assign(size_t(N.nx) * N.nz, 0.f);
        N.ok.assign(size_t(N.nx) * N.nz, 0);
        for (int z = 0; z < N.nz; ++z)
            for (int x = 0; x < N.nx; ++x)
            {
                double sum = 0.0;
                int cnt = 0;
                for (int dz = 0; dz < 2; ++dz)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = x * 2 + dx, sz = z * 2 + dz;
                        if (sx < L.nx && sz < L.nz && L.ok[size_t(sz) * L.nx + sx])
                        {
                            sum += L.v[size_t(sz) * L.nx + sx];
                            ++cnt;
                        }
                    }
                if (cnt)
                {
                    N.v[size_t(z) * N.nx + x] = float(sum / cnt);
                    N.ok[size_t(z) * N.nx + x] = 1;
                }
            }
        levels.push_back(std::move(N));
    }
    for (size_t li = levels.size() - 1; li > 0; --li)
    {
        const SLevel& C = levels[li];
        SLevel& F = levels[li - 1];
        for (int z = 0; z < F.nz; ++z)
            for (int x = 0; x < F.nx; ++x)
            {
                const size_t i = size_t(z) * F.nx + x;
                if (F.ok[i])
                    continue;
                const size_t j = size_t(z / 2) * C.nx + size_t(x / 2);
                if (C.ok[j])
                {
                    F.v[i] = C.v[j];
                    F.ok[i] = 1;
                }
            }
    }
    v = std::move(levels.front().v);
}

// 2D value noise on a hashed lattice (smooth-step fade), -1..1, a pure function of the world position.
float value_noise2(float x, float z, u32 seed)
{
    const float fx = std::floor(x), fz = std::floor(z);
    const int ix = int(fx), iz = int(fz);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.f - 2.f * tx);
    tz = tz * tz * (3.f - 2.f * tz);
    const auto h = [seed](int cx, int cz) {
        u32 v = u32(cx) * 0x8da6b343u ^ u32(cz) * 0xd8163841u ^ seed;
        v ^= v >> 15;
        v *= 0x2c1b3c6du;
        v ^= v >> 12;
        v *= 0x297a2d39u;
        v ^= v >> 15;
        return float(v >> 8) * (2.f / 16777216.f) - 1.f;
    };
    const float a = h(ix, iz) + (h(ix + 1, iz) - h(ix, iz)) * tx;
    const float b = h(ix, iz + 1) + (h(ix + 1, iz + 1) - h(ix, iz + 1)) * tx;
    return a + (b - a) * tz;
}

// ---- level.details ------------------------------------------------------------------------------------

// The slot heightmap of level.details (DetailFormat.h: DetailHeader, DetailSlot of 16 bytes, 2 m
// slots; y_base 12 bits of 20 cm from -200 m, y_height 8 bits of 10 cm).
struct SDetailSlots
{
    IReader* file{};
    const u8* slots{};
    u32 count{};
    int size_x{}, size_z{}, offs_x{}, offs_z{};

    bool open()
    {
        if (!FS.exist("$level$", "level.details"))
            return false;
        string_path fn;
        FS.update_path(fn, "$level$", "level.details");
        file = FS.r_open(fn);
        if (!file)
            return false;
        bool ok = false;
        if (IReader* head = file->open_chunk(0))
        {
            if (head->length() >= 6 * sizeof(u32) && head->r_u32() == 3)
            {
                head->r_u32();
                offs_x = head->r_s32();
                offs_z = head->r_s32();
                size_x = head->r_s32();
                size_z = head->r_s32();
                ok = size_x > 0 && size_z > 0;
            }
            head->close();
        }
        if (ok)
            if (IReader* body = file->open_chunk(2))
            {
                slots = static_cast<const u8*>(body->pointer());
                count = u32(body->length() / 16);
                body->close();
            }
        return ok && slots;
    }
    void close()
    {
        if (file)
            FS.r_close(file);
        file = nullptr;
        slots = nullptr;
    }
    // The ground span of the slot under a world XZ; false where the compiler wrote nothing.
    bool span(float wx, float wz, float& lo, float& hi) const
    {
        const int dx = iFloor(wx * 0.5f) + offs_x, dz = iFloor(wz * 0.5f) + offs_z;
        if (dx < 0 || dz < 0 || dx >= size_x || dz >= size_z)
            return false;
        const u32 at = u32(dz) * u32(size_x) + u32(dx);
        if (at >= count)
            return false;
        const u8* p = slots + size_t(at) * 16;
        const u32 packed = u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
        const u32 y_base = packed & 0xfffu;
        if (!y_base)
            return false;
        lo = float(y_base) * 0.2f - 200.f;
        hi = lo + float((packed >> 12) & 0xffu) * 0.1f;
        return true;
    }
};

// ---- Season LUT ------------------------------------------------------------------------------------------

struct SBin
{
    float w, a, m;
};

struct SClassSamples
{
    xr_vector<SBin> bins;
    bool p90 = false;     // the drift key is the 90th percentile of the covered texels
    float eps = 0.01f;    // covered above this depth
};

// Bins (A_c, M_c) of one class: lut_bins along A by 16 along M, each over the class's own range and
// holding its weight and the mean of its members. Fine along A, where the cover quantile cuts.
SClassSamples make_bins(const xr_vector<Fvector2>& am, bool p90, u32 lut_bins, float eps)
{
    SClassSamples out;
    out.p90 = p90;
    out.eps = eps;
    if (am.empty())
        return out;
    const int na = int(lut_bins), nm = 16;
    float a_lo = flt_max, a_hi = -flt_max, m_lo = flt_max, m_hi = -flt_max;
    for (const Fvector2& v : am)
    {
        a_lo = std::min(a_lo, v.x);
        a_hi = std::max(a_hi, v.x);
        m_lo = std::min(m_lo, v.y);
        m_hi = std::max(m_hi, v.y);
    }
    const float a_scale = float(na) / std::max(a_hi - a_lo, 1e-4f);
    const float m_scale = float(nm) / std::max(m_hi - m_lo, 1e-4f);
    xr_vector<double> w(size_t(na) * nm, 0.0), sa(size_t(na) * nm, 0.0), sm(size_t(na) * nm, 0.0);
    for (const Fvector2& v : am)
    {
        const int ia = clampr(int((v.x - a_lo) * a_scale), 0, na - 1);
        const int im = clampr(int((v.y - m_lo) * m_scale), 0, nm - 1);
        const size_t k = size_t(im) * na + size_t(ia);
        w[k] += 1.0;
        sa[k] += v.x;
        sm[k] += v.y;
    }
    for (size_t k = 0; k < w.size(); ++k)
        if (w[k] > 0.0)
            out.bins.push_back({float(w[k]), float(sa[k] / w[k]), float(sm[k] / w[k])});
    return out;
}

struct SSolveScratch
{
    xr_vector<std::pair<float, float>> rw; // value, weight
};

// Weighted q-quantile of rw (sorted in place).
float weighted_quantile(xr_vector<std::pair<float, float>>& rw, float q)
{
    std::sort(rw.begin(), rw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    double total = 0.0;
    for (const auto& v : rw)
        total += v.second;
    const double target = total * double(clampr(q, 0.f, 1.f));
    double acc = 0.0;
    for (const auto& v : rw)
    {
        acc += v.second;
        if (acc >= target)
            return v.first;
    }
    return rw.empty() ? 0.f : rw.back().first;
}

// S, Mc of one class for one day: Mc puts the covered share at `cover` in closed form, S is bisected
// until the covered mean (or 90th percentile) reaches `depth`.
void solve_class(const SClassSamples& c, float cover, float depth, float cap, SSolveScratch& s, float& S_out,
    float& Mc_out)
{
    S_out = Mc_out = 0.f;
    if (c.bins.empty() || cover <= 1e-4f || depth <= 1e-4f)
        return;
    if (c.p90)
        depth = std::min(depth, cap - 1e-4f);
    const auto at = [&](float S, float& Mc) {
        if (cover >= 0.9999f)
            Mc = 0.f;
        else
        {
            s.rw.clear();
            for (const SBin& b : c.bins)
                s.rw.emplace_back((S * b.a - c.eps) / b.m, b.w);
            Mc = std::max(0.f, weighted_quantile(s.rw, 1.f - cover));
        }
        s.rw.clear();
        double sum = 0.0, wsum = 0.0;
        for (const SBin& b : c.bins)
        {
            const float D = clampr(S * b.a - Mc * b.m, 0.f, cap);
            if (D <= c.eps)
                continue;
            if (c.p90)
                s.rw.emplace_back(D, b.w);
            sum += double(D) * b.w;
            wsum += b.w;
        }
        if (wsum <= 0.0)
            return 0.f;
        return c.p90 ? weighted_quantile(s.rw, 0.9f) : float(sum / wsum);
    };
    float lo = 1e-4f, hi = 50.f, Mc = 0.f;
    for (int it = 0; it < 28; ++it)
    {
        const float mid = std::sqrt(lo * hi);
        if (at(mid, Mc) < depth)
            lo = mid;
        else
            hi = mid;
    }
    at(hi, Mc);
    S_out = hi;
    Mc_out = Mc;
}
} // namespace

void da_snow_bake_level()
{
    // A new level: nothing of the old one's deformer memory survives.
    da_snow_deform_reset();

    if (!g_pGameLevel)
        return;
    CTimer timer;
    timer.Start();

    CObjectSpace& space = Level().ObjectSpace;
    const CDB::TRI* tris = space.GetStaticTris();
    const Fvector* verts = space.GetStaticVerts();
    const CDB::MODEL* model = space.GetStaticModel();
    if (!model || !tris || !verts || !model->get_tris_count())
        return;

    // Underground and interior levels carry no level.details: no snow there.
    SDetailSlots details;
    if (!details.open())
    {
        details.close();
        Msg("* [snow] %s: no level.details - no snow on this level", Level().name().c_str());
        return;
    }

    const SBakeParams P = read_params();
    const xr_vector<u16> mtl = material_classes(P);
    const auto mclass = [&](u32 idx) { return idx < mtl.size() ? mtl[idx] : u16(0); };

    const Fbox& bb = space.GetBoundingVolume();
    SGrid g;
    g.texel = P.texel;
    g.x0 = std::floor(bb.vMin.x);
    g.z0 = std::floor(bb.vMin.z);
    g.nx = clampr(iCeil((bb.vMax.x - g.x0) / g.texel) + 1, 1, int(max_dim));
    g.nz = clampr(iCeil((bb.vMax.z - g.z0) / g.texel) + 1, 1, int(max_dim));
    const size_t n = g.size();

    // ---- Collision raster: top, walls, crowns, water sheets --------------------------------------------
    xr_vector<float> top(n, none_y), top_ny(n, 0.f), wall(n, none_y), crown(n, none_y), water(n, none_y);
    xr_vector<u16> top_mtl(n, 0xffff), wall_mtl(n, 0xffff);
    const u32 tcount = u32(model->get_tris_count());
    for (u32 t = 0; t < tcount; ++t)
    {
        const CDB::TRI& tri = tris[t];
        const Fvector& a = verts[tri.verts[0]];
        const Fvector& b = verts[tri.verts[1]];
        const Fvector& c = verts[tri.verts[2]];
        Fvector nrm;
        nrm.mknormal_non_normalized(a, b, c);
        const float len = nrm.magnitude();
        if (len < 1e-8f)
            continue;
        const float ny = nrm.y / len;
        const u16 mc = mclass(tri.material);
        const float ymax = std::max({a.y, b.y, c.y});
        if (mc & mc_crown)
        {
            raster_cover(g, a, b, c, [&](size_t i) { crown[i] = std::max(crown[i], ymax); });
            continue;
        }
        if (mc & mc_water)
        {
            if (_abs(ny) >= 0.7071f)
                raster_centres(g, a, b, c, [&](size_t i, float y) { water[i] = std::max(water[i], y); });
            continue;
        }
        if (mc & mc_skip)
            continue;
        if (_abs(ny) < P.wall_ny)
            raster_cover(g, a, b, c, [&](size_t i) {
                if (ymax > wall[i])
                {
                    wall[i] = ymax;
                    wall_mtl[i] = u16(tri.material);
                }
            });
        // Trunks and branches stand in the snow, they carry no snow surface (their walls still count).
        if (ny > 0.05f && !(mc & mc_trunk))
            raster_centres(g, a, b, c, [&](size_t i, float y) {
                if (y > top[i])
                {
                    top[i] = y;
                    top_ny[i] = ny;
                    top_mtl[i] = u16(tri.material);
                }
            });
    }

    // The highest solid surface facing up anywhere in each texel, sampled at nine points of it: what the covers are
    // told by (the centre alone missed a stump or a post narrower than the metre, and the window, finding no cover
    // there, laid the snow's base on its top and tore the snow round it).
    xr_vector<float> top_max(top);
    // (and where it was: against the ground right there, a slope's higher corner is no cover)
    xr_vector<Fvector2> top_max_at(n);
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
            top_max_at[g.at(x, z)].set(g.cx(x), g.cz(z));
    {
        SGrid g3 = g;
        g3.texel = g.texel / 3.f;
        g3.nx = g.nx * 3;
        g3.nz = g.nz * 3;
        for (u32 t = 0; t < tcount; ++t)
        {
            const CDB::TRI& tri = tris[t];
            const u16 mc = mclass(tri.material);
            if (mc & (mc_crown | mc_water | mc_skip | mc_trunk))
                continue;
            const Fvector& a = verts[tri.verts[0]];
            const Fvector& b = verts[tri.verts[1]];
            const Fvector& c = verts[tri.verts[2]];
            Fvector nrm;
            nrm.mknormal_non_normalized(a, b, c);
            const float len = nrm.magnitude();
            if (len < 1e-8f || nrm.y / len <= 0.05f)
                continue;
            raster_centres(g3, a, b, c, [&](size_t k, float y) {
                const size_t sx = k % size_t(g3.nx), sz = k / size_t(g3.nx);
                const size_t i = g.at(int(sx / 3), int(sz / 3));
                if (y > top_max[i])
                {
                    top_max[i] = y;
                    top_max_at[i].set(g3.cx(int(sx)), g3.cz(int(sz)));
                }
            });
        }
    }

    // ---- Ground: the terrain under everything, hole-filled -------------------------------------------
    xr_vector<float> ground(n, 0.f);
    xr_vector<u8> slot_ok(n, 0);
    {
        xr_vector<float> g0(n, 0.f);
        xr_vector<u8> g0_ok(n, 0);
        xr_vector<float> slo(n, flt_max), shi(n, -flt_max);
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x)
            {
                float lo, hi;
                if (details.span(g.cx(x), g.cz(z), lo, hi))
                {
                    slo[g.at(x, z)] = lo;
                    shi[g.at(x, z)] = hi;
                }
            }
        details.close();
        // The highest solid face up within the slot's span under each texel: the floor under a roof, an eave, a deck.
        // (The span's middle stood in there before: up to 0.8 m over the path between a house and its fence, and the
        // snow lay raised along it, out of reach of the boots in it.)
        xr_vector<float> floor_in(n, none_y);
        for (u32 t = 0; t < tcount; ++t)
        {
            const CDB::TRI& tri = tris[t];
            if (mclass(tri.material) & (mc_crown | mc_water | mc_skip | mc_trunk))
                continue;
            const Fvector& a = verts[tri.verts[0]];
            const Fvector& b = verts[tri.verts[1]];
            const Fvector& c = verts[tri.verts[2]];
            Fvector nrm;
            nrm.mknormal_non_normalized(a, b, c);
            const float len = nrm.magnitude();
            if (len < 1e-8f || nrm.y / len <= 0.3f)
                continue;
            raster_centres(g, a, b, c, [&](size_t i, float y) {
                if (slo[i] != flt_max && y >= slo[i] - 0.5f && y <= shi[i] + 0.5f && y > floor_in[i])
                    floor_in[i] = y;
            });
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (slo[i] == flt_max)
                continue;
            slot_ok[i] = 1;
            g0_ok[i] = 1;
            // The collision wherever it lies within the slot's ground span: 1 m sharper than the 2 m slot. A roof
            // takes the floor under it in the span, a pit or no floor the span's middle.
            if (top[i] > none_y && top[i] >= slo[i] - 0.5f && top[i] <= shi[i] + 0.5f)
                g0[i] = top[i];
            else
                g0[i] = floor_in[i] > none_y ? floor_in[i] : 0.5f * (slo[i] + shi[i]);
        }
        xr_vector<float> filled = g0;
        fill_holes(g.nx, g.nz, filled, g0_ok);
        for (size_t i = 0; i < n; ++i)
        {
            if (slot_ok[i])
                ground[i] = g0[i];
            else
                ground[i] = (top[i] > none_y && _abs(top[i] - filled[i]) < 1.5f) ? top[i] : filled[i];
        }
    }

    // The highest node of the level's AI in each texel: the surfaces it walks on.
    xr_vector<float> walk(n, -flt_max);
    if (ai().get_level_graph())
    {
        const CLevelGraph& graph = ai().level_graph();
        for (u32 v = 0, nv = graph.header().vertex_count(); v < nv; ++v)
        {
            const Fvector p = graph.vertex_position(v);
            const int x = iFloor((p.x - g.x0) / g.texel), z = iFloor((p.z - g.z0) / g.texel);
            if (x >= 0 && z >= 0 && x < g.nx && z < g.nz)
                walk[g.at(x, z)] = std::max(walk[g.at(x, z)], p.y);
        }
    }

    // Small things standing on the ground are no ground: the slot's span (a few decimetres over its lowest point)
    // or the 1.5 m off the filled ground took a stump's, a block's, a cart's top for the terrain, and the snow lay on
    // them as on a bank, torn at their edges. The ground opened with a 3 x 3 m square (erode, then dilate) drops what
    // is narrower than that and keeps slopes; where the AI walks on a top, it is terrain after all.
    {
        xr_vector<float> ero(n), open(n);
        const auto pass = [&](const xr_vector<float>& src, xr_vector<float>& dst, bool lo) {
            parallel_rows(g.nz, [&](int z) {
                for (int x = 0; x < g.nx; ++x)
                {
                    float m = src[g.at(x, z)];
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int sx = clampr(x + dx, 0, g.nx - 1), sz = clampr(z + dz, 0, g.nz - 1);
                            const float v = src[g.at(sx, sz)];
                            m = lo ? std::min(m, v) : std::max(m, v);
                        }
                    dst[g.at(x, z)] = m;
                }
            });
        };
        pass(ground, ero, true);
        pass(ero, open, false);
        u32 dropped = 0;
        for (size_t i = 0; i < n; ++i)
            if (ground[i] - open[i] > 0.25f && !da_snow_walkable(ground[i], walk[i]))
            {
                ground[i] = open[i];
                ++dropped;
            }
        Msg("* [snow] ground: %u texels of small objects off the terrain", dropped);
    }

    // ---- Masks -----------------------------------------------------------------------------------------
    xr_vector<u8> is_ground(n, 0), recv(n, 0), is_water(n, 0);
    for (size_t i = 0; i < n; ++i)
    {
        const bool has_top = top[i] > none_y;
        is_ground[i] = has_top && _abs(top[i] - ground[i]) < P.ground_band;
        recv[i] = is_ground[i] && top_ny[i] > P.receiver_ny;
        // A water sheet over the solid top: the bed of a pond. A roof over an indoor pool is a roof.
        is_water[i] = water[i] > none_y && (!has_top || water[i] > top[i] + 0.02f);
    }
    xr_vector<float>().swap(water);

    // Canopy: passable crowns well above the ground, plus a disc round every tree visual.
    xr_vector<float> canopy(n, 0.f);
    for (size_t i = 0; i < n; ++i)
        if (crown[i] > none_y && crown[i] - ground[i] >= P.canopy_height)
            canopy[i] = 1.f;
    if (g_pGamePersistent)
    {
        const int r = std::max(iCeil(P.tree_disc_radius / g.texel), 1);
        const float r2 = _sqr(P.tree_disc_radius / g.texel);
        for (const Fvector& t : g_pGamePersistent->Environment().wind_veg_trees)
        {
            const int cx = iFloor(g.fx(t.x) + 0.5f), cz = iFloor(g.fz(t.z) + 0.5f);
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx)
                {
                    const int x = cx + dx, z = cz + dz;
                    if (x < 0 || z < 0 || x >= g.nx || z >= g.nz || float(dx * dx + dz * dz) > r2)
                        continue;
                    float& c = canopy[g.at(x, z)];
                    c = std::max(c, P.tree_disc_weight);
                }
        }
    }
    const xr_vector<float> T = box_blur(g, canopy, std::max(int(P.canopy_blur / g.texel) / 2, 1));
    xr_vector<float>().swap(canopy);

    // Roads: paved collision at ground level, without the railway ballast; wide paved yards apart.
    xr_vector<u8> road(n, 0), road_line(n, 0);
    {
        xr_vector<u8> rail(n, 0);
        for (size_t i = 0; i < n; ++i)
            rail[i] = top_mtl[i] != 0xffff && (mclass(top_mtl[i]) & mc_rail);
        const xr_vector<float> rail_d = distance_to(g, rail);
        for (size_t i = 0; i < n; ++i)
        {
            const u16 mc = top_mtl[i] != 0xffff ? mclass(top_mtl[i]) : u16(0);
            road[i] = is_ground[i] && (mc & mc_road) && !((mc & mc_ballast) && rail_d[i] <= P.rail_dilation);
        }
        // A yard, a plaza, an apron: wherever a disc of wide_paved fits inside the paving.
        xr_vector<u8> off_road(n);
        for (size_t i = 0; i < n; ++i)
            off_road[i] = !road[i];
        const xr_vector<float> inside = distance_to(g, off_road);
        const float half = 0.5f * P.wide_paved;
        xr_vector<u8> centre(n, 0);
        for (size_t i = 0; i < n; ++i)
            centre[i] = road[i] && inside[i] >= half;
        const xr_vector<float> to_centre = distance_to(g, centre);
        for (size_t i = 0; i < n; ++i)
            road_line[i] = road[i] && to_centre[i] > half;
    }

    // ---- Drifts: lee, windward toe, hollows, yards ------------------------------------------------------
    xr_vector<float> lee(n, 0.f), toe(n, 0.f), solid(n, 0.f), ob(n, 0.f);
    {
        for (size_t i = 0; i < n; ++i)
        {
            float h = 0.f;
            if (top[i] > none_y)
                h = std::max(h, top[i] - ground[i]);
            if (wall[i] > none_y)
                h = std::max(h, wall[i] - ground[i]);
            const bool trunk = (top_mtl[i] != 0xffff && (mclass(top_mtl[i]) & mc_trunk)) ||
                (wall_mtl[i] != 0xffff && (mclass(wall_mtl[i]) & mc_trunk));
            solid[i] = trunk ? 0.f : clampr(h, 0.f, 50.f);
        }
        // Posts and tiny props make no drift worth the name.
        xr_vector<u8> blob(n);
        for (size_t i = 0; i < n; ++i)
            blob[i] = solid[i] >= P.obstacle;
        filter_components(g, blob, P.blob_area / (g.texel * g.texel), true);
        for (size_t i = 0; i < n; ++i)
            if (solid[i] >= P.obstacle && !blob[i])
                solid[i] = 0.f;
        // Forest edges: connected crown cover large enough to act as a wall (a lone tree is porous).
        xr_vector<u8> forest(n);
        for (size_t i = 0; i < n; ++i)
            forest[i] = crown[i] > none_y && crown[i] - ground[i] >= P.canopy_height;
        filter_components(g, forest, P.forest_area / (g.texel * g.texel), true);
        for (size_t i = 0; i < n; ++i)
            ob[i] = std::max(solid[i], forest[i] ? clampr(crown[i] - ground[i], 0.f, 50.f) : 0.f);

        // Wind the drifts build in: from wind_from (compass degrees, 0 = from +z) and the two headings
        // wind_spread either side, which count wind_side_weight.
        struct SDir
        {
            float x, z, w;
        };
        SDir dirs[8];
        for (u32 k = 0; k < P.wind_dirs; ++k)
        {
            const float from = deg2rad(P.wind_from[k]);
            dirs[k] = {-_sin(from), -_cos(from), P.wind_weight[k]};
        }
        const int reach = std::max(iCeil(P.lee_reach / g.texel), 1);
        const float span = std::max(P.lee_end - P.lee_flat, 0.01f);
        parallel_rows(g.nz, [&](int z) {
            for (int x = 0; x < g.nx; ++x)
            {
                float l = 0.f, t = 0.f;
                for (u32 k = 0; k < P.wind_dirs; ++k)
                    for (int s = 1; s <= reach; ++s)
                    {
                        const float dist = float(s) * g.texel;
                        const SDir& d = dirs[k];
                        // The obstacle `dist` upwind of this texel.
                        const int ux = iFloor(float(x) - d.x * float(s) + 0.5f), uz = iFloor(float(z) - d.z * float(s) + 0.5f);
                        if (ux >= 0 && uz >= 0 && ux < g.nx && uz < g.nz)
                        {
                            const float up = ob[g.at(ux, uz)];
                            if (up >= P.obstacle)
                            {
                                const float h = std::min(up, P.lee_cap);
                                const float ratio = dist / std::max(h, 1e-3f);
                                l = std::max(l, d.w * clampr((P.lee_end - ratio) / span, 0.f, 1.f));
                            }
                        }
                        // A solid obstacle just downwind: the windward toe in front of it.
                        const int wx = iFloor(float(x) + d.x * float(s) + 0.5f), wz = iFloor(float(z) + d.z * float(s) + 0.5f);
                        if (wx >= 0 && wz >= 0 && wx < g.nx && wz < g.nz)
                        {
                            const float dn = solid[g.at(wx, wz)];
                            if (dn >= P.obstacle && dist <= P.toe_reach * std::min(dn, P.lee_cap))
                                t = std::max(t, P.toe_weight * d.w);
                        }
                    }
                const size_t i = g.at(x, z);
                lee[i] = T[i] >= P.canopy_lee_cut ? 0.f : l;
                toe[i] = t;
            }
        });
    }
    xr_vector<float>().swap(crown);

    // Hollows: ground lower than its neighbourhood mean.
    xr_vector<float> H(n);
    for (size_t i = 0; i < n; ++i)
        H[i] = recv[i] ? top[i] : ground[i];
    xr_vector<float> hollow(n, 0.f);
    {
        const xr_vector<float> hb = box_blur(g, H, std::max(int(P.hollow_blur / g.texel) / 2, 1));
        for (size_t i = 0; i < n; ++i)
            hollow[i] = clampr((hb[i] - H[i] - P.hollow_lo) / P.hollow_range, 0.f, 1.f);
    }

    // Yards: near buildings, and along fences.
    xr_vector<float> yard(n, 0.f);
    {
        xr_vector<u8> bld(n);
        for (size_t i = 0; i < n; ++i)
            bld[i] = top[i] > none_y && top[i] - ground[i] >= P.building_height &&
                !(top_mtl[i] != 0xffff && (mclass(top_mtl[i]) & mc_trunk));
        filter_components(g, bld, P.building_area / (g.texel * g.texel), true);
        const xr_vector<float> db = distance_to(g, bld);
        xr_vector<u8> fence(n);
        for (size_t i = 0; i < n; ++i)
            fence[i] = wall[i] > none_y && wall_mtl[i] != 0xffff && (mclass(wall_mtl[i]) & mc_fence) &&
                wall[i] - ground[i] >= P.fence_height;
        const xr_vector<float> df = distance_to(g, fence);
        for (size_t i = 0; i < n; ++i)
        {
            const float yb = (db[i] > 0.f && db[i] <= P.yard_reach) ? P.yard_weight * std::exp(-db[i] / P.yard_decay) : 0.f;
            const float yf = df[i] <= P.fence_reach ? P.fence_weight : 0.f;
            yard[i] = std::max(yb, yf);
        }
    }
    xr_vector<float>().swap(wall);

    xr_vector<float> neglect(n, 0.f);
    for (size_t i = 0; i < n; ++i)
    {
        const bool soil = is_ground[i] && top_mtl[i] != 0xffff && (mclass(top_mtl[i]) & mc_soil);
        if (soil && !road[i])
            neglect[i] = std::max({lee[i], toe[i], hollow[i], yard[i]});
    }
    xr_vector<float>().swap(toe);
    xr_vector<float>().swap(hollow);
    xr_vector<float>().swap(yard);

    // ---- Classes: road over drift over trees over open; water carries none ----------------------------
    constexpr u8 cls_none = 255;
    xr_vector<u8> cls(n, da_snow_open);
    u32 cls_count[da_snow_classes]{};
    for (size_t i = 0; i < n; ++i)
    {
        if (is_water[i])
            cls[i] = cls_none;
        else if (recv[i])
        {
            if (road_line[i])
                cls[i] = da_snow_road;
            else if (neglect[i] >= P.drift_class)
                cls[i] = da_snow_drift;
            else if (T[i] >= P.canopy_class)
                cls[i] = da_snow_trees;
        }
        if (cls[i] != cls_none && recv[i])
            ++cls_count[cls[i]];
    }

    SSnowField field;
    field.dim_x = u32(g.nx);
    field.dim_z = u32(g.nz);
    field.origin.set(g.x0, g.z0);
    field.texel = g.texel;
    field.ground = ground;
    field.top.resize(n);
    field.solid.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        field.solid[i] = top[i] > none_y ? 1 : 0;
        field.top[i] = top[i] > none_y ? top[i] : ground[i];
    }
    field.walk = std::move(walk);
    // A cover's kind, along a ray down from over its top: the top entered, the next solid face met either leaves the
    // solid (faces down) or is a floor (faces up). A slab left well over the ground under it (a roof's or a deck's
    // underside, a ceiling) or a floor more than 1.2 m under a roof of one sheet with nothing between is an overhang;
    // a solid left at the ground (a block's, a car's bottom) or a low object of one surface (a stump) is solid.
    const auto overhang_at = [&](CDB::COLLIDER& xrc, float x, float z, float y_top, float y_ground) {
        const float y0 = y_top + 0.1f, y1 = y_ground - 0.5f;
        xrc.ray_query(0, model, Fvector{x, y0, z}, Fvector{0.f, -1.f, 0.f}, y0 - y1);
        struct Hit
        {
            float y, ny;
        };
        Hit hits[16];
        u32 k = 0;
        for (size_t r = 0; r < xrc.r_count() && k < 16; ++r)
        {
            const CDB::RESULT& res = xrc.r_begin()[r];
            const CDB::TRI& tri = tris[res.id];
            if (mclass(tri.material) & mc_skip)
                continue;
            Fvector nrm;
            nrm.mknormal_non_normalized(verts[tri.verts[0]], verts[tri.verts[1]], verts[tri.verts[2]]);
            const float len = nrm.magnitude();
            // (the walls along the ray say nothing)
            if (len < 1e-8f || _abs(nrm.y / len) < 0.3f)
                continue;
            hits[k++] = {y0 - res.range, nrm.y / len};
        }
        std::sort(hits, hits + k, [](const Hit& a, const Hit& b) { return a.y > b.y; });
        u32 i = 0;
        while (i < k && hits[i].ny < 0.f)
            ++i;
        if (i >= k)
            return false;
        if (i + 1 >= k)
            return hits[i].y - y_ground > 1.2f;
        const Hit& next = hits[i + 1];
        if (next.ny > 0.f)
            return hits[i].y - next.y > 1.2f;
        float below = y_ground;
        for (u32 j = i + 2; j < k; ++j)
            if (hits[j].ny > 0.f)
            {
                below = hits[j].y;
                break;
            }
        // (a vehicle's or a cart's body under 1.5 m: no roof - the wind fills the snow in under it, bare it read as
        // a pit round its wheels)
        return next.y - below > 1.5f;
    };
    // The ground bilinear at a point (texel centres at +0.5).
    const auto field_at = [&](const xr_vector<float>& v, const Fvector2& p) {
        const float fx = clampr(g.fx(p.x), 0.f, float(g.nx - 1)), fz = clampr(g.fz(p.y), 0.f, float(g.nz - 1));
        const int x0 = clampr(iFloor(fx), 0, std::max(g.nx - 2, 0)), z0 = clampr(iFloor(fz), 0, std::max(g.nz - 2, 0));
        const int x1 = std::min(x0 + 1, g.nx - 1), z1 = std::min(z0 + 1, g.nz - 1);
        const float tx = fx - float(x0), tz = fz - float(z0);
        const float a = v[g.at(x0, z0)] + (v[g.at(x1, z0)] - v[g.at(x0, z0)]) * tx;
        const float b = v[g.at(x0, z1)] + (v[g.at(x1, z1)] - v[g.at(x0, z1)]) * tx;
        return a + (b - a) * tz;
    };
    // The covers (da_snow_cover_at): a solid top over the terrain no node of the 3 x 3 texels round it walks on (a
    // deck's own nodes stop short of its edge), an overhang or a solid object; the shelter, the overhangs eroded by a
    // texel.
    field.cover.assign(n, 0);
    parallel_rows(g.nz, [&](int z) {
        CDB::COLLIDER xrc;
        for (int x = 0; x < g.nx; ++x)
        {
            const size_t i = g.at(x, z);
            const float tm = top_max[i];
            if (tm <= none_y || tm - field_at(field.ground, top_max_at[i]) <= da_snow_cover_over)
                continue;
            bool walked = false;
            for (int dz = -1; dz <= 1 && !walked; ++dz)
                for (int dx = -1; dx <= 1 && !walked; ++dx)
                {
                    const int sx = x + dx, sz = z + dz;
                    walked = sx >= 0 && sz >= 0 && sx < g.nx && sz < g.nz && da_snow_walkable(tm, field.walk[g.at(sx, sz)]) &&
                        da_snow_deck_node(field.walk[g.at(sx, sz)], field.ground[g.at(sx, sz)]);
                }
            if (walked)
                continue;
            const float cx = g.x0 + (float(x) + 0.5f) * g.texel, cz = g.z0 + (float(z) + 0.5f) * g.texel;
            field.cover[i] = overhang_at(xrc, cx, cz, tm, field.ground[i]) ? 2 : 1;
        }
    });
    // An overhang is a roof over open space, broad enough to keep the snow off: a cover's connected texels with fewer
    // than five of overhang are a solid object (a tall stump or a post open at its bottom read as a sheet over a floor
    // more than 1.2 m under it, and sheltered a bare pit round itself).
    {
        constexpr u32 overhang_min = 5;
        xr_vector<u8> seen(n, 0);
        xr_vector<size_t> part, stack;
        for (size_t s = 0; s < n; ++s)
        {
            if (seen[s] || !field.cover[s])
                continue;
            part.clear();
            stack.assign(1, s);
            seen[s] = 1;
            u32 over = 0;
            while (!stack.empty())
            {
                const size_t i = stack.back();
                stack.pop_back();
                part.push_back(i);
                over += field.cover[i] == 2 ? 1u : 0u;
                const int x = int(i % size_t(g.nx)), z = int(i / size_t(g.nx));
                for (const auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                {
                    const int sx = x + dx, sz = z + dz;
                    if (sx < 0 || sz < 0 || sx >= g.nx || sz >= g.nz)
                        continue;
                    const size_t j = g.at(sx, sz);
                    if (!seen[j] && field.cover[j])
                    {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
            if (over < overhang_min)
                for (const size_t i : part)
                    field.cover[i] = std::min<u8>(field.cover[i], 1);
        }
    }
    field.shelter.assign(n, 0);
    parallel_rows(g.nz, [&](int z) {
        if (z == 0 || z + 1 >= g.nz)
            return;
        for (int x = 1; x + 1 < g.nx; ++x)
        {
            bool all = true;
            for (int dz = -1; dz <= 1 && all; ++dz)
                for (int dx = -1; dx <= 1 && all; ++dx)
                    all = field.cover[g.at(x + dx, z + dz)] == 2;
            field.shelter[g.at(x, z)] = all ? 255 : 0;
        }
    });
    {
        u32 kinds[3]{};
        for (const u8 c : field.cover)
            ++kinds[std::min<u32>(c, 2u)];
        Msg("* [snow] covers: %u texels of overhangs, %u of solid objects", kinds[2], kinds[1]);
    }

    // Soft weights: one-hot classes over the class_blur neighbourhood, normalised; water stays at zero.
    field.weights.assign(n, 0);
    const int blur_r = std::max(int(P.class_blur / g.texel) / 2, 0);
    parallel_rows(g.nz, [&](int z) {
        for (int x = 0; x < g.nx; ++x)
        {
            const size_t i = g.at(x, z);
            if (cls[i] == cls_none)
                continue;
            float w[da_snow_classes]{};
            float sum = 0.f;
            for (int dz = -blur_r; dz <= blur_r; ++dz)
                for (int dx = -blur_r; dx <= blur_r; ++dx)
                {
                    const int xx = x + dx, zz = z + dz;
                    if (xx < 0 || zz < 0 || xx >= g.nx || zz >= g.nz)
                        continue;
                    const u8 c = cls[g.at(xx, zz)];
                    if (c == cls_none)
                        continue;
                    w[c] += 1.f;
                    sum += 1.f;
                }
            // Rounded to bytes that sum to exactly 255; the dominant class takes the rounding.
            int q[da_snow_classes], acc = 0;
            u32 big = 0;
            for (u32 c = 0; c < da_snow_classes; ++c)
            {
                q[c] = iFloor(w[c] / sum * 255.f + 0.5f);
                acc += q[c];
                if (w[c] > w[big])
                    big = c;
            }
            q[big] += 255 - acc;
            u32 packed = 0;
            for (u32 c = 0; c < da_snow_classes; ++c)
                packed |= u32(clampr(q[c], 0, 255)) << (8 * c);
            field.weights[i] = packed;
        }
    });

    // ---- Accumulation, melt, noise ----------------------------------------------------------------------
    field.factors.assign(n, 0);
    {
        const xr_vector<float> lap_blur = box_blur(g, H, std::max(int(P.depression_blur / g.texel) / 2, 1));
        // Ground aspect from a smoothed height (~2 m), for the noon sun of a winter day from the south.
        const xr_vector<float> hs = box_blur(g, box_blur(g, H, 2), 2);
        const float se = deg2rad(P.sun_elevation);
        const Fvector sun{0.f, _sin(se), -_cos(se)};
        xr_vector<float> noise(n);
        float noise_max = 1e-6f;
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x)
            {
                const float wx = g.cx(x), wz = g.cz(z);
                float v = 0.f;
                for (u32 o = 0; o < P.noise_octaves; ++o)
                {
                    const float inv = 1.f / std::max(P.noise_len[o], 0.1f);
                    v += P.noise_weight[o] * value_noise2(wx * inv, wz * inv, 0x51A7E5u + 0x9E3779B9u * o);
                }
                noise[g.at(x, z)] = v;
                noise_max = std::max(noise_max, _abs(v));
            }
        parallel_rows(g.nz, [&](int z) {
            for (int x = 0; x < g.nx; ++x)
            {
                const size_t i = g.at(x, z);
                const float dep = clampr(1.f + P.depression_gain * (lap_blur[i] - H[i]), P.depression_min, P.depression_max);
                const float drift = clampr(1.f + P.lee_gain * lee[i], P.drift_min, P.drift_max);
                float ny = top_ny[i];
                const float gx = (hs[g.at(std::min(x + 1, g.nx - 1), z)] - hs[g.at(std::max(x - 1, 0), z)]) / (2.f * g.texel);
                const float gz = (hs[g.at(x, std::min(z + 1, g.nz - 1))] - hs[g.at(x, std::max(z - 1, 0))]) / (2.f * g.texel);
                Fvector nrm{-gx, 1.f, -gz};
                nrm.normalize();
                if (top[i] <= none_y)
                    ny = nrm.y;
                const float slope = clampr((ny - P.slope_lo) / (P.slope_hi - P.slope_lo), 0.f, 1.f);
                const float can = 1.f - P.canopy_snow * T[i];
                const float A = clampr(slope * drift * dep * can, 0.f, 2.f);
                const float well = (T[i] > P.well_lo && T[i] < P.well_hi) ? 1.f : 0.f;
                const float M = clampr(1.f + P.melt_sun * nrm.dotproduct(sun) + P.melt_treewell * well -
                        P.melt_lee * clampr(lee[i], 0.f, 1.f) - P.melt_road * (road[i] ? 1.f : 0.f),
                    P.floor_m, 2.f);
                const float nn = clampr(noise[i] / noise_max, -1.f, 1.f);
                const auto q = [](float v) { return u32(clampr(iFloor(v * 255.f + 0.5f), 0, 255)); };
                field.factors[i] = q(A * 0.5f) | (q(M * 0.5f) << 8) | (q(nn * 0.5f + 0.5f) << 16) |
                    (q(clampr(neglect[i], 0.f, 1.f)) << 24);
            }
        });
    }

    // ---- The season LUT: per class and day, S_c and Mc_c that hit this level's keys --------------------
    // Calibrated on the hard classes of the ground receivers that can hold snow at all, from the very
    // quantised factors the renderer will read.
    xr_vector<Fvector2> am[da_snow_classes];
    for (size_t i = 0; i < n; ++i)
    {
        const u8 c = cls[i];
        if (c == cls_none || !recv[i])
            continue;
        const u32 f = field.factors[i];
        const float A = float(f & 0xff) * (2.f / 255.f);
        const float M = float((f >> 8) & 0xff) * (2.f / 255.f);
        const float nq = float((f >> 16) & 0xff) * (2.f / 255.f) - 1.f;
        const float Nq = float(f >> 24) * (1.f / 255.f);
        // Texels too steep to hold snow (A = 0) are not part of any class's cover.
        if (A <= 0.f)
            continue;
        float a, m;
        switch (c)
        {
        case da_snow_road:
            a = 1.f + 0.15f * nq;
            m = 1.f;
            break;
        case da_snow_drift:
            a = (1.f + 0.35f * nq) * (0.5f + Nq);
            m = 1.f - 0.3f * Nq;
            break;
        default:
            a = A * (1.f + 0.35f * nq);
            m = M;
            break;
        }
        am[c].push_back({std::max(a, P.floor_a), clampr(m, P.floor_m, 2.f)});
    }
    SClassSamples samples[da_snow_classes];
    for (u32 c = 0; c < da_snow_classes; ++c)
    {
        samples[c] = make_bins(am[c], c == da_snow_drift, P.lut_bins, P.cover_eps);
        xr_vector<Fvector2>().swap(am[c]);
    }

    const SSnowTargets& targets = da_snow_targets();
    field.lut_S.assign(365, Fvector4().set(0.f, 0.f, 0.f, 0.f));
    field.lut_Mc.assign(365, Fvector4().set(0.f, 0.f, 0.f, 0.f));
    xr_parallel_for(TaskRange<size_t>(0, 365, 8), [&](const TaskRange<size_t>& r) {
        SSolveScratch scratch;
        for (int day = int(r.begin()); day != int(r.end()); ++day)
        {
            float S[da_snow_classes], Mc[da_snow_classes];
            for (u32 c = 0; c < da_snow_classes; ++c)
                solve_class(samples[c], targets.cover_at(c, float(day)), targets.depth_at(c, float(day)), targets.cap,
                    scratch, S[c], Mc[c]);
            field.lut_S[day].set(S[0], S[1], S[2], S[3]);
            field.lut_Mc[day].set(Mc[0], Mc[1], Mc[2], Mc[3]);
        }
    });

    const u32 receivers = cls_count[0] + cls_count[1] + cls_count[2] + cls_count[3];
    const float inv_r = receivers ? 1.f / float(receivers) : 0.f;
    Msg("* [snow] %s: %d x %d texels of %.1f m, classes open %.3f trees %.3f road %.3f drift %.3f, %.0f ms",
        Level().name().c_str(), g.nx, g.nz, g.texel, cls_count[0] * inv_r, cls_count[1] * inv_r, cls_count[2] * inv_r,
        cls_count[3] * inv_r, timer.GetElapsed_sec() * 1000.f);
    static constexpr pcstr months[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int m = 0; m < 12; ++m)
    {
        // The 15th of the month (0-based day of the year of a 365-day year).
        static constexpr int key_day[12] = {14, 45, 73, 104, 134, 165, 195, 226, 257, 287, 318, 348};
        const Fvector4& S = field.lut_S[key_day[m]];
        const Fvector4& Mc = field.lut_Mc[key_day[m]];
        if (S.x + S.y + S.z + S.w > 0.f)
            Msg("* [snow]   15 %s: S %.3f %.3f %.3f %.3f, Mc %.3f %.3f %.3f %.3f", months[m], S.x, S.y, S.z, S.w, Mc.x,
                Mc.y, Mc.z, Mc.w);
    }

    da_snow_field_set(std::move(field));
}
