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
    reset_water_body(); // no level yet, so no water known and an empty wave table

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

float CEnvironment::GetRainExposure() const { return eff_Rain ? eff_Rain->GetExposure() : 0.f; }

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
// A seed asked for before the environment existed (user.ltx runs before the level does):
// consumed at the service's first tick. -1 = none.
ENGINE_API float g_wind_seed_override = -1.f;
// Likewise for wind_force issued from user.ltx: applied at the first tick.
ENGINE_API float g_wind_force_override = -1.f;

// The wind-field maths, shared with the vertex shaders: one file, two compilers.
#include "../../packaging/dead-air-x64/compatibility/gamedata/shaders/r3/da_wind_core.h"

namespace
{
// Deterministic generator for the gust events. Seeded from the session seed, so a pinned
// -wind_seed replays the same gusts at the same times; never touches the engine's global
// Random, whose sequence gameplay consumes.
struct wind_rng
{
    u32 state{0x9E3779B9u};
    float next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return float(state & 0xFFFFFFu) * (1.f / 16777216.f);
    }
} g_wind_rng;

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

// The gust field at a world point - the very function the vertex shaders run
// (da_wind_core.h), so what the audio hears, what a bullet drifts by and what the eye sees
// leaning are one wind.
float CEnvironment::SampleWindField(float x, float z) const
{
    return da_wind_field_amp(SampleWindGust(x, z));
}

float CEnvironment::SampleWindGust(float x, float z) const
{
    return da_wind_field_gust(x, z, eff_wind_field_ofs.x, eff_wind_field_ofs.y);
}

float CEnvironment::SampleWindDeviation(float x, float z) const
{
    return da_wind_field_dev(x, z, eff_wind_field_ofs.x, eff_wind_field_ofs.y);
}

float CEnvironment::WindSpeedMs() const { return eff_wind_norm * DA_WIND_MS_PER_NORM; }

Fvector CEnvironment::SampleWindMotorsVec(const Fvector& p) const
{
    // The shader's da_wind_motors_bend, direction kept, in metres per second of air: a blast
    // ring pushes outward from its centre, a line motor drags along the shot, a press pushes
    // radially. The bend amplitudes are unit-less; 6 m/s per unit turns "grass flattened by a
    // grenade" into a push that sends a can rolling, which is what the eye expects to see.
    Fvector out{0.f, 0.f, 0.f};
    for (u32 i = 0; i < u32(wind_motor_active); ++i)
    {
        const Fmatrix& P = wind_motor_pos[i / 4];
        const Fmatrix& A = wind_motor_par[i / 4];
        const float* prow = &P.m[i % 4][0];
        const float* arow = &A.m[i % 4][0];
        if (prow[3] <= 0.f || _abs(arow[0]) <= 0.001f)
            continue;
        float dx = p.x - prow[0];
        float dz = p.z - prow[2];
        if (arow[3] > 0.5f)
        {
            const float along = clampr(dx * arow[1] + dz * arow[2], 0.f, prow[3]);
            dx -= arow[1] * along;
            dz -= arow[2] * along;
            const float dist = _sqrt(dx * dx + dz * dz);
            const float trace_y = prow[1] + (arow[3] - 1.f) * along;
            const float dy = trace_y - p.y;
            const float dist3 = _sqrt(dist * dist + dy * dy);
            if (dist3 > 0.45f)
                continue;
            const float t = dist3 * (1.f / 0.16f);
            const float w = expf(-t * t) * arow[0] * 6.f;
            out.x += arow[1] * w;
            out.z += arow[2] * w;
            continue;
        }
        const float dist = _sqrt(dx * dx + dz * dz);
        if (dist > prow[3] + arow[2] * 2.f || dist < 0.001f)
            continue;
        float amp = arow[0];
        const bool is_blast = arow[3] < -0.5f;
        if (is_blast)
            amp *= std::min(1.1f, (0.25f * prow[3]) / std::max(dist, 0.5f)) *
                clampr((prow[3] - dist) / (0.30f * prow[3]), 0.f, 1.f);
        const float t = (dist - arow[1]) / arow[2];
        float w = expf(-t * t);
        if (is_blast && dist < arow[1])
        {
            const float tau = (arow[1] - dist) * (1.f / 22.f);
            w = expf(-tau * 3.5f) * cosf(tau * 9.f);
        }
        const float k = w * amp * 6.f / dist;
        out.x += dx * k;
        out.z += dz * k;
    }
    return out;
}

Fvector CEnvironment::WindAt(const Fvector& pos, float height_above_ground) const
{
    const float g = da_wind_field_gust(pos.x, pos.z, eff_wind_field_ofs.x, eff_wind_field_ofs.y);
    const float dev = da_wind_field_dev(pos.x, pos.z, eff_wind_field_ofs.x, eff_wind_field_ofs.y);
    const float heading = eff_wind_dir + da_wind_dev_angle(dev, eff_wind_norm);
    const float speed = WindSpeedMs() * da_wind_field_amp(g) * da_wind_profile(height_above_ground, eff_wind_z0);
    Fvector v;
    v.set(_sin(heading) * speed, 0.f, _cos(heading) * speed);
    v.add(SampleWindMotorsVec(pos));
    return v;
}

namespace
{
bool wind_occluded(const Fvector& start, const Fvector& direction, float range)
{
    // Preserve RayPick's back-face rejection, but stop at the first blocker. RayTest
    // cannot substitute here because it deliberately includes back faces.
    static thread_local CDB::COLLIDER collider;
    collider.ray_query(CDB::OPT_ONLYFIRST | CDB::OPT_CULL,
        g_pGameLevel->ObjectSpace.GetStaticModel(), start, direction, range);
    if (!collider.r_count())
        return false;
    if (collider.r_begin()->range < range)
        return true;
    // RayPick excludes the exact far endpoint. An endpoint hit must not hide a nearer one.
    collide::rq_result hit;
    return g_pGameLevel->ObjectSpace.RayPick(start, direction, range, collide::rqtStatic, hit, nullptr);
}
}

