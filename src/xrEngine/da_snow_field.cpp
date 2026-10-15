// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The world side of the season snow (docs/dead-air/WINTER.md): the level's baked snow field, the snow
// constants of every frame evaluated from dead_air_x64_seasons.ltx at the season day, the deformer
// stamps of the frame, and the precipitation phase - the weather says how much falls, the season and
// the air temperature say what falls.

#include "stdafx.h"
#include "da_snow_field.h"
#include "da_season_keys.h"
#include "da_climate.h"
#include "Environment.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include <limits>
#include <mutex>

namespace
{
// The 15th of each month at noon, 0-based days (da_season.h).
constexpr float month_day[12] = {14.5f, 45.5f, 73.5f, 104.5f, 134.5f, 165.5f, 195.5f, 226.5f, 257.5f, 287.5f,
    318.5f, 348.5f};
// Fresh snow density (kg/m3): what a millimetre of water equivalent turns into on the ground.
constexpr float rho_fresh = 120.f;
// Snowfall at full weather density, mm/h of water equivalent (winter rates run ~10x below rain's).
constexpr float snow_we_full = 2.5f;
// Most stamps a frame keeps; past it the frame is a pathological crowd and the rest wait a frame.
constexpr size_t stamps_max = 4096;

float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void set_months(DaSeasonCurve& c, const float (&v)[12])
{
    c.clear();
    for (int i = 0; i < 12; ++i)
        c.add(month_day[i], v[i]);
}

void set_colour(SColourCurve& c, const u32 (&rgb)[12])
{
    for (int ch = 0; ch < 3; ++ch)
    {
        c.ch[ch].clear();
        for (int i = 0; i < 12; ++i)
            c.ch[ch].add(month_day[i], srgb_to_linear(float((rgb[i] >> (16 - 8 * ch)) & 0xff) / 255.f));
    }
}

// Built-in keys: the brief's tables (T3 snow, precipitation phase, water ice) with decisions D1 (calendar
// winter: open cover >= 0.9, roads covered from 1 Dec to 28 Feb) and D2 (no snow before 1 Nov). Used
// key by key wherever dead_air_x64_seasons.ltx does not say otherwise.
void set_defaults(SSnowKeys& k)
{
    // The months without snow hold the November values of the look curves, so the first snow of a
    // November is fresh rather than the slush the April keys would carry over.
    set_months(k.targets.cover[da_snow_open], {1.f, 1.f, .45f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .35f, 1.f});
    set_months(k.targets.cover[da_snow_trees], {.95f, .95f, .72f, .05f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .2f, .9f});
    set_months(k.targets.cover[da_snow_road], {1.f, 1.f, .45f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .35f, 1.f});
    set_months(k.targets.cover[da_snow_drift], {1.f, 1.f, .9f, .1f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .5f, 1.f});
    set_months(k.targets.depth[da_snow_open], {.17f, .22f, .10f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .03f, .10f});
    set_months(k.targets.depth[da_snow_trees], {.11f, .14f, .10f, .05f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .015f, .06f});
    set_months(k.targets.depth[da_snow_road], {.084f, .12f, .04f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .021f, .047f});
    set_months(k.targets.depth[da_snow_drift], {.40f, .536f, .32f, .12f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .06f, .24f});
    for (u32 c = 0; c < da_snow_classes; ++c)
    {
        k.targets.cover[c].add(304.f, 0.f);
        k.targets.depth[c].add(304.f, 0.f);
    }
    k.targets.cover[da_snow_open].add(334.f, .9f);
    k.targets.cover[da_snow_open].add(58.5f, .9f);
    k.targets.cover[da_snow_road].add(334.f, 1.f);
    k.targets.cover[da_snow_road].add(58.5f, 1.f);
    k.targets.cap = 0.536f;

    set_months(k.state[0], {.35f, .25f, 0.f, 0.f, .5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f});
    set_months(k.state[1], {.40f, .35f, 0.f, 0.f, .1f, .1f, .1f, .1f, .1f, .1f, .1f, .35f});
    set_months(k.state[2], {.20f, .30f, .20f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .10f});
    set_months(k.state[3], {.05f, .10f, .55f, .30f, .4f, .4f, .4f, .4f, .4f, .4f, .4f, .05f});
    set_months(k.age, {.4f, .6f, .9f, 1.f, .15f, .15f, .15f, .15f, .15f, .15f, .15f, .25f});
    set_months(k.crust, {.4f, .5f, .3f, .1f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .25f});
    set_months(k.wet, {.08f, .15f, .7f, .9f, .45f, .45f, .45f, .45f, .45f, .45f, .45f, .12f});
    set_months(k.drift, {.45f, .5f, .15f, 0.f, .1f, .1f, .1f, .1f, .1f, .1f, .1f, .3f});
    set_months(k.comp_road, {.85f, .9f, .95f, 1.f, .4f, .4f, .4f, .4f, .4f, .4f, .4f, .7f});
    set_months(k.rho_open, {226.f, 260.f, 451.f, 546.f, 243.f, 243.f, 243.f, 243.f, 243.f, 243.f, 243.f, 194.f});
    set_months(k.rho_road, {459.f, 476.f, 498.f, 500.f, 346.f, 346.f, 346.f, 346.f, 346.f, 346.f, 346.f, 408.f});
    set_months(k.dirt_road, {.15f, .25f, .5f, .6f, .05f, .05f, .05f, .05f, .05f, .05f, .05f, .1f});
    set_months(k.litter_trees, {.05f, .1f, .35f, .5f, .1f, .1f, .1f, .1f, .1f, .1f, .1f, .05f});
    set_colour(k.albedo[0], {0xEAECF1, 0xE7E9EE, 0xCDCED0, 0xBEC0C1, 0xE3E5E9, 0xE3E5E9, 0xE3E5E9, 0xE3E5E9,
        0xE3E5E9, 0xE3E5E9, 0xE3E5E9, 0xECEEF3});
    set_colour(k.albedo[1], {0xD9DADD, 0xD4D5D7, 0xC3C2C1, 0xBBBAB8, 0xDBDCDF, 0xDBDCDF, 0xDBDCDF, 0xDBDCDF,
        0xDBDCDF, 0xDBDCDF, 0xDBDCDF, 0xDCDDE0});
    set_colour(k.albedo[2], {0xE9EBEF, 0xE5E6EA, 0xC5C4C4, 0xB3B3AF, 0xE1E2E5, 0xE1E2E5, 0xE1E2E5, 0xE1E2E5,
        0xE1E2E5, 0xE1E2E5, 0xE1E2E5, 0xEBEDF1});

    set_months(k.p_snow, {.674f, .696f, .416f, .076f, .007f, 0.f, 0.f, 0.f, 0.f, .103f, .419f, .662f});
    set_months(k.p_mixed, {.129f, .141f, .192f, .104f, .007f, 0.f, 0.f, 0.f, .008f, .128f, .199f, .148f});
    set_months(k.p_rain, {.197f, .163f, .392f, .821f, .986f, 1.f, 1.f, 1.f, .992f, .769f, .382f, .191f});
    set_months(k.snowfall_share, {.739f, .766f, .512f, .128f, .011f, 0.f, 0.f, 0.f, .004f, .167f, .518f, .736f});
    set_months(k.p_precip, {.261f, .318f, .261f, .25f, .281f, .34f, .297f, .229f, .29f, .239f, .29f, .294f});
    set_months(k.thunder, {.024f, .007f, .016f, .176f, .68f, .902f, 1.f, .74f, .227f, .054f, .002f, .009f});
    set_months(k.glaze, {.247f, .225f, .049f, .013f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .115f, .22f});
    set_months(k.frost_night, {.914f, .925f, .697f, .141f, .012f, .001f, 0.f, 0.f, .01f, .154f, .646f, .934f});
    // Monthly means (NOAA Chornobyl 1981-2010), the day-to-day spread of the daily mean (Chornobyl 2017)
    // and the diurnal range Tmax - Tmin (NOAA), C.
    set_months(k.t_mean, {-3.5f, -3.4f, 1.5f, 8.9f, 14.9f, 17.9f, 19.9f, 18.8f, 13.4f, 7.7f, 1.4f, -2.8f});
    set_months(k.t_sigma, {5.1f, 4.2f, 4.f, 3.6f, 3.8f, 3.3f, 3.4f, 3.8f, 3.9f, 3.5f, 3.8f, 4.3f});
    set_months(k.t_range, {5.3f, 6.8f, 8.3f, 10.6f, 11.9f, 11.4f, 11.2f, 11.7f, 10.2f, 8.6f, 5.3f, 4.9f});

    set_months(k.ice_still, {1.f, 1.f, .5f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .05f, .85f});
    set_months(k.ice_flowing, {.92f, .92f, .6f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .6f});
    set_months(k.ice_snow_cover, {1.f, 1.f, .27f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .02f, .85f});
    set_months(k.ice_snow_depth, {.136f, .176f, .08f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, .024f, .08f});
}

// "day:value, day:value, ..." -> a curve. False (and the built-in kept) when the line is absent or holds no key.
bool read_curve(const CInifile& ini, pcstr sect, pcstr line, DaSeasonCurve& out)
{
    if (!ini.line_exist(sect, line))
        return false;
    pcstr src = ini.r_string(sect, line);
    DaSeasonCurve c;
    const int n = _GetItemCount(src, ',');
    for (int i = 0; i < n; ++i)
    {
        string128 item;
        _GetItem(src, i, item, ',');
        float d, v;
        if (sscanf(item, " %f : %f", &d, &v) == 2 && _valid(d) && _valid(v))
            c.add(d, v);
    }
    if (!c.n)
    {
        Msg("! [snow] [%s] %s: no 'day:value' key, the built-in curve stays", sect, line);
        return false;
    }
    out = c;
    return true;
}

// "day:#RRGGBB, ..." (sRGB of the dry albedo) -> three LINEAR curves.
bool read_colour(const CInifile& ini, pcstr sect, pcstr line, SColourCurve& out)
{
    if (!ini.line_exist(sect, line))
        return false;
    pcstr src = ini.r_string(sect, line);
    SColourCurve c;
    const int n = _GetItemCount(src, ',');
    for (int i = 0; i < n; ++i)
    {
        string128 item;
        _GetItem(src, i, item, ',');
        float d;
        u32 rgb;
        if (sscanf(item, " %f : #%x", &d, &rgb) == 2 && _valid(d))
            for (int ch = 0; ch < 3; ++ch)
                c.ch[ch].add(d, srgb_to_linear(float((rgb >> (16 - 8 * ch)) & 0xff) / 255.f));
    }
    if (!c.ch[0].n)
    {
        Msg("! [snow] [%s] %s: no 'day:#RRGGBB' key, the built-in colour stays", sect, line);
        return false;
    }
    out = c;
    return true;
}

void read_float(const CInifile& ini, pcstr sect, pcstr line, float& out)
{
    if (ini.line_exist(sect, line))
    {
        const float v = ini.r_float(sect, line);
        if (_valid(v))
            out = v;
    }
}

void build_all(SSnowKeys& k)
{
    for (u32 c = 0; c < da_snow_classes; ++c)
    {
        k.targets.cover[c].build();
        k.targets.depth[c].build();
    }
    DaSeasonCurve* curves[] = {&k.state[0], &k.state[1], &k.state[2], &k.state[3], &k.age, &k.crust, &k.wet,
        &k.drift, &k.comp_road, &k.rho_open, &k.rho_road, &k.dirt_road, &k.litter_trees, &k.p_snow, &k.p_mixed,
        &k.p_rain, &k.snowfall_share, &k.p_precip, &k.thunder, &k.glaze, &k.frost_night, &k.t_mean, &k.t_sigma,
        &k.t_range,
        &k.ice_still, &k.ice_flowing, &k.ice_snow_cover, &k.ice_snow_depth};
    for (DaSeasonCurve* c : curves)
        c->build();
    for (auto& a : k.albedo)
        for (auto& ch : a.ch)
            ch.build();
}

SSnowKeys g_keys;
bool g_keys_loaded = false;

const SSnowKeys& keys()
{
    if (g_keys_loaded)
        return g_keys;
    g_keys_loaded = true;

    SSnowKeys k;
    set_defaults(k);

    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_seasons.ltx");
    if (FS.exist(path))
    {
        const CInifile ini(path, TRUE);
        u32 read = 0;
        if (ini.section_exist("snow_keys"))
        {
            static constexpr pcstr cls[da_snow_classes] = {"open", "trees", "road", "drift"};
            for (u32 c = 0; c < da_snow_classes; ++c)
            {
                string64 line;
                xr_sprintf(line, "cover_%s", cls[c]);
                read += read_curve(ini, "snow_keys", line, k.targets.cover[c]);
                xr_sprintf(line, "depth_%s", cls[c]);
                read += read_curve(ini, "snow_keys", line, k.targets.depth[c]);
            }
            read += read_curve(ini, "snow_keys", "state_fresh", k.state[0]);
            read += read_curve(ini, "snow_keys", "state_settled", k.state[1]);
            read += read_curve(ini, "snow_keys", "state_old", k.state[2]);
            read += read_curve(ini, "snow_keys", "state_wet", k.state[3]);
            read += read_curve(ini, "snow_keys", "age", k.age);
            read += read_curve(ini, "snow_keys", "crust", k.crust);
            read += read_curve(ini, "snow_keys", "wet", k.wet);
            read += read_curve(ini, "snow_keys", "drift", k.drift);
            read += read_curve(ini, "snow_keys", "comp_road", k.comp_road);
            read += read_curve(ini, "snow_keys", "rho_open", k.rho_open);
            read += read_curve(ini, "snow_keys", "rho_road", k.rho_road);
            read += read_curve(ini, "snow_keys", "dirt_road", k.dirt_road);
            read += read_curve(ini, "snow_keys", "litter_trees", k.litter_trees);
            read += read_colour(ini, "snow_keys", "albedo_open", k.albedo[0]);
            read += read_colour(ini, "snow_keys", "albedo_road", k.albedo[1]);
            read += read_colour(ini, "snow_keys", "albedo_trees", k.albedo[2]);
            read_float(ini, "snow_keys", "cap", k.targets.cap);
            k.targets.cap = clampr(k.targets.cap, 0.05f, 2.f);
        }
        if (ini.section_exist("precip_keys"))
        {
            read += read_curve(ini, "precip_keys", "snow", k.p_snow);
            read += read_curve(ini, "precip_keys", "mixed", k.p_mixed);
            read += read_curve(ini, "precip_keys", "rain", k.p_rain);
            read += read_curve(ini, "precip_keys", "snowfall_share", k.snowfall_share);
            read += read_curve(ini, "precip_keys", "p_precip", k.p_precip);
            read += read_curve(ini, "precip_keys", "thunder_weight", k.thunder);
            read += read_curve(ini, "precip_keys", "glaze", k.glaze);
            read += read_curve(ini, "precip_keys", "frost_night", k.frost_night);
            read += read_curve(ini, "precip_keys", "t_sigma", k.t_sigma);
            // The monthly means when the data has them; a data file with only the annual fit gets the fit.
            const bool t_curve = read_curve(ini, "precip_keys", "t_mean", k.t_mean);
            read += t_curve;
            k.t_mean_keys = t_curve || !ini.line_exist("precip_keys", "t_mean_a");
            read += read_curve(ini, "precip_keys", "t_range", k.t_range);
            read_float(ini, "precip_keys", "t_mean_a", k.t_a);
            read_float(ini, "precip_keys", "t_mean_b", k.t_b);
            read_float(ini, "precip_keys", "t_mean_phase", k.t_phase);
            read_float(ini, "precip_keys", "ts", k.ts);
            read_float(ini, "precip_keys", "tl", k.tl);
            read_float(ini, "precip_keys", "w", k.tw);
            k.tw = std::max(k.tw, 0.1f);
            read_float(ini, "precip_keys", "t_precip_bias", k.t_precip_bias);
            read_float(ini, "precip_keys", "t_precip_sigma", k.t_precip_sigma);
            if (ini.line_exist("precip_keys", "snow_only"))
            {
                float from, to;
                if (sscanf(ini.r_string("precip_keys", "snow_only"), "%f , %f", &from, &to) == 2 && _valid(from) &&
                    _valid(to))
                {
                    k.snow_from = da_season_wrap(from);
                    k.snow_to = da_season_wrap(to);
                }
                else
                    Msg("! [snow] [precip_keys] snow_only: expected 'from, to' (days)");
            }
        }
        if (ini.section_exist("ice_keys"))
        {
            read += read_curve(ini, "ice_keys", "still", k.ice_still);
            read += read_curve(ini, "ice_keys", "flowing", k.ice_flowing);
            read += read_curve(ini, "ice_keys", "snow_cover_on_ice", k.ice_snow_cover);
            read += read_curve(ini, "ice_keys", "snow_depth_on_ice", k.ice_snow_depth);
        }
        Msg("* [snow] season keys: %u curve(s) from dead_air_x64_seasons.ltx, the rest built in", read);
    }
    else
        Msg("! [snow] dead_air_x64_seasons.ltx not found - built-in snow, precipitation and ice keys");

    build_all(k);
    g_keys = k;
    return g_keys;
}

// ---- Hash -----------------------------------------------------------------------------------------------

// The QA sweep's draws (the climate has the weather's own noise, da_climate.cpp).
float hash01(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) * (1.f / 16777216.f);
}

