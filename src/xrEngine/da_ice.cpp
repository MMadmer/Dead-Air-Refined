// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The ice of the water for gameplay (docs/dead-air/WEATHER.md, ice). See da_ice.h.

#include "stdafx.h"
#include "da_ice.h"
#include "da_climate.h"
#include "Environment.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xrMaterialSystem/GameMtlLib.h"

#include <array>

namespace
{
constexpr size_t holes_max = da_ice_holes_max;

xr_vector<SDaIceHole> s_holes;
u32 s_hole_seed = 0x1CE5EEDu;

double game_day()
{
    return g_pGameLevel ? double(g_pGameLevel->GetEnvironmentGameTime()) * 0.001 / 86400.0 : 0.0;
}
} // namespace

const SDaIceCfg& da_ice_cfg()
{
    static SDaIceCfg cfg;
    static bool loaded = false;
    if (loaded)
        return cfg;
    loaded = true;
    string_path path;
    if (!FS.exist(path, "$game_config$", "dead_air_x64_water.ltx"))
        return cfg;
    const CInifile ini(path, TRUE);
    constexpr pcstr sect = "ice";
    if (!ini.section_exist(sect))
        return cfg;
    const auto rf = [&](pcstr line, float& v) {
        if (ini.line_exist(sect, line))
            v = ini.r_float(sect, line);
    };
    const auto rs = [&](pcstr line, shared_str& v) {
        if (ini.line_exist(sect, line))
            v = ini.r_string(sect, line);
    };
    rs("material", cfg.material);
    rf("safe", cfg.safe);
    rf("hold", cfg.hold);
    rf("crack_min", cfg.crack_min);
    rf("crack_max", cfg.crack_max);
    rf("hole_radius", cfg.hole_radius);
    rf("trail_radius", cfg.trail_radius);
    rf("bullet_radius", cfg.bullet_radius);
    rf("refreeze_hours", cfg.refreeze_hours);
    rs("crack_sounds", cfg.snd_crack);
    rs("break_sounds", cfg.snd_break);
    rs("bullet_thin_sounds", cfg.snd_bullet_thin);
    rs("break_particles", cfg.ps_break);
    rs("splash_particles", cfg.ps_splash);
    rs("bullet_thin_particles", cfg.ps_bullet_thin);
    cfg.hold = std::max(cfg.hold, 0.005f);
    cfg.safe = std::max(cfg.safe, cfg.hold + 0.005f);
    return cfg;
}

u16 da_ice_material()
{
    static u16 idx = u16(GAMEMTL_NONE_IDX);
    static bool looked = false;
    if (!looked && GMLib.CountMaterial())
    {
        looked = true;
        idx = GMLib.GetMaterialIdx(da_ice_cfg().material.c_str());
    }
    return idx;
}

EDaIce da_ice_at(const Fvector& p, float* strength)
{
    if (strength)
        *strength = 0.f;
    if (!da_climate_enabled() || !g_pGamePersistent)
        return da_ice_none;
    const CEnvironment& env = g_pGamePersistent->Environment();
    const float cover = env.ice_at(p);
    // Under half the cover it is water (the front's outer half, as water.ps draws it).
    if (cover <= 0.5f)
        return da_ice_none;
    if (da_ice_in_hole(p))
        return da_ice_none;
    const float h = da_climate_ice_strength(cover, env.water_flowing_at(p));
    if (strength)
        *strength = h;
    const SDaIceCfg& cfg = da_ice_cfg();
    return h >= cfg.safe ? da_ice_solid : h >= cfg.hold ? da_ice_weak : da_ice_thin;
}

bool da_ice_holds(const Fvector& p)
{
    const EDaIce s = da_ice_at(p);
    return s == da_ice_solid || s == da_ice_weak;
}

void da_ice_hole_add(const Fvector& p, float radius, u8 kind)
{
    if (radius <= 0.f)
        return;
    // A hole inside a bigger one of the same kind adds nothing; a long trail of a wading man merges as it goes.
    for (const SDaIceHole& h : s_holes)
        if (h.kind == kind && Fvector2().set(p.x - h.xz.x, p.z - h.xz.y).magnitude() + radius <= h.radius)
            return;
    if (s_holes.size() >= holes_max)
        s_holes.erase(s_holes.begin());
    SDaIceHole h;
    h.xz.set(p.x, p.z);
    h.radius = radius;
    h.surface = p.y;
    h.born = game_day();
    s_hole_seed = s_hole_seed * 1664525u + 1013904223u;
    h.seed = s_hole_seed >> 8;
    h.kind = kind;
    s_holes.push_back(h);
}

bool da_ice_in_hole(const Fvector& p)
{
    for (const SDaIceHole& h : s_holes)
    {
        if (h.kind != 0)
            continue;
        const float dx = p.x - h.xz.x, dz = p.z - h.xz.y;
        // The broken edge is ragged; the physics takes its mean circle.
        if (dx * dx + dz * dz < h.radius * h.radius && std::abs(p.y - h.surface) < 1.5f)
            return true;
    }
    return false;
}

const xr_vector<SDaIceHole>& da_ice_holes() { return s_holes; }

u32 da_ice_holes_near(const Fvector& eye, Fvector4 (&out)[da_ice_holes_gpu])
{
    // The nearest first: a partial sort of indices by distance.
    std::array<std::pair<float, u32>, holes_max> order;
    const u32 n = u32(std::min(s_holes.size(), holes_max));
    for (u32 i = 0; i < n; ++i)
    {
        const SDaIceHole& h = s_holes[i];
        order[i] = {Fvector2().set(eye.x - h.xz.x, eye.z - h.xz.y).magnitude() - h.radius, i};
    }
    const u32 m = std::min(n, da_ice_holes_gpu);
    std::partial_sort(order.begin(), order.begin() + m, order.begin() + n);
    for (u32 i = 0; i < m; ++i)
    {
        const SDaIceHole& h = s_holes[order[i].second];
        out[i].set(h.xz.x, h.xz.y, h.radius, float(h.kind) * 1000.f + float(h.seed % 1000u));
    }
    return m;
}

void da_ice_reset() { s_holes.clear(); }

void da_ice_frame()
{
    if (s_holes.empty())
        return;
    if (!g_pGameLevel)
    {
        s_holes.clear();
        return;
    }
    // In frost a hole skins over in its hours; a thaw leaves it open. The ice gone, its holes go with it.
    const double now = game_day();
    const float air = da_climate_now().t_air;
    const float life = std::max(da_ice_cfg().refreeze_hours, 0.5f) / 24.f;
    const bool frozen = da_climate_state().ice[0] > 0.f || da_climate_state().ice[1] > 0.f;
    s_holes.erase(std::remove_if(s_holes.begin(), s_holes.end(),
                      [&](const SDaIceHole& h) {
                          return !frozen || now < h.born - 1.0 || (air < -1.f && now - h.born > life);
                      }),
        s_holes.end());
}