float CEnvironment::WindExposure(const Fvector& pos) const
{
    if (!g_pGameLevel)
        return 0.f;
    Fvector start = pos;
    start.y += 0.8f; // clear the caller's own capsule and the ground
    static const Fvector up = {0.f, 1.f, 0.f};
    // Under a roof or indoors: no wind at all. The old test stopped here, which is why a wall
    // on the windward side never sheltered anyone.
    if (wind_occluded(start, up, 35.f))
        return 0.f;
    // Upwind: the air arrives from the side the wind blows FROM.
    Fvector upwind;
    upwind.set(-_sin(eff_wind_dir), 0.f, -_cos(eff_wind_dir));
    if (wind_occluded(start, upwind, 12.f))
        return 0.35f; // in the lee: the wind still swirls round, at a third
    return 1.f;
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
            // Service tunables: the roughness length that shapes the vertical profile (0.03 m
            // is open grassland; a level of forest and village sits nearer 0.3).
            if (ini.section_exist("wind_service") && ini.line_exist("wind_service", "z0"))
                eff_wind_z0 = clampr(ini.r_float("wind_service", "z0"), 0.001f, 2.f);
            if (ini.section_exist("clouds"))
            {
                if (ini.line_exist("clouds", "altitude"))
                    eff_cloud_altitude = clampr(ini.r_float("clouds", "altitude"), 200.f, 6000.f);
                if (ini.line_exist("clouds", "thickness"))
                    eff_cloud_thickness = clampr(ini.r_float("clouds", "thickness"), 50.f, 3000.f);
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
    // claim a free slot. Presses may take at most SIX of the eight slots: with every NPC now
    // pressing, a village crowd once filled the whole pool and a grenade blast could not get
    // a motor at all ("no shockwave through the grass") - two slots stay reserved for
    // transient events, and blasts additionally evict presses outright (see impulse).
    SWindMotor* slot = nullptr;
    u32 press_count = 0;
    for (auto& m : wind_motors)
    {
        if (m.used && m.type == EWindMotor::press)
        {
            ++press_count;
            if (!slot && m.released == 0.f && m.pos.distance_to_sqr(pos) < 1.f)
                slot = &m;
        }
    }
    if (!slot)
    {
        if (press_count >= 6)
            return;
        for (auto& m : wind_motors)
            if (!m.used)
            {
                slot = &m;
                break;
            }
    }
    if (!slot)
        return;

    // Smoothed ground speed of the presser, for the rustle volume: a fresh claim starts
    // still, a refresh measures the move since the last one.
    if (slot->used && slot->type == EWindMotor::press && slot->released == 0.f)
    {
        const float dt = Device.fTimeGlobal - slot->touched;
        if (dt > EPS_S)
        {
            const float v = slot->pos.distance_to(pos) / dt;
            slot->speed = slot->speed * 0.85f + v * 0.15f;
        }
    }
    else
    {
        // veg is deliberately left alone: the detail manager's calc task is its only writer
        // (it re-judges every live press each pass), so the main thread never touches it.
        slot->speed = 0.f;
    }

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
// Slot claims follow a strict pecking order: blast > press > shot. The order matters in one
// very real scenario: an F1 explodes, its ring motor is born, and THE SAME FRAME sprays 20+
// fragments through AddBullet - each spawning a shot motor. With a naive "steal the oldest
// transient" the fragments evicted the freshly born blast ring of their own grenade, and the
// explosion read as nothing (while the fragment-free debug ring worked fine).
CEnvironment::SWindMotor* wind_motor_free_slot(CEnvironment::SWindMotor (&motors)[CEnvironment::wind_motor_count])
{
    for (auto& m : motors)
        if (!m.used)
            return &m;
    return nullptr;
}

CEnvironment::SWindMotor* wind_motor_oldest_of(
    CEnvironment::SWindMotor (&motors)[CEnvironment::wind_motor_count], CEnvironment::EWindMotor type)
{
    CEnvironment::SWindMotor* slot = nullptr;
    float oldest = flt_max;
    for (auto& m : motors)
        if (m.used && m.type == type && m.touched < oldest)
        {
            oldest = m.touched;
            slot = &m;
        }
    return slot;
}
} // namespace

void CEnvironment::wind_motor_impulse(const Fvector& pos, float lethal_r, float strength)
{
    // A blast outranks everything: free slot, else the oldest shot, else the oldest OTHER
    // blast, else any press (the standing actor re-claims a slot next frame anyway).
    SWindMotor* slot = wind_motor_free_slot(wind_motors);
    if (!slot)
        slot = wind_motor_oldest_of(wind_motors, EWindMotor::shot);
    if (!slot)
        slot = wind_motor_oldest_of(wind_motors, EWindMotor::impulse);
    if (!slot)
        for (auto& m : wind_motors)
            if (m.type == EWindMotor::press)
            {
                slot = &m;
                break;
            }
    if (!slot)
        return;

    slot->used = true;
    slot->type = EWindMotor::impulse;
    slot->pos = pos;
    // Hopkinson-Cranz cube-root scaling by proxy: the ring IS the charge's own authored
    // lethal radius (blast_r), so a bigger charge reaches proportionally further. All the
    // spatial shaping (1/R falloff, edge fade, per-root spring-back) lives in the shader,
    // computed from each root's own distance - the CPU only carries the peak strength.
    slot->radius = clampr(lethal_r, 3.f, 20.f);
    slot->dir_y = 0.f;
    slot->strength = strength;
    slot->touched = Device.fTimeGlobal;
    slot->released = 0.f;

    if (ps_e_wind_dbg)
        Msg("* [wind] blast motor: ring=%.0f s=%.2f at (%.0f, %.0f, %.0f)", slot->radius,
            strength, pos.x, pos.y, pos.z);
}

void CEnvironment::wind_motor_shot(const Fvector& pos, const Fvector& dir, float length, float strength)
{
    Fvector2 flat{dir.x, dir.z};
    const float flat_len = _sqrt(flat.x * flat.x + flat.y * flat.y);
    // Near-vertical shots have no meaningful ground trace - firing at the sky must not stir
    // anything (field report). Flatter shots carry their vertical slope into the motor so
    // the shader can gate by the trace's actual height above each tuft.
    if (flat_len < 0.35f)
        return;
    const float slope_y = clampr(dir.y / flat_len, -0.45f, 0.95f);
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
    // Shots are the LOWEST caste: free slot or the oldest fellow shot - never a blast (an
    // F1's own fragments once evicted its freshly born ring), never a press.
    if (!slot)
        slot = wind_motor_free_slot(wind_motors);
    if (!slot)
        slot = wind_motor_oldest_of(wind_motors, EWindMotor::shot);
    if (!slot)
        return;

    slot->used = true;
    slot->type = EWindMotor::shot;
    slot->pos = pos;
    slot->dir = flat;
    slot->dir_y = slope_y;
    slot->radius = length;
    slot->strength = strength;
    slot->touched = Device.fTimeGlobal;
    slot->released = 0.f;
}

bool CEnvironment::wind_sheltered(const Fvector& pos) const
{
    if (!g_pGameLevel)
        return true;
    Fvector start = pos;
    start.y += 0.6f; // clear own capsule/ground
    static const Fvector up = {0.f, 1.f, 0.f};
    return wind_occluded(start, up, 35.f);
}

float CEnvironment::SampleWindMotors(const Fvector& p) const
{
    // Mirrors da_wind_motors_bend without the direction terms: just "how hard is a motor
    // shaking this spot", for the vegetation-audio triggers. Keeps the shader's line height
    // gate and the per-tuft spring-back behind a blast front so what is heard matches
    // what is seen.
    float total = 0.f;
    for (u32 i = 0; i < u32(wind_motor_active); ++i)
    {
        const Fmatrix& P = wind_motor_pos[i / 4];
        const Fmatrix& A = wind_motor_par[i / 4];
        const float* prow = &P.m[i % 4][0];
        const float* arow = &A.m[i % 4][0];
        if (prow[3] <= 0.f || _abs(arow[0]) <= 0.001f)
            continue;
        float dx = p.x - prow[0];
        float dz = p.z - prow[2];
        if (arow[3] > 0.5f)
        {
            // Line motor: distance to the trace segment (arow[1]/arow[2] carry the direction).
            const float along = clampr(dx * arow[1] + dz * arow[2], 0.f, prow[3]);
            dx -= arow[1] * along;
            dz -= arow[2] * along;
            const float dist = _sqrt(dx * dx + dz * dz);
            const float trace_y = prow[1] + (arow[3] - 1.f) * along;
            const float h_gate = clampr(1.f - (trace_y - p.y - 0.7f) / 1.1f, 0.f, 1.f);
            const float t = dist * (1.f / 0.16f);
            total += _abs(arow[0]) * expf(-t * t) * h_gate;
            continue;
        }
        const float dist = _sqrt(dx * dx + dz * dz);
        if (dist > prow[3] + arow[2] * 2.f)
            continue;
        float amp = _abs(arow[0]);
        const float t = (dist - arow[1]) / arow[2];
        float w = expf(-t * t);
        if (arow[3] < -0.5f)
        {
            // Blast: the same local shaping the shader does - 1/R falloff anchored at a
            // quarter of the ring reach, edge fade, and the damped spring-back behind the
            // front (abs - audio only cares how agitated the spot is, not which way).
            amp *= std::min(1.1f, (0.25f * prow[3]) / std::max(dist, 0.5f)) *
                clampr((prow[3] - dist) / (0.30f * prow[3]), 0.f, 1.f);
            if (dist < arow[1])
            {
                const float tau = (arow[1] - dist) * (1.f / 22.f);
                w = _abs(expf(-tau * 3.5f) * cosf(tau * 9.f));
            }
        }
        total += amp * w;
    }
    return total;
}

// ---- CPU replica of the puddle-shader noise (da_puddles.h) -------------------------------
// Bit-for-bit the same formulas in fp32: gameplay decides "did this land in water" with the
// same picture the player sees. Minor GPU/CPU rounding drift only matters within a few
// centimetres of a puddle's edge - acceptable for a hit test.
static float da_cpu_hash21(float px, float py)
{
    // HLSL source (da_puddles.h): p = frac(p * (127.1, 311.7)); p += dot(p, p + 34.23);
    // return frac(p.x * p.y);
    float fx = px * 127.1f, fy = py * 311.7f;
    fx -= floorf(fx);
    fy -= floorf(fy);
    const float dp = fx * (fx + 34.23f) + fy * (fy + 34.23f);
    fx += dp;
    fy += dp;
    const float r = fx * fy;
    return r - floorf(r);
}

static float da_cpu_vnoise(float px, float py)
{
    const float ix = floorf(px), iy = floorf(py);
    float fx = px - ix, fy = py - iy;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    const float a = da_cpu_hash21(ix, iy);
    const float b = da_cpu_hash21(ix + 1.f, iy);
    const float c = da_cpu_hash21(ix, iy + 1.f);
    const float d = da_cpu_hash21(ix + 1.f, iy + 1.f);
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}

// The fill-map rung of the preset ladder (r__puddle_fill), the same number the shader reads out
// of da_wet_params.x. Declared here rather than pulled from the render DLL: this file is the
// CPU half of the mask and has to branch on exactly what the pixel shader branched on.
extern ENGINE_API int ps_r__puddle_fill;

// How far the value noise is allowed to move the fill map's level - the width of the edge it
// shapes, in the noise's own 0..1 domain. DA_PUDDLE_FILL_EDGE in da_puddles.h is the same
// number; the two masks are one mask.
static constexpr float da_puddle_fill_edge = .30f;

float CEnvironment::SamplePuddleMask(const Fvector& pos, float ground_ny)
{
    const float wet = eff_puddle_wet;
    if (wet < 0.01f || eff_puddle_size < 0.005f)
        return 0.f;
    const float slope = clampr((_abs(ground_ny) - 0.62f) * 2.2f, 0.f, 1.f);
    if (slope < 0.01f)
        return 0.f;
    // The shader's hemi "open sky" gate is stood in by the shelter ray: rain does not pool
    // under a roof, and neither does the shader draw water there.
    if (wind_sheltered(pos))
        return 0.f;
    float n = da_cpu_vnoise(pos.x * 0.33f, pos.z * 0.33f) * 0.62f +
        da_cpu_vnoise(pos.x * 1.10f, pos.z * 1.10f) * 0.38f;

    // The baked fill map decides WHERE, the noise only shapes the edge - the same two lines the
    // shader runs, on the same data, behind the same rung of the ladder. Off the map (or on a
    // level with no bake) the noise stays the placement, which is also what the shader does, so
    // the footprint test is spelled out here rather than hidden behind a zero the map could
    // legitimately hold.
    if (ps_r__puddle_fill > 0 && !puddle_fill.empty())
    {
        const float u = (pos.x - water_field_bounds.vMin.x) * water_field_inv_extent.x;
        const float v = (pos.z - water_field_bounds.vMin.z) * water_field_inv_extent.y;
        if (u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)
            n = clampr(puddle_fill_at(pos) + (n - .5f) * da_puddle_fill_edge, 0.f, 1.f);
    }

    const float thr =
        0.86f + (0.30f - 0.86f) * clampr(eff_puddle_size, 0.f, 1.f) + (1.f - wet) * 0.15f;
    float s = clampr((n - thr) / 0.10f, 0.f, 1.f);
    s = s * s * (3.f - 2.f * s); // smoothstep(0, 0.10, n - thr)
    return s * wet * slope;
}

void CEnvironment::water_hit(const Fvector& pos, float radius, EWaterHit kind)
{
    // A body sliding into water fires a contact every physics step: one ring per spot per
    // quarter second, the rest is the same splash.
    if (kind == EWaterHit::ring)
        for (const auto& h : water_hits)
            if (h.used && h.kind == EWaterHit::ring && Device.fTimeGlobal - h.birth < 0.25f &&
                h.pos.distance_to_sqr(pos) < 0.35f * 0.35f)
                return;

    // Drains outrank rings (the pecking-order lesson from the wind motors: a burst of
    // bullet rings must never evict the crater a grenade just dried).
    SWaterHit* slot = nullptr;
    for (auto& h : water_hits)
        if (!h.used)
        {
            slot = &h;
            break;
        }
    if (!slot)
    {
        // A full pool. The victim is the ring the eye can see LEAST of, not the oldest by the
        // clock - and if even that one is still plainly visible, the new ring is dropped rather
        // than snapping a live one out of the water. Eight rings on screen at once is already a
        // firefight; a ninth nobody will miss, a wave vanishing mid-spread everybody will.
        for (auto& h : water_hits)
            if (h.kind == EWaterHit::ring && (!slot || h.amp < slot->amp))
                slot = &h;
        if (slot && slot->amp > 0.30f)
            slot = nullptr;
    }
    if (!slot && kind == EWaterHit::drain)
        for (auto& h : water_hits)
            if (!slot || h.birth < slot->birth)
                slot = &h;
    if (!slot)
        return;

    slot->used = true;
    slot->kind = kind;
    slot->pos = pos;
    slot->radius = radius;
    slot->birth = Device.fTimeGlobal;
    slot->amp = 1.f;
    // Whether the ripple field sees this ring born. Inside its window the sim digs the dimple
    // and carries the wave from there, and the analytic ring is only drawn past the window's
    // edge; born outside it the ring has no wave in the field at all, so the analytic one is
    // drawn everywhere, window included - otherwise a ring watched from the bank vanishes the
    // moment the player walks far enough for the window to swallow it. The slot's radius is
    // how far the ring is meant to run, not how big the splash was: the dimple is the CAVITY
    // the hit opens, four and a half centimetres per metre of reach - 6 cm for a bullet, 7 for
    // a boot - growing faster past the two metres only a blast reaches, to half a metre and
    // up. The cavity's radius is what decides the ring's wavelengths - a Gaussian trough of
    // radius a puts exp(-k^2 a^2 / 4) into wavenumber k, so at 11 cm the hand-wide crests that
    // make a pistol's ring readable got a twentieth of the amplitude the invisible half-metre
    // swell did - and a bullet's rings ARE those crests: a narrow, deep cavity, which is what
    // a round at seven hundred metres a second opens.
    slot->crater = 0.f;
    if (kind == EWaterHit::ring && water_ripple_win.z > 0.f)
    {
        const float half = water_ripple_win.z * 0.5f - water_ripple_edge;
        if (_abs(pos.x - water_ripple_win.x) < half && _abs(pos.z - water_ripple_win.y) < half)
            slot->crater = clampr(0.045f * radius + 0.08f * std::max(radius - 2.f, 0.f), 0.04f, 1.0f);
    }

    if (ps_e_wind_dbg)
        Msg("* [water] %s r=%.1f at (%.0f, %.0f, %.0f)", kind == EWaterHit::drain ? "drain" : "ring",
            radius, pos.x, pos.y, pos.z);
}

// The preset's wave-row budget (xrRender_console.cpp), defined next to the other shared render
// knobs in xr_ioc_cmd.cpp.
extern ENGINE_API int ps_r__water_waves;
// Ripple-field resolution the preset picked, in texels (0 = no field, the analytic rings only).
// The window solved below has to agree with the target the renderer created at that size.
extern ENGINE_API int ps_r__water_ripple_active;
// "This level holds water", published for the render DLL, which cannot see CEnvironment. Written
// only here so the two can never disagree about it.
extern ENGINE_API bool g_da_level_has_water;

void CEnvironment::set_water_bodies(const SWaterBody* bodies, int count)
{
    water_body_count = clampr(count, 0, water_body_max);
    for (int i = 0; i < water_body_count; ++i)
    {
        water_bodies[i] = bodies[i];
        water_bodies[i].depth = clampr(bodies[i].depth, 0.05f, 40.f);
    }
    // The fetch and the depth these carry are inputs of the wave table, so whatever the old
    // level seeded is stale: force the next solve to re-seed rather than wait for the wind.
    water_seed_wind = -1.f;
    g_da_level_has_water = water_body_count > 0;
}

void CEnvironment::reset_water_body()
{
    water_body_count = 0;
    g_da_level_has_water = false;
    water_mean_depth = 1.5f;
    water_seed_wind = -1.f;
    water_wave_count = 0.f;
    ZeroMemory(water_wave, sizeof(water_wave));
    water_body.set(water_mean_depth, 120.f, 0.f, 0.f);
    // The baked field describes the SAME level, so it is stale the moment the bodies are.
    // Both level load and level unload already go through here; the bake therefore has to run
    // after this call, which is where it sits (measure_water_body, Level_load.cpp).
    reset_water_field();
    // Nothing is wading on a level that is not loaded yet.
    for (auto& w : water_wakes)
        w.used = false;
    ZeroMemory(water_wake_pos, sizeof(water_wake_pos));
    ZeroMemory(water_wake_par, sizeof(water_wake_par));
    water_wake_active = 0.f;
    water_ripple_win.set(0.f, 0.f, 0.f, 0.f);
}

// ---- The baked water field ----------------------------------------------------------------
void CEnvironment::set_water_field(xr_vector<SWaterTexel>&& texels, xr_vector<float>&& fill, const Fbox& world_bounds)
{
    constexpr size_t need = size_t(water_field_dim) * size_t(water_field_dim);
    if (texels.size() != need || !world_bounds.is_valid())
    {
        reset_water_field();
        return;
    }

    water_field = std::move(texels);
    // Kept exactly as long as the field, whatever the bake handed over: everything downstream -
    // the upload, the CPU replica - indexes the two with one index and one bounds test.
    puddle_fill = std::move(fill);
    if (puddle_fill.size() != need)
        puddle_fill.assign(need, 0.f);
    water_field_bounds = world_bounds;

    const float ex = std::max(world_bounds.vMax.x - world_bounds.vMin.x, EPS_S);
    const float ez = std::max(world_bounds.vMax.z - world_bounds.vMin.z, EPS_S);
    water_field_inv_extent.set(1.f / ex, 1.f / ez);
    water_field_texel = ex / float(water_field_dim);
}

void CEnvironment::reset_water_field()
{
    water_field.clear();
    water_field.shrink_to_fit(); // 16 MB has no business surviving into the next level
    puddle_fill.clear();
    puddle_fill.shrink_to_fit();
    water_field_bounds.invalidate();
    water_field_inv_extent.set(0.f, 0.f);
    water_field_texel = 1.f;
    eye_under_depth = 0.f;
    eye_under_surface = 0.f;
}

// Texel of the baked field a world point falls in, or -1 when there is no field or the point
// is outside its footprint. Nearest texel, no filtering: gameplay wants a yes/no answer and
// the bake is already a rasterisation.
static int da_water_field_texel(const CEnvironment& env, const Fvector& p)
{
    if (env.water_field.empty())
        return -1;
    const float u = (p.x - env.water_field_bounds.vMin.x) * env.water_field_inv_extent.x;
    const float v = (p.z - env.water_field_bounds.vMin.z) * env.water_field_inv_extent.y;
    if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
        return -1;
    constexpr int dim = CEnvironment::water_field_dim;
    const int x = clampr(int(u * float(dim)), 0, dim - 1);
    const int z = clampr(int(v * float(dim)), 0, dim - 1);
    return z * dim + x;
}

float CEnvironment::water_surface_at(const Fvector& p) const
{
    const int at = da_water_field_texel(*this, p);
    if (at < 0 || !water_field[at].mask)
        return -flt_max;
    return water_field[at].surface;
}

float CEnvironment::water_bed_at(const Fvector& p) const
{
    const int at = da_water_field_texel(*this, p);
    if (at < 0 || !water_field[at].mask)
        return -flt_max;
    return water_field[at].bed;
}

float CEnvironment::water_shore_dist(const Fvector& p) const
{
    const int at = da_water_field_texel(*this, p);
    // The distance transform is defined everywhere the field is, water or bank, so this one
    // does not gate on the mask - "how far to the nearest bank" is the question a wader asks
    // from the dry side too.
    if (at < 0)
        return -flt_max;
    return water_field[at].shore;
}

bool CEnvironment::water_at(const Fvector& p, float& surface, float& bed) const
{
    const int at = da_water_field_texel(*this, p);
    if (at < 0 || !water_field[at].mask)
        return false;
    surface = water_field[at].surface;
    bed = water_field[at].bed;
    return true;
}

// The fill map, read the way the shader reads it: bilinear over the same footprint, clamped at
// the edges, zero outside. Point-sampling the field is right for a rasterised coverage mask and
// wrong here - the fill is a smooth scalar and the two halves of a puddle have to agree.
//
// The one drift left against the GPU is that the upload rounds each texel to fp16 before the
// hardware interpolates, so the two answers differ by well under a millimetre of depth.
float CEnvironment::puddle_fill_at(const Fvector& p) const
{
    if (puddle_fill.empty())
        return 0.f;

    const float u = (p.x - water_field_bounds.vMin.x) * water_field_inv_extent.x;
    const float v = (p.z - water_field_bounds.vMin.z) * water_field_inv_extent.y;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f)
        return 0.f;

    constexpr int dim = water_field_dim;
    const float fx = u * float(dim) - .5f;
    const float fz = v * float(dim) - .5f;
    const int bx = iFloor(fx), bz = iFloor(fz);
    const float tx = fx - float(bx), tz = fz - float(bz);

    const int x0 = clampr(bx, 0, dim - 1), x1 = clampr(bx + 1, 0, dim - 1);
    const int z0 = clampr(bz, 0, dim - 1), z1 = clampr(bz + 1, 0, dim - 1);

    const float top = puddle_fill[size_t(z0) * dim + size_t(x0)] * (1.f - tx) +
        puddle_fill[size_t(z0) * dim + size_t(x1)] * tx;
    const float bot = puddle_fill[size_t(z1) * dim + size_t(x0)] * (1.f - tx) +
        puddle_fill[size_t(z1) * dim + size_t(x1)] * tx;
    return top + (bot - top) * tz;
}

