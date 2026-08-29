#pragma once

// Water response of the procedural puddles to bullets and explosions. Data lives in
// dead_air_x64_water.ltx (the mechanism-in-code / bindings-in-data rule): the material
// name routes a puddle hit into the game's own bullet-x-water pair, the particle names
// are the game's own hit_fx set. Header-only: both the bullet manager and the explosive
// share one lazily loaded copy.

#include "xrCore/xr_ini.h"

struct SDaWaterImpactCfg
{
    shared_str material{"materials\\water"};
    float mask_threshold{0.22f};
    float ring_radius_bullet{0.9f};
    float ring_radius_scale{0.9f};
    float drain_radius_scale{0.55f};
    float drain_radius_min{1.5f};
    float drain_radius_max{4.0f};
    float deep_threshold{0.45f};
    float mid_threshold{0.20f};
    shared_str ps_fountain{"amik\\hit_fx\\water_splash\\water_spurt"};
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
