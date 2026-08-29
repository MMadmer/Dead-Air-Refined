#include "stdafx.h"
#pragma hdrstop

#ifndef _EDITOR
#include "Render.h"
#endif

#include "Environment.h"
#include "xr_efflensflare.h"
#include "Rain.h"
#include "thunderbolt.h"
#include "WindVegSound.h"
#include "xrHemisphere.h"
#include "perlin.h"

#ifndef _EDITOR
#include "IGame_Level.h"
#endif

#include "xrCore/xrCore.h"
#include "xrCommon/xr_hash_map.h"

#include "Include/xrRender/EnvironmentRender.h"
#include "Include/xrRender/LensFlareRender.h"
#include "Include/xrRender/RainRender.h"
#include "Include/xrRender/ThunderboltRender.h"

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
ENGINE_API float psVisDistance = 1.f;
static const float MAX_NOISE_FREQ = 0.03f;

//#define WEATHER_LOGGING

// real WEATHER->WFX transition time
#define WFX_TRANS_TIME 5.f

namespace
{
struct EnvironmentResourcePair
{
    CEnvDescriptor* first{};
    CEnvDescriptor* second{};
};

struct EnvironmentResourceState
{
    bool device_ready{};
    EnvironmentResourcePair active;
};

// Keep lazy GPU ownership outside the exported CEnvironment layout.
xr_flat_hash_map<CEnvironment*, EnvironmentResourceState> environment_resource_states;
std::atomic<float> environment_season{0.f};

bool contains(const EnvironmentResourcePair& pair, const CEnvDescriptor* descriptor)
{
    return pair.first == descriptor || pair.second == descriptor;
}

template <typename F>
void for_each_unique(const EnvironmentResourcePair& pair, F&& function)
{
    if (pair.first)
        function(pair.first);
    if (pair.second && pair.second != pair.first)
        function(pair.second);
}
} // namespace


//////////////////////////////////////////////////////////////////////////
// environment
CEnvironment::CEnvironment()
    : PerlinNoise1D(xr_new<CPerlinNoise1D>(Random.randI(0, 0xFFFF)))
{
    environment_season.store(0.f, std::memory_order_relaxed);
    OnDeviceCreate();

    fTimeFactor = 12.f;

    wind_blast_direction.set(1.f, 0.f, 0.f);

    // fill clouds hemi verts & faces
    const Fvector* verts;
    CloudsVerts.resize(xrHemisphereVertices(2, verts));
    CopyMemory(&CloudsVerts.front(), verts, CloudsVerts.size() * sizeof(Fvector));
    const u16* indices;
    CloudsIndices.resize(xrHemisphereIndices(2, indices));
    CopyMemory(&CloudsIndices.front(), indices, CloudsIndices.size() * sizeof(u16));

    // perlin noise
    PerlinNoise1D->SetOctaves(2);
    PerlinNoise1D->SetAmplitude(0.66666f);

    // tsky0 = Device.Resources->_CreateTexture("$user$sky0");
    // tsky1 = Device.Resources->_CreateTexture("$user$sky1");

    string_path filePath;
    const auto load_config = [&filePath](pcstr path) -> CInifile*
    {
        if (FS.update_path(filePath, "$game_config$", path, false))
            return xr_new<CInifile>(filePath, true, true, false);
        return nullptr;
    };

    m_ambients_config                = load_config("environment\\ambients.ltx");
    m_sound_channels_config          = load_config("environment\\sound_channels.ltx");
    m_effects_config                 = load_config("environment\\effects.ltx");
}

CEnvironment::~CEnvironment()
{
    xr_delete(PerlinNoise1D);
    OnDeviceDestroy();

    CInifile::Destroy(m_ambients_config);
    m_ambients_config = nullptr;

    CInifile::Destroy(m_sound_channels_config);
    m_sound_channels_config = nullptr;

    CInifile::Destroy(m_effects_config);
    m_effects_config = nullptr;
}

float CEnvironment::GetRainVolume() { return eff_Rain ? eff_Rain->GetVolume() : 0.f; }

float CEnvironment::GetWetness() const { return GetCurrentWetness(); }

float CEnvironment::GetSnowFactor() const { return GetCurrentSnowFactor(); }

float GetCurrentSeason() { return environment_season.load(std::memory_order_relaxed); }

float CEnvironment::GetSeason() const { return GetCurrentSeason(); }

void CEnvironment::SetSeason(float factor)
{
    R_ASSERT2(_valid(factor), "Invalid season factor");
    environment_season.store(factor, std::memory_order_relaxed);
}

void CEnvironment::SetCurrentEnvironmentPair(CEnvDescriptor* first, CEnvDescriptor* second)
{
    environment_detail::acquire_resources(*this, first, second);
    Current[0] = first;
    Current[1] = second;
    environment_detail::commit_resources(*this, first, second);
}

void CEnvironment::RefreshCurrentEnvironmentResources()
{
    R_ASSERT2((Current[0] && Current[1]) || (!Current[0] && !Current[1]),
        "Weather resource refresh requires a complete descriptor pair");
    if (!Current[0])
        return;

    CEnvDescriptor* first = Current[0];
    CEnvDescriptor* second = Current[1];
    SetCurrentEnvironmentPair(nullptr, nullptr);
    SetCurrentEnvironmentPair(first, second);
}