// A hand across the visor. One sweep at a time: the animation that triggers this is a second long
// and the player can hold the key down through all of it, and restarting the sweep every frame
// would park the hand at the near edge and clean nothing.
//
// The delay is not padding. The game has to put away whatever is in the hands before the cleaning
// item can be drawn, and then that item has its own draw animation - so the glass is wiped some
// way into all of it, not at the moment the player asked. Without it the water cleared while the
// player was still watching their own hands holster a rifle.
void CEnvironment::visor_wipe(float delay, float len)
{
    if (visor.wipe_start >= 0.f && Device.fTimeGlobal < visor.wipe_start + visor.wipe_len)
        return;
    visor.wipe_len = clampr(len, 0.05f, 4.f);
    visor.wipe_start = Device.fTimeGlobal + clampr(delay, 0.f, 4.f);
}

// 0 while the hand has not reached the glass, 1 once it is off the far edge. The renderer turns
// this into a position and a direction; keeping the mapping there is what lets the sweep be a
// shape - it eases in and out, as an arm does - without the engine having to agree about it.
float CEnvironment::visor_wipe_phase() const
{
    if (visor.wipe_start < 0.f)
        return 1.f;
    return clampr((Device.fTimeGlobal - visor.wipe_start) / _max(visor.wipe_len, 0.01f), 0.f, 1.f);
}

