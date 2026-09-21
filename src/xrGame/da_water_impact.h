// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Water response of the procedural puddles to bullets and explosions. Data lives in
// dead_air_x64_water.ltx (the mechanism-in-code / bindings-in-data rule): the material
// name routes a puddle hit into the game's own bullet-x-water pair, the particle names
// are the game's own hit_fx set. Header-only: both the bullet manager and the explosive
// share one lazily loaded copy.

#include "xrCore/xr_ini.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Level.h"

struct SDaWaterImpactCfg
{
    shared_str material{"materials\\water"};
    float mask_threshold{0.22f};
    float ring_radius_bullet{1.4f};
    float ring_radius_scale{0.9f};
    float drain_radius_scale{0.55f};
    float drain_radius_min{1.5f};
    float drain_radius_max{4.0f};
    float deep_threshold{0.45f};
    float mid_threshold{0.20f};
    // Rings from feet and bodies (step_manager.cpp, physics_game.cpp).
    float ring_radius_step{1.6f};
    float ring_radius_object_min{0.7f};
    float ring_radius_object_max{2.4f};
    float ring_object_threshold{1.5f};
    float ring_object_velocity{12.f};
    float ring_distance{45.f};
    // Escaped: "\h" is not an escape sequence, and the unescaped spelling collapsed to
    // "amikhit_fxhit_water_splash_00" - a name that is in no particle set.
    shared_str ps_fountain{"amik\\hit_fx\\hit_water_splash_00"};
    shared_str ps_hit_big{"hit_fx\\effects\\hit_water_hit_big"};
    shared_str ps_distort_big{"hit_fx\\effects\\hit_water_hit_distort_big"};
    shared_str ps_hit{"hit_fx\\effects\\hit_water_hit"};
    bool enabled{false};
};

inline const SDaWaterImpactCfg& da_water_impact_cfg()
{
    static SDaWaterImpactCfg cfg;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        string_path path;
        FS.update_path(path, "$game_config$", "dead_air_x64_water.ltx");
        if (FS.exist(path))
        {
            CInifile ini(path, TRUE);
            if (ini.section_exist("water_impact"))
            {
                const auto rf = [&](pcstr line, float def) {
                    return ini.line_exist("water_impact", line) ? ini.r_float("water_impact", line) : def;
                };
                const auto rs = [&](pcstr line, const shared_str& def) {
                    return ini.line_exist("water_impact", line) ? ini.r_string("water_impact", line) : def;
                };
                cfg.material = rs("material", cfg.material);
                cfg.mask_threshold = rf("mask_threshold", cfg.mask_threshold);
                cfg.ring_radius_bullet = rf("ring_radius_bullet", cfg.ring_radius_bullet);
                cfg.ring_radius_scale = rf("ring_radius_scale", cfg.ring_radius_scale);
                cfg.drain_radius_scale = rf("drain_radius_scale", cfg.drain_radius_scale);
                cfg.drain_radius_min = rf("drain_radius_min", cfg.drain_radius_min);
                cfg.drain_radius_max = rf("drain_radius_max", cfg.drain_radius_max);
                cfg.deep_threshold = rf("deep_threshold", cfg.deep_threshold);
                cfg.mid_threshold = rf("mid_threshold", cfg.mid_threshold);
                cfg.ring_radius_step = rf("ring_radius_step", cfg.ring_radius_step);
                cfg.ring_radius_object_min = rf("ring_radius_object_min", cfg.ring_radius_object_min);
                cfg.ring_radius_object_max = rf("ring_radius_object_max", cfg.ring_radius_object_max);
                cfg.ring_object_threshold = rf("ring_object_threshold", cfg.ring_object_threshold);
                cfg.ring_object_velocity = rf("ring_object_velocity", cfg.ring_object_velocity);
                cfg.ring_distance = rf("ring_distance", cfg.ring_distance);
                cfg.ps_fountain = rs("ps_fountain", cfg.ps_fountain);
                cfg.ps_hit_big = rs("ps_hit_big", cfg.ps_hit_big);
                cfg.ps_distort_big = rs("ps_distort_big", cfg.ps_distort_big);
                cfg.ps_hit = rs("ps_hit", cfg.ps_hit);
                cfg.enabled = true;
            }
        }
        if (!cfg.enabled)
            Msg("! [water] dead_air_x64_water.ltx not found - puddle impact response disabled");
    }
    return cfg;
}

// Where a hit lands in water, and the point on the surface the ring belongs to: on a liquid
// material (a lake, a river) the spot itself; on the procedural rain puddle the spot; and
// under a liquid surface - a foot on the lake floor, a body that sank to it: the water mesh
// is passable, so what touches the bottom reports the bottom's material - the point straight
// above on that surface. Water is water; every response treats them alike, except that open
// water never dries. mtl_idx may be GAMEMTL_NONE_IDX or negative.
inline bool da_water_surface(const Fvector& pos, int mtl_idx, float ground_ny, Fvector& surface)
{
    surface = pos;
    if (mtl_idx >= 0 && mtl_idx < int(GMLib.CountMaterial()))
        if (const SGameMtl* m = GMLib.GetMaterialByIdx(u16(mtl_idx)); m && m->Flags.test(SGameMtl::flLiquid))
            return true;
    if (!g_pGamePersistent)
        return false;
    if (g_pGamePersistent->Environment().SamplePuddleMask(pos, ground_ny) > da_water_impact_cfg().mask_threshold)
        return true;
    if (!g_pGameLevel)
        return false;
    // The surface above the spot, looked for both ways: down from three metres above (the
    // face of a surface that faces up) and up from the spot (a surface wound the other way;
    // the static query culls back faces, so only one of the two can see a given mesh).
    auto& space = Level().ObjectSpace;
    const auto liquid_hit = [&](const collide::rq_result& rq) {
        const CDB::TRI& tri = space.GetStaticTris()[rq.element];
        const SGameMtl* m = GMLib.GetMaterialByIdx(u16(tri.material));
        return m && m->Flags.test(SGameMtl::flLiquid);
    };
    collide::rq_result rq;
    Fvector from = pos;
    from.y += 3.f;
    if (space.RayPick(from, Fvector{0.f, -1.f, 0.f}, 2.95f, collide::rqtStatic, rq, nullptr) && liquid_hit(rq))
    {
        surface.y = from.y - rq.range;
        return true;
    }
    from = pos;
    from.y += 0.05f;
    if (space.RayPick(from, Fvector{0.f, 1.f, 0.f}, 3.f, collide::rqtStatic, rq, nullptr) && liquid_hit(rq))
    {
        surface.y = from.y + rq.range;
        return true;
    }
    return false;
}

inline bool da_water_here(const Fvector& pos, int mtl_idx, float ground_ny)
{
    Fvector surface;
    return da_water_surface(pos, mtl_idx, ground_ny, surface);
}