void environment_detail::acquire_resources(
    CEnvironment& environment, CEnvDescriptor* first, CEnvDescriptor* second)
{
    R_ASSERT2((first && second) || (!first && !second),
        "Weather resource transition requires either two descriptors or none");

    auto& state = environment_resource_states[&environment];
    if (!state.device_ready)
        return;

    const EnvironmentResourcePair next{ first, second };
    for_each_unique(next, [&state](CEnvDescriptor* descriptor)
    {
        if (contains(state.active, descriptor))
            return;

        R_ASSERT3(descriptor->m_pDescriptor, "Weather descriptor has no renderer", descriptor->m_identifier.c_str());
        descriptor->on_device_create();
    });
}

void environment_detail::commit_resources(
    CEnvironment& environment, CEnvDescriptor* first, CEnvDescriptor* second)
{
    R_ASSERT2((first && second) || (!first && !second),
        "Weather resource transition requires either two descriptors or none");
    R_ASSERT2(environment.Current[0] == first && environment.Current[1] == second,
        "Weather resource transition was committed before publishing the descriptor pair");

    auto& state = environment_resource_states[&environment];
    if (!state.device_ready)
        return;

    if (first && g_pGameLevel)
        environment.m_pRender->lerp(environment.CurrentEnv, &*first->m_pDescriptor, &*second->m_pDescriptor);
    else
        environment.m_pRender->Clear();

    const EnvironmentResourcePair previous = state.active;
    state.active = { first, second };
    for_each_unique(previous, [&state](CEnvDescriptor* descriptor)
    {
        if (!contains(state.active, descriptor))
            descriptor->on_device_destroy();
    });
}

void environment_detail::restore_resources(CEnvironment& environment)
{
    auto& state = environment_resource_states[&environment];
    R_ASSERT2(!state.device_ready, "Weather resources were restored without a matching device destroy");

    state.device_ready = !GEnv.isDedicatedServer;
    if (!state.device_ready)
        return;

    acquire_resources(environment, environment.Current[0], environment.Current[1]);
    commit_resources(environment, environment.Current[0], environment.Current[1]);
}

void environment_detail::release_resources(CEnvironment& environment)
{
    const auto it = environment_resource_states.find(&environment);
    if (it == environment_resource_states.end())
        return;

    EnvironmentResourceState& state = it->second;
    if (state.device_ready)
    {
        environment.m_pRender->Clear();
        for_each_unique(state.active, [](CEnvDescriptor* descriptor) { descriptor->on_device_destroy(); });
    }
    environment_resource_states.erase(it);
}

void CEnvironment::Invalidate()
{
    bWFX = false;
    Current[0] = nullptr;
    Current[1] = nullptr;
    if (eff_LensFlare)
        eff_LensFlare->Invalidate();

    CurrentEnv.env_ambient = nullptr; // hack
    CurrentEnv.lens_flare  = nullptr; // hack
    CurrentEnv.thunderbolt = nullptr; // hack

    environment_detail::commit_resources(*this, nullptr, nullptr);
}

float CEnvironment::TimeDiff(float prev, float cur)
{
    if (prev > cur)
        return (DAY_LENGTH - prev) + cur;
    else
        return cur - prev;
}

float CEnvironment::TimeWeight(float val, float min_t, float max_t)
{
    float weight = 0.f;
    float length = TimeDiff(min_t, max_t);
    if (!fis_zero(length, EPS))
    {
        if (min_t > max_t)
        {
            if ((val >= min_t) || (val <= max_t))
                weight = TimeDiff(min_t, val) / length;
        }
        else
        {
            if ((val >= min_t) && (val <= max_t))
                weight = TimeDiff(min_t, val) / length;
        }
        clamp(weight, 0.f, 1.f);
    }
    return weight;
}

void CEnvironment::ChangeGameTime(float game_time)
{
    fGameTime = NormalizeTime(fGameTime + game_time);
}

void CEnvironment::SetGameTime(float game_time, float time_factor)
{
    if (bWFX)
        wfx_time -= TimeDiff(fGameTime, game_time);
    fGameTime = game_time;
    fTimeFactor = time_factor;
}

void CEnvironment::SplitTime(float time, u32& hours, u32& minutes, u32& seconds) const
{
    u32 current_time_u32 = iFloor(time);
    current_time_u32 = current_time_u32 % (24 * 60 * 60);

    hours = current_time_u32 / (60 * 60);
    current_time_u32 %= (60 * 60);

    minutes = current_time_u32 / 60;
    seconds = current_time_u32 % 60;
}

float CEnvironment::NormalizeTime(float tm)
{
    R_ASSERT2(_valid(tm), "Invalid environment time");
    if (tm >= 0.f && tm <= DAY_LENGTH)
        return tm;

    tm = fmodf(tm, DAY_LENGTH);
    if (fis_zero(tm))
        return 0.f;
    return tm < 0.f ? tm + DAY_LENGTH : tm;
}

