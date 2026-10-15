// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "da_season.h"

// The regional climate of the Zone (docs/dead-air/WEATHER.md): what the sky does, how warm the air is, how much
// falls and how hard the wind blows at any game moment, and what that left on the ground and on the water - the
// snowpack and the ice. Everything is a pure function of the absolute game time, the season day and the game's
// climate seed: a reload replays the same weather, and the snow and the ice of a moment are the integral of the
// weather that came before it, recomputed at a load instead of being saved.

// The sky of an hour. da_sky_name gives the weather cycle's state (af3_<night brightness>_<state>[_<moon>]): the
// clear, the rain, the snow and the blizzard are the climate's own cycles, which hold their sky the whole day (the
// stock AtmosFear cycles tell a day's story - a sunny noon in the rain cycle, a fog at noon in the cloudy one).
enum EDaSky : u8
{
    da_sky_clear = 0,
    da_sky_partly,
    da_sky_overcast,
    da_sky_foggy,
    da_sky_rain,
    da_sky_storm,
    da_sky_thunder,
    da_sky_snow,
    da_sky_blizzard,
    da_sky_count
};
ENGINE_API pcstr da_sky_name(u8 sky);

// The weather at a moment.
struct SDaClimateSample
{
    float t_air = 10.f;   // C at 2 m: the month's mean, the synoptic anomaly, the day's swing
    float t_day = 10.f;   // C, the day's mean (no swing)
    float cloud = 0.5f;   // sky cover 0..1
    float precip = 0.f;   // 0..1, what the weather's rain_density means: rain 25 mm/h, snow 2.5 mm/h w.e. at 1
    float wind_ms = 3.f;  // mean wind at 10 m
    float fog = 0.f;      // 0..1 how far into the fog the hour is (0 = none)
    int phase = 2;        // EPhase (da_season_keys.h) of the six-hour block: 0 snow, 1 sleet, 2 rain
    bool thunder = false; // the hour's lightning (rain only)
    bool glaze = false;   // freezing rain in the six-hour block
    u8 sky = da_sky_clear;
};
// day = the absolute game day (game seconds / 86400), D = the season day it belongs to.
ENGINE_API void da_climate_sample(double day, float D, SDaClimateSample& out);

// What the weather left: the snowpack of open ground and the ice of the water.
struct SDaClimateState
{
    float swe = 0.f;      // mm of water in the open snowpack
    float rho = 100.f;    // kg/m3, its mean density
    float depth = 0.f;    // m, swe / rho
    float fresh = 0.f;    // m of the last snowfall still lying loose on top (part of depth)
    float melt = 0.f;     // mm/h melting now
    float drift = 0.f;    // 0..1 snow the wind lifts and carries now (a blizzard, a ground blizzard)
    float water_t[2]{8.f, 8.f}; // C of standing and running water under no ice
    float ice[2]{};       // m of ice on standing and running water
    float ice_rot[2]{};   // C days of thaw the ice took since it last grew (rotten spring ice)
    float ice_snow[2]{};  // m of snow on that ice
    float ice_cover[2]{}; // the share of the water it closes over: the front out from the banks (da_snow_ice.h's key)
};
ENGINE_API const SDaClimateState& da_climate_state();
// The weather of this frame (the same function at the level's game time and season day).
ENGINE_API const SDaClimateSample& da_climate_now();

// r__season_weather: the climate drives the weather (the sky, what falls, the wind, the snowpack, the ice). Off:
// AtmosFear's own graphs and the season keys of the snow and the ice, as before 1.6.0's third phase.
ENGINE_API bool da_climate_enabled();
// The climate drives this level's weather: on, and the level is out under the sky (it has a snow field; an
// underground level keeps its own indoor weather).
ENGINE_API bool da_climate_outdoors();
// The weather manager (Lua) set the climate's sky on this level: the precipitation, its phase and the wind of the
// climate replace the cycle's own. A level that keeps a weather of its own (a design fog) still takes what falls.
ENGINE_API void da_climate_set_sky_driven(bool driven);
ENGINE_API bool da_climate_sky_driven();
// The game's climate seed (saved by the scripts with the game): another seed is another weather history.
ENGINE_API void da_climate_set_seed(u32 seed);
ENGINE_API u32 da_climate_seed();
// The integrated snowpack and ice as text, kept with the game by the scripts: a load continues the weather it left
// instead of a history integrated again (that one sees the season move with every game day, the live one does not).
// Empty while there is none; a load of an unknown or broken text is refused and the history runs.
ENGINE_API pcstr da_climate_state_save();
ENGINE_API bool da_climate_state_load(pcstr text);

// The ice a point of water carries for gameplay: its thickness (m) where the season's ice covers it (cover from
// CEnvironment::ice_at at the point, flowing from the level's water class), and how much of it a weight can trust
// (the rotten spring ice is weaker than its thickness).
ENGINE_API float da_climate_ice_thickness(float cover, bool flowing);
ENGINE_API float da_climate_ice_strength(float cover, bool flowing);

// xrEngine only: the climate of this frame (device.cpp, after da_season_frame) and its console.
void da_climate_frame();
void da_climate_register_console();
// xrEngine only: the hour of the sky clock (seasonal day length, docs/dead-air/WEATHER.md) for a level clock time
// in seconds of the day at season day D; identity while r__season_daylight is off.
float da_sky_clock(float level_seconds, float D);
// The sun for the level clock and season day (Chornobyl, Kyiv civil time): the heading in the convention of
// CEnvDescriptorMixer::calculate_dynamic_sun_dir (azimuth_offset = the descriptor's sun_azimuth, added the way it
// adds it) and the elevation.
struct SDaSun
{
    float azimuth = 0.f;   // radians, setHP heading
    float elevation = 0.f; // radians above the horizon (negative = below)
};
SDaSun da_sun_at(float level_seconds, float D, float azimuth_offset);
bool da_season_daylight();
