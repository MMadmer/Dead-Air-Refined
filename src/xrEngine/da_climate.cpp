// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The regional climate (docs/dead-air/WEATHER.md). Five smooth random processes of game time - the temperature
// anomaly, the cloud, the wetness, the wind and the fog - are read through thresholds calibrated so that every
// month gets the share of clear, overcast, wet, foggy and stormy hours and the amount of precipitation the
// Chornobyl records give it ([weather_keys] of dead_air_x64_seasons.ltx). The snowpack and the ice integrate
// that same weather from five months back, so a load recomputes exactly what the weather left.

#include "stdafx.h"
#include "da_climate.h"
#include "da_season_keys.h"
#include "da_snow_field.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "Environment.h"
#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include <limits>

namespace
{
constexpr pcstr sky_names[da_sky_count] = {
    "clear_steady", "partly", "cloudy_no_sun", "foggy", "rain_steady", "storm", "thunder", "snow", "blizzard"};

// What a weather density of 1 drops, mm/h of water: a downpour of rain, a heavy snowfall's water equivalent.
constexpr float rain_mm_full = 25.f;
constexpr float snow_mm_full = 2.5f;
// How far back a load integrates the weather (game days): the ice of a February starts in October.
constexpr double history_days = 150.0;

Flags32 s_flags{};
enum : u32
{
    flag_weather = 1u << 0, // r__season_weather
    flag_daylight = 1u << 1 // r__season_daylight
};

// ---- Keys ----------------------------------------------------------------------------------------------------

struct SClimateKeys
{
    // Monthly climatology (keys on the 15th): shares of the hours.
    DaSeasonCurve p_clear, p_overcast, p_wet, precip_mm, p_fog, wind_ms, p_blizzard, p_storm, p_thunder,
        cloud_t_corr, fog_dawn, ddf;
    // Scales of the processes (game days) and their couplings.
    float tau_cloud = 1.3f, tau_wet = 0.28f, tau_wind = 0.45f, tau_fog = 0.6f;
    float wind_sigma = 0.55f;   // log-normal spread of the hourly wind about the month's mean
    float blizzard_ms = 9.f;    // the least wind of a blizzard or a rainstorm
    float fog_wind_max = 4.f;   // radiation and advection fog need a light wind
    float hysteresis = 0.15f;   // standard deviations a cloud class holds on past its threshold
    float thunder_day = 1.8f;   // afternoon weight of a thunderstorm against the night's 0.4
    // Snow.
    float rho_max_dry = 260.f, rho_max_wet = 450.f; // kg/m3 the pack settles towards
    float tau_dry = 240.f, tau_wet_h = 30.f;        // hours of that settling
    float fresh_tau = 30.f;                         // hours a fresh fall stays loose on top
    float drift_ms = 6.f;                           // wind that lifts dry loose snow
    // Ice (Stefan's law and a degree-day thaw).
    float ice_alpha = 0.024f;     // m per sqrt(C day) of frost on bare ice
    float ice_snow_ins = 3.f;     // 1/m: the snow on the ice slows its growth by 1 + this * depth
    float ice_melt = 0.006f;      // m per C day of thaw (under a low sun; the open sky's share adds)
    float water_tau_still = 4.f;  // days the standing water takes to follow the air
    float water_tau_flow = 9.f;   // and the running water
    float flow_growth = 0.8f;     // running water grows its ice this much slower
    float flow_melt = 1.3f;       // and loses it this much faster
    float ice_cover_still = 0.03f, ice_cover_flow = 0.12f; // m of ice that closes the water over
    float rot_full = 15.f;        // C days of thaw that halve what the ice carries
    // Precipitation: the rate's spread over the wetness above its threshold (mm/h = base * exp(shape * excess)),
    // and the phase: a precipitation hour runs this much colder than the day with this factor on its spread
    // (fitted to the hours of snow, sleet and rain at Kyiv and Gomel; [precip_keys] fit the days).
    float rate_shape = 1.5f;
    float phase_bias = -1.f, phase_sigma = 1.f;
};

void set_months(DaSeasonCurve& c, const float (&v)[12])
{
    c.clear();
    for (int i = 0; i < 12; ++i)
        c.add(da_season_key_day[i], v[i]);
}

// Built-in climatology: Chornobyl 33231 and Kyiv 33345 normals (docs/dead-air/WEATHER.md has the sources). The data
// file overrides any line.
void set_defaults(SClimateKeys& k)
{
    set_months(k.p_clear, {.15f, .17f, .17f, .17f, .18f, .17f, .20f, .25f, .22f, .15f, .08f, .08f});
    set_months(k.p_overcast, {.62f, .55f, .48f, .38f, .30f, .28f, .25f, .22f, .32f, .45f, .66f, .68f});
    set_months(k.p_wet, {.21f, .21f, .15f, .10f, .083f, .092f, .083f, .066f, .086f, .093f, .17f, .20f});
    set_months(k.precip_mm, {34.f, 37.f, 36.f, 40.f, 61.f, 73.f, 80.f, 55.f, 56.f, 42.f, 48.f, 43.f});
    set_months(k.p_fog, {.039f, .044f, .031f, .018f, .0087f, .0063f, .0057f, .01f, .023f, .044f, .059f, .057f});
    set_months(k.wind_ms, {2.9f, 2.9f, 3.f, 2.7f, 2.4f, 2.3f, 2.2f, 2.f, 2.2f, 2.3f, 2.6f, 2.7f});
    set_months(k.p_blizzard, {.10f, .12f, .08f, .03f, 0.f, 0.f, 0.f, 0.f, 0.f, .02f, .07f, .08f});
    set_months(k.p_storm, {.05f, .05f, .05f, .06f, .08f, .1f, .1f, .1f, .08f, .07f, .06f, .05f});
    set_months(k.p_thunder, {0.f, 0.f, .005f, .034f, .145f, .21f, .23f, .16f, .05f, .008f, 0.f, 0.f});
    set_months(k.cloud_t_corr, {.5f, .45f, .25f, 0.f, -.2f, -.3f, -.35f, -.3f, -.15f, .15f, .4f, .5f});
    set_months(k.fog_dawn, {.3f, .35f, .6f, .8f, 1.f, 1.f, 1.f, 1.f, .9f, .7f, .4f, .3f});
    set_months(k.ddf, {.7f, 1.f, 2.8f, 4.5f, 5.f, 5.f, 5.f, 5.f, 5.f, 4.f, 1.3f, .7f});
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
        Msg("! [climate] [%s] %s: no 'day:value' key, the built-in curve stays", sect, line);
        return false;
    }
    out = c;
    return true;
}