// ---- State --------------------------------------------------------------------------------------------

SSnowField s_field;
bool s_field_valid = false;
u32 s_field_stamp = 0;
float s_share[da_snow_classes]{}; // class shares of the snow-receiving texels

SSnowFrame s_frame;

std::mutex s_stamp_lock;
xr_vector<SSnowStamp> s_stamps_pending;
xr_vector<SSnowStamp> s_stamps;

SPrecipState s_precip;

struct SPrecipRuntime
{
    s64 block = std::numeric_limits<s64>::min();
    int phase = phase_rain;
    bool thunder = true;
    bool glaze = false;
    bool precipitating = false;
    float t_day = 10.f;
    float hour = 12.f;
    float qa_hour = -1.f;
    double last_game_s = -1.0;
    // The surface of the snow between the season keys: age since the last fresh fall (0 = fresh, 1 = as
    // old as the key allows), a glaze crust that decays, rain soaking into it.
    float age_since = 1.f;
    float crust_evt = 0.f;
    float rain_on_snow = 0.f;
    float fresh_mh = 0.f;
};
SPrecipRuntime s_rt;

// QA overrides (qa_precip, qa_air_temp): never saved.
int s_qa_phase = -1;
float s_qa_air = flt_max;

void lut_at(const SSnowField& f, float day, Fvector4& S, Fvector4& Mc)
{
    const float d = da_season_wrap(day);
    const int i0 = std::min(int(d), 364);
    const int i1 = (i0 + 1) % 365;
    const float t = d - float(i0);
    const Fvector4& s0 = f.lut_S[i0];
    const Fvector4& s1 = f.lut_S[i1];
    const Fvector4& m0 = f.lut_Mc[i0];
    const Fvector4& m1 = f.lut_Mc[i1];
    S.set(s0.x + (s1.x - s0.x) * t, s0.y + (s1.y - s0.y) * t, s0.z + (s1.z - s0.z) * t, s0.w + (s1.w - s0.w) * t);
    Mc.set(m0.x + (m1.x - m0.x) * t, m0.y + (m1.y - m0.y) * t, m0.z + (m1.z - m0.z) * t, m0.w + (m1.w - m0.w) * t);
}