// A continuous wake: something is moving through the water here. Unlike water_hit this is fed
// every frame for as long as the source keeps wading, so a repeat call REFRESHES the slot it
// already owns instead of consuming a new one.
void CEnvironment::water_wake(const Fvector& pos, float radius, float strength, const Fvector2& vel, u32 id)
{
    const float now = Device.fTimeGlobal;

    // Same source, same slot: the footprint moves with the foot rather than leaving a trail of
    // eight dying spots behind it. By id and not by distance - a walker's two feet pass within
    // a hand of each other every stride, and a slot that jumped between them was a source
    // flickering at the frame rate.
    SWaterWake* slot = nullptr;
    for (auto& w : water_wakes)
        if (w.used && w.id == id)
        {
            slot = &w;
            break;
        }
    if (!slot)
        for (auto& w : water_wakes)
            if (!w.used)
            {
                slot = &w;
                break;
            }
    // Full pool: the faintest wake gives way, so the actor's own bow wave never loses its slot
    // to a distant mutant's.
    if (!slot)
        for (auto& w : water_wakes)
            if (!slot || w.strength < slot->strength)
                slot = &w;
    if (!slot)
        return;

    if (!slot->used)
    {
        slot->used = true;
        slot->birth = now;
    }
    slot->pos = pos;
    slot->radius = std::max(radius, 0.02f);
    slot->strength = strength;
    slot->vel = vel;
    slot->id = id;
    slot->touched = now;
}

// Shortest signed distance between two headings, radians.
static float da_wrap_pi(float a)
{
    a = fmodf(a + PI, PI_MUL_2);
    if (a < 0.f)
        a += PI_MUL_2;
    return a - PI;
}

// Cox & Munk 1954 below their own data. The clean fit's 0.003 intercept is the ocean's residual
// swell, and a pond has none: under about half a metre a second the wind raises no capillary at
// all and the water is a mirror, so the threshold comes off the wind and the law fades in over
// the next metre a second instead of starting from the intercept. da_w_mss in
// da_water_common.h is the same law per pixel and must stay so.
static float water_mss(float U)
{
    const float u = std::max(U - 0.5f, 0.f);
    return 1e-4f + (0.003f + 5.12e-3f * u) * std::min(u, 1.f);
}