const SClimateKeys& keys()
{
    static SClimateKeys k;
    static bool loaded = false;
    if (loaded)
        return k;
    loaded = true;
    set_defaults(k);

    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_seasons.ltx");
    u32 read = 0;
    if (FS.exist(path))
    {
        const CInifile ini(path, TRUE);
        if (ini.section_exist("weather_keys"))
        {
            constexpr std::pair<pcstr, DaSeasonCurve SClimateKeys::*> curves[] = {{"p_clear", &SClimateKeys::p_clear},
                {"p_overcast", &SClimateKeys::p_overcast}, {"p_wet", &SClimateKeys::p_wet},
                {"precip_mm", &SClimateKeys::precip_mm}, {"p_fog", &SClimateKeys::p_fog},
                {"wind_ms", &SClimateKeys::wind_ms}, {"p_blizzard", &SClimateKeys::p_blizzard},
                {"p_storm", &SClimateKeys::p_storm}, {"p_thunder", &SClimateKeys::p_thunder},
                {"cloud_t_corr", &SClimateKeys::cloud_t_corr}, {"fog_dawn", &SClimateKeys::fog_dawn},
                {"ddf", &SClimateKeys::ddf}};
            for (const auto& [line, member] : curves)
                read += read_curve(ini, "weather_keys", line, k.*member);
        }
        if (ini.section_exist("climate"))
        {
            constexpr std::pair<pcstr, float SClimateKeys::*> floats[] = {{"tau_cloud", &SClimateKeys::tau_cloud},
                {"tau_wet", &SClimateKeys::tau_wet}, {"tau_wind", &SClimateKeys::tau_wind},
                {"tau_fog", &SClimateKeys::tau_fog}, {"wind_sigma", &SClimateKeys::wind_sigma},
                {"blizzard_ms", &SClimateKeys::blizzard_ms}, {"fog_wind_max", &SClimateKeys::fog_wind_max},
                {"hysteresis", &SClimateKeys::hysteresis},
                {"thunder_day", &SClimateKeys::thunder_day}, {"rho_max_dry", &SClimateKeys::rho_max_dry},
                {"rho_max_wet", &SClimateKeys::rho_max_wet}, {"tau_dry", &SClimateKeys::tau_dry},
                {"tau_wet_h", &SClimateKeys::tau_wet_h}, {"fresh_tau", &SClimateKeys::fresh_tau},
                {"drift_ms", &SClimateKeys::drift_ms}, {"ice_alpha", &SClimateKeys::ice_alpha},
                {"ice_snow_ins", &SClimateKeys::ice_snow_ins}, {"ice_melt", &SClimateKeys::ice_melt},
                {"water_tau_still", &SClimateKeys::water_tau_still}, {"water_tau_flow", &SClimateKeys::water_tau_flow},
                {"flow_growth", &SClimateKeys::flow_growth}, {"flow_melt", &SClimateKeys::flow_melt},
                {"ice_cover_still", &SClimateKeys::ice_cover_still}, {"ice_cover_flow", &SClimateKeys::ice_cover_flow},
                {"rot_full", &SClimateKeys::rot_full}, {"rate_shape", &SClimateKeys::rate_shape},
                {"phase_bias", &SClimateKeys::phase_bias}, {"phase_sigma", &SClimateKeys::phase_sigma}};
            for (const auto& [line, member] : floats)
                read_float(ini, "climate", line, k.*member);
        }
    }
    DaSeasonCurve* all[] = {&k.p_clear, &k.p_overcast, &k.p_wet, &k.precip_mm, &k.p_fog, &k.wind_ms, &k.p_blizzard,
        &k.p_storm, &k.p_thunder, &k.cloud_t_corr, &k.fog_dawn, &k.ddf};
    for (DaSeasonCurve* c : all)
        c->build();
    k.tau_cloud = std::max(k.tau_cloud, 0.05f);
    k.tau_wet = std::max(k.tau_wet, 0.02f);
    k.tau_wind = std::max(k.tau_wind, 0.02f);
    k.tau_fog = std::max(k.tau_fog, 0.02f);
    Msg("* [climate] %u weather curve(s) from dead_air_x64_seasons.ltx, the rest built in", read);
    return k;
}

// ---- Noise --------------------------------------------------------------------------------------------------

u32 hash_u32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(u32 x) { return float(hash_u32(x) >> 8) * (1.f / 16777216.f); }

// A standard normal value per lattice point (Box-Muller over two hashes).
float lattice_gauss(s64 i, u32 seed)
{
    const u32 a = hash_u32(u32(i) * 0x9E3779B1u ^ u32(u64(i) >> 32) ^ seed);
    const float u1 = std::max(float(a >> 8) * (1.f / 16777216.f), 1e-7f);
    const float u2 = hash01(a ^ 0x68E31DA4u);
    return std::sqrt(-2.f * std::log(u1)) * std::cos(PI_MUL_2 * u2);
}

float value_noise(double x, u32 seed)
{
    const double fl = std::floor(x);
    const s64 i = s64(fl);
    float t = float(x - fl);
    t = t * t * (3.f - 2.f * t);
    const float a = lattice_gauss(i, seed);
    return a + (lattice_gauss(i + 1, seed) - a) * t;
}

// Two octaves of value noise at unit variance on average over its argument: E over the smoothstep of
// (1 - s)^2 + s^2 is 26/35, and the second octave adds a quarter.
constexpr float noise_norm = 1.0377490f; // 1 / sqrt(26/35 * 1.25)
float process(double days, float tau, u32 seed_a, u32 seed_b)
{
    return (value_noise(days / tau, seed_a) + 0.5f * value_noise(days / (0.44 * tau), seed_b)) * noise_norm;
}

// The seed shifts the whole history along the lattice: another weather, the same statistics.
double seeded(double day, u32 seed) { return day + double(seed % 1000003u) * 731.0; }

// ---- Latent processes ---------------------------------------------------------------------------------------

struct SLatent
{
    float zt;   // temperature anomaly
    float zc;   // cloud
    float q;    // wetness (cloud and its own)
    float zw;   // wind (fronts are windy)
    float zf;   // fog propensity, without the hour
};

// The synoptic temperature anomaly is the one da_precip always had (the same scales and lattices): seed 0 keeps
// the temperatures of the earlier builds.
float synoptic(double days)
{
    return (value_noise(days / 2.5, 0x5A17C0DEu) + 0.5f * value_noise(days / 1.1, 0x33C1A5E7u)) * noise_norm;
}

SLatent latent(double day, float D, u32 seed)
{
    const SClimateKeys& k = keys();
    const double t = seeded(day, seed);
    SLatent l;
    l.zt = synoptic(t);
    const float c = clampr(k.cloud_t_corr.at(D), -0.9f, 0.9f);
    l.zc = c * l.zt + std::sqrt(1.f - c * c) * process(t, k.tau_cloud, 0xC10D0001u, 0xC10D0002u);
    l.q = 0.6f * l.zc + 0.8f * process(t, k.tau_wet, 0x3E770001u, 0x3E770002u);
    l.zw = 0.45f * l.q + 0.89f * process(t, k.tau_wind, 0x91AD0001u, 0x91AD0002u);
    l.zf = process(t, k.tau_fog, 0xF0660001u, 0xF0660002u);
    return l;
}

// Fog's hour: radiation fog forms before dawn and burns off by noon (in summer); a winter's fog lasts the day.
float fog_hour(float hour, float dawn)
{
    const float h = hour - 24.f * std::floor(hour / 24.f);
    const float morning = std::exp(-0.5f * (h - 6.f) * (h - 6.f) / (2.2f * 2.2f));
    const float afternoon = std::exp(-0.5f * (h - 15.f) * (h - 15.f) / (3.f * 3.f));
    return dawn * (1.1f * morning - 0.8f * afternoon);
}

