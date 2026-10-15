// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// The ice of the water for gameplay (docs/dead-air/WEATHER.md, ice): where the season's ice carries a man, what has
// broken it, and the material the physics, the bullets and the steps see there. The cover is CEnvironment::ice_at's
// (the picture's rule), the thickness and its strength the climate's (da_climate_ice_strength). The level's AI graph
// never changes: the creatures that walk the graph keep off the water as they always did.

// What the ice of a point does under a weight.
enum EDaIce : u8
{
    da_ice_none = 0, // open water, or no ice at all
    da_ice_thin,     // ice too thin to carry anything: whatever steps on it goes through
    da_ice_weak,     // carries a man for a moment, cracking, then gives way
    da_ice_solid     // carries him
};

struct SDaIceCfg
{
    shared_str material{"materials\\ice"};
    float safe = 0.07f;       // m of sound ice that carries a man (DSNS: 7 cm of clear ice for one person)
    float hold = 0.03f;       // m under which it carries nothing
    float crack_min = 0.25f;  // s a man stands on ice just over `hold` before it gives way ...
    float crack_max = 4.f;    // ... and on ice just under `safe`
    float hole_radius = 0.9f; // m of a hole a man breaks
    float trail_radius = 0.6f; // m of the channel a man wading through thin ice opens
    float bullet_radius = 0.06f;
    float refreeze_hours = 8.f; // game hours a hole needs to skin over again in frost
    shared_str snd_crack, snd_break, snd_bullet_thin;
    shared_str ps_break, ps_splash, ps_bullet;
};
ENGINE_API const SDaIceCfg& da_ice_cfg();

// The ice's own material index (GAMEMTL_NONE_IDX when the library has none).
ENGINE_API u16 da_ice_material();

// The state of the ice at a point of a water surface (p on or near the surface), and its strength (m of sound ice).
ENGINE_API EDaIce da_ice_at(const Fvector& p, float* strength = nullptr);
// Whether a liquid triangle carries a weight at p: solid or weak ice, not broken. The physics' rule.
ENGINE_API bool da_ice_holds(const Fvector& p);

// A hole broken into the ice (by a man, a blast, a bullet): kind 0 a hole of open water, 1 a bullet's star (the ice
// stays, cracked); it refreezes after a while in frost.
struct SDaIceHole
{
    Fvector2 xz{};
    float radius = 0.f;
    float surface = 0.f; // world y of the water surface there
    double born = 0.0;   // game day it was made
    u32 seed = 0;
    u8 kind = 0;
};
ENGINE_API void da_ice_hole_add(const Fvector& p, float radius, u8 kind);
ENGINE_API bool da_ice_in_hole(const Fvector& p);
ENGINE_API const xr_vector<SDaIceHole>& da_ice_holes();
// The holes nearest the camera for the shaders: x, z, radius, kind * 1000 + seed % 1000 (up to da_ice_holes_gpu).
constexpr u32 da_ice_holes_gpu = 16;
ENGINE_API u32 da_ice_holes_near(const Fvector& eye, Fvector4 (&out)[da_ice_holes_gpu]);
// A level load clears them (the holes of another level, or of a load, are not this one's).
ENGINE_API void da_ice_reset();
// xrEngine only: the refreeze of the frame (device.cpp).
void da_ice_frame();