float night_factor(float hour)
{
    // 1 from 21:00 to 06:00, ramps down to 0 by 09:30 and back up from 18:00.
    const float h = hour - 24.f * std::floor(hour / 24.f);
    if (h >= 21.f || h <= 6.f)
        return 1.f;
    if (h < 9.5f)
        return 1.f - (h - 6.f) / 3.5f;
    if (h > 18.f)
        return (h - 18.f) / 3.f;
    return 0.f;
}

void reset_runtime()
{
    const float qa_hour = s_rt.qa_hour;
    s_rt = SPrecipRuntime{};
    s_rt.qa_hour = qa_hour;
}

// ---- The snowpack in the season's picture -------------------------------------------------------------------
// The climate's snowpack (da_climate) is one depth of open ground. The level's picture of snow - where it lies
// deeper, thinner, where the melt bares the ground first - is the season LUT of a day; the pack shows the day
// whose open mean depth it has: on the way in (1 Nov .. 15 Feb) a winter that has come that far, on the way out
// (15 Feb .. 15 May) a spring that melted that far. Past the deepest key the same picture deepens.

// Open-ground mean depth of the keys at a day: the covered share times the depth of the covered part.
float open_mean(const SSnowKeys& k, float day)
{
    return k.targets.cover_at(da_snow_open, day) * k.targets.depth_at(da_snow_open, day);
}

