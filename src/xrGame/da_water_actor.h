#pragma once

// The actor in water (DESIGN2 section 4): wading, footstep wakes and splashes, the entry
// splash, the landing damage water takes and the dose an injurious liquid delivers. Data lives
// in dead_air_x64_water.ltx [water_actor] - mechanism in code, bindings in data - and the
// particle names are the game's own hit_fx set. Header-only, one lazily loaded copy shared by
// the movement control, the step manager, the actor and the physics collision path.
//
// Everything here reads the baked water field on CEnvironment. A level without a field simply
// reports no depth, which degrades to the behaviour before this work rather than to nothing.

#include "xrCore/xr_ini.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "GamePersistent.h"
#include "ParticlesObject.h"
#include "xrEngine/da_particle_suppress.h"

struct SDaWaterActorCfg
{
    // ---- Wading. Depths are metres of water standing over the feet. ----
    float ankle{0.12f}; // the wade state, the step splash and the dose all start here
    float wade_full{1.30f}; // depth at which the slow-down bottoms out
    float wade_speed_min{0.40f}; // movement scale at wade_full and past it
    float sprint_depth{0.45f}; // no sprinting deeper than this
    float jump_depth{0.80f}; // no jumping deeper than this

    // ---- Continuous disturbance under a moving foot (CEnvironment::water_wake). ----
    float wake_radius{0.5f};
    float wake_strength{1.f};
    float wake_speed_ref{2.5f}; // speed at which the wake reaches full strength

    // ---- The discrete splash a footstep throws. ----
    float step_splash_depth{0.05f};
    shared_str ps_step{"hit_fx\\hit_water_00"};

    // ---- Breaking the surface: a jump into a pond, a body dropped into one. ----
    float entry_speed_min{2.5f};
    float entry_speed_max{11.f};
    float entry_radius_min{1.f};
    float entry_radius_max{3.2f};
    shared_str ps_entry{"hit_fx\\effects\\hit_water_splash_01"};
    shared_str ps_entry_big{"hit_fx\\effects\\hit_water_splash_02"};

    // ---- Landing damage. Water this deep takes the whole landing; from the ankle to it the
    // damage tapers, so a puddle is still a hard floor. ----
    float fall_safe_depth{0.55f};

    // ---- The dose from an injurious liquid: the binary tick becomes a depth and a motion. ----
    float rad_depth_ref{1.0f}; // depth at which the material's own rate is reached
    float rad_depth_floor{0.20f}; // share of that rate when barely in it
    float rad_move_bonus{0.50f}; // extra share at rad_move_speed - stirring it up costs more
    float rad_move_speed{3.0f};

    // ---- Screen droplets after the head comes back out (r2_lenswater_val -> visor_drops). ----
    float drops_rise{2.5f}; // per second while the head is under
    float drops_fall{0.25f}; // per second once it is out
    float drops_step{0.05f}; // how far the value must move before the console is touched
};