float wind_ms_of(float zw, float mean, float sigma) { return mean * std::exp(sigma * zw - 0.5f * sigma * sigma); }

// ---- Calibration --------------------------------------------------------------------------------------------

// Per month: the thresholds that give its shares of hours, and the rate base (mm/h) that gives its precipitation.
struct SMonthCal
{
    float z_wet, gain, z_clear, z_overcast, z_fog, z_blizzard, z_storm;
};
SMonthCal s_cal[12];
bool s_cal_done = false;

float quantile(xr_vector<float>& v, float share_above)
{
    if (v.empty())
        return std::numeric_limits<float>::infinity();
    share_above = clampr(share_above, 0.f, 1.f);
    if (share_above <= 0.f)
        return std::numeric_limits<float>::infinity();
    if (share_above >= 1.f)
        return -std::numeric_limits<float>::infinity();
    const size_t i = std::min(size_t(float(v.size()) * (1.f - share_above)), v.size() - 1);
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

float liquid_of(int phase) { return phase == phase_rain ? 1.f : phase == phase_sleet ? 0.5f : 0.f; }

// The phase a sample would draw with the climate on, as a share of liquid: the expectation of block_phase's draw at
// that anomaly. (The blizzard and storm thresholds it splits only matter with the climate on, and the calibration runs
// once whatever the option says then.)
float expected_liquid(const SSnowKeys& s, float D, float zt)
{
    const SClimateKeys& k = keys();
    const float t = s.mean_temperature(D) + k.phase_bias + k.phase_sigma * std::max(s.t_sigma.at(D), 0.f) * zt;
    const float p_snow = 1.f / (1.f + std::exp((t - s.ts) / s.tw));
    const float p_rain = 1.f / (1.f + std::exp(-(t - s.tl) / s.tw));
    const float p_mixed = std::max(1.f - p_snow - p_rain, 0.f);
    return p_rain + 0.5f * p_mixed;
}

void calibrate()
{
    if (s_cal_done)
        return;
    s_cal_done = true;
    const SClimateKeys& k = keys();
    const SSnowKeys& s = da_snow_keys();
    constexpr int n = 4000;
    xr_vector<SLatent> lat(n);
    xr_vector<float> hours(n);
    xr_vector<float> v;
    v.reserve(n);
    for (int m = 0; m < 12; ++m)
    {
        const float D = da_season_key_day[m];
        for (int i = 0; i < n; ++i)
        {
            // Far apart against every scale, never on a lattice point; the hours run round the clock.
            const double day = 2000.0 + double(i) * 1.6180339 + double(m) * 9973.0;
            lat[i] = latent(day, D, 0);
            hours[i] = float(std::fmod(day, 1.0) * 24.0);
        }
        SMonthCal& c = s_cal[m];
        const float p_wet = clampr(k.p_wet.at(D), 0.f, 0.9f);
        v.clear();
        for (const SLatent& l : lat)
            v.push_back(l.q);
        c.z_wet = quantile(v, p_wet);

        // The rate's base: the month's precipitation over its wet hours (mm/h = base * exp(shape * excess)).
        double sum = 0.0;
        for (const SLatent& l : lat)
            if (l.q > c.z_wet)
                sum += std::exp(double(k.rate_shape) * double(l.q - c.z_wet));
        c.gain = sum > 0.0 ? float(double(std::max(k.precip_mm.at(D), 0.f)) / (sum * 730.0 / double(n))) : 0.f;

        // Fog among the dry, calm hours.
        const float wind_mean = std::max(k.wind_ms.at(D), 0.5f);
        const float dawn = k.fog_dawn.at(D);
        v.clear();
        for (int i = 0; i < n; ++i)
            if (lat[i].q <= c.z_wet && wind_ms_of(lat[i].zw, wind_mean, k.wind_sigma) < k.fog_wind_max)
                v.push_back(lat[i].zf + fog_hour(hours[i], dawn));
        const float fog_share = clampr(k.p_fog.at(D), 0.f, 0.5f) * float(n) / float(std::max<size_t>(v.size(), 1));
        c.z_fog = quantile(v, fog_share);

        // Cloud classes among the dry hours without fog; the wet hours are overcast, a fog hides the sky.
        u32 dry = 0, fog = 0;
        v.clear();
        for (int i = 0; i < n; ++i)
        {
            if (lat[i].q > c.z_wet)
                continue;
            if (wind_ms_of(lat[i].zw, wind_mean, k.wind_sigma) < k.fog_wind_max &&
                lat[i].zf + fog_hour(hours[i], dawn) > c.z_fog)
            {
                ++fog;
                continue;
            }
            ++dry;
            v.push_back(lat[i].zc);
        }
        const float wet_n = p_wet * float(n);
        const float over_share = (clampr(k.p_overcast.at(D), 0.f, 1.f) * float(n) - wet_n - 0.5f * float(fog)) /
            float(std::max(dry, 1u));
        const float clear_share = (clampr(k.p_clear.at(D), 0.f, 1.f) * float(n) - 0.2f * float(fog)) /
            float(std::max(dry, 1u));
        xr_vector<float> below = v;
        c.z_overcast = quantile(v, clampr(over_share, 0.02f, 0.98f));
        c.z_clear = quantile(below, 1.f - clampr(clear_share, 0.01f, 0.98f));
        c.z_clear = std::min(c.z_clear, c.z_overcast - 0.05f);

        // Blizzard among the snowy wet hours, rainstorm among the rainy ones: the windiest of them.
        xr_vector<float> snowy, rainy;
        for (const SLatent& l : lat)
        {
            if (l.q <= c.z_wet)
                continue;
            (expected_liquid(s, D, l.zt) < 0.5f ? snowy : rainy).push_back(l.zw);
        }
        c.z_blizzard = quantile(snowy, clampr(k.p_blizzard.at(D), 0.f, 1.f));
        c.z_storm = quantile(rainy, clampr(k.p_storm.at(D), 0.f, 1.f));
    }
    constexpr pcstr months[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    for (int m = 0; m < 12; ++m)
        Msg("* [climate] %s: wet z %.2f rate %.3f mm/h, clear z %.2f, overcast z %.2f, fog z %.2f, blizzard z %.2f, storm "
            "z %.2f",
            months[m], s_cal[m].z_wet, s_cal[m].gain, s_cal[m].z_clear, s_cal[m].z_overcast, s_cal[m].z_fog,
            s_cal[m].z_blizzard, s_cal[m].z_storm);
}

// The thresholds at D: linear between the months' keys (an infinite one stays infinite).
SMonthCal cal_at(float D)
{
    calibrate();
    const float d = da_season_wrap(D);
    int m1 = 0;
    while (m1 < 12 && da_season_key_day[m1] <= d)
        ++m1;
    const int m0 = (m1 + 11) % 12;
    m1 %= 12;
    const float span = da_season_wrap(da_season_key_day[m1] - da_season_key_day[m0]);
    const float t = span > 0.f ? clampr(da_season_wrap(d - da_season_key_day[m0]) / span, 0.f, 1.f) : 0.f;
    const auto mix = [t](float a, float b) {
        if (!std::isfinite(a) || !std::isfinite(b))
            return t < 0.5f ? a : b;
        return a + (b - a) * t;
    };
    const SMonthCal& a = s_cal[m0];
    const SMonthCal& b = s_cal[m1];
    return {mix(a.z_wet, b.z_wet), mix(a.gain, b.gain), mix(a.z_clear, b.z_clear), mix(a.z_overcast, b.z_overcast),
        mix(a.z_fog, b.z_fog), mix(a.z_blizzard, b.z_blizzard), mix(a.z_storm, b.z_storm)};
}

// ---- The weather of an hour block ----------------------------------------------------------------------------

u32 s_seed = 0;

// QA: a sky, an intensity and an air temperature forced on the live weather (never saved, never in the history).
int s_qa_sky = -1;
float s_qa_precip = -1.f;
float s_qa_temp = std::numeric_limits<float>::quiet_NaN();

// What a forced sky drops: snow from snow and blizzard skies, rain from the rain skies, else the hour's own phase.
int qa_forced_phase(int phase)
{
    if (s_qa_sky == da_sky_snow || s_qa_sky == da_sky_blizzard)
        return phase_snow;
    if (s_qa_sky == da_sky_rain || s_qa_sky == da_sky_storm || s_qa_sky == da_sky_thunder)
        return phase_rain;
    return phase;
}

double hour_mid(s64 b) { return (double(b) + 0.5) / 24.0; }

bool wet_block(s64 b, float D, const SMonthCal& c) { return latent(hour_mid(b), D, s_seed).q > c.z_wet; }

// The phase of a six-hour block, as da_precip always drew it.
int block_phase(s64 block6, float D, bool& thunder6, bool& glaze)
{
    const SSnowKeys& s = da_snow_keys();
    const u32 b = u32(u64(block6) & 0xffffffffu) ^ u32(u64(block6) >> 32) ^ hash_u32(s_seed);
    int phase;
    // The season keys' snow-only window holds while the climate is off; the climate's own winters thaw and rain
    // now and then, as the Chornobyl winters do.
    if (!da_climate_enabled() && s.snow_only(D))
        phase = phase_snow;
    else
    {
        const float sigma = std::max(s.t_sigma.at(D), 0.f);
        const bool climate = da_climate_enabled();
        const float bias = climate ? keys().phase_bias : s.t_precip_bias;
        const float spread = climate ? keys().phase_sigma : s.t_precip_sigma;
        const float t_phase = s.mean_temperature(D) + bias + spread * sigma * synoptic(seeded(double(block6) * 0.25, s_seed));
        const float p_snow = 1.f / (1.f + std::exp((t_phase - s.ts) / s.tw));
        const float p_rain = 1.f / (1.f + std::exp(-(t_phase - s.tl) / s.tw));
        const float p_mixed = std::max(1.f - p_snow - p_rain, 0.f);
        const float u = hash01(b * 0x9E3779B1u ^ 0x6A09E667u);
        phase = u < p_snow ? phase_snow : u < p_snow + p_mixed ? phase_sleet : phase_rain;
    }
    thunder6 = hash01(b * 0x85EBCA6Bu ^ 0x7417u) < clampr(s.thunder.at(D), 0.f, 1.f);
    const float glaze_p = clampr(s.glaze.at(D), 0.f, 1.f) / std::max(1.f - clampr(s.p_snow.at(D), 0.f, 1.f), 0.05f);
    glaze = phase != phase_snow && hash01(b * 0xC2B2AE35u ^ 0x61A2u) < glaze_p;
    return phase;
}

enum ECloud : int
{
    cloud_clear = 0,
    cloud_partly,
    cloud_overcast
};

ECloud cloud_class(float zc, const SMonthCal& c, int prev, float h)
{
    if (prev == cloud_clear && zc < c.z_clear + h)
        return cloud_clear;
    if (prev == cloud_overcast && zc > c.z_overcast - h)
        return cloud_overcast;
    if (zc < c.z_clear - (prev < 0 ? 0.f : h))
        return cloud_clear;
    if (zc > c.z_overcast + (prev < 0 ? 0.f : h))
        return cloud_overcast;
    return cloud_partly;
}

struct SBlock
{
    s64 b = std::numeric_limits<s64>::min();
    u32 seed = 0;
    float D = -1.f;
    u8 sky = da_sky_clear;
    bool wet = false, wet_prev = false, wet_next = false, thunder = false, glaze = false;
    int phase = phase_rain;
    float fog = 0.f;
};

// The sky of hour block b: wet blocks by what falls, then fog, then the cloud class with hysteresis (followed from
// half a day back, so it is still a function of the block alone).
SBlock sky_block(s64 b, float D, bool hysteresis = true)
{
    const SClimateKeys& k = keys();
    const SMonthCal c = cal_at(D);
    SBlock r;
    r.b = b;
    r.seed = s_seed;
    r.D = D;
    const double mid = hour_mid(b);
    const SLatent l = latent(mid, D, s_seed);
    r.wet = l.q > c.z_wet;
    r.wet_prev = wet_block(b - 1, D, c);
    r.wet_next = wet_block(b + 1, D, c);
    bool thunder6; // da_precip's six-hour lightning draw; the climate draws its own by the hour
    r.phase = block_phase(s64(std::floor(mid * 4.0)), D, thunder6, r.glaze);
    const float hour = float(std::fmod(mid, 1.0) * 24.0);
    if (r.wet)
    {
        if (r.phase == phase_snow)
            r.sky = l.zw > c.z_blizzard ? da_sky_blizzard : da_sky_snow;
        else
        {
            // Thunder by two-hour episodes, weighted to the afternoon's convection (the weights average to one).
            const float conv = (hour >= 12.f && hour < 21.f ? k.thunder_day : 0.4f) * 24.f / (9.f * k.thunder_day + 15.f * 0.4f);
            const float p = clampr(k.p_thunder.at(D) * conv, 0.f, 1.f);
            const s64 pair = s64(std::floor(double(b) * 0.5));
            const u32 hb = hash_u32(u32(u64(pair) & 0xffffffffu) ^ hash_u32(s_seed ^ 0x7D3Au));
            r.thunder = r.phase == phase_rain && hash01(hb) < p;
            r.sky = r.thunder ? da_sky_thunder : l.zw > c.z_storm ? da_sky_storm : da_sky_rain;
        }
        return r;
    }
    const float wind = wind_ms_of(l.zw, std::max(k.wind_ms.at(D), 0.5f), k.wind_sigma);
    const float zf = l.zf + fog_hour(hour, k.fog_dawn.at(D));
    if (wind < k.fog_wind_max && zf > c.z_fog)
    {
        r.sky = da_sky_foggy;
        r.fog = clampr(0.35f + (zf - c.z_fog) * 0.8f, 0.f, 1.f);
        return r;
    }
    int cls = -1;
    for (s64 i = hysteresis ? b - 12 : b; i <= b; ++i)
        cls = cloud_class(i == b ? l.zc : latent(hour_mid(i), D, s_seed).zc, c, cls, k.hysteresis);
    r.sky = cls == cloud_clear ? da_sky_clear : cls == cloud_partly ? da_sky_partly : da_sky_overcast;
    return r;
}

SBlock s_block_cache;
const SBlock& block_at(s64 b, float D)
{
    // A block keeps its answer within a tenth of a season day (the calendar creeps, a glide sweeps).
    if (s_block_cache.b != b || s_block_cache.seed != s_seed || std::abs(s_block_cache.D - D) > 0.1f)
        s_block_cache = sky_block(b, D);
    return s_block_cache;
}

float smooth01(float x)
{
    x = clampr(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

void sample(double day, float D, SDaClimateSample& out, const SBlock& blk)
{
    const SClimateKeys& k = keys();
    const SSnowKeys& s = da_snow_keys();
    const SMonthCal c = cal_at(D);
    const SLatent l = latent(day, D, s_seed);

    out.sky = blk.sky;
    out.phase = blk.phase;
    out.glaze = blk.glaze;
    out.thunder = blk.sky == da_sky_thunder;
    out.fog = blk.fog;
    out.cloud = blk.wet || blk.sky == da_sky_overcast ? 1.f :
        blk.sky == da_sky_foggy                            ? 0.9f :
        blk.sky == da_sky_partly                           ? 0.45f :
                                                             0.1f;

    // Rain or snow starts and stops softly at the edge of a wet block next to a dry one.
    float precip = 0.f;
    if (blk.wet)
    {
        // The rate (mm/h of water) and the weather density that drops it in this phase (25 mm/h of rain or 2.5 of
        // snow at density 1, density^1.5 between).
        const float rate = c.gain * std::exp(k.rate_shape * std::max(l.q - c.z_wet, 0.f));
        const float liq = liquid_of(blk.phase);
        const float full = liq * rain_mm_full + (1.f - liq) * snow_mm_full;
        precip = clampr(std::pow(rate / full, 2.f / 3.f), 0.02f, 1.f);
        const float f = float(day * 24.0 - std::floor(day * 24.0));
        if (!blk.wet_prev)
            precip *= smooth01(f / 0.35f);
        if (!blk.wet_next)
            precip *= smooth01((1.f - f) / 0.35f);
    }
    out.precip = precip;

    const float mean = std::max(k.wind_ms.at(D), 0.5f);
    float wind = wind_ms_of(l.zw, mean, k.wind_sigma);
    if (blk.sky == da_sky_blizzard || blk.sky == da_sky_storm)
        wind = std::max(wind, k.blizzard_ms + 4.f * std::max(l.zw - (blk.sky == da_sky_blizzard ? c.z_blizzard : c.z_storm), 0.f));
    out.wind_ms = wind;

    const float sigma = std::max(s.t_sigma.at(D), 0.f);
    const float range = std::max(s.t_range.at(D), 0.f);
    out.t_day = s.mean_temperature(D) + sigma * l.zt;
    const float hour = float(std::fmod(day, 1.0) * 24.0);
    // Cloud and rain flatten the day's swing; a clear sky widens it.
    const float swing = clampr(1.25f - 0.6f * out.cloud - 0.3f * precip, 0.2f, 1.3f);
    float t = out.t_day + 0.5f * range * swing * std::cos(PI_MUL_2 * (hour - 15.f) / 24.f);
    // What falls keeps the air consistent with it while it falls: no snow at +8 C, no rain at -6 C.
    if (precip > 0.f)
    {
        if (blk.glaze)
            t = std::min(t, -0.5f);
        else if (blk.phase == phase_snow)
            t = std::min(t, 0.5f);
        else if (blk.phase == phase_rain)
            t = std::max(t, 0.5f);
        else
            t = clampr(t, -1.f, 2.5f);
    }
    out.t_air = t;
}

// ---- Snowpack and ice ---------------------------------------------------------------------------------------

SDaClimateState s_state;
SDaClimateSample s_now;
double s_state_day = -1.0;  // the game day the state is integrated to
float s_state_D = -1.f;     // the season day the integration ran for
u32 s_state_seed = 0;
bool s_state_valid = false;

// One step of the snowpack and the ice over `hours` at the weather `w`.
void step(SDaClimateState& st, const SDaClimateSample& w, float D, float hours)
{
    const SClimateKeys& k = keys();
    // The water that falls (sample's density inverted at the phase's full rate), shared by the phase: sleet is half snow.
    const float liq = liquid_of(w.phase);
    const float water = w.precip * std::sqrt(w.precip) * (liq * rain_mm_full + (1.f - liq) * snow_mm_full) * hours;
    const float snow_mm = (1.f - liq) * water;
    const float rain_mm = liq * water;
    const float T = w.t_air;

    // Fresh snow: lighter the colder (Hedstrom and Pomeroy), packed by a strong wind.
    float rho_new = clampr(67.9f + 51.3f * std::exp(T / 2.6f), 50.f, 200.f);
    if (w.wind_ms > k.drift_ms)
        rho_new = std::min(rho_new + 12.f * (w.wind_ms - k.drift_ms), 300.f);

    // Melt: degree hours (the sun counts when the sky is open) and the warmth of the rain.
    const float ddf = std::max(k.ddf.at(D), 0.f) * (0.6f + 0.8f * (1.f - w.cloud));
    // Degree days on the day's mean (a cold pack does not melt in an afternoon's hour over zero); the rain's warmth by
    // the hour.
    float melt = std::max(w.t_day, 0.f) * ddf * hours / 24.f + rain_mm * std::max(T, 0.f) * 0.0126f;

    const float old_depth = st.swe > 0.f ? st.swe / st.rho : 0.f;
    const float swe = st.swe + snow_mm;
    melt = std::min(melt, swe);
    st.melt = hours > 0.f ? melt / hours : 0.f;
    // A thaw's patches lie under what falls on them: about 5 mm of water (5 cm of new snow) covers them over.
    st.peak = swe + std::max(std::max(st.peak, st.swe) - swe, 0.f) * std::exp(-snow_mm / 5.f);
    if (swe - melt <= 0.01f)
    {
        st.swe = 0.f;
        st.rho = 100.f;
        st.depth = 0.f;
        st.fresh = 0.f;
        st.peak = 0.f;
    }
    else
    {
        // The new snow joins at its own density; then the pack settles towards its limit.
        float depth = old_depth + snow_mm / rho_new;
        float rho = swe / std::max(depth, 1e-4f);
        const bool wet = T > 0.f || rain_mm > 0.f;
        const float cap = wet ? k.rho_max_wet : k.rho_max_dry;
        if (rho < cap)
            rho += (cap - rho) * (1.f - std::exp(-hours / (wet ? k.tau_wet_h : k.tau_dry)));
        st.swe = swe - melt;
        st.rho = clampr(rho, 50.f, 550.f);
        st.depth = st.swe / st.rho;
        // The loose top: what just fell, settling into the pack and melting first.
        float fresh = st.fresh + snow_mm / rho_new;
        fresh -= melt / std::max(rho_new, 80.f);
        fresh *= std::exp(-hours / k.fresh_tau);
        st.fresh = clampr(fresh, 0.f, st.depth);
    }

    // Blowing snow: a wind over loose dry snow lifts it.
    const float loose = st.depth > 0.005f ? clampr(st.fresh / 0.03f, 0.f, 1.f) * (T < -0.5f ? 1.f : 0.f) : 0.f;
    st.drift = clampr((w.wind_ms - k.drift_ms) / 6.f, 0.f, 1.f) * std::max(loose, w.sky == da_sky_blizzard ? 1.f : 0.f);

    // Water and ice, standing [0] and running [1].
    for (int i = 0; i < 2; ++i)
    {
        const float tau = (i ? k.water_tau_flow : k.water_tau_still) * 24.f;
        float& h = st.ice[i];
        float& tw = st.water_t[i];
        if (h <= 0.f)
        {
            // Open water follows the air; at freezing it skins over.
            const float target = T;
            tw += (target - tw) * (1.f - std::exp(-hours / tau));
            if (tw <= 0.f && T < 0.f)
            {
                tw = 0.f;
                h = 0.002f;
                st.ice_rot[i] = 0.f;
            }
            tw = std::max(tw, 0.f);
        }
        if (h > 0.f)
        {
            if (T < 0.f)
            {
                const float ins = 1.f + k.ice_snow_ins * st.ice_snow[i];
                const float a = k.ice_alpha * (i ? k.flow_growth : 1.f) / ins;
                const float fdd = -T * hours / 24.f;
                h = std::sqrt(h * h + a * a * fdd);
                // Hard frost heals the rot of a thaw.
                st.ice_rot[i] *= std::exp(-fdd / 10.f);
            }
            else
            {
                const float tdd = T * hours / 24.f;
                h -= k.ice_melt * (i ? k.flow_melt : 1.f) * tdd * (0.6f + 0.8f * (1.f - w.cloud));
                st.ice_rot[i] += tdd;
            }
            if (h <= 0.f)
            {
                h = 0.f;
                tw = 0.5f;
                st.ice_rot[i] = 0.f;
                st.ice_snow[i] = 0.f;
            }
        }
        // The ice closes the water over as it thickens: a skin at the banks first, the whole pond at a few cm; running
        // water keeps a lane over the current until its ice is thicker.
        st.ice_cover[i] = clampr(h / std::max(i ? k.ice_cover_flow : k.ice_cover_still, 1e-3f), 0.f, 1.f);
        // Snow on the ice: it holds once the ice carries it; the wind sweeps open ice.
        if (h > 0.04f)
        {
            float on = st.ice_snow[i] + snow_mm / rho_new - melt / std::max(st.rho, 100.f);
            if (w.wind_ms > k.drift_ms)
                on *= std::exp(-hours * 0.03f * (w.wind_ms - k.drift_ms));
            st.ice_snow[i] = clampr(on, 0.f, std::max(st.depth, 0.f) * 1.2f);
        }
        else
            st.ice_snow[i] = 0.f;
    }
}

// The season day `ago` game days before now, as the history sees it: the season moved with the game's days.
float history_D(float D, double ago) { return da_season_wrap(D - float(ago)); }

void integrate_history(double now_day, float D)
{
    SDaClimateState st;
    // October water: still warm. Two-hour steps over the last five months; the live frames take it from here.
    st.water_t[0] = st.water_t[1] = 10.f;
    constexpr float step_h = 2.f;
    const s64 steps = s64(history_days * 24.0 / step_h);
    for (s64 i = 0; i < steps; ++i)
    {
        const double t0 = now_day - history_days + double(i) * step_h / 24.0;
        const double mid = t0 + step_h / 48.0;
        const float Dm = history_D(D, now_day - mid);
        SDaClimateSample w;
        sample(mid, Dm, w, sky_block(s64(std::floor(mid * 24.0)), Dm, false));
        step(st, w, Dm, step_h);
    }
    s_state = st;
    s_state_day = now_day;
    s_state_D = D;
    s_state_seed = s_seed;
    s_state_valid = true;
}

double game_day_now()
{
    return g_pGameLevel ? double(g_pGameLevel->GetEnvironmentGameTime()) * 0.001 / 86400.0 :
                          double(Device.fTimeGlobal) / 86400.0;
}
} // namespace

pcstr da_sky_name(u8 sky) { return sky < da_sky_count ? sky_names[sky] : "?"; }

void da_climate_sample(double day, float D, SDaClimateSample& out)
{
    sample(day, D, out, block_at(s64(std::floor(day * 24.0)), D));
}

const SDaClimateState& da_climate_state() { return s_state; }
const SDaClimateSample& da_climate_now() { return s_now; }

bool da_climate_enabled() { return s_flags.test(flag_weather); }
bool da_climate_outdoors() { return da_climate_enabled() && da_snow_field(); }
bool da_season_daylight() { return s_flags.test(flag_daylight); }

namespace
{
bool s_sky_driven = false;
}
void da_climate_set_sky_driven(bool driven) { s_sky_driven = driven; }
bool da_climate_sky_driven() { return s_sky_driven && da_climate_enabled(); }

void da_climate_set_seed(u32 seed)
{
    if (seed == s_seed)
        return;
    s_seed = seed;
    s_block_cache = {};
    s_state_valid = false;
    Msg("* [climate] seed %u", seed);
}
u32 da_climate_seed() { return s_seed; }

pcstr da_climate_state_save()
{
    static xr_string text;
    text.clear();
    if (!s_state_valid)
        return text.c_str();
    const SDaClimateState& s = s_state;
    string512 buf;
    xr_sprintf(buf, "2 %.9f %.4f %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g", s_state_day,
        s_state_D, s.swe, s.rho, s.depth, s.fresh, s.water_t[0], s.water_t[1], s.ice[0], s.ice[1], s.ice_rot[0],
        s.ice_rot[1], s.ice_snow[0], s.ice_snow[1], s.ice_cover[0], s.ice_cover[1], s.peak);
    text = buf;
    return text.c_str();
}

bool da_climate_state_load(pcstr text)
{
    int version = 0;
    double day = 0.0;
    float D = 0.f;
    SDaClimateState s;
    // (Version 1, before the pack's peak was kept: the pack as it lies, nothing melted off it.)
    const int read = text ? sscanf(text, "%d %lf %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &version, &day, &D, &s.swe,
                                &s.rho, &s.depth, &s.fresh, &s.water_t[0], &s.water_t[1], &s.ice[0], &s.ice[1], &s.ice_rot[0],
                                &s.ice_rot[1], &s.ice_snow[0], &s.ice_snow[1], &s.ice_cover[0], &s.ice_cover[1], &s.peak) : 0;
    if (!((version == 1 && read == 17) || (version == 2 && read == 18)) || !std::isfinite(day) || !std::isfinite(s.swe) ||
        s.rho < 1.f)
    {
        Msg("! [climate] the saved snowpack and ice are unreadable: the history runs");
        return false;
    }
    if (version == 1)
        s.peak = s.swe;
    s.peak = std::max(s.peak, s.swe);
    s_state = s;
    s_state_day = day;
    s_state_D = D;
    s_state_seed = s_seed;
    s_state_valid = true;
    return true;
}

float da_climate_ice_thickness(float cover, bool flowing)
{
    // The front is the outer half of the cover (CEnvironment::ice_at under 0.5 is water): the ice thins to
    // nothing across it.
    return s_state.ice[flowing ? 1 : 0] * clampr(cover * 2.f - 1.f, 0.f, 1.f);
}

float da_climate_ice_strength(float cover, bool flowing)
{
    const int i = flowing ? 1 : 0;
    const float rot = clampr(s_state.ice_rot[i] / std::max(keys().rot_full, 0.1f), 0.f, 1.f);
    return da_climate_ice_thickness(cover, flowing) * (1.f - 0.5f * rot);
}

void da_climate_frame()
{
    const float D = da_season_day();
    const double now = game_day_now();
    if (!g_pGameLevel || !da_climate_enabled())
    {
        // The air and the phase still come from here (da_precip); the snowpack and the ice wait for the climate.
        s_state_valid = false;
        if (g_pGameLevel)
            da_climate_sample(now, D, s_now);
        return;
    }

    // A QA sweep drives the season day through the year: the snow and the ice show the season keys then
    // (da_snow_field), and the history waits; so does a glide of the season day between two choices.
    if (da_season_qa() || std::abs(da_season_wrap(D - da_season_day_prev() + 182.5f) - 182.5f) > 0.02f)
    {
        da_climate_sample(now, D, s_now);
        return;
    }
    // A load, a jump back, a new seed or a season moved by more than a day: the history again.
    const double since = now - s_state_day;
    if (!s_state_valid || s_state_seed != s_seed || since < -1e-6 || since > 3.0 ||
        std::abs(da_season_wrap(D - s_state_D + 182.5f) - 182.5f) > 1.f)
        integrate_history(now, D);
    else if (since > 0.0)
    {
        // Live: the same weather, in steps of at most an hour.
        double t = s_state_day;
        while (t < now)
        {
            const double dt = std::min(now - t, 1.0 / 24.0);
            SDaClimateSample w;
            da_climate_sample(t + dt * 0.5, D, w);
            // A QA force acts on the live integration too: a forced snowfall piles up, a forced frost grows ice.
            if (s_qa_sky >= 0)
                w.sky = u8(s_qa_sky);
            if (s_qa_sky == da_sky_blizzard || s_qa_sky == da_sky_storm)
                w.wind_ms = std::max(w.wind_ms, keys().blizzard_ms + 3.f);
            if (s_qa_precip >= 0.f)
            {
                w.precip = s_qa_precip;
                w.phase = qa_forced_phase(w.phase);
            }
            if (!std::isnan(s_qa_temp))
                w.t_air = w.t_day = s_qa_temp;
            step(s_state, w, D, float(dt * 24.0));
            t += dt;
        }
        s_state_day = now;
        // The calendar creeps on; the integration keeps pace without a rerun.
        s_state_D = D;
    }

    da_climate_sample(now, D, s_now);
    if (s_qa_sky >= 0)
        s_now.sky = u8(s_qa_sky);
    if (s_qa_sky == da_sky_blizzard || s_qa_sky == da_sky_storm)
        s_now.wind_ms = std::max(s_now.wind_ms, keys().blizzard_ms + 3.f);
    if (s_qa_precip >= 0.f)
    {
        s_now.precip = s_qa_precip;
        s_now.phase = qa_forced_phase(s_now.phase);
    }
    if (!std::isnan(s_qa_temp))
        s_now.t_air = s_qa_temp;
}

// ---- Sky clock and sun ---------------------------------------------------------------------------------------

namespace
{
// Chornobyl.
constexpr float site_lat = 51.27f, site_lon = 30.22f;
// The day the AtmosFear cycles were drawn for: the sun comes up at 6:15, goes down at 19:55 (their keys light the
// sun from 06:00-07:00 to 19:50); twilights of an hour and a half either side keep their own length.
constexpr float ref_rise = 6.25f, ref_set = 19.92f, twilight = 1.5f;

struct SSolar
{
    float decl;     // radians
    float eot_h;    // equation of time, hours
    float zone_h;   // civil time zone, hours east of UTC (Kyiv: +2, +3 in summer)
};

SSolar solar(float D)
{
    const float g = PI_MUL_2 / 365.f * D;
    SSolar s;
    s.decl = 0.006918f - 0.399912f * std::cos(g) + 0.070257f * std::sin(g) - 0.006758f * std::cos(2.f * g) +
        0.000907f * std::sin(2.f * g) - 0.002697f * std::cos(3.f * g) + 0.00148f * std::sin(3.f * g);
    s.eot_h = 229.18f / 60.f *
        (0.000075f + 0.001868f * std::cos(g) - 0.032077f * std::sin(g) - 0.014615f * std::cos(2.f * g) -
            0.040849f * std::sin(2.f * g));
    // Summer time from the last Sunday of March to the last Sunday of October (days 86..300 on average).
    s.zone_h = D >= 86.f && D < 300.f ? 3.f : 2.f;
    return s;
}

// Solar noon and the half day the sun is up (horizon with refraction), civil hours.
void sun_events(float D, float& noon, float& half)
{
    const SSolar s = solar(D);
    noon = 12.f + s.zone_h - site_lon / 15.f - s.eot_h;
    const float lat = deg2rad(site_lat);
    const float cosH = (std::sin(deg2rad(-0.83f)) - std::sin(lat) * std::sin(s.decl)) / (std::cos(lat) * std::cos(s.decl));
    half = rad2deg(std::acos(clampr(cosH, -1.f, 1.f))) / 15.f;
}
} // namespace

float da_sky_clock(float level_seconds, float D)
{
    if (!da_season_daylight())
        return level_seconds;
    float noon, half;
    sun_events(D, noon, half);
    const float rise = noon - half, set = noon + half;
    const float ref_noon = 0.5f * (ref_rise + ref_set);
    // Points of the real day and of the drawn one, from the dawn's start round to the next.
    const float real[6] = {rise - twilight, rise, noon, set, set + twilight, rise + 24.f - twilight};
    const float drawn[6] = {ref_rise - twilight, ref_rise, ref_noon, ref_set, ref_set + twilight, ref_rise + 24.f - twilight};
    float h = level_seconds / 3600.f;
    while (h < real[0])
        h += 24.f;
    while (h >= real[5])
        h -= 24.f;
    float out = h;
    for (int i = 0; i < 5; ++i)
        if (h < real[i + 1])
        {
            const float t = (h - real[i]) / std::max(real[i + 1] - real[i], 1e-3f);
            out = drawn[i] + (drawn[i + 1] - drawn[i]) * t;
            break;
        }
    out -= 24.f * std::floor(out / 24.f);
    return out * 3600.f;
}

SDaSun da_sun_at(float level_seconds, float D, float azimuth_offset)
{
    const SSolar s = solar(D);
    const float hour = level_seconds / 3600.f;
    // Hour angle: solar time from the civil clock.
    const float solar_h = hour - s.zone_h + site_lon / 15.f + s.eot_h;
    float sha = (solar_h - 12.f) * 15.f;
    if (sha > 180.f)
        sha -= 360.f;
    if (sha < -180.f)
        sha += 360.f;
    const float lat = deg2rad(site_lat);
    const float cos_sza =
        clampr(std::sin(lat) * std::sin(s.decl) + std::cos(lat) * std::cos(s.decl) * std::cos(deg2rad(sha)), -1.f, 1.f);
    const float sza = std::acos(cos_sza);
    SDaSun sun;
    sun.elevation = PI_DIV_2 - sza;
    // The azimuth in the convention of CEnvDescriptorMixer::calculate_dynamic_sun_dir (mirrored in the morning).
    float cos_az = 0.f;
    const float d = std::sin(sza) * std::cos(lat);
    if (!fis_zero(d))
        cos_az = (std::sin(s.decl) - std::sin(lat) * std::cos(sza)) / d;
    float az = std::acos(clampr(cos_az, -1.f, 1.f)) + azimuth_offset;
    if (sha < 0.f)
        az = PI_MUL_2 - az;
    sun.azimuth = az;
    return sun;
}

// ---- Console ----------------------------------------------------------------------------------------------------

namespace
{
// qa_rain_state: the rain and snow sheet as it stands (Rain.cpp).
class CCC_QaRainState final : public IConsole_Command
{
public:
    CCC_QaRainState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        if (g_pGamePersistent)
            g_pGamePersistent->Environment().QaRainReport();
    }
};

class CCC_QaClimateState final : public IConsole_Command
{
public:
    CCC_QaClimateState(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr) override
    {
        const SDaClimateSample& w = s_now;
        const SDaClimateState& st = s_state;
        const double now = game_day_now();
        const SMonthCal c = cal_at(da_season_day());
        Msg("[qa] climate weather %s, seed %u, day %.3f (game %.4f), sky %s%s, air %.1f C (day %.1f), cloud %.2f, precip "
            "%.3f %s%s, wind %.1f m/s, fog %.2f",
            da_climate_enabled() ? "on" : "off", s_seed, da_season_day(), now, da_sky_name(w.sky),
            da_climate_sky_driven() ? " (driven)" : "", w.t_air, w.t_day, w.cloud, w.precip, phase_name[w.phase],
            w.glaze ? ", glaze" : "", w.wind_ms, w.fog);
        Msg("[qa] climate pack swe %.1f mm (peak %.1f) rho %.0f depth %.3f m fresh %.3f m melt %.2f mm/h drift %.2f", st.swe,
            st.peak, st.rho, st.depth, st.fresh, st.melt, st.drift);
        Msg("[qa] climate ice still %.3f m (water %.1f C, rot %.1f, snow %.3f) running %.3f m (water %.1f C, rot %.1f, "
            "snow %.3f)",
            st.ice[0], st.water_t[0], st.ice_rot[0], st.ice_snow[0], st.ice[1], st.water_t[1], st.ice_rot[1],
            st.ice_snow[1]);
        Msg("[qa] climate thresholds wet %.2f rate %.3f mm/h clear %.2f overcast %.2f fog %.2f blizzard %.2f storm %.2f",
            c.z_wet, c.gain, c.z_clear, c.z_overcast, c.z_fog, c.z_blizzard, c.z_storm);
        // The next day of skies: what the weather manager will set.
        string512 line{};
        const s64 b0 = s64(std::floor(now * 24.0));
        for (s64 b = b0; b < b0 + 24; ++b)
        {
            const SBlock blk = sky_block(b, da_season_day());
            xr_strcat(line, da_sky_name(blk.sky));
            xr_strcat(line, b + 1 < b0 + 24 ? " " : "");
        }
        Msg("[qa] climate next 24 h: %s", line);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "print the climate of this moment: sky, air, snowpack, ice (QA)"); }
};

// qa_climate_force <sky|off> [precip 0..1] [air C]: forces the live weather (never saved, the history keeps the
// climate's own).
class CCC_QaClimateForce final : public IConsole_Command
{
public:
    CCC_QaClimateForce(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr args) override
    {
        string64 sky{};
        float precip = -1.f, air = std::numeric_limits<float>::quiet_NaN();
        const int n = sscanf(args, "%63s %f %f", sky, &precip, &air);
        if (n < 1)
        {
            InvalidSyntax();
            return;
        }
        if (!xr_stricmp(sky, "off"))
        {
            s_qa_sky = -1;
            s_qa_precip = -1.f;
            s_qa_temp = std::numeric_limits<float>::quiet_NaN();
            Msg("[qa] climate_force off");
            return;
        }
        int found = -1;
        for (int i = 0; i < da_sky_count; ++i)
            if (!xr_stricmp(sky, sky_names[i]))
                found = i;
        if (found < 0)
        {
            Msg("! qa_climate_force: unknown sky '%s'", sky);
            return;
        }
        s_qa_sky = found;
        s_qa_precip = n >= 2 ? clampr(precip, 0.f, 1.f) : -1.f;
        s_qa_temp = n >= 3 ? air : std::numeric_limits<float>::quiet_NaN();
        Msg("[qa] climate_force %s precip %.2f air %.1f", sky_names[found], s_qa_precip, s_qa_temp);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "qa_climate_force <sky|off> [precip 0..1] [air C] (QA, never saved)"); }
};

// qa_snowpack <depth m> [fresh m] | qa_ice <still m> [running m]: set the integrated state (QA; the next load
// recomputes it).
class CCC_QaSnowpack final : public IConsole_Command
{
public:
    CCC_QaSnowpack(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr args) override
    {
        float depth = 0.f, fresh = 0.f;
        if (sscanf(args, "%f %f", &depth, &fresh) < 1 || depth < 0.f || depth > 2.f)
        {
            InvalidSyntax();
            return;
        }
        s_state.rho = depth > 0.f ? std::max(s_state.rho, 150.f) : 100.f;
        s_state.depth = depth;
        s_state.swe = depth * s_state.rho;
        s_state.peak = s_state.swe;
        s_state.fresh = clampr(fresh, 0.f, depth);
        Msg("[qa] snowpack depth %.3f m fresh %.3f m (rho %.0f)", depth, s_state.fresh, s_state.rho);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "qa_snowpack <depth m> [fresh m] (QA)"); }
};

