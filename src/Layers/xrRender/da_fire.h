#pragma once

#include "ParticleEffect.h"

namespace xray::render::RENDER_NAMESPACE::PS
{
// A fire whose flame is a volume marched in a pixel shader (da_fire.ps) instead of a sprite
// flipbook, with a smoke plume simulated here and drawn as eroded billboards (da_smoke.ps).
// It stands in for a CParticleEffect: [shader_fire] in dead_air_x64_fire.ltx maps an effect
// name to a preset section, CModelPool::CreatePE builds this class for such a name, and the
// campfire object, the group it plays and the scripts around it change nothing.
struct SDaFirePreset
{
    shared_str name;
    float base_height{0.72f};   // metres above the effect origin where the flame starts
    float radius{0.32f};        // fuel bed radius, m (D = 2 radius)
    float height{1.2f};         // calm mean flame height, m
    float intensity{1.f};       // emission gain
    float lean{1.f};            // wind tilt gain (1 = the AGA correlation)
    bool smoke{true};
    float smoke_rate{14.f};     // puffs per second
    float smoke_life{16.f};     // seconds, +-25%
    float smoke_alpha{0.28f};   // opacity of a fresh puff
    float smoke_size{0.2f};     // radius of a fresh puff, m
    float smoke_grey{0.5f};     // 0 = light grey (a well-burning fire), 1 = dark resinous soot
    float heat_kw{100.f};       // convective heat release, drives the plume rise

    // The preset [shader_fire_<name>]; nullptr for "off" (the effect draws nothing) or unknown.
    static const SDaFirePreset* find(const shared_str& name);
};

class CDaFireEffect final : public CParticleEffect
{
    struct SPuff
    {
        Fvector pos;
        Fvector vel;
        float age;
        float life;
        float r0;
        float seed;
        float rot;
        float spin;
        float ou_x, ou_z; // Ornstein-Uhlenbeck turbulence state, m/s
    };

    const SDaFirePreset* m_preset{};
    float m_time{};
    float m_dying{};            // 0 while burning; seconds since the deferred stop otherwise
    Fvector m_wind{};           // wind at the flame, m/s, shelter applied
    float m_wind_exposure{1.f};
    float m_wind_stamp{1.f};
    float m_tilt{};             // smoothed tan(theta) of the flame axis
    Fvector2 m_wdir{};          // unit wind heading, world XZ (kept when the wind dies)
    float m_gust{};
    float m_puff_acc{};
    xr_vector<SPuff> m_puffs;
    ref_shader m_smoke_shader;
    ref_geom m_smoke_geom;
    float m_seed{};

    Fvector origin() const;
    Fvector flame_base() const;
    float flame_length() const;
    float wind_speed() const;
    Fvector flame_tip() const;
    void update_smoke(float dt, const Fvector& tip);
    void render_flame(CBackend& cmd_list, float fade);
    void render_smoke(CBackend& cmd_list);

public:
    CDaFireEffect() = default;
    ~CDaFireEffect() override;

    BOOL Compile(CPEDef* def) override;
    void OnFrame(u32 dt) override;
    void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override;
    void OnDeviceCreate() override;
    void OnDeviceDestroy() override;
    void Play() override;
    void Stop(BOOL bDefferedStop = TRUE) override;
    float GetTimeLimit() override { return -1.f; }
    u32 ParticlesCount() override { return u32(m_puffs.size()); }
};
} // namespace xray::render::RENDER_NAMESPACE::PS