struct SPackBranch
{
    static constexpr int n = 241;
    float day[n]{}, depth[n]{};
};

// The branches sampled densely and made monotone (in: running maximum, out: running minimum).
const SPackBranch& pack_branch(bool melting)
{
    static SPackBranch in, out;
    static bool built = false;
    if (!built)
    {
        built = true;
        const SSnowKeys& k = keys();
        constexpr float in_from = 304.f, in_to = 45.5f + 365.f;
        constexpr float out_from = 45.5f, out_to = 135.f;
        for (int i = 0; i < SPackBranch::n; ++i)
        {
            const float t = float(i) / float(SPackBranch::n - 1);
            in.day[i] = da_season_wrap(in_from + (in_to - in_from) * t);
            in.depth[i] = open_mean(k, in.day[i]);
            out.day[i] = out_from + (out_to - out_from) * t;
            out.depth[i] = open_mean(k, out.day[i]);
            if (i)
            {
                in.depth[i] = std::max(in.depth[i], in.depth[i - 1]);
                out.depth[i] = std::min(out.depth[i], out.depth[i - 1]);
            }
        }
    }
    return melting ? out : in;
}

// The season day whose picture the pack shows, and the factor the picture deepens by past the deepest key.
void pack_day(float old_depth, float D, float& day, float& ratio)
{
    const float d = da_season_wrap(D);
    const bool melting = d >= 45.5f && d < 150.f;
    const SPackBranch& br = pack_branch(melting);
    constexpr int last = SPackBranch::n - 1;
    ratio = 1.f;
    const float peak = melting ? br.depth[0] : br.depth[last];
    if (old_depth <= 1e-4f)
    {
        day = melting ? br.day[last] : br.day[0];
        return;
    }
    if (old_depth >= peak)
    {
        day = melting ? br.day[0] : br.day[last];
        ratio = peak > 1e-4f ? old_depth / peak : 1.f;
        return;
    }
    day = br.day[last];
    for (int i = 1; i < SPackBranch::n; ++i)
    {
        const float a = br.depth[i - 1], b = br.depth[i];
        if (melting ? b > old_depth : b < old_depth)
            continue;
        const float t = std::abs(b - a) > 1e-6f ? clampr((old_depth - a) / (b - a), 0.f, 1.f) : 1.f;
        day = da_season_wrap(br.day[i - 1] + da_season_wrap(br.day[i] - br.day[i - 1]) * t);
        return;
    }
}

// How much of a fresh fall each class keeps: the crowns catch some, the snow on a road settles faster.
constexpr float fresh_share[da_snow_classes] = {1.f, 0.7f, 0.8f, 1.f};
// A fresh fall of this many metres whitens the bare patches of the picture to a third.
constexpr float fresh_whiten = 0.02f;

// The last frame's dynamic S / Mc (motion vectors), and the field they belong to.
Fvector4 s_last_S{}, s_last_Mc{};
u32 s_last_stamp = 0;
bool s_last_valid = false;
} // namespace

// ---- Field ----------------------------------------------------------------------------------------------

const SSnowField* da_snow_field() { return s_field_valid ? &s_field : nullptr; }
u32 da_snow_field_stamp() { return s_field_stamp; }

void da_snow_field_set(SSnowField&& field)
{
    const size_t n = size_t(field.dim_x) * size_t(field.dim_z);
    const bool ok = n && field.texel > 0.f && field.ground.size() == n && field.top.size() == n &&
        field.weights.size() == n && field.factors.size() == n && field.lut_S.size() == 365 &&
        field.lut_Mc.size() == 365 && (field.solid.empty() || field.solid.size() == n) &&
        (field.cover.empty() || field.cover.size() == n) && (field.shelter.empty() || field.shelter.size() == n);
    if (!ok)
    {
        Msg("! [snow] the level's snow field is malformed (%u x %u) - no snow on this level", field.dim_x, field.dim_z);
        da_snow_field_reset();
        return;
    }
    s_field = std::move(field);
    s_field_valid = true;
    ++s_field_stamp;

    double sum[da_snow_classes]{};
    double total = 0.0;
    for (const u32 w : s_field.weights)
    {
        if (!w)
            continue;
        for (u32 c = 0; c < da_snow_classes; ++c)
            sum[c] += double((w >> (8 * c)) & 0xff);
        total += 255.0;
    }
    for (u32 c = 0; c < da_snow_classes; ++c)
        s_share[c] = total > 0.0 ? float(sum[c] / total) : 0.f;

    {
        std::lock_guard lock(s_stamp_lock);
        s_stamps_pending.clear();
        s_stamps.clear();
    }
    reset_runtime();
}

void da_snow_field_reset()
{
    s_field = {};
    s_field_valid = false;
    ++s_field_stamp;
    for (float& s : s_share)
        s = 0.f;
    {
        std::lock_guard lock(s_stamp_lock);
        s_stamps_pending.clear();
        s_stamps_pending.shrink_to_fit();
        s_stamps.clear();
        s_stamps.shrink_to_fit();
    }
    reset_runtime();
}

const SSnowKeys& da_snow_keys() { return keys(); }
const SSnowTargets& da_snow_targets() { return keys().targets; }