inline const SDaWaterActorCfg& da_water_actor_cfg()
{
    static SDaWaterActorCfg cfg;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        string_path path;
        FS.update_path(path, "$game_config$", "dead_air_x64_water.ltx");
        if (FS.exist(path))
        {
            CInifile ini(path, TRUE);
            if (ini.section_exist("water_actor"))
            {
                const auto rf = [&](pcstr line, float def) {
                    return ini.line_exist("water_actor", line) ? ini.r_float("water_actor", line) : def;
                };
                const auto rs = [&](pcstr line, const shared_str& def) {
                    return ini.line_exist("water_actor", line) ? ini.r_string("water_actor", line) : def;
                };
                cfg.ankle = rf("ankle", cfg.ankle);
                cfg.wade_full = rf("wade_full", cfg.wade_full);
                cfg.wade_speed_min = rf("wade_speed_min", cfg.wade_speed_min);
                cfg.sprint_depth = rf("sprint_depth", cfg.sprint_depth);
                cfg.jump_depth = rf("jump_depth", cfg.jump_depth);
                cfg.wake_radius = rf("wake_radius", cfg.wake_radius);
                cfg.wake_strength = rf("wake_strength", cfg.wake_strength);
                cfg.wake_speed_ref = rf("wake_speed_ref", cfg.wake_speed_ref);
                cfg.step_splash_depth = rf("step_splash_depth", cfg.step_splash_depth);
                cfg.ps_step = rs("ps_step", cfg.ps_step);
                cfg.entry_speed_min = rf("entry_speed_min", cfg.entry_speed_min);
                cfg.entry_speed_max = rf("entry_speed_max", cfg.entry_speed_max);
                cfg.entry_radius_min = rf("entry_radius_min", cfg.entry_radius_min);
                cfg.entry_radius_max = rf("entry_radius_max", cfg.entry_radius_max);
                cfg.ps_entry = rs("ps_entry", cfg.ps_entry);
                cfg.ps_entry_big = rs("ps_entry_big", cfg.ps_entry_big);
                cfg.fall_safe_depth = rf("fall_safe_depth", cfg.fall_safe_depth);
                cfg.rad_depth_ref = rf("rad_depth_ref", cfg.rad_depth_ref);
                cfg.rad_depth_floor = rf("rad_depth_floor", cfg.rad_depth_floor);
                cfg.rad_move_bonus = rf("rad_move_bonus", cfg.rad_move_bonus);
                cfg.rad_move_speed = rf("rad_move_speed", cfg.rad_move_speed);
                cfg.drops_rise = rf("drops_rise", cfg.drops_rise);
                cfg.drops_fall = rf("drops_fall", cfg.drops_fall);
                cfg.drops_step = rf("drops_step", cfg.drops_step);
            }
        }
        // The defaults above are shipping values, not placeholders: wading, water footsteps
        // and the fall-damage exemption cost nothing and are unconditional (DESIGN2 section 7),
        // so a missing section must not disable them.
        clamp(cfg.wade_speed_min, 0.05f, 1.f);
        clamp(cfg.rad_depth_floor, 0.f, 1.f);
        cfg.wade_full = _max(cfg.wade_full, cfg.ankle + 0.05f);
        cfg.entry_speed_max = _max(cfg.entry_speed_max, cfg.entry_speed_min + 0.1f);
    }
    return cfg;
}

// One splash per spot per quarter second. A body sliding into water fires a contact every
// physics step and both feet land inside the same puddle; CEnvironment::water_hit folds its
// rings exactly this way, and the particle has to be folded with them or a single entry
// spawns a dozen systems.
inline void da_water_splash(const Fvector& pos, const shared_str& ps_name)
{
    static Fvector last_pos{};
    static float last_time{-1.f};

    const float now = Device.fTimeGlobal;
    // The statics outlive the level; fTimeGlobal does not. A negative delta means the clock
    // restarted under us, i.e. a new level - not a repeat of the splash we folded last time.
    const float since = now - last_time;
    if (last_time > 0.f && since >= 0.f && since < 0.25f && last_pos.distance_to_sqr(pos) < 0.35f * 0.35f)
        return;
    last_pos = pos;
    last_time = now;

    if (!ps_name.size() || da_particle_suppressed(ps_name.c_str()))
        return;
    // A name that is not in particles.xr yields a null visual, which CParticlesObject already
    // survives with a log line - but the object itself must still be checked before use.
    CParticlesObject* ps = CParticlesObject::Create(ps_name.c_str(), TRUE);
    if (!ps)
        return;

    Fmatrix xform;
    xform.identity();
    xform.k.set(0.f, 1.f, 0.f);
    Fvector::generate_orthonormal_basis(xform.k, xform.j, xform.i);
    xform.c.set(pos);
    ps->UpdateParent(xform, Fvector().set(0.f, 0.f, 0.f));
    GamePersistent().ps_schedule_play(ps);
}
