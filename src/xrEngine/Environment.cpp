#include "stdafx.h"
#pragma hdrstop

#ifndef _EDITOR
#include "Render.h"
#endif

#include "Environment.h"
#include "xr_efflensflare.h"
#include "Rain.h"
#include "thunderbolt.h"
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
    if (bWFX)
        return false;
    if (name.size())
    {
        auto it = WeatherFXs.find(name);
        R_ASSERT3(it != WeatherFXs.end(), "Invalid weather effect name.", name.c_str());
        R_ASSERT2(CurrentWeather && Current[0] && Current[1],
            "Cannot start a weather effect before the base weather pair is selected");

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

void CEnvironment::OnFrame()
{
    ZoneScoped;

    if (!g_pGameLevel)
        return;

    lerp();

    PerlinNoise1D->SetFrequency(wind_gust_factor * MAX_NOISE_FREQ);
    wind_strength_factor = clampr(PerlinNoise1D->GetContinious(Device.fTimeGlobal) + 0.5f, 0.f, 1.f);

    eff_LensFlare->OnFrame(CurrentEnv, fTimeFactor);
    eff_Thunderbolt->OnFrame(CurrentEnv);
    eff_Rain->OnFrame();
}
