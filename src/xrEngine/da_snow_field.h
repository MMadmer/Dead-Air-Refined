// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "da_season.h"

// Snow of the season (docs/dead-air/SNOW.md): the level bake (xrGame, da_snow_bake.cpp), the snow
// constants of this frame, the deformer stamps and the precipitation phase. xrEngine owns the storage;
// the renderer only reads it.

// Level-wide bake at 1 m, row-major (index = z * dim_x + x), texel (x, z) covers
// [origin + (x, z) * texel, origin + (x + 1, z + 1) * texel).
struct SSnowField
{
    u32 dim_x = 0, dim_z = 0;
    Fvector2 origin{};
    float texel = 1.f;
    xr_vector<float> ground;  // world y of the ground (terrain, hole-filled)
    xr_vector<float> top;     // world y of the topmost non-passable surface (roofs, decks get snow)
    xr_vector<u8> solid;      // 1 where collision lies under the texel; 0: top is the hole-filled ground
    xr_vector<float> walk;    // world y of the highest surface the level's AI walks on (its nodes), -flt_max: none
    xr_vector<u8> cover;      // where top is a cover over the ground (da_snow_cover_at): 2 an overhang (a roof, an
                              // eave, a deck over open space), 1 a solid object standing there (a block, a car), 0 none
    xr_vector<u8> shelter;    // 255 deep under an overhang (the overhangs eroded by a texel): no snow of the sky there
    xr_vector<u32> weights;   // RGBA8 unorm soft class weights: r open, g under trees, b road, a drift (sum 1)
    xr_vector<u32> factors;   // RGBA8 unorm: r A/2 (accumulation, 0..2), g M/2 (melt, 0..2), b n*0.5+0.5 (noise -1..1), a N (neglect 0..1)
    // This level's season scalars per day (365 entries), per class x open, y trees, z road, w drift:
    // depth of a class c at a texel = clamp(S_c * A_c - Mc_c * M_c, 0, cap) with
    // A_open = A_trees = A * (1 + 0.35 n), A_road = 1 + 0.15 n, A_drift = (1 + 0.35 n) * (0.5 + N),
    // M_open = M_trees = M, M_road = 1, M_drift = 1 - 0.3 N; depth = sum_c w_c * depth_c.
    xr_vector<Fvector4> lut_S, lut_Mc;
};

// A cover (docs/dead-air/SNOW.md, covers): a solid top this far over the terrain which the level's AI does not walk
// on - a roof, a vehicle, a block, a stump; the ground's snow lies on the terrain or the floor under it, and the snow
// on top is the shells' (da_ground.cpp, the high ones) or the layer's cover. Only an overhang shelters the ground
// under it: round a solid object the ground's snow runs on at its full depth up to its sides (sheltered, the snow
// sank into a ragged moat a texel or two wide round every block and stump). A deck the AI walks (a bridge, a
// platform, a flat roof with a ladder) is ground: its snow is the trodden ground's, with its trails. The bake (1 m),
// the window's resolve (da_ground_resolve.cs, DA_GR_OVER_TERRAIN) and the shells take the same test.
constexpr float da_snow_cover_over = 0.35f;
inline bool da_snow_walkable(float top, float walk) { return walk > top - 0.5f && walk < top + 0.3f; }

// Snow constants of this frame (season day D and D_prev of da_season.h).
struct SSnowFrame
{
    Fvector4 S{}, Mc{}, S_prev{}, Mc_prev{}; // per class at D / D_prev (this level's LUT)
    Fvector4 cover{};       // class cover keys at D (roads == 1 -> roads fully covered)
    Fvector4 state{};       // weights fresh, settled, old, wet (slush = 1 - sum)
    Fvector4 surf{};        // age, crust, wetness, drift strength (0..1)
    Fvector4 dirt{};        // roadside dirt, litter under trees, road compaction, cap (m)
    Fvector4 fall{};        // snowfall now (m/h of fresh snow), refill rate (m/s), air temperature (C), unused
    Fvector4 ice{};         // still water ice, flowing water ice, snow cover on ice, snow depth on ice (m)
    Fvector4 albedo_open{}, albedo_road{}, albedo_trees{}; // linear rgb, w unused
    float grass_visible = 1.f; // saturate((0.9 - cover_open) / 0.6): 0 = not a single blade
    float mean_depth = 0.f;    // level-mean depth (m): 0 = every snow pass is skipped
    bool active = false;       // any snow anywhere this frame
};