// The sea the wind has raised, and the eight waves seeded from it. Solved once per frame on the
// REAL frame dt, not the wind service's fixed tick: the low-pass below is the sea's own inertia
// and has nothing to do with the noise clock.
void CEnvironment::water_tick(float delta)
{
    // A sea has inertia; a gust does not resize it. Three seconds of low-pass over the 10 m
    // wind is exactly what separates "the wind has picked up" from "a gust went through".
    // Frozen wind (screenshots) must not let the sea keep growing under a still frame.
    const float step = eff_wind_freeze ? 0.f : delta;
    const float u10 = std::max(WindSpeedMs(), 0.f);
    if (water_wind_lp < 0.f)
        water_wind_lp = u10; // first frame: no swell-in from zero
    else
        water_wind_lp += (u10 - water_wind_lp) * (1.f - expf(-step / 3.f));
    const float U = std::max(water_wind_lp, 0.f);

    // Fetch: the run of open water the wind gets to work over, taken from the water body the
    // camera is standing at and projected onto the current heading - a pond that is long
    // east-west genuinely gets choppier in an easterly. Picking the NEAREST body rather than
    // one box around every puddle on the level matters: a map-wide box hands a two-metre pool
    // the fetch of a lake, and fetch is most of what sets the wave size.
    const float dx = _sin(eff_wind_dir);
    const float dz = _cos(eff_wind_dir);
    float fetch = 120.f;
    if (water_body_count > 0)
    {
        const Fvector& cam = Device.vCameraPosition;
        int best = 0;
        float best_d = flt_max;
        for (int i = 0; i < water_body_count; ++i)
        {
            Fvector near_pt;
            water_bodies[i].extent.getcenter(near_pt);
            near_pt.x = clampr(cam.x, water_bodies[i].extent.vMin.x, water_bodies[i].extent.vMax.x);
            near_pt.z = clampr(cam.z, water_bodies[i].extent.vMin.z, water_bodies[i].extent.vMax.z);
            const float ddx = cam.x - near_pt.x;
            const float ddz = cam.z - near_pt.z;
            const float d = ddx * ddx + ddz * ddz;
            if (d < best_d)
            {
                best_d = d;
                best = i;
            }
        }
        const Fbox& e = water_bodies[best].extent;
        water_mean_depth = water_bodies[best].depth;
        fetch = (e.vMax.x - e.vMin.x) * _abs(dx) + (e.vMax.z - e.vMin.z) * _abs(dz);
    }
    // The body's box is the fetch a lake would have; the profile says what the water in it
    // really offers. A marsh is a lattice of reed islands - half a kilometre by the box, tens
    // of metres by the water - and that difference is a slow swell against no swell at all.
    const SWaterProfile& wprof = water_profile[0];
    const float X = clampr(fetch, 5.f, std::max(wprof.fetch_max, 5.f));
    const float damp = clampr(wprof.wave_damp, 0.f, 1.f);

    // Fetch-limited growth (Hasselmann/JONSWAP). Every term is a product of the friction
    // velocity, so dead calm gives zero rather than a division by one: U = 0 -> ust = 0 -> Hs
    // and lambda collapse to zero, and the floors below keep both finite and positive.
    constexpr float cbrt_g = 2.140703f; // cbrt(9.81)
    const float cd = 0.001f * (1.1f + 0.035f * U);
    const float ust = U * _sqrt(cd);
    const float hs = std::max(0.0413f * ust * _sqrt(X / 9.81f), 0.0005f) * damp;
    const float lam = std::max(0.0898f * powf(X * ust, 2.f / 3.f) / cbrt_g, 0.06f);
    // Cox & Munk for everything too small to be a wave, through the calm threshold
    // (water_mss), then the profile's damping on the whole budget: vegetation and a still
    // backwater take slope out of waves, detail and roughness alike. The shader re-derives
    // the same law per pixel against the local gust tongue and the scum.
    const float mss = water_mss(U) * damp;

    // ---- The wave table. -------------------------------------------------------------------
    // The shader reads a heading as dir = (cos theta, sin theta) in world XZ, while the service
    // publishes the wind as da_wind_state.xy = (sin, cos) of eff_wind_dir. Same vector, quarter
    // turn between the two conventions - hence the pi/2 minus, and not a negation.
    const float heading = PI_DIV_2 - eff_wind_dir;
    // Re-seeded only when the sea has actually moved. It is a handful of sqrts, but a re-seed
    // every frame would also walk the phases, which the eye reads as the whole surface twitching.
    // The thresholds are deliberately coarse. A re-seed moves every wavenumber, and the shader
    // runs its phase off an absolute clock, so even a one per cent change lands as a jump of
    // whole cycles once that clock is minutes old. The carry-over below cancels the temporal
    // part of it, but the spatial k*x term still shifts, and doing that on every gust would
    // make the surface crawl. A re-seed should be a change in the weather, not a gust.
    const bool moved = water_seed_wind < 0.f ||
        _abs(U - water_seed_wind) > 0.20f * std::max(water_seed_wind, 1.0f) ||
        _abs(da_wrap_pi(heading - water_seed_dir)) > deg2rad(15.f);

    if (moved)
    {
        water_seed_wind = U;
        water_seed_dir = heading;

        // The amplitudes come out of the SLOPE budget, not out of the wave height. This surface
        // is drawn as a normal and never as geometry, and the Cox-Munk mss above is the slope
        // variance of the whole real surface - so the explicit waves may take only a share of
        // it and what is left is the width of the specular lobe.
        //
        // Distributing the wave HEIGHT across the bands instead, with a steepness cap on top,
        // is the obvious thing to write and it is wrong: eight bands then sum to several times
        // the real slope variance, the shader's roughness budget (mss - explicit - detail) goes
        // negative, and the sun collapses to a mirror line on a visibly choppy pond.
        //
        // Equal variance per band is the Phillips equilibrium result - a k^-3 slope spectrum is
        // flat per octave - so var_i = target/n and A_i = sqrt(2*var_i)/k_i. Hs stays what the
        // fetch relations said and is published for the one thing that wants a HEIGHT: the
        // width of the shoreline foam band.
        // How many rows the preset lets the surface evaluate. The slope budget is spread over
        // the rows that survive rather than the tail being simply dropped, so a lower tier is a
        // coarser sea and not a calmer one.
        //
        // The band set also stops where the waves stop being renderable. At a light breeze the
        // peak is already only 6-9 cm long and eight geometric bands run down to 8 mm - a
        // wavelength no pixel of this surface can resolve at any distance, so all it can add is
        // aliasing. Everything below the floor belongs in the Cox-Munk roughness instead, which
        // is exactly where it goes: fewer bands means each surviving one carries more of the
        // slope budget, and what is never spent stays in the specular lobe.
        constexpr float lam_min = 0.10f;
        int n_waves = clampr(ps_r__water_waves, 1, 8);
        while (n_waves > 0 && lam * powf(0.75f, float(n_waves - 1)) < lam_min)
            --n_waves;

        // The bands that fit above the floor, and the rows the preset allows. A young sea at a
        // short fetch has one or two bands at most, and one sinusoid per band is corduroy - long
        // parallel crests marching in step, which no real chop has ever looked like. The rows
        // are therefore bands TIMES headings: the surplus rows re-draw the same band at other
        // headings across the wind, which is what a young sea's wide directional spread IS, so
        // eight rows are always eight crossing trains whatever the fetch.
        const int n_bands = n_waves;
        const int n_rows = n_bands ? clampr(ps_r__water_waves, 1, 8) : 0;

        float wnum[8];
        for (int b = 0; b < n_bands; ++b)
            wnum[b] = PI_MUL_2 / (lam * powf(0.75f, float(b)));
        // Equal variance per ROW: var = target / n_rows and A = sqrt(2 var) / k.
        const float kA = n_rows ? _sqrt(2.f * (0.5f * mss / float(n_rows))) : 0.f;
        // Every row carries the same slope amplitude by construction (A = kA / k), so the
        // steepness sum is just n*kA. The cap is a safety net rather than a shaping term: at
        // these slope budgets it does not bind below a full gale, but without it a summed sine
        // surface folds its normals over and turns inside out.
        const float steep = kA * float(n_rows);
        const float cap = steep > 0.8f ? 0.8f / steep : 1.f;
        // Where the copies of a band sit across the wind, in units of the band's own spread:
        // the first pair a full spread either side, the rest filling in between, so the crests
        // cross at every angle the spread allows instead of at one.
        static constexpr float across[8] = {1.f, -1.f, 0.45f, -0.45f, 0.8f, -0.8f, 0.2f, -0.2f};

        ZeroMemory(water_wave, sizeof(water_wave));
        for (int r = 0; r < n_rows; ++r)
        {
            // Interleaved, so the first rows already cover every band; widening sharply for the
            // short bands, which genuinely ride far off the mean heading.
            const int b = r % n_bands;
            const int j = r / n_bands;
            const float spread = (0.35f + 0.10f * float(b)) * across[j];
            float* row = &water_wave[r / 4].m[r % 4][0];
            row[0] = heading + spread;
            row[1] = wnum[b];
            row[2] = (kA / wnum[b]) * cap;

            // Carry the phase across the re-seed. The shader evaluates
            // k*dot(dir,xz) - omega*t + phi against an absolute clock, so a new omega on an old
            // t moves the crest by (d_omega * t) radians - tens of whole cycles once the level
            // has been up a few minutes, which reads as the surface flashing. Choosing phi so
            // that (-omega*t + phi) is unchanged makes the temporal term continuous by
            // construction; only the spatial k*x shift is left, and that reads as the sea
            // changing rather than as a cut.
            const float omega = _sqrt(9.81f * wnum[b]);
            const float phi0 = float(r) * 2.399963f; // golden angle: the crests never line up
            const float carry = water_seed_omega[r] > 0.f ? (omega - water_seed_omega[r]) * eff_wind_time : 0.f;
            row[3] = da_wrap_pi(phi0 + carry);
            water_seed_omega[r] = omega;
        }
        water_wave_count = float(n_rows);
    }

    water_sea.set(hs, lam, mss, U);
    water_body.set(water_mean_depth, X, 0.f, water_wave_count);

    // ---- Rain rate: the one number the whole rain system is parameterised on. ---------------
    // DA weathers author a 0..1 density, which says nothing physical. R = 25*density^1.5 puts a
    // full-density storm at 25 mm/h (heavy rain) and a drizzle at a fraction of a millimetre,
    // which is what the drop size distribution, the splash rate, the puddle fill rate and the
    // extinction below all want as their input.
    const float rain_d = clampr(CurrentEnv.rain_density, 0.f, 1.f);
    rain_rate_mmh = 25.f * powf(rain_d, 1.5f);
    rain_ext_km = rain_rate_mmh > 0.f ? 0.312f * powf(rain_rate_mmh, 0.67f) : 0.f;

    // ---- Camera against the baked field. ----------------------------------------------------
    // Nothing else knows whether the eye is under water: the surface is a forward pass and the
    // combine has no other way to ask. Levels with no field simply never go under.
    const Fvector& eye = Device.vCameraPosition;
    const float surf = water_surface_at(eye);
    if (surf > -flt_max && eye.y < surf)
    {
        eye_under_depth = surf - eye.y;
        eye_under_surface = surf;
    }
    else
    {
        eye_under_depth = 0.f;
        eye_under_surface = 0.f;
    }

    // ---- The ripple window. -----------------------------------------------------------------
    // Snapped to whole texels: a window that slides continuously resamples the field every
    // frame and smears it into mush (the known defect in OpenMW's version). Solved here so the
    // sim pass and every CPU consumer land on the same texel grid.
    // Against the size the target pair was actually created at, not the preset's wish.
    const int rip_texels = ps_r__water_ripple_active;
    if (rip_texels > 0)
    {
        const float texel = water_ripple_window / float(rip_texels);
        water_ripple_win.set(floorf(eye.x / texel + 0.5f) * texel, floorf(eye.z / texel + 0.5f) * texel,
            water_ripple_window, 1.f / float(rip_texels));
        water_ripple_speed = 0.6f;
    }
    else
    {
        water_ripple_win.set(0.f, 0.f, 0.f, 0.f);
        water_ripple_speed = 0.9f;
    }
}

