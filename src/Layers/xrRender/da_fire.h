#pragma once

#include "ParticleEffect.h"
#include <memory>

namespace xray::render::RENDER_NAMESPACE
{
class dx113DFluidData;
}

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
    bool fluid{true};           // simulate on the 3D fluid grid when near (r__fire_fluid)

    // The fluid campfire. Lengths in metres, rates per second; the grid works in cells and
    // steps and CDaFireEffect converts. Everything here is a key of the same ltx section
    // with a fluid_ prefix, so the look can be tuned without a rebuild.
    float fl_base{0.12f};       // the fuel disc's centre above the effect origin
    float fl_bed{0.35f};        // how far above and below it a surface still burns
    float fl_radius{0.f};       // the disc's radius; 0 = radius * 1.15
    float fl_cell{0.035f};      // one cell
    float fl_ignition{0.08f};   // the temperature a cell needs before it burns
    float fl_burn{6.f};         // burn per degree over the ignition point
    float fl_fuel_per_burn{1.f};
    float fl_t_per_burn{1.5f};
    float fl_smoke_per_burn{0.6f};
    float fl_cooling{2.2f};     // radiative cooling; this is what sets the flame's height
    float fl_fuel{0.8f};        // how much fuel the burning surface holds
    float fl_couple{4.f};       // how fast the surface hands it to the gas above
    float fl_expansion{0.5f};   // the volume the reaction makes; fullness and puffing
    float fl_buoyancy{20.f};    // m/s2 on gas at the core temperature
    float fl_inject{0.35f};     // m/s of gas off the burning surface, along its normal
    float fl_wind_relax{2.f};   // how fast a parcel takes the wind up
    float fl_vort{0.12f};       // vorticity confinement
    float fl_smoke_fade{0.4f};
    float fl_vel_damp{0.3f};
    int fl_iterations{16};      // pressure iterations
    float fl_emission{70.f};    // how hard the hot soot radiates, per metre of flame
    float fl_absorb{2.f};       // how much the cooled plume swallows, per metre
    float fl_albedo{0.6f};
    float fl_ember{1.1f};       // the glow the hot gas leaves on the wood under it
    float fl_core_t{1.8f};      // the temperature that reads as a white core
    float fl_smoke_gain{1.f};

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
    // The fluid grid behind the flame while the fire is near: built and torn down on the
    // render thread (its textures are cleared through the immediate context).
    std::unique_ptr<dx113DFluidData> m_fluid;
    bool m_fluid_wanted{};
    float m_fluid_fade{};       // 0 = the marched flame, 1 = the grid; they cross over
    float m_fluid_time{};
    float m_fluid_acc{};        // real seconds waiting to be stepped
    float m_fluid_retry{};      // seconds before another attempt to build the grid
    Fvector m_fluid_centre{};   // the grid box's centre in the world
    float m_fluid_cell{};
#if defined(USE_DX11)
    ID3DTexture3D* m_fluid_obst{}; // the level, voxelised once into the box
#endif
    void fluid_create();
    void fluid_destroy();
    void fluid_render(CBackend& cmd_list);
    void fluid_params();
    bool fluid_voxelize();
    Fvector fluid_plume_start() const;

    Fvector origin() const;
    Fvector flame_base() const;
    float flame_length() const;
    float wind_speed() const;
    Fvector flame_tip() const;
    void update_smoke(float dt, const Fvector& tip);
    void render_flame(CBackend& cmd_list, float fade);
    void render_smoke(CBackend& cmd_list);

public:
    // Both out of line: the fluid grid behind the unique_ptr is a complete type only here.
    CDaFireEffect();
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