class CCC_QaIce final : public IConsole_Command
{
public:
    CCC_QaIce(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr args) override
    {
        float still = 0.f, running = -1.f;
        if (sscanf(args, "%f %f", &still, &running) < 1 || still < 0.f || still > 2.f)
        {
            InvalidSyntax();
            return;
        }
        s_state.ice[0] = still;
        s_state.ice[1] = running >= 0.f ? running : still;
        const SClimateKeys& k = keys();
        s_state.ice_cover[0] = clampr(s_state.ice[0] / std::max(k.ice_cover_still, 1e-3f), 0.f, 1.f);
        s_state.ice_cover[1] = clampr(s_state.ice[1] / std::max(k.ice_cover_flow, 1e-3f), 0.f, 1.f);
        s_state.ice_rot[0] = s_state.ice_rot[1] = 0.f;
        for (float& t : s_state.water_t)
            t = 0.f;
        Msg("[qa] ice still %.3f m running %.3f m", s_state.ice[0], s_state.ice[1]);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "qa_ice <still m> [running m] (QA)"); }
};
} // namespace

void da_climate_register_console()
{
    s_flags.set(flag_weather | flag_daylight, true);
    CMD3(CCC_Mask, "r__season_weather", &s_flags, flag_weather);
    CMD3(CCC_Mask, "r__season_daylight", &s_flags, flag_daylight);
    CMD1(CCC_QaClimateState, "qa_climate_state");
    CMD1(CCC_QaRainState, "qa_rain_state");
    CMD1(CCC_QaClimateForce, "qa_climate_force");
    CMD1(CCC_QaSnowpack, "qa_snowpack");
    CMD1(CCC_QaIce, "qa_ice");
}