void CEnvironment::wind_reseed(float seed)
{
    eff_wind_seed = seed < 0.f ? ::Random.randF(0.f, 4096.f) : seed;
    eff_wind_time = 0.f;
    eff_wind_tick_acc = 0.f;
    for (auto& g : wind_gusts)
        g.used = false;
    eff_wind_gust_event = 0.f;
    g_wind_rng.state = 0x9E3779B9u ^ u32(eff_wind_seed * 977.f);
    if (g_wind_rng.state == 0)
        g_wind_rng.state = 1;
    Msg("* [wind] reseeded: %.1f", eff_wind_seed);
}

// One fixed step of the wind service. Everything that is a function of time or accumulates
// lives here and sees the same dt on every machine; the per-frame packing of motors stays in
// UpdateEffectiveWind.
void CEnvironment::wind_tick(float delta)
{
    eff_wind_time += delta;
    const float t = eff_wind_time + eff_wind_seed;
    // Weather ceiling. Two hard facts from the field (wind_dbg on real DA configs):
    //  * wind_velocity is a LEGACY 0..1000-ish scale (typical live values 10..500), not m/s -
    //    dividing by 20 saturated every nonzero weather to "hurricane" and erased the range;
    //  * 1028 of ~1100 DA weather entries say wind_velocity = 0 - their storms are authored as
    //    rain and clouds only, so a config-only ceiling turned a rainstorm into dead calm.
    // The ceiling is the strongest of three voices: the authored value on its own curve
    // (blowout fx set 100..500 and keep their gale), the PER-WEATHER PROFILE mapped from the
    // cycle name (dead_air_x64_wind.ltx - a storm cycle IS windy even though its config only
    // says "rain and clouds"), and the wind implied by precipitation as the floor under both.

    const float base_cfg = powf(clampr(CurrentEnv.wind_velocity / 400.f, 0.f, 1.f), 0.8f);
    // Rain implies wind; nothing else does. This used to carry an unconditional +0.10 floor,
    // which is what made "dead calm" impossible: no weather, no console, nothing could put the
    // ceiling below it.
    const float base_implied = 0.58f * clampr(CurrentEnv.rain_density, 0.f, 1.f);
    // The profile switches as a step on the cycle boundary - low-pass it so a new weather
    // swells the wind over ~half a minute instead of snapping the whole world at once.
    const float profile = weather_wind_profile();
    if (wind_profile_smooth < 0.f)
        wind_profile_smooth = std::max(profile, 0.f); // first frame: no swell-in from zero
    wind_profile_smooth +=
        (std::max(profile, 0.f) - wind_profile_smooth) * (1.f - expf(-delta / 12.f));
    // Who decides how hard it blows.
    //
    // The authored curve cannot: DA's own configs put wind_velocity 350 on a CLEAR noon and 400
    // on a storm, so read at face value a clear day blows nine tenths as hard as a gale - and
    // taking the strongest of the three voices meant the authored number always won. That is
    // what "in dead calm the trees still sway" was, at the source.
    //
    // The cycle NAME knows what weather this is, and the profile table says what that weather
    // does (dead_air_x64_wind.ltx). So the profile sets the MAGNITUDE and the authored curve
    // only shapes it: a time key that says "windier" still reads as windier within the day, it
    // just cannot promote a clear sky to Beaufort 6. A cycle the table has no key for - a
    // weather effect, most likely - has no profile, and there the authored value is meant
    // literally and is all we have.
    float base;
    if (eff_wind_force >= 0.f)
        base = eff_wind_force;
    else if (profile >= 0.f)
        base = std::max(wind_profile_smooth * (0.70f + 0.60f * base_cfg), base_implied);
    else
        base = std::max(base_cfg, base_implied);
    eff_wind_base = base;

    // Three time scales, deliberately incommensurable so the pattern never visibly loops:
    // a minute-scale trend (lulls and freshenings), ~14 s waves, and a fast layer that only
    // matters when it spikes - that spike IS a discrete gust. Calm weather raises the spike
    // threshold (gusts become rare), storms lower it (gusts come often). Field rule: the
    // SPEED wobble must read clearly faster than the heading wander (real turbulence pumps
    // the speed on tens of seconds while the direction only meanders over minutes), so the
    // waves - not the trend - carry the dominant weight below.
    const float n_trend = wind_vnoise(t * (1.f / 170.f) + 3.7f);
    const float n_wave = wind_vnoise(t * (1.f / 14.f) + 17.3f);
    const float n_fast = wind_vnoise(t * (1.f / 5.5f) + 29.1f);
    const float gust_thr = 0.55f + 0.25f * (1.f - base);
    float gust_ev = clampr((n_fast - gust_thr) / std::max(1.f - gust_thr, 0.05f), 0.f, 1.f);

    // ---- Discrete gust events. -------------------------------------------------------------
    // Poisson arrivals: a calm day gets one every couple of minutes, a storm several a minute.
    // Each is a raised-cosine envelope with a quicker attack than release (a gust hits, then
    // lets go), 3-10 s long, stronger in stronger weather. Three may overlap.
    {
        const float rate = (1.f / 150.f) + base * (1.f / 25.f); // per second
        if (g_wind_rng.next() < rate * delta)
        {
            for (auto& g : wind_gusts)
            {
                if (g.used)
                    continue;
                g.used = true;
                g.start = eff_wind_time;
                g.duration = 3.f + 7.f * g_wind_rng.next();
                g.amplitude = (0.25f + 0.35f * g_wind_rng.next()) * (0.5f + base);
                break;
            }
        }
        float sum = 0.f;
        for (auto& g : wind_gusts)
        {
            if (!g.used)
                continue;
            const float u = (eff_wind_time - g.start) / g.duration;
            if (u >= 1.f)
            {
                g.used = false;
                continue;
            }
            // u^0.7 skews the peak early: attack ~40 % of the duration, release the rest.
            const float phase = powf(clampr(u, 0.f, 1.f), 0.7f);
            sum += g.amplitude * (0.5f - 0.5f * cosf(phase * PI_MUL_2));
        }
        eff_wind_gust_event = clampr(sum, 0.f, 1.f);
        gust_ev = clampr(gust_ev + eff_wind_gust_event, 0.f, 1.f);
    }

    // Variability inside the weather envelope: a calm day swings between near-nothing and its
    // own light ceiling, a storm between fresh and violent. Balanced so the AVERAGE sits near
    // the weather's nominal strength (multiplying three attenuating layers - variability, the
    // spatial field, the consumer envelope - once collapsed a storm into a flat calm).
    // The floor rises with the weather: a storm may breathe, but it never sinks to a flat
    // calm - that read as "the weather has no effect at all" in the field.
    eff_wind_var = clampr(0.30f + 0.35f * base + n_trend * 0.35f + n_wave * 0.55f + gust_ev * 0.55f,
        0.f, 1.35f + 0.15f * base);
    eff_wind_norm = clampr(base * eff_wind_var, 0.f, 1.25f);

    // Gustiness: the fast layers, with the legacy Perlin mixed in - blowout zones override
    // wind_strength_factor directly, and that surge must keep reaching every consumer.
    eff_wind_gust = clampr(0.5f * wind_strength_factor + n_wave * 0.25f + gust_ev * 0.75f, 0.f, 1.f);

    eff_wind_gust_smooth += (eff_wind_gust - eff_wind_gust_smooth) * (1.f - expf(-delta / 1.5f));

    // Direction, three time scales on top of the weather's authored heading:
    //  * synoptic drift - over ~ten minutes the WHOLE wind swings tens of degrees, the way
    //    real fronts turn the wind (field report: "the global heading never changes");
    //  * bounded wander - broad and lazy in light air (real light wind meanders), tight in
    //    strong wind (a storm holds its line);
    //  * fine jitter. The per-PLACE deviation (eddies) lives in the shader field's z channel.
    const float drift = (wind_vnoise(t * (1.f / 540.f) + 71.3f) * 2.f - 1.f) * deg2rad(70.f);
    const float wander_amp = deg2rad(35.f - 22.f * base);
    const float wander = (wind_vnoise(t * (1.f / 30.f) + 41.7f) * 2.f - 1.f) * wander_amp +
        (wind_vnoise(t * (1.f / 8.f) + 53.9f) * 2.f - 1.f) * deg2rad(8.f);
    // The engine's heading convention (Fvector::setHP, the stock rain and the sun's cloud
    // scroll) points a heading h along (-sin h, 0, cos h); every consumer of this service
    // builds its vector as (sin, 0, cos), so the authored heading is negated here once and
    // the weather blows the way its author saw it blow in the rain.
    eff_wind_dir = -CurrentEnv.wind_direction + drift + wander;
    // Aloft: the authored heading plus the slow synoptic drift, then the Ekman veer - in the
    // northern hemisphere the wind turns clockwise with height, about 25 degrees by the cloud
    // deck. None of the surface wander or jitter reaches it.
    eff_wind_dir_aloft = -CurrentEnv.wind_direction + drift + deg2rad(25.f);

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

    // The wind aloft: the surface-layer log profile evaluated at the cloud deck (1.5 km) over
    // the level's roughness, capped absolutely - the profile is unbounded as a ratio and a
    // storm would otherwise put the clouds at highway speed. The cloud deck and its shadow
    // travel this many METRES per second along eff_wind_dir_aloft; the accumulator wraps far
    // out only to guard against infinity.
    eff_wind_aloft_ms = std::min(25.f, WindSpeedMs() * da_wind_profile(1500.f, eff_wind_z0));
    eff_cloud_run = fmodf(eff_cloud_run + std::max(eff_wind_aloft_ms, 0.4f) * delta, 1.e6f);

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

}