// The deformers of a frame: feet, body capsules, settled objects, craters.
struct SSnowStamp
{
    Fvector2 xz{};      // world
    float foot_y = 0.f; // world y of the sole / lowest point of the body
    float radius = 0.f; // m
    float sharp = 0.f;  // rim steepness: stamped height = foot + (dist / radius)^2 * sharp
    Fvector2 push{};    // horizontal velocity (m/s), the grass push map's direction and strength
    u32 kind = 0;       // 0 foot, 1 body capsule (a creature's, a moving physics element's), 2 settled object,
                        // 3 crater (steep walls; sink > 0: a soft bowl, a campfire's thaw)
    float sink = 0.f;   // deepest a foot or body sinks under the snow surface (m); 0 = down to foot_y, < 0 = the
                        // snow ignores it (the grass push only)
    // Where the deformer was on the previous frame: the stamp is the capsule swept from there to xz, so a
    // fast foot or body leaves a continuous furrow. Equal to xz / foot_y for a disc.
    Fvector2 xz0{};
    float foot_y0 = 0.f;
    // How the snow under the deformer gives way (both 0: carved down to foot_y):
    //   pack > 0  a planted foot packs the loose snow under it to this density (kg/m3): it sinks until the
    //             compacted column carries it, deep in fresh powder, hardly in old snow;
    //   fall > 0  a deformer ploughing through the snow (a swinging leg, a low body) pushes aside what lies
    //             above it, and this share of it falls back into the furrow behind.
    float pack = 0.f;
    float fall = 0.f;
};

// The precipitation phase of this moment (weather decides how much falls, the season what falls).
struct SPrecipState
{
    float liquid = 1.f;    // liquid share of what falls now (rain 1, snow 0): wetness, rain FX gates
    float snow_rate = 0.f; // snowfall intensity 0..1 for the flakes
    float air_temp = 10.f; // C
    float frost = 0.f;     // hoarfrost on vegetation now 0..1
    float dew = 0.f;       // dew now 0..1
    bool thunder_ok = true;
};

ENGINE_API const SSnowField* da_snow_field();  // null without a bake (no level, underground level)
ENGINE_API u32 da_snow_field_stamp();           // changes on every new bake or reset
ENGINE_API void da_snow_field_set(SSnowField&& field);
ENGINE_API void da_snow_field_reset();

ENGINE_API const SSnowFrame& da_snow_frame();   // valid after da_snow_frame_update() of this frame
ENGINE_API void da_snow_frame_update();          // once per frame on the main thread, before rendering

// Thread-safe, during the frame's updates. False when the frame's list is full and the stamp was dropped.
ENGINE_API bool da_snow_stamp_add(const SSnowStamp& stamp);
// How many more stamps this frame's list takes now (thread-safe; others may add meanwhile).
ENGINE_API u32 da_snow_stamps_room();
ENGINE_API const xr_vector<SSnowStamp>& da_snow_stamps();       // the previous frame's complete list
ENGINE_API void da_snow_stamps_publish();                       // once per frame (device.cpp, by FrameMove)

// CPU replica of the season snow (no trails): depth (m) and the snow surface y at a world position.
ENGINE_API float da_snow_depth_at(float x, float z);
// The same without the overhangs' shelter: the snow lying on a cover's top (a roof's, a trailer's) - the shells'.
ENGINE_API float da_snow_open_depth_at(float x, float z);
ENGINE_API float da_snow_surface_y(float x, float z);

ENGINE_API const SPrecipState& da_precip();

// ---- Additions of the WORLD stage (docs/dead-air/WINTER.md) ---------------------------------------

// Calendar winter, 1 Dec .. 28 Feb (decision D1): not a single grass blade, roads fully covered and
// compacted, precipitation falls as snow only.
inline bool da_snow_winter_window(float day)
{
    const float d = da_season_wrap(day);
    return d >= 334.f || d < 59.f;
}

// Class order of every per-class vector: open, under trees, road, drift.
enum : u32
{
    da_snow_open = 0,
    da_snow_trees,
    da_snow_road,
    da_snow_drift,
    da_snow_classes
};

// The class targets of [snow_keys]: the share of a class under snow and the depth of its covered part
// (game metres; the drift key is the 90th percentile). The level bake solves its LUT against these, the
// frame publishes them as SSnowFrame::cover.
struct SSnowTargets
{
    DaSeasonCurve cover[da_snow_classes], depth[da_snow_classes];
    float cap = 0.536f;

    float cover_at(u32 c, float day) const
    {
        float v = std::min(std::max(cover[c].at(day), 0.f), 1.f);
        if (da_snow_winter_window(day))
        {
            if (c == da_snow_open)
                v = std::max(v, 0.9f);
            // Under the crowns the snow lies thinner, never bare: a deciduous wood is snowed over whole in
            // midwinter (bare tree wells drew brown discs round every trunk).
            else if (c == da_snow_road || c == da_snow_trees)
                v = 1.f;
        }
        return v;
    }
    float depth_at(u32 c, float day) const { return std::min(std::max(depth[c].at(day), 0.f), cap); }
};
ENGINE_API const SSnowTargets& da_snow_targets();

// Depth of a texel of the field under the class scalars S/Mc (the formula of SSnowField, without trails).
ENGINE_API float da_snow_texel_depth(const SSnowField& field, size_t index, const Fvector4& S, const Fvector4& Mc);

// xrEngine internal: the precipitation phase of this frame (CEnvironment::OnFrame, after the weather
// lerp and before the rain and the lightning) and the QA console commands (registered once).
class CEnvironment;
void da_precip_tick(CEnvironment& env);
void da_snow_register_console();
