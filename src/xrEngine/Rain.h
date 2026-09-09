// Rain.h: interface for the CRain class.
//
//////////////////////////////////////////////////////////////////////

#ifndef RainH
#define RainH
#pragma once

#include "xrCDB/xr_collide_defs.h"

#include "Include/xrRender/FactoryPtr.h"
#include "Include/xrRender/RainRender.h"

// refs
class ENGINE_API IRender_DetailModel;

namespace xray::render
{
namespace render_r4
{
class dxRainRender;
}
namespace render_gl
{
class dxRainRender;
}
} // namespace xray::render

// The constants both halves of the system need. The simulation lives in Rain.cpp and the
// integrator in dxRainRender.cpp, and these used to be written out once in each file with
// hand-maintained "Warning: duplicated in ..." comments. Both files include this header.
namespace da_rain
{
// Height of the spawn column above the eye plane, and how far below the eye a drop may sink
// before it is recycled.
constexpr float source_offset = 40.f;
constexpr float max_distance = source_offset * 1.25f;
constexpr float sink_offset = -(max_distance - source_offset);

// A large drop reaches ~9 m/s terminal velocity - the denominator of the wind slant. The speed
// the streaks are DRAWN at is several times that on purpose: a physically timed drop crosses
// the frame too fast to read as a streak at all.
constexpr float fall_ms = 9.0f;
constexpr float speed_min = 40.f;
constexpr float speed_max = 80.f;
constexpr float speed_mean = 0.5f * (speed_min + speed_max);

// Readability cap on the wind slant: the physical atan(wind/fall) reaches ~55 deg in a
// hurricane, but past ~45 the sheets read as a renderer glitch rather than as weather.
constexpr float max_slant = deg2rad(45.f);
// Per-drop scatter around the fall axis.
constexpr float spread = deg2rad(3.0f);
// How fast a drop already in the air answers a change in the wind. A raindrop's real
// relaxation time is a fraction of a second, and this is what turns a gust front into a
// visible ripple travelling through the sheet instead of a slow population turnover.
constexpr float wind_tau = 0.30f;

// Splash pool. Was 1000 with every second hit refused; now every drop splashes and a dense
// rain runs out of a thousand - new splashes were simply never born.
constexpr int max_particles = 4000;
// A real crown peaks within a few milliseconds and is gone inside a tenth of a second. The
// stock 0.3 s, at full size for the whole of it, is the single biggest reason splashes read as
// decals - so r__rain_splash_time trims INSIDE the physical envelope rather than setting it.
constexpr float crown_life_min = 0.06f;
constexpr float crown_life_max = 0.18f;
// A crown needs something like level ground under it; on a wall a drop runs off instead.
constexpr float crown_min_ny = 0.35f;

// Sky-cover probe: rays, their reach, and the share of the weight the fall axis itself carries.
constexpr int cover_rays = 5;
constexpr float cover_range = 30.f;
} // namespace da_rain

class ENGINE_API CEffect_Rain
{
    friend class xray::render::render_r4::dxRainRender;
    friend class xray::render::render_gl::dxRainRender;

private:
    struct Item
    {
        Fvector P;
        Fvector Phit;
        Fvector D;
        // What the birth ray found under the drop. The query carried the surface all along and
        // the old code kept only the range, which is why every crown stood world-up on a
        // pitched roof and water got the same twelve-triangle cone as concrete.
        Fvector Nhit;
        s32 mtl_hit;
        float fSpeed;
        u32 dwTime_Life;
        u32 dwTime_Hit;
        u32 uv_set;
        // The spawn column is blocked at its very top: the drop would be born inside geometry.
        // Parked out of the sheet instead of being reborn, and re-raycast, every frame.
        bool sheltered;
        void invalidate() { dwTime_Life = 0; }
    };
    struct Particle
    {
        Particle *next, *prev;
        Fmatrix mXForm;
        Fsphere bounds;
        float time; // seconds left
        float life; // seconds it was born with - the renderer needs both to shape the crown
        float size; // per-hit size jitter, so a sheet of crowns is not one stamp repeated
    };
    enum States
    {
        stIdle = 0,
        stWorking
    };

private:
    // Visualization (rain) and (drops)
    FactoryPtr<IRainRender> m_pRender;

    // Data and logic
    xr_vector<Item> items;
    States state;

    // Particles
    xr_vector<Particle> particle_pool;
    Particle* particle_active;
    Particle* particle_idle;

    // Sounds
    ref_sound snd_Ambient;
    // What the ambient bed plays at: density x real sky cover. This is the corrected oracle.
    float rain_volume_snd;
    // What GetVolume() - i.e. the shipped level.get_rain_volume() Lua export - returns. It is
    // the sound volume with the OLD lighting-hemi term still folded in, on purpose: shipped
    // scripts turn this number into radiation damage (fallout_manager) and into campfire
    // dousing, and they were written against a value that dies at night. Cover alone would
    // make a night storm as radioactive as noon, which is a gameplay change nobody asked for.
    // The indoor half of the correction still lands, because cover multiplies it.
    float rain_volume;

    // Sky cover at the eye: how much of the sky the rain can actually arrive from. Measured by
    // a round-robin of static rays, one per frame (see CoverTick).
    float cover_factor;
    // Smoothed lighting hemi-cube of the view entity, kept only to feed rain_volume above.
    // A member, not the old function-local static: that one survived a level change.
    float hemi_factor;
    u32 cover_ray;
    bool cover_open[da_rain::cover_rays];

    // Utilities
    void p_create();
    void p_destroy();

    void p_remove(Particle* P, Particle*& LST);
    void p_insert(Particle* P, Particle*& LST);
    int p_size(Particle* LST);
    Particle* p_allocate();
    void p_free(Particle* P);

    // Some methods
    void Born(Item& dest, float radius);
    void Hit(Fvector& pos);
    bool RayPick(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt);
    void RenewItem(Item& dest, float height, bool bHit);

    // The same query, keeping the surface normal and the game material the old one discarded.
    bool RayPickEx(const Fvector& s, const Fvector& d, float& range, collide::rq_target tgt, Fvector& normal,
        s32& material);
    // What a landed drop does, given what it landed on: a crown, a ring on water, or nothing.
    void Splash(const Fvector& pos, const Fvector& n, s32 mtl);
    // A drop's own landing - splashes where the drop actually IS, so the in-flight wind and the
    // crown agree.
    void HitItem(Item& src);
    bool SurfaceIsWater(const Fvector& pos, s32 mtl) const;
    void CoverTick(float dt);
    // The old lighting oracle, kept alive for the Lua export only. See rain_volume.
    void HemiTick(float dt);

public:
    CEffect_Rain();
    ~CEffect_Rain();

    float GetVolume() { return rain_volume; }
    void Render();
    void OnFrame();
    void InvalidateState();
};

#endif // RainH