void CEnvironment::SetWeather(shared_str name, bool forced)
{
    if (name.size())
    {
        auto it = WeatherCycles.find(name);
        if (it == WeatherCycles.end())
        {
            Msg("! Invalid weather name: %s", name.c_str());
            return;
        }
        R_ASSERT3(it != WeatherCycles.end(), "Invalid weather name.", name.c_str());
        R_ASSERT3(!it->second.empty(), "Weather cycle has no descriptors", name.c_str());

        CEnvDescriptor* next_first = nullptr;
        CEnvDescriptor* next_second = nullptr;
        if (forced)
        {
            SelectEnvs(&it->second, next_first, next_second, fGameTime);
            environment_detail::acquire_resources(*this, next_first, next_second);
        }

        CurrentCycleName = it->first;
        if (forced)
        {
            bWFX = false;
            if (eff_LensFlare)
                eff_LensFlare->Invalidate();
            CurrentEnv.env_ambient = nullptr;
            CurrentEnv.lens_flare = nullptr;
            CurrentEnv.thunderbolt = nullptr;
        }

        if (!bWFX)
        {
            CurrentWeather = &it->second;
            CurrentWeatherName = it->first;
            CurrentEnv.soc_style = CurrentWeather->soc_style;
        }

        if (forced)
        {
            Current[0] = next_first;
            Current[1] = next_second;
            environment_detail::commit_resources(*this, next_first, next_second);
        }
#ifdef WEATHER_LOGGING
        Msg("Starting Cycle: %s [%s]", name.c_str(), forced ? "forced" : "deferred");
#endif
    }
    else
    {
#ifndef _EDITOR
        FATAL("! Empty weather name");
#endif
    }
}

bool CEnvironment::SetWeatherFX(shared_str name)
{
    // A refusal here used to be silent, and the callers ignore the result: a surge whose
    // fx_blowout was refused runs to completion with a clear sky and 10x game time - the
    // "racing moon, no blowout" reports. Name every refusal so the session log carries the cause.
    if (bWFX)
    {
        Msg("! SetWeatherFX [%s]: refused, effect [%s] is already playing", name.c_str() ? name.c_str() : "",
            Current[0] ? Current[0]->m_identifier.c_str() : "");
        return false;
    }
    if (name.size())
    {
        auto it = WeatherFXs.find(name);
        R_ASSERT3(it != WeatherFXs.end(), "Invalid weather effect name.", name.c_str());
        // Reachable, not impossible: a save made mid-surge resumes its manager before the first
        // weather pair is selected. Refuse instead of asserting; the caller already handles false.
        if (!CurrentWeather || !Current[0] || !Current[1])
        {
            Msg("! SetWeatherFX [%s]: refused, the base weather pair is not selected yet", name.c_str());
            return false;
        }

        EnvVec* previous_weather = CurrentWeather;
        EnvVec* next_weather = &it->second;
        R_ASSERT3(next_weather->size() >= 3, "Weather effect requires a transition, body, and terminator", name.c_str());

        const float rewind_tm = WFX_TRANS_TIME * fTimeFactor;
        const float start_tm = fGameTime + rewind_tm;
        const float current_length = TimeDiff(Current[0]->exec_time, Current[1]->exec_time);
        const float time_to_next = TimeDiff(fGameTime, Current[1]->exec_time);

        std::sort(next_weather->begin(), next_weather->end(), sort_env_etl_pred);
        CEnvDescriptor* C0 = next_weather->at(0);
        CEnvDescriptor* C1 = next_weather->at(1);
        CEnvDescriptor* CE = next_weather->at(next_weather->size() - 2);
        CEnvDescriptor* CT = next_weather->at(next_weather->size() - 1);
        if (fis_zero(time_to_next, EPS))
        {
            C0->copy(*Current[1]);
            C0->exec_time = NormalizeTime(fGameTime);
        }
        else
        {
            C0->copy(*Current[0]);
            C0->exec_time = NormalizeTime(fGameTime - ((rewind_tm / time_to_next) * current_length - rewind_tm));
        }
        C1->copy(*Current[1]);
        C1->exec_time = NormalizeTime(start_tm);
        for (auto t_it = next_weather->begin() + 2; t_it != next_weather->end() - 1; ++t_it)
            (*t_it)->exec_time = NormalizeTime(start_tm + (*t_it)->exec_time_loaded);
        SelectEnv(previous_weather, WFX_end_desc[0], CE->exec_time);
        SelectEnv(previous_weather, WFX_end_desc[1], WFX_end_desc[0]->exec_time + 0.5f);
        CT->copy(*WFX_end_desc[0]);
        CT->exec_time = NormalizeTime(CE->exec_time + rewind_tm);

        std::sort(next_weather->begin(), next_weather->end(), sort_env_pred);
        environment_detail::acquire_resources(*this, C0, C1);

        CurrentWeather = next_weather;
        CurrentWeatherName = it->first;
        CurrentEnv.soc_style = next_weather->soc_style;
        wfx_time = TimeDiff(fGameTime, CT->exec_time);
        bWFX = true;
        Current[0] = C0;
        Current[1] = C1;
        environment_detail::commit_resources(*this, C0, C1);
#ifdef WEATHER_LOGGING
        Msg("Starting WFX: '%s' - %3.2f sec", name.c_str(), wfx_time);
// for (auto l_it=CurrentWeather->begin(); l_it!=CurrentWeather->end(); l_it++)
// Msg (". Env: '%s' Tm: %3.2f",*(*l_it)->m_identifier.c_str(),(*l_it)->exec_time);
#endif
    }
    else
    {
#ifndef _EDITOR
        FATAL("! Empty weather effect name");
#endif
    }
    return true;
}