float da_snow_texel_depth(const SSnowField& field, size_t index, const Fvector4& S, const Fvector4& Mc)
{
    const u32 w = field.weights[index];
    if (!w)
        return 0.f;
    const u32 f = field.factors[index];
    const float cap = keys().targets.cap;
    const float A = float(f & 0xff) * (2.f / 255.f);
    const float M = float((f >> 8) & 0xff) * (2.f / 255.f);
    const float n = float((f >> 16) & 0xff) * (2.f / 255.f) - 1.f;
    const float N = float(f >> 24) * (1.f / 255.f);
    const float a_open = A * (1.f + 0.35f * n);
    const float a_road = 1.f + 0.15f * n;
    const float a_drift = (1.f + 0.35f * n) * (0.5f + N);
    const float m_drift = 1.f - 0.3f * N;
    const auto dc = [cap](float s, float a, float mc, float m) { return clampr(s * a - mc * m, 0.f, cap); };
    return (float(w & 0xff) * dc(S.x, a_open, Mc.x, M) + float((w >> 8) & 0xff) * dc(S.y, a_open, Mc.y, M) +
               float((w >> 16) & 0xff) * dc(S.z, a_road, Mc.z, 1.f) + float(w >> 24) * dc(S.w, a_drift, Mc.w, m_drift)) *
        (1.f / 255.f);
}

// ---- Frame ------------------------------------------------------------------------------------------------

const SSnowFrame& da_snow_frame() { return s_frame; }

void da_snow_frame_update()
{
    const SSnowKeys& k = keys();
    const float D = da_season_day();
    const float Dp = da_season_day_prev();
    const bool qa = da_season_qa();

    SSnowFrame f;
    const SSnowField* field = da_snow_field();

    // The climate's snowpack (docs/dead-air/WEATHER.md); the season keys while it is off or a QA sweep drives D.
    const bool dynamic = da_climate_enabled() && !qa;
    const SDaClimateState& pack = da_climate_state();
    float day = D, ratio = 1.f, fresh = 0.f;
    if (dynamic)
    {
        // Quantised to 2 mm: the composites rerun only when the picture moves by a step they can store.
        constexpr float q = 0.002f;
        fresh = std::round(pack.fresh / q) * q;
        const float old = std::max(std::round(pack.depth / q) * q - fresh, 0.f);
        pack_day(old, D, day, ratio);
    }
    const float whiten = std::exp(-fresh / fresh_whiten);

    if (field)
    {
        lut_at(*field, day, f.S, f.Mc);
        if (dynamic)
        {
            f.S.set(f.S.x * ratio + fresh * fresh_share[0], f.S.y * ratio + fresh * fresh_share[1],
                f.S.z * ratio + fresh * fresh_share[2], f.S.w * ratio + fresh * fresh_share[3]);
            f.Mc.mul(ratio * whiten);
            const bool same_field = s_last_valid && s_last_stamp == da_snow_field_stamp();
            f.S_prev = same_field ? s_last_S : f.S;
            f.Mc_prev = same_field ? s_last_Mc : f.Mc;
            s_last_S = f.S;
            s_last_Mc = f.Mc;
            s_last_stamp = da_snow_field_stamp();
            s_last_valid = true;
        }
        else
        {
            lut_at(*field, Dp, f.S_prev, f.Mc_prev);
            s_last_valid = false;
        }
    }

    float cover[da_snow_classes], depth[da_snow_classes];
    for (u32 c = 0; c < da_snow_classes; ++c)
    {
        cover[c] = k.targets.cover_at(c, day);
        depth[c] = k.targets.depth_at(c, day);
        if (dynamic)
        {
            // A fresh fall whitens the bare patches; the picture deepens with the pack.
            cover[c] = 1.f - (1.f - cover[c]) * whiten;
            depth[c] = std::min(depth[c] * ratio + fresh * fresh_share[c], k.targets.cap);
        }
    }
    f.cover.set(cover[0], cover[1], cover[2], cover[3]);

    // State weights of the keys, shifted toward fresh snow by a recent fall.
    float st[4];
    float total = 0.f;
    for (int i = 0; i < 4; ++i)
    {
        st[i] = clampr(k.state[i].at(D), 0.f, 1.f);
        total += st[i];
    }
    if (total > 1.f)
    {
        for (float& s : st)
            s /= total;
        total = 1.f;
    }
    const float age_key = clampr(k.age.at(D), 0.f, 1.f);
    const float age = qa ? age_key : std::min(age_key, s_rt.age_since);
    const float fresh_boost = age_key > 1e-3f ? clampr(1.f - age / age_key, 0.f, 1.f) : 0.f;
    for (float& s : st)
        s *= 1.f - fresh_boost;
    st[0] += fresh_boost * total;
    f.state.set(st[0], st[1], st[2], st[3]);

    const float crust = clampr(qa ? k.crust.at(D) : std::max(k.crust.at(D), s_rt.crust_evt), 0.f, 1.f);
    float wet = clampr(qa ? k.wet.at(D) : std::max(k.wet.at(D), s_rt.rain_on_snow), 0.f, 1.f);
    // A thawing pack is wet whatever the month: half a millimetre of melt an hour soaks it.
    if (dynamic)
        wet = std::max(wet, clampr(pack.melt / 0.5f, 0.f, 1.f));
    // The wind's drift: the month's key, and the snow the wind carries now.
    const float drift = clampr(std::max(k.drift.at(D), dynamic ? pack.drift : 0.f), 0.f, 1.f);
    f.surf.set(age, crust, wet, drift);
    f.dirt.set(clampr(k.dirt_road.at(D), 0.f, 1.f), clampr(k.litter_trees.at(D), 0.f, 1.f),
        clampr(k.comp_road.at(D), 0.f, 1.f), k.targets.cap);

    // Trails refill from fresh snow in game time, and from what a blizzard carries (4 cm an hour at full drift);
    // the renderer integrates over real seconds.
    const float time_factor = g_pGamePersistent ? std::max(g_pGamePersistent->Environment().fTimeFactor, 0.f) : 1.f;
    const float fill_mh = s_rt.fresh_mh + (dynamic ? 0.04f * pack.drift : 0.f);
    f.fall.set(s_rt.fresh_mh, fill_mh * time_factor * (1.f / 3600.f), s_precip.air_temp, 0.f);

    if (!field)
    {
        // A level without the snow's field (underground, interiors): its water never freezes.
        f.ice.set(0.f, 0.f, 0.f, 0.f);
    }
    else if (dynamic)
    {
        // The climate's ice: the cover closes over with the thickness (thin ice at the banks first); the snow on it is
        // what fell once it held.
        const float still = pack.ice_cover[0];
        const float flowing = pack.ice_cover[1];
        const float snow_on = std::max(pack.ice_snow[0], pack.ice_snow[1]);
        f.ice.set(still, flowing, clampr(snow_on / 0.02f, 0.f, 1.f) * std::max(still, flowing),
            clampr(snow_on, 0.f, k.targets.cap));
    }
    else
        f.ice.set(clampr(k.ice_still.at(D), 0.f, 1.f), clampr(k.ice_flowing.at(D), 0.f, 1.f),
            clampr(k.ice_snow_cover.at(D), 0.f, 1.f), clampr(k.ice_snow_depth.at(D), 0.f, k.targets.cap));

    const Fvector a_open = k.albedo[0].at(D), a_road = k.albedo[1].at(D), a_trees = k.albedo[2].at(D);
    f.albedo_open.set(a_open.x, a_open.y, a_open.z, 0.f);
    f.albedo_road.set(a_road.x, a_road.y, a_road.z, 0.f);
    f.albedo_trees.set(a_trees.x, a_trees.y, a_trees.z, 0.f);

    f.grass_visible = clampr((0.9f - cover[da_snow_open]) / 0.6f, 0.f, 1.f);

    // Level mean depth: class share x covered share x depth of the covered part (the drift key is a 90th
    // percentile; its mean runs at ~0.8 of it).
    float mean = 0.f, any_cover = 0.f;
    if (field)
    {
        for (u32 c = 0; c < da_snow_classes; ++c)
        {
            mean += s_share[c] * cover[c] * depth[c] * (c == da_snow_drift ? 0.8f : 1.f);
            any_cover = std::max(any_cover, s_share[c] > 0.f ? cover[c] : 0.f);
        }
    }
    f.mean_depth = mean;
    f.active = field && (mean > 0.f || any_cover > 0.f);

    s_frame = f;
}