void CEnvironment::UpdateEffectiveWind()
{
    // Per-session seed: every noise is a pure function of the service clock, which starts
    // near zero every launch - so every session used to OPEN with the same wind heading and
    // the same first gusts. One offset shifts the whole session elsewhere in the field.
    // -wind_seed N pins it (benchmarks, screenshot comparisons); so does the console.
    if (eff_wind_seed < 0.f)
    {
        float pinned = g_wind_seed_override;
        if (pcstr p = strstr(Core.Params, "-wind_seed "))
            pinned = float(atof(p + 11));
        eff_wind_seed = pinned >= 0.f ? pinned : ::Random.randF(0.f, 4096.f);
        g_wind_rng.state = 0x9E3779B9u ^ u32(eff_wind_seed * 977.f);
        if (g_wind_rng.state == 0)
            g_wind_rng.state = 1;
        Msg("* [wind] seed %.1f%s", eff_wind_seed, pinned >= 0.f ? " (pinned)" : "");
        if (g_wind_force_override >= 0.f)
        {
            eff_wind_force = g_wind_force_override;
            Msg("* [wind] force pinned at %.2f", eff_wind_force);
        }
    }

    // Fixed 60 Hz ticks. A frame longer than eight ticks (a load hitch) drops its remainder
    // rather than fast-forwarding the wind through it.
    float delta = Device.fTimeDelta;
    if (delta < 0.f || delta > 1.f)
        delta = 0.03f;
    if (!eff_wind_freeze)
    {
        constexpr float tick = 1.f / 60.f;
        eff_wind_tick_acc += delta;
        int steps = 0;
        while (eff_wind_tick_acc >= tick && steps < 8)
        {
            wind_tick(tick);
            eff_wind_tick_acc -= tick;
            ++steps;
        }
        if (steps == 8)
            eff_wind_tick_acc = 0.f;
    }

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
            // Expanding blast ring. Field lesson: at 14 m/s the front crossed a tuft in a
            // few FRAMES - the eye never caught it and grenades read as "nothing happened".
            // 10 m/s keeps the front readable; the shader reconstructs a per-tuft spring-back
            // BEHIND the front from this same speed (da_wind_motors.h) - keep them in sync.
            const float age = now - m.touched;
            // 22 m/s: the real shock is transonic - invisible - so this is the slowest speed
            // that still reads as a POP rather than a pond ripple (10 m/s did). Kept in sync
            // with the shader's spring-back tau (da_wind_motors.h).
            ring_r = 22.f * age;
            ring_w = 1.0f + age * 1.5f;
            // The amplitude packed here is the PEAK strength only: the spatial 1/R falloff,
            // the edge fade and the per-root spring-back are all computed in the shader from
            // each root's own distance. That lets the motor outlive its front: the ring edge
            // goes quiet at m.radius, while roots hit earlier finish their own oscillation.
            // Kill only after the LAST hit root has settled (spring tail ~0.85 s).
            amp = m.strength;
            if (age > m.radius / 22.f + 0.85f)
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

        // Live motors pack into rows 0..n-1 and the shader walks exactly n rows. Packing by
        // slot index made the row count "the highest used slot", so one press in slot 7 cost
        // every vertex eight fetch-and-compare iterations for one real motor.
        if (!m.used)
            continue;
        const u32 row = highest++;

        Fmatrix& P = wind_motor_pos[row / 4];
        Fmatrix& A = wind_motor_par[row / 4];
        float* prow = &P.m[row % 4][0];
        float* arow = &A.m[row % 4][0];
        prow[0] = m.pos.x; prow[1] = m.pos.y; prow[2] = m.pos.z; prow[3] = m.used ? m.radius : 0.f;
        arow[0] = m.used ? amp : 0.f;
        // Line motors carry their direction where radial ones carry the ring shape.
        arow[1] = line ? m.dir.x : ring_r;
        arow[2] = line ? m.dir.y : std::max(ring_w, 0.05f);
        // w disambiguates the three motor kinds for the shader: line = 1 + trace slope
        // (slope clamped well above -0.5 so the >0.5 test never breaks), blast = -1
        // (the shader shapes its own falloff), press = 0.
        arow[3] = line ? 1.f + m.dir_y : (m.type == EWindMotor::impulse ? -1.f : 0.f);
    }
    // Rows past the live count hold whatever the last frame packed; the CPU sampler walks all
    // eight and gates on radius, so they must read as empty.
    for (u32 row = highest; row < wind_motor_count; ++row)
    {
        wind_motor_pos[row / 4].m[row % 4][3] = 0.f;
        wind_motor_par[row / 4].m[row % 4][0] = 0.f;
    }
    wind_motor_active = float(highest);

    // ---- Water impact spots: simulate and pack for the puddle shader. ----------------------
    // Envelopes live on the CPU (the wind-motor pattern): the shader only draws what the
    // rows say, so every kind dies at zero amplitude by construction - no snap, ever.
    u32 wh_highest = 0;
    for (u32 i = 0; i < water_hit_count; ++i)
    {
        SWaterHit& h = water_hits[i];
        float amp = 0.f, ring_r = 0.f;
        if (h.used && h.kind == EWaterHit::ring)
        {
            // A ring on water lives until it is too faint to see, not for a number of seconds:
            // the crest thins as the circle grows - energy over the circumference and the
            // packet stretching as it disperses, together about 1/r - and viscosity takes a
            // minute to matter at these wavelengths. The slot's radius no longer ends it; it
            // sizes the splash. A ring the field carries runs at the field's own speed, so the
            // two fronts stay one front where the window ends; a blast the field never saw is
            // a bore and runs at 4.5.
            const float age = now - h.birth;
            const bool big = h.radius > 2.5f;
            const float speed = h.crater > 0.f ? water_ripple_speed : (big ? 4.5f : 0.9f);
            ring_r = speed * age;
            amp = expf(-age / 60.f) / (1.f + ring_r * 1.5f);
            if (amp < 0.02f)
                h.used = false;
        }
        else if (h.used)
        {
            // Drain: the blast threw the water out. Hold dry, then seep back - rain refills
            // it quickly, dry weather takes a minute and a half. The dark wet ground under
            // it stays untouched (the shader's damp term never sees the drain).
            const float age = now - h.birth;
            if (age <= 6.f)
                amp = 1.f;
            else
            {
                const float rain = clampr(CurrentEnv.rain_density, 0.f, 1.f);
                const float rate = rain > 0.05f ? (0.030f + 0.09f * rain) : (1.f / 90.f);
                amp = 1.f - (age - 6.f) * rate;
            }
            if (amp < 0.02f)
                h.used = false;
        }

        h.amp = amp;

        // Packed compactly, like the wind motors: the puddle shader walks exactly the live
        // count, not the highest used slot.
        if (!h.used)
            continue;
        const u32 row = wh_highest++;

        Fmatrix& P = water_hit_pos[row / 4];
        Fmatrix& A = water_hit_par[row / 4];
        float* prow = &P.m[row % 4][0];
        float* arow = &A.m[row % 4][0];
        prow[0] = h.pos.x; prow[1] = h.pos.y; prow[2] = h.pos.z; prow[3] = h.radius;
        arow[0] = amp;
        arow[1] = ring_r;
        arow[2] = (h.kind == EWaterHit::drain) ? 1.f : 0.f;
        arow[3] = h.crater;
    }
    for (u32 row = wh_highest; row < water_hit_count; ++row)
    {
        water_hit_pos[row / 4].m[row % 4][3] = 0.f;
        water_hit_par[row / 4].m[row % 4][0] = 0.f;
    }
    water_hit_active = float(wh_highest);

    // ---- Wading wakes: simulate and pack for the ripple field. -----------------------------
    // A wake is fed for as long as its source keeps moving through the water, so the envelope
    // here is a RELEASE and not a lifetime: while the source refreshes the slot the wake sits
    // at full strength, and once it stops it fades to zero in a fraction of a second. Computed
    // on the CPU for the same reason the impact slots are - a slot always dies at zero.
    u32 wk_highest = 0;
    for (u32 i = 0; i < u32(water_wake_count); ++i)
    {
        SWaterWake& w = water_wakes[i];
        if (!w.used)
            continue;

        // A foot lifting out of the water lets its trough go in a tenth of a second; the hold
        // before the fade only bridges a frame or two the feed may skip.
        constexpr float wake_release = 0.15f;
        const float idle = now - w.touched;
        const float fade = 1.f - clampr((idle - 0.05f) / wake_release, 0.f, 1.f);
        if (fade <= 0.f)
        {
            w.used = false;
            continue;
        }

        const u32 row = wk_highest++;
        Fmatrix& P = water_wake_pos[row / 4];
        Fmatrix& A = water_wake_par[row / 4];
        float* prow = &P.m[row % 4][0];
        float* arow = &A.m[row % 4][0];
        prow[0] = w.pos.x; prow[1] = w.pos.y; prow[2] = w.pos.z; prow[3] = w.radius;
        arow[0] = w.strength * fade;
        arow[1] = w.vel.x;
        arow[2] = w.vel.y;
        arow[3] = 0.f;
    }
    for (u32 row = wk_highest; row < u32(water_wake_count); ++row)
    {
        water_wake_pos[row / 4].m[row % 4][3] = 0.f;
        water_wake_par[row / 4].m[row % 4][0] = 0.f;
    }
    water_wake_active = float(wk_highest);

    // ---- The water model: the sea this wind has raised. -------------------------------------
    water_tick(delta);

    // ---- Self-test blast ring (wind_dbg 2/3): a blast motor spawns 18 m ahead of the camera
    // every 5 s - a realistic grenade-throw distance, so the test verifies the ring's
    // READABILITY at the range players actually watch it from. Level 3 additionally sprays
    // 24 fragment shot-motors from the same point in the same frame - the exact F1 scenario
    // whose fragments once evicted their own freshly born ring from the motor pool.
    if (ps_e_wind_dbg > 1)
    {
        static float next_test_blast = 0.f;
        if (now >= next_test_blast)
        {
            next_test_blast = now + 5.f;
            Fvector p = Device.vCameraPosition;
            p.mad(Device.vCameraDirection, 10.f); // realistic throw range for the ~8 m ring
            p.y -= 1.5f;
            wind_motor_impulse(p, 8.f, 1.15f); // F1-equivalent charge (blast_r 8 = ring reach)
            if (ps_e_wind_dbg > 2)
                for (u32 fi = 0; fi < 24; ++fi)
                {
                    const float a = float(fi) * (PI_MUL_2 / 24.f);
                    Fvector fd;
                    fd.set(_sin(a), -0.1f, _cos(a));
                    wind_motor_shot(p, fd, 25.f, 0.5f);
                }
        }
    }

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
            Msg("* [wind] vel=%.1f base=%.2f norm=%.2f (%.1f m/s) var=%.2f gust=%.2f ev=%.2f dir=%.0f deg "
                "aloft=%.1f m/s @%.0f deg | motors=%u green=%.2f rain=%.2f | t=%.1f",
                CurrentEnv.wind_velocity, eff_wind_base, eff_wind_norm, WindSpeedMs(), eff_wind_var,
                eff_wind_gust, eff_wind_gust_event, rad2deg(eff_wind_dir), eff_wind_aloft_ms,
                rad2deg(eff_wind_dir_aloft), live, wind_veg_green, CurrentEnv.rain_density, eff_wind_time);
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