bool CEnvironment::StartWeatherFXFromTime(shared_str name, float time)
{
    if (!SetWeatherFX(name))
        return false;

    for (auto& env : *CurrentWeather)
        env->exec_time = NormalizeTime(env->exec_time - wfx_time + time);

    std::sort(CurrentWeather->begin(), CurrentWeather->end(), sort_env_pred);
    CEnvDescriptor* resumed_first = nullptr;
    CEnvDescriptor* resumed_second = nullptr;
    SelectEnvs(CurrentWeather, resumed_first, resumed_second, fGameTime);
    environment_detail::acquire_resources(*this, resumed_first, resumed_second);
    Current[0] = resumed_first;
    Current[1] = resumed_second;
    environment_detail::commit_resources(*this, resumed_first, resumed_second);
    wfx_time = time;
    return true;
}

void CEnvironment::StopWFX()
{
    VERIFY(CurrentCycleName.size());
    const auto cycle = WeatherCycles.find(CurrentCycleName);
    R_ASSERT3(cycle != WeatherCycles.end(), "Invalid weather cycle after weather effect", CurrentCycleName.c_str());
    R_ASSERT2(WFX_end_desc[0] && WFX_end_desc[1], "Weather effect has no valid destination pair");

    environment_detail::acquire_resources(*this, WFX_end_desc[0], WFX_end_desc[1]);

    bWFX = false;
    CurrentCycleName = cycle->first;
    CurrentWeather = &cycle->second;
    CurrentWeatherName = cycle->first;
    CurrentEnv.soc_style = CurrentWeather->soc_style;
    Current[0] = WFX_end_desc[0];
    Current[1] = WFX_end_desc[1];
    environment_detail::commit_resources(*this, Current[0], Current[1]);
#ifdef WEATHER_LOGGING
    Msg("WFX - end. Weather: '%s' Desc: '%s'/'%s' GameTime: %3.2f", CurrentWeatherName.c_str(),
        Current[0]->m_identifier.c_str(), Current[1]->m_identifier.c_str(), fGameTime);
#endif
}

IC bool lb_env_pred(const CEnvDescriptor* x, float val) { return x->exec_time < val; }
void CEnvironment::SelectEnv(EnvVec* envs, CEnvDescriptor*& e, float gt)
{
    auto env = std::lower_bound(envs->begin(), envs->end(), gt, lb_env_pred);
    if (env == envs->end())
    {
        e = envs->front();
    }
    else
    {
        e = *env;
    }
}

void CEnvironment::SelectEnvs(EnvVec* envs, CEnvDescriptor*& e0, CEnvDescriptor*& e1, float gt)
{
    auto env = std::lower_bound(envs->begin(), envs->end(), gt, lb_env_pred);
    if (env == envs->end())
    {
        e0 = *(envs->end() - 1);
        e1 = envs->front();
    }
    else
    {
        e1 = *env;
        if (env == envs->begin())
            e0 = *(envs->end() - 1);
        else
            e0 = *(env - 1);
    }
}

void CEnvironment::SelectEnvs(float gt)
{
    VERIFY(CurrentWeather);
    CEnvDescriptor* next_first = Current[0];
    CEnvDescriptor* next_second = Current[1];

    if (!Current[0] || !Current[1])
    {
        R_ASSERT2(!Current[0] && !Current[1], "Weather descriptor pair is only partially initialized");
        VERIFY(!bWFX);
        SelectEnvs(CurrentWeather, next_first, next_second, gt);
    }
    else
    {
        bool bSelect = false;
        if (Current[0]->exec_time > Current[1]->exec_time)
        {
            // terminator
            bSelect = (gt > Current[1]->exec_time) && (gt < Current[0]->exec_time);
        }
        else
        {
            bSelect = (gt > Current[1]->exec_time);
        }
        if (bSelect)
        {
            next_first = Current[1];
            SelectEnv(CurrentWeather, next_second, gt);
#ifdef WEATHER_LOGGING
            Msg("Weather: '%s' Desc: '%s' Time: %3.2f/%3.2f", CurrentWeatherName.c_str(),
                next_second->m_identifier.c_str(), next_second->exec_time, fGameTime);
#endif
        }
        else
        {
            return;
        }
    }

    environment_detail::acquire_resources(*this, next_first, next_second);
    Current[0] = next_first;
    Current[1] = next_second;
    environment_detail::commit_resources(*this, next_first, next_second);
}

void CEnvironment::lerp()
{
    if (bWFX && (wfx_time <= 0.f))
        StopWFX();

    SelectEnvs(fGameTime);
    VERIFY(Current[0] && Current[1]);

    // modifiers
    CEnvModifier EM;
    EM.far_plane = 0;
    EM.fog_color.set(0, 0, 0);
    EM.fog_density = 0;
    EM.ambient.set(0, 0, 0);
    EM.sky_color.set(0, 0, 0);
    EM.hemi_color.set(0, 0, 0);
    EM.use_flags.zero();

    Fvector view = Device.vCameraPosition;
    float mpower = 0;
    for (auto& mit : Modifiers)
        mpower += EM.sum(mit, view);

    // final lerp
    const float current_weight = TimeWeight(fGameTime, Current[0]->exec_time, Current[1]->exec_time);
    CurrentEnv.lerp(*this, *Current[0], *Current[1], current_weight, EM, mpower);
    m_pRender->lerp(CurrentEnv, &*Current[0]->m_pDescriptor, &*Current[1]->m_pDescriptor);
}