// ---- Stamps ---------------------------------------------------------------------------------------------

bool da_snow_stamp_add(const SSnowStamp& stamp)
{
    std::lock_guard lock(s_stamp_lock);
    if (s_stamps_pending.size() >= stamps_max)
        return false;
    s_stamps_pending.push_back(stamp);
    return true;
}

u32 da_snow_stamps_room()
{
    std::lock_guard lock(s_stamp_lock);
    return u32(stamps_max - std::min(s_stamps_pending.size(), stamps_max));
}

const xr_vector<SSnowStamp>& da_snow_stamps() { return s_stamps; }

void da_snow_stamps_publish()
{
    std::lock_guard lock(s_stamp_lock);
    s_stamps.swap(s_stamps_pending);
    s_stamps_pending.clear();
}

// ---- CPU replica ------------------------------------------------------------------------------------------

namespace
{
struct STexelTap
{
    size_t i00, i10, i01, i11;
    float tx, tz;
};

STexelTap tap(const SSnowField& f, float x, float z)
{
    const float fx = (x - f.origin.x) / f.texel - 0.5f;
    const float fz = (z - f.origin.y) / f.texel - 0.5f;
    const float flx = std::floor(fx), flz = std::floor(fz);
    const int mx = int(f.dim_x) - 1, mz = int(f.dim_z) - 1;
    const int x0 = clampr(int(flx), 0, mx), x1 = clampr(int(flx) + 1, 0, mx);
    const int z0 = clampr(int(flz), 0, mz), z1 = clampr(int(flz) + 1, 0, mz);
    STexelTap t;
    t.i00 = size_t(z0) * f.dim_x + size_t(x0);
    t.i10 = size_t(z0) * f.dim_x + size_t(x1);
    t.i01 = size_t(z1) * f.dim_x + size_t(x0);
    t.i11 = size_t(z1) * f.dim_x + size_t(x1);
    t.tx = clampr(fx - flx, 0.f, 1.f);
    t.tz = clampr(fz - flz, 0.f, 1.f);
    return t;
}

float bilerp(const STexelTap& t, float a00, float a10, float a01, float a11)
{
    const float a = a00 + (a10 - a00) * t.tx;
    const float b = a01 + (a11 - a01) * t.tx;
    return a + (b - a) * t.tz;
}
} // namespace

float da_snow_depth_at(float x, float z)
{
    const SSnowField* f = da_snow_field();
    if (!f || !s_frame.active)
        return 0.f;
    const STexelTap t = tap(*f, x, z);
    const Fvector4& S = s_frame.S;
    const Fvector4& Mc = s_frame.Mc;
    // under a cover the sky's snow does not reach the ground (the renderer's shelter, da_snow_level.cs)
    const bool sheltered = !f->shelter.empty();
    const auto at = [&](size_t i) {
        return da_snow_texel_depth(*f, i, S, Mc) * (sheltered ? 1.f - float(f->shelter[i]) * (1.f / 255.f) : 1.f);
    };
    return bilerp(t, at(t.i00), at(t.i10), at(t.i01), at(t.i11));
}

float da_snow_open_depth_at(float x, float z)
{
    const SSnowField* f = da_snow_field();
    if (!f || !s_frame.active)
        return 0.f;
    const STexelTap t = tap(*f, x, z);
    const Fvector4& S = s_frame.S;
    const Fvector4& Mc = s_frame.Mc;
    return bilerp(t, da_snow_texel_depth(*f, t.i00, S, Mc), da_snow_texel_depth(*f, t.i10, S, Mc),
        da_snow_texel_depth(*f, t.i01, S, Mc), da_snow_texel_depth(*f, t.i11, S, Mc));
}

float da_snow_surface_y(float x, float z)
{
    const SSnowField* f = da_snow_field();
    if (!f)
        return -flt_max;
    const STexelTap t = tap(*f, x, z);
    // the ground's snow lies on the terrain under a cover, on the top elsewhere
    const bool covers = !f->cover.empty();
    const auto base = [&](size_t i) { return covers && f->cover[i] ? f->ground[i] : f->top[i]; };
    return bilerp(t, base(t.i00), base(t.i10), base(t.i01), base(t.i11)) + da_snow_depth_at(x, z);
}

// ---- Precipitation -------------------------------------------------------------------------------------

const SPrecipState& da_precip() { return s_precip; }

