// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// xrEngine internal: the season keys of dead_air_x64_seasons.ltx that both the snow (da_snow_field.cpp) and the
// climate (da_climate.cpp) read - the snow targets and looks, the precipitation phase, the temperature and the
// built-in ice keys. Loaded once, on first use.

#include "da_snow_field.h"

enum EPhase : int
{
    phase_snow = 0,
    phase_sleet,
    phase_rain
};
inline constexpr pcstr phase_name[3] = {"snow", "sleet", "rain"};

struct SColourCurve
{
    DaSeasonCurve ch[3];

    Fvector at(float day) const
    {
        Fvector c;
        c.set(std::max(ch[0].at(day), 0.f), std::max(ch[1].at(day), 0.f), std::max(ch[2].at(day), 0.f));
        return c;
    }
};

struct SSnowKeys
{
    SSnowTargets targets;
    DaSeasonCurve state[4]; // fresh, settled, old, wet
    DaSeasonCurve age, crust, wet, drift, comp_road, rho_open, rho_road, dirt_road, litter_trees;
    SColourCurve albedo[3]; // open, road, trees

    DaSeasonCurve p_snow, p_mixed, p_rain, snowfall_share, p_precip, thunder, glaze, frost_night;
    DaSeasonCurve t_mean, t_sigma, t_range;
    bool t_mean_keys = true; // false: the annual cosine fit below stands in for the monthly means
    float t_a = 7.89f, t_b = 12.14f, t_phase = 194.5f;
    float ts = -0.4f, tl = 2.8f, tw = 2.f;
    // Precipitation days run this much colder, with this factor on the day-to-day spread.
    float t_precip_bias = -0.5f, t_precip_sigma = 1.4f;
    // Decision D6: everything falls as snow from snow_from to snow_to (days, wrapping the new year).
    float snow_from = 334.f, snow_to = 58.5f;

    // Never shorter than the zero-grass window (28 Feb runs to its midnight there, to noon in the data).
    bool snow_only(float day) const
    {
        const float d = da_season_wrap(day);
        if (da_snow_winter_window(d))
            return true;
        return snow_from <= snow_to ? (d >= snow_from && d <= snow_to) : (d >= snow_from || d <= snow_to);
    }
    float mean_temperature(float day) const
    {
        return t_mean_keys ? t_mean.at(day) : t_a + t_b * std::cos(PI_MUL_2 * (day - t_phase) / 365.f);
    }

    DaSeasonCurve ice_still, ice_flowing, ice_snow_cover, ice_snow_depth;
};

// The keys, loaded on first use (dead_air_x64_seasons.ltx over the built-in tables).
const SSnowKeys& da_snow_keys();