// Console "wind_dbg 1": dump the live wind-service numbers every 2 s. Tuning is done against
// what the game actually computes, not against what the formulas promise.
ENGINE_API int ps_e_wind_dbg = 0;

namespace
{
// C1-smooth 1D value noise for the wind service: random values on an integer lattice,
// smoothstep-interpolated between them. Cheap, continuous, no library dependencies.
float wind_vnoise(float x)
{
    const float i = floorf(x);
    const float f = x - i;
    const float s = f * f * (3.f - 2.f * f);
    const auto h = [](float n) {
        const float v = sinf(n * 127.1f) * 43758.5453f;
        return v - floorf(v);
    };
    return h(i) * (1.f - s) + h(i + 1.f) * s;
}
} // namespace

// CPU twin of da_wind_field_eval (the amplitude half). MUST stay formula-identical to
// packaging\...\shaders\r3\da_wind_field.h - the audio layer decides "does that tree rustle"
// with this, and the tree it hears must be the tree it sees leaning.
float CEnvironment::SampleWindField(float x, float z) const
{
    // shader: p = frac(fmod(i+4096,64) * {127.1,311.7}); p += dot(p, p+34.23); frac(p.x*p.y)
    const auto lattice = [](float ix, float iz) {
        float hx = fmodf(fmodf(ix + 4096.f, 64.f) * 127.1f, 1.f);
        float hz = fmodf(fmodf(iz + 4096.f, 64.f) * 311.7f, 1.f);
        const float d = hx * (hx + 34.23f) + hz * (hz + 34.23f);
        hx += d;
        hz += d;
        const float r = hx * hz;
        return r - floorf(r);
    };
    const auto vnoise = [&](float px, float pz) {
        const float ix = floorf(px), iz = floorf(pz);
        float fx = px - ix, fz = pz - iz;
        fx = fx * fx * (3.f - 2.f * fx);
        fz = fz * fz * (3.f - 2.f * fz);
        const float a = lattice(ix, iz), b = lattice(ix + 1.f, iz);
        const float c = lattice(ix, iz + 1.f), d = lattice(ix + 1.f, iz + 1.f);
        return (a * (1.f - fx) + b * fx) * (1.f - fz) + (c * (1.f - fx) + d * fx) * fz;
    };

    const float qx = (x - eff_wind_field_ofs.x) * (1.f / 40.f);
    const float qz = (z - eff_wind_field_ofs.y) * (1.f / 40.f);
    const float n = vnoise(qx, qz) * 0.62f + vnoise(qx * 2.17f + 13.7f, qz * 2.17f + 13.7f) * 0.38f;
    float g = clampr((n - 0.35f) / 0.5f, 0.f, 1.f);
    g = g * g * (3.f - 2.f * g); // smoothstep
    g *= g;
    return 0.40f + 0.75f * g;
}