void da_precip_tick(CEnvironment& env)
{
    const SSnowKeys& k = keys();
    const float D = da_season_day();
    const bool qa = da_season_qa();
    const float dt = Device.fTimeDelta;
    const float density = clampr(env.CurrentEnv.rain_density, 0.f, 1.f);
    const bool precipitating = density >= EPS_L;

    // Game clock: absolute environment time (saved with the game), so every draw below is a function of
    // where the game is, not of when this session started.
    const double game_s = g_pGameLevel ? double(g_pGameLevel->GetEnvironmentGameTime()) * 0.001 : double(Device.fTimeGlobal);
    const double game_days = game_s / 86400.0;
    double dt_game_s = s_rt.last_game_s < 0.0 ? 0.0 : game_s - s_rt.last_game_s;
    s_rt.last_game_s = game_s;
    // A sleep or a time skip advances the snow by the skip, a load backwards does nothing.
    dt_game_s = std::clamp(dt_game_s, 0.0, 3.0 * 86400.0);

    // Hour: the game's, frozen while a QA command drives the season (frost and dew stop strobing).
    float hour = env.GetGameTime() / 3600.f;
    if (qa)
    {
        if (s_rt.qa_hour < 0.f)
            s_rt.qa_hour = hour;
        hour = s_rt.qa_hour;
    }
    else
        s_rt.qa_hour = -1.f;
    s_rt.hour = hour;

    // ---- Air temperature: climatology + synoptic anomaly + diurnal swing (da_climate) ----------------
    // The climate's air when it drives the weather (its own swing under its own cloud and rain, docs/dead-air/
    // WEATHER.md); otherwise its day's mean with the swing the weather's density flattens.
    const SDaClimateSample& w = da_climate_now();
    const bool climate = da_climate_enabled() && !qa;
    const float t_mean = k.mean_temperature(D);
    const float range = std::max(k.t_range.at(D), 0.f);
    s_rt.t_day = qa ? t_mean : w.t_day;
    float t_air = climate ? w.t_air :
                            s_rt.t_day + 0.5f * range * (1.f - 0.6f * density) * std::cos(PI_MUL_2 * (hour - 15.f) / 24.f);

    // ---- Phase: drawn once per six-hour block of game time (QA: every half second of wall time) ------
    const s64 block = qa ? s64(std::floor(Device.fTimeGlobal / 0.5f)) : s64(std::floor(game_days * 4.0));
    const bool first_tick = s_rt.block == std::numeric_limits<s64>::min();
    if (block != s_rt.block)
    {
        s_rt.block = block;
        const u32 b = u32(u64(block) & 0xffffffffu) ^ u32(u64(block) >> 32);
        if (!qa)
        {
            // The six-hour block's draw of the climate (the same one the snowpack's history took).
            s_rt.phase = w.phase;
            s_rt.glaze = w.glaze;
        }
        else
        {
            if (k.snow_only(D))
                s_rt.phase = phase_snow;
            else
            {
                // No weather noise in a sweep: the climatological shares of the day itself.
                const float ps = std::max(k.p_snow.at(D), 0.f), pm = std::max(k.p_mixed.at(D), 0.f),
                            pr = std::max(k.p_rain.at(D), 0.f);
                const float sum = std::max(ps + pm + pr, 1e-4f);
                const float u = hash01(b * 0x9E3779B1u ^ 0x6A09E667u);
                s_rt.phase = u < ps / sum ? phase_snow : u < (ps + pm) / sum ? phase_sleet : phase_rain;
            }
            const float glaze_p = clampr(k.glaze.at(D), 0.f, 1.f) / std::max(1.f - clampr(k.p_snow.at(D), 0.f, 1.f), 0.05f);
            s_rt.glaze = s_rt.phase != phase_snow && hash01(b * 0xC2B2AE35u ^ 0x61A2u) < glaze_p;
        }
        s_rt.thunder = hash01(b * 0x85EBCA6Bu ^ 0x7417u) < clampr(k.thunder.at(D), 0.f, 1.f);
    }
    // The phase follows the climate's draw as its blocks turn over between two ticks of ours.
    if (!qa && s_rt.phase != w.phase)
    {
        s_rt.phase = w.phase;
        s_rt.glaze = w.glaze;
    }

    const int phase = s_qa_phase >= 0 ? s_qa_phase : s_rt.phase;
    const bool glaze = s_qa_phase < 0 && s_rt.glaze;

    // What falls keeps the air consistent with it while it falls: no snow at +8 C, no rain at -6 C.
    if (precipitating)
    {
        if (glaze)
            t_air = std::min(t_air, -0.5f);
        else if (phase == phase_snow)
            t_air = std::min(t_air, 0.5f);
        else if (phase == phase_rain)
            t_air = std::max(t_air, 0.5f);
        else
            t_air = clampr(t_air, -1.f, 2.5f);
    }
    if (s_qa_air < flt_max)
        t_air = s_qa_air;

    // Liquid share eases over a few seconds: the sheet changes from streaks to flakes as items are reborn.
    const float liquid_target = phase == phase_rain ? 1.f : phase == phase_sleet ? 0.5f : 0.f;
    if (qa || first_tick)
        s_precip.liquid = liquid_target;
    else
        s_precip.liquid += (liquid_target - s_precip.liquid) * (1.f - std::exp(-dt / 6.f));
    if (std::abs(s_precip.liquid - liquid_target) < 1e-3f)
        s_precip.liquid = liquid_target;

    const float d15 = density * std::sqrt(density);
    s_precip.snow_rate = precipitating ? clampr(d15 * (1.f - s_precip.liquid), 0.f, 1.f) : 0.f;
    s_precip.air_temp = t_air;
    // The climate's lightning comes with its thunder sky; the cycles' own (the climate off) by the six-hour draw.
    s_precip.thunder_ok = climate ? w.thunder && phase == phase_rain : s_rt.thunder && phase == phase_rain;
    s_rt.precipitating = precipitating;

    // ---- Frost and dew --------------------------------------------------------------------------------
    const float night = night_factor(hour);
    float frost_target, dew_target;
    if (qa)
    {
        frost_target = clampr(k.frost_night.at(D), 0.f, 1.f) * night;
        dew_target = (1.f - clampr(k.frost_night.at(D), 0.f, 1.f)) * night * (t_mean > 2.f ? 1.f : 0.f);
    }
    else
    {
        // Hoarfrost needs a freeze and no rain washing it off; dew needs a dry night above freezing.
        frost_target = clampr((0.5f - t_air) / 3.f, 0.f, 1.f) * (1.f - density * s_precip.liquid);
        dew_target = night * clampr(t_air / 2.f, 0.f, 1.f) * (1.f - clampr((0.5f - t_air) / 3.f, 0.f, 1.f)) * (1.f - density);
    }
    if (qa)
    {
        s_precip.frost = frost_target;
        s_precip.dew = dew_target;
    }
    else
    {
        // Frost forms over ~1.5 game hours and goes in ~45 min of sun; dew in about an hour either way.
        const float hours = float(dt_game_s) / 3600.f;
        const float k_frost = 1.f - std::exp(-hours / (frost_target > s_precip.frost ? 1.5f : 0.75f));
        const float k_dew = 1.f - std::exp(-hours / 1.f);
        s_precip.frost += (frost_target - s_precip.frost) * k_frost;
        s_precip.dew += (dew_target - s_precip.dew) * k_dew;
    }
    s_precip.frost = clampr(s_precip.frost, 0.f, 1.f);
    s_precip.dew = clampr(s_precip.dew, 0.f, 1.f);

    // ---- Fresh snow and the snow surface between the keys -------------------------------------------
    s_rt.fresh_mh = s_precip.snow_rate * snow_we_full / rho_fresh;
    if (!qa)
    {
        const float days = float(dt_game_s / 86400.0);
        const float fresh_m = s_rt.fresh_mh * float(dt_game_s / 3600.0);
        // Every 2 cm of new snow makes the surface fresh again; otherwise it ages on the key's 7-day scale.
        if (fresh_m > 0.f)
            s_rt.age_since = std::max(s_rt.age_since - fresh_m / 0.02f, 0.f);
        else
            s_rt.age_since = std::min(s_rt.age_since + days / 7.f, 1.f);
        s_rt.crust_evt = std::max(s_rt.crust_evt - days / 3.f, 0.f);
        if (precipitating && glaze)
            s_rt.crust_evt = 1.f;
        const float soak = precipitating && s_precip.liquid > 0.25f && t_air > 0.f ? density * s_precip.liquid : 0.f;
        s_rt.rain_on_snow = soak > s_rt.rain_on_snow ? soak :
                                                       std::max(s_rt.rain_on_snow - days * 2.f, soak);
    }
}