float CEnvironment::weather_wind_profile()
{
    // Longest-substring table from dead_air_x64_wind.ltx, loaded once. Exact cycle-name keys
    // win over substrings; a name that matches nothing contributes no profile (-1).
    static xr_vector<std::pair<xr_string, float>> table;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        string_path path;
        FS.update_path(path, "$game_config$", "dead_air_x64_wind.ltx");
        if (FS.exist(path))
        {
            CInifile ini(path, TRUE);
            if (ini.section_exist("wind_profiles"))
            {
                for (const auto& item : ini.r_section("wind_profiles").Data)
                    if (item.first.size() && item.second.size())
                        table.emplace_back(item.first.c_str(), float(atof(item.second.c_str())));
                // Longest key first, so "veryfoggy" is tried before "foggy".
                std::sort(table.begin(), table.end(),
                    [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
                Msg("* [wind] %u weather wind profile(s) loaded", u32(table.size()));
            }
        }
        else
            Msg("! [wind] dead_air_x64_wind.ltx not found - weather wind profiles disabled");
    }

    const shared_str& name = CurrentWeatherName;
    if (!name.size())
        return -1.f;
    if (name != wind_profile_for)
    {
        wind_profile_for = name;
        wind_profile_target = -1.f;
        for (const auto& [key, value] : table)
            if (key == name.c_str())
            {
                wind_profile_target = value;
                break;
            }
        if (wind_profile_target < 0.f)
            for (const auto& [key, value] : table)
                if (strstr(name.c_str(), key.c_str()))
                {
                    wind_profile_target = value;
                    break;
                }
        if (ps_e_wind_dbg)
            Msg("* [wind] weather '%s' -> profile %.2f", name.c_str(), wind_profile_target);
    }
    return wind_profile_target;
}

void CEnvironment::wind_motor_press(const Fvector& pos, float radius, float strength)
{
    // Refresh an existing press motor near this position (one motor per walking actor), or
    // claim a free slot. No slot free - transient motors yield (see claim below), presses don't.
    SWindMotor* slot = nullptr;
    for (auto& m : wind_motors)
    {
        if (m.used && m.type == EWindMotor::press && m.released == 0.f &&
            m.pos.distance_to_sqr(pos) < 1.f)
        {
            slot = &m;
            break;
        }
    }
    if (!slot)
        for (auto& m : wind_motors)
            if (!m.used)
            {
                slot = &m;
                break;
            }
    if (!slot)
        return;

    slot->used = true;
    slot->type = EWindMotor::press;
    slot->pos = pos;
    slot->radius = radius;
    slot->strength = strength;
    slot->touched = Device.fTimeGlobal;
    slot->released = 0.f;
}

namespace
{
// Shared slot claim for the short-lived motor types: take a free slot, otherwise steal the
// oldest transient (impulse/shot) - a fresh event beats a dying one, presses are never evicted.
CEnvironment::SWindMotor* wind_motor_claim_transient(CEnvironment::SWindMotor (&motors)[CEnvironment::wind_motor_count])
{
    for (auto& m : motors)
        if (!m.used)
            return &m;

    CEnvironment::SWindMotor* slot = nullptr;
    float oldest = flt_max;
    for (auto& m : motors)
        if (m.type != CEnvironment::EWindMotor::press && m.touched < oldest)
        {
            oldest = m.touched;
            slot = &m;
        }
    return slot;
}
} // namespace

void CEnvironment::wind_motor_impulse(const Fvector& pos, float radius, float strength)
{
    SWindMotor* slot = wind_motor_claim_transient(wind_motors);
    if (!slot)
        return;

    slot->used = true;
    slot->type = EWindMotor::impulse;
    slot->pos = pos;
    slot->radius = radius;
    slot->strength = strength;
    slot->touched = Device.fTimeGlobal;
    slot->released = 0.f;
}

void CEnvironment::wind_motor_shot(const Fvector& pos, const Fvector& dir, float length, float strength)
{
    Fvector2 flat{dir.x, dir.z};
    const float flat_len = _sqrt(flat.x * flat.x + flat.y * flat.y);
    if (flat_len < 0.2f)
        return; // near-vertical shot: no meaningful ground trace
    flat.x /= flat_len;
    flat.y /= flat_len;

    // Automatic fire must not eat the whole motor pool: a fresh shot from the same spot in the
    // same direction re-arms the existing trace instead of claiming a new slot.
    SWindMotor* slot = nullptr;
    for (auto& m : wind_motors)
    {
        if (m.used && m.type == EWindMotor::shot && m.pos.distance_to_sqr(pos) < 4.f &&
            (m.dir.x * flat.x + m.dir.y * flat.y) > 0.9f)
        {
            slot = &m;
            break;
        }
    }
    if (!slot)
        slot = wind_motor_claim_transient(wind_motors);
    if (!slot)
        return;

    slot->used = true;
    slot->type = EWindMotor::shot;
    slot->pos = pos;
    slot->dir = flat;
    slot->radius = length;
    slot->strength = strength;
    slot->touched = Device.fTimeGlobal;
    slot->released = 0.f;
}

float CEnvironment::SampleWindMotors(float x, float z) const
{
    // Mirrors da_wind_motors_bend without the height/direction terms: just "how hard is a
    // motor shaking this spot", for the vegetation-audio triggers.
    float total = 0.f;
    for (u32 i = 0; i < wind_motor_count; ++i)
    {
        const Fmatrix& P = wind_motor_pos[i / 4];
        const Fmatrix& A = wind_motor_par[i / 4];
        const float* prow = &P.m[i % 4][0];
        const float* arow = &A.m[i % 4][0];
        if (prow[3] <= 0.f || _abs(arow[0]) <= 0.001f)
            continue;
        float dx = x - prow[0];
        float dz = z - prow[2];
        if (arow[3] > 0.5f)
        {
            // Line motor: distance to the trace segment (arow[1]/arow[2] carry the direction).
            const float along = clampr(dx * arow[1] + dz * arow[2], 0.f, prow[3]);
            dx -= arow[1] * along;
            dz -= arow[2] * along;
            const float dist = _sqrt(dx * dx + dz * dz);
            const float t = dist * (1.f / 1.1f);
            total += _abs(arow[0]) * expf(-t * t);
            continue;
        }
        const float dist = _sqrt(dx * dx + dz * dz);
        if (dist > prow[3] + arow[2] * 2.f)
            continue;
        const float t = (dist - arow[1]) / arow[2];
        total += _abs(arow[0]) * expf(-t * t);
    }
    return total;
}

void CEnvironment::UpdateEffectiveWind()
{
    const float t = Device.fTimeGlobal;
    // Weather ceiling. Two hard facts from the field (wind_dbg on real DA configs):
    //  * wind_velocity is a LEGACY 0..1000-ish scale (typical live values 10..500), not m/s -
    //    dividing by 20 saturated every nonzero weather to "hurricane" and erased the range;
    //  * 1028 of ~1100 DA weather entries say wind_velocity = 0 - their storms are authored as
    //    rain and clouds only, so a config-only ceiling turned a rainstorm into dead calm.
    // The ceiling is the strongest of three voices: the authored value on its own curve
    // (blowout fx set 100..500 and keep their gale), the PER-WEATHER PROFILE mapped from the
    // cycle name (dead_air_x64_wind.ltx - a storm cycle IS windy even though its config only
    // says "rain and clouds"), and the wind implied by precipitation as the floor under both.
    float delta = Device.fTimeDelta;
    if (delta < 0.f || delta > 1.f)
        delta = 0.03f;

    const float base_cfg = powf(clampr(CurrentEnv.wind_velocity / 400.f, 0.f, 1.f), 0.8f);
    const float base_implied = 0.10f + 0.58f * clampr(CurrentEnv.rain_density, 0.f, 1.f);
    // The profile switches as a step on the cycle boundary - low-pass it so a new weather
    // swells the wind over ~half a minute instead of snapping the whole world at once.
    const float profile = weather_wind_profile();
    if (wind_profile_smooth < 0.f)
        wind_profile_smooth = std::max(profile, 0.f); // first frame: no swell-in from zero
    wind_profile_smooth +=
        (std::max(profile, 0.f) - wind_profile_smooth) * (1.f - expf(-delta / 12.f));
    const float base = std::max({base_cfg, wind_profile_smooth, base_implied});

    // Three time scales, deliberately incommensurable so the pattern never visibly loops:
    // a minute-scale trend (lulls and freshenings), tens-of-seconds waves, and a fast layer
    // that only matters when it spikes - that spike IS a discrete gust. Calm weather raises
    // the spike threshold (gusts become rare), storms lower it (gusts come often).
    const float n_trend = wind_vnoise(t * (1.f / 170.f) + 3.7f);
    const float n_wave = wind_vnoise(t * (1.f / 23.f) + 17.3f);
    const float n_fast = wind_vnoise(t * (1.f / 5.5f) + 29.1f);
    const float gust_thr = 0.55f + 0.25f * (1.f - base);
    const float gust_ev = clampr((n_fast - gust_thr) / std::max(1.f - gust_thr, 0.05f), 0.f, 1.f);

    // Variability inside the weather envelope: a calm day swings between near-nothing and its
    // own light ceiling, a storm between fresh and violent. Balanced so the AVERAGE sits near
    // the weather's nominal strength (multiplying three attenuating layers - variability, the
    // spatial field, the consumer envelope - once collapsed a storm into a flat calm).
    // The floor rises with the weather: a storm may breathe, but it never sinks to a flat
    // calm - that read as "the weather has no effect at all" in the field.
    eff_wind_var = clampr(0.30f + 0.35f * base + n_trend * 0.55f + n_wave * 0.35f + gust_ev * 0.55f,
        0.f, 1.35f + 0.15f * base);
    eff_wind_norm = clampr(base * eff_wind_var, 0.f, 1.25f);

    // Gustiness: the fast layers, with the legacy Perlin mixed in - blowout zones override
    // wind_strength_factor directly, and that surge must keep reaching every consumer.
    eff_wind_gust = clampr(0.5f * wind_strength_factor + n_wave * 0.25f + gust_ev * 0.75f, 0.f, 1.f);

    eff_wind_gust_smooth += (eff_wind_gust - eff_wind_gust_smooth) * (1.f - expf(-delta / 1.5f));

    // Direction: the weather heading with a bounded wander - broad and lazy in light air
    // (real light wind meanders), tight in strong wind (a storm holds its line). Sped up
    // after a field test: at 45 s the heading read as "never changes" over a minute of
    // watching. The per-place deviation (eddies) lives in the shader field's z channel.
    const float wander_amp = deg2rad(35.f - 22.f * base);
    const float wander = (wind_vnoise(t * (1.f / 30.f) + 41.7f) * 2.f - 1.f) * wander_amp +
        (wind_vnoise(t * (1.f / 8.f) + 53.9f) * 2.f - 1.f) * deg2rad(8.f);
    eff_wind_dir = CurrentEnv.wind_direction + wander;

    // Spatial gust field scroll (the Ghost of Tsushima scheme: constant heading, magnitude
    // varied place-to-place by travelling noise). Gust fronts ride downwind at a speed that
    // grows with the weather - light air drifts its tongues, a storm drives them. The
    // accumulator wraps at the field repeat length (64 lattice cells x 40 m; the shader hash
    // is periodic over the same 64), so precision never decays over a long session.
    const float front_speed = 5.f + 9.f * base;
    eff_wind_field_ofs.x += _sin(eff_wind_dir) * front_speed * delta;
    eff_wind_field_ofs.y += _cos(eff_wind_dir) * front_speed * delta;
    constexpr float field_repeat = 40.f * 64.f;
    eff_wind_field_ofs.x = fmodf(eff_wind_field_ofs.x + field_repeat, field_repeat);
    eff_wind_field_ofs.y = fmodf(eff_wind_field_ofs.y + field_repeat, field_repeat);

    // ---- Tree sway phase. ------------------------------------------------------------------
    // Integrated with the weather's CURRENT tree speed (the mixer lerps it smoothly) at a
    // CONSTANT rate: a tree is a damped harmonic oscillator swinging at its own natural
    // frequency, set by mass and stiffness - wind changes how FAR it leans, not how fast it
    // swings (the earlier gust "whip-up" factor was right for grass, wrong for trees; grass
    // keeps its own whip in the detail manager). Accumulated, never time*speed - a varying
    // speed times absolute time jumps the phase. The consumer divides by 2*pi (FTreeVisual);
    // wrapping at 1024 * 2*pi keeps a whole number of wave periods.
    eff_tree_phase = fmodf(eff_tree_phase + delta * CurrentEnv.m_fTreeSpeed, 1024.f * PI_MUL_2);

    // ---- Puddle ripple travel. -------------------------------------------------------------
    // Two accumulated path lengths in noise-space units (world m x 12): rain drives downhill
    // flow (per-pixel slope scales it in the shader), wind drives along-wind drift (per-pixel
    // depth scales it). A tiny idle term keeps standing water barely alive instead of frozen.
    // The wrap CANNOT be the 64-cell noise period: the shader shifts along an arbitrary local
    // direction, so a 64 jump is not lattice-aligned and would visibly pop. 16384 keeps frac()
    // precision smooth and pops the pattern once per ~1.5 h of continuous rain - a single
    // reseed of shapeless noise the eye cannot catch.
    const float rain_k = clampr(CurrentEnv.rain_density * 1.5f, 0.f, 1.f);
    constexpr float water_wrap = 16384.f;
    eff_water_run_rain = fmodf(eff_water_run_rain + delta * (rain_k * 2.2f) * 12.f, water_wrap);
    eff_water_run_wind =
        fmodf(eff_water_run_wind + delta * (0.05f + 0.75f * eff_wind_norm) * 12.f, water_wrap);

    // ---- Wind motors: simulate and pack for the vegetation shaders. ------------------------
    const float now = Device.fTimeGlobal;
    u32 highest = 0;
    for (u32 i = 0; i < wind_motor_count; ++i)
    {
        SWindMotor& m = wind_motors[i];
        float amp = 0.f, ring_r = 0.f, ring_w = 0.f;
        const bool line = m.type == EWindMotor::shot;

        if (m.used && m.type == EWindMotor::impulse)
        {
            // Expanding blast ring: front travels at 14 m/s, height of the bend decays as it
            // goes. Dead once the ring leaves the authored radius.
            const float age = now - m.touched;
            ring_r = 14.f * age;
            ring_w = 1.5f + age * 2.0f; // the front smears out as it expands
            amp = m.strength * expf(-age * 2.2f);
            if (ring_r > m.radius || amp < 0.02f)
                m.used = false;
        }
        else if (m.used && line)
        {
            // Shot trace: a short shiver along the bullet path, gone in ~a third of a second.
            // Re-armed by every following shot of a burst (see wind_motor_shot).
            const float age = now - m.touched;
            amp = m.strength * expf(-age * 6.5f);
            if (amp < 0.02f)
                m.used = false;
        }
        else if (m.used)
        {
            if (m.released == 0.f && now - m.touched > 0.15f)
                m.released = now; // the actor moved on - start the spring-back
            if (m.released == 0.f)
            {
                amp = m.strength; // pressed down while the actor stands in it
                ring_w = m.radius * 0.55f;
            }
            else
            {
                // Damped spring-back (the Tsushima "damped waves" fix): the grass overshoots
                // and settles instead of snapping straight. cos keeps phase 0 = still pressed.
                const float age = now - m.released;
                amp = m.strength * cosf(age * 13.f) * expf(-age * 3.2f);
                ring_w = m.radius * 0.55f;
                if (age > 1.1f)
                    m.used = false;
            }
        }

        if (m.used)
            highest = i + 1;

        Fmatrix& P = wind_motor_pos[i / 4];
        Fmatrix& A = wind_motor_par[i / 4];
        float* prow = &P.m[i % 4][0];
        float* arow = &A.m[i % 4][0];
        prow[0] = m.pos.x; prow[1] = m.pos.y; prow[2] = m.pos.z; prow[3] = m.used ? m.radius : 0.f;
        arow[0] = m.used ? amp : 0.f;
        // Line motors carry their direction where radial ones carry the ring shape.
        arow[1] = line ? m.dir.x : ring_r;
        arow[2] = line ? m.dir.y : std::max(ring_w, 0.05f);
        arow[3] = line ? 1.f : 0.f;
    }
    wind_motor_active = float(highest);

    // ---- Optional service dump (wind_dbg 1): ground the tuning in real numbers. ------------
    if (ps_e_wind_dbg)
    {
        static float next_dump = 0.f;
        if (now >= next_dump)
        {
            next_dump = now + 2.f;
            u32 live = 0;
            for (const auto& m : wind_motors)
                if (m.used)
                    ++live;
            Msg("* [wind] vel=%.1f base=%.2f norm=%.2f var=%.2f gust=%.2f dir=%.0f deg | motors=%u "
                "green=%.2f rain=%.2f",
                CurrentEnv.wind_velocity, base, eff_wind_norm, eff_wind_var, eff_wind_gust,
                rad2deg(eff_wind_dir), live, wind_veg_green, rain_k);
        }
    }
}

void CEnvironment::OnFrame()
{
    ZoneScoped;

    if (!g_pGameLevel)
        return;

    lerp();

    PerlinNoise1D->SetFrequency(wind_gust_factor * MAX_NOISE_FREQ);
    wind_strength_factor = clampr(PerlinNoise1D->GetContinious(Device.fTimeGlobal) + 0.5f, 0.f, 1.f);

    UpdateEffectiveWind();

    eff_LensFlare->OnFrame(CurrentEnv, fTimeFactor);
    eff_Thunderbolt->OnFrame(CurrentEnv);
    eff_Rain->OnFrame();
    if (eff_WindVeg)
        eff_WindVeg->OnFrame();
}