// ---- QA console -----------------------------------------------------------------------------------------

namespace
{
class CCC_QaPrecip final : public IConsole_Command
{
public:
    CCC_QaPrecip(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr args) override
    {
        if (!xr_strcmp(args, "auto"))
            s_qa_phase = -1;
        else if (!xr_strcmp(args, "snow"))
            s_qa_phase = phase_snow;
        else if (!xr_strcmp(args, "sleet"))
            s_qa_phase = phase_sleet;
        else if (!xr_strcmp(args, "rain"))
            s_qa_phase = phase_rain;
        else
        {
            InvalidSyntax();
            return;
        }
        Msg("[qa] precip %s", s_qa_phase < 0 ? "auto" : phase_name[s_qa_phase]);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "force what falls: auto | snow | sleet | rain (QA, never saved)"); }
    void fill_tips(vecTips& tips, u32) override
    {
        tips.emplace_back("auto");
        tips.emplace_back("snow");
        tips.emplace_back("sleet");
        tips.emplace_back("rain");
    }
};

class CCC_QaAirTemp final : public IConsole_Command
{
public:
    CCC_QaAirTemp(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr args) override
    {
        if (!xr_strcmp(args, "off"))
        {
            s_qa_air = flt_max;
            Msg("[qa] air_temp off");
            return;
        }
        float t;
        if (sscanf(args, "%f", &t) != 1 || !_valid(t) || t < -60.f || t > 60.f)
        {
            InvalidSyntax();
            return;
        }
        s_qa_air = t;
        Msg("[qa] air_temp %.1f C", t);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "pin the air temperature: <C> | off (QA, never saved)"); }
};

class CCC_QaSnowState final : public IConsole_Command
{
public:
    CCC_QaSnowState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        const SSnowFrame& f = s_frame;
        const SPrecipState& p = s_precip;
        const SSnowField* field = da_snow_field();
        Msg("[qa] snow_state day %.2f (prev %.2f)%s, active %d, mean %.3f m, grass_visible %.3f", da_season_day(),
            da_season_day_prev(), da_season_qa() ? " qa" : "", f.active ? 1 : 0, f.mean_depth, f.grass_visible);
        Msg("[qa] snow_state S %.3f %.3f %.3f %.3f | Mc %.3f %.3f %.3f %.3f | cover %.2f %.2f %.2f %.2f", f.S.x, f.S.y,
            f.S.z, f.S.w, f.Mc.x, f.Mc.y, f.Mc.z, f.Mc.w, f.cover.x, f.cover.y, f.cover.z, f.cover.w);
        Msg("[qa] snow_state state %.2f %.2f %.2f %.2f | age %.2f crust %.2f wet %.2f drift %.2f | dirt %.2f litter %.2f "
            "comp %.2f cap %.3f",
            f.state.x, f.state.y, f.state.z, f.state.w, f.surf.x, f.surf.y, f.surf.z, f.surf.w, f.dirt.x, f.dirt.y,
            f.dirt.z, f.dirt.w);
        Msg("[qa] snow_state fall %.4f m/h refill %.6f m/s | ice still %.2f flowing %.2f cover %.2f depth %.3f",
            f.fall.x, f.fall.y, f.ice.x, f.ice.y, f.ice.z, f.ice.w);
        Msg("[qa] snow_state precip %s (%s%s) liquid %.2f snow %.2f air %.1f C day %.1f C hour %.1f frost %.2f dew "
            "%.2f thunder %d",
            phase_name[s_qa_phase >= 0 ? s_qa_phase : s_rt.phase], s_qa_phase >= 0 ? "forced" : "drawn",
            s_rt.glaze ? ", glaze" : "", p.liquid, p.snow_rate, p.air_temp, s_rt.t_day, s_rt.hour, p.frost, p.dew,
            p.thunder_ok ? 1 : 0);
        if (field)
        {
            const Fvector& eye = Device.vCameraPosition;
            Msg("[qa] snow_state field %u x %u @ %.2f m, shares %.3f %.3f %.3f %.3f, stamps %u, depth here %.3f m, "
                "surface %.2f",
                field->dim_x, field->dim_z, field->texel, s_share[0], s_share[1], s_share[2], s_share[3],
                u32(s_stamps.size()), da_snow_depth_at(eye.x, eye.z), da_snow_surface_y(eye.x, eye.z));
        }
        else
            Msg("[qa] snow_state no snow field on this level");
    }
    void Info(TInfo& I) override { xr_strcpy(I, "print the snow and precipitation state of this frame (QA)"); }
};
} // namespace

void da_snow_register_console()
{
    static bool registered = false;
    if (registered || !Console)
        return;
    registered = true;
    static CCC_QaPrecip qa_precip("qa_precip");
    static CCC_QaAirTemp qa_air_temp("qa_air_temp");
    static CCC_QaSnowState qa_snow_state("qa_snow_state");
    Console->AddCommand(&qa_precip);
    Console->AddCommand(&qa_air_temp);
    Console->AddCommand(&qa_snow_state);
}
