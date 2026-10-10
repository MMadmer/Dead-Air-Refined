#pragma once
#include "MosquitoBald.h"

class CZoneCampfire : public CMosquitoBald
{
    typedef CMosquitoBald inherited;

protected:
    CParticlesObject* m_pEnablingParticles{};
    CParticlesObject* m_pDisabledParticles{};
    ref_sound m_disabled_sound;
    bool m_turned_on{ true };
    u32 m_turn_time{};
    //  How long it has been raining on this fire without a break, seconds. Rain does not put a
    //  fire out the instant the first drops land.
    float m_rain_soak{};
    //  How far the fire has thawed the snow round it (-1: not yet updated; a fire found burning has burned a while).
    float m_snow_melt{-1.f};

    virtual void PlayIdleParticles(bool bIdleLight = true);
    virtual void StopIdleParticles(bool bIdleLight = true);
    virtual bool AlwaysTheCrow();
    virtual void UpdateWorkload(u32 dt);
    //  Nothing solid between the fire and the sky.
    bool SkyAbove() const;
    void UpdateRainDouse(u32 dt);

public:
    CZoneCampfire();
    virtual ~CZoneCampfire();
    virtual void Load(LPCSTR section);
    virtual void GoEnabledState();
    virtual void GoDisabledState();

    void turn_on_script();
    void turn_off_script();
    bool is_on();
    //  The snow round the fire (da_snow_deformers.cpp): every campfire on the level, and how far a lit one has
    //  thawed it, 0 .. 1.
    static const xr_vector<CZoneCampfire*>& all();
    float snow_melt() const { return m_snow_melt < 0.f ? (m_turned_on ? 1.f : 0.f) : m_snow_melt; }
    void set_snow_melt(float melt) { m_snow_melt = clampr(melt, 0.f, 1.f); } // QA (qa_snow_fire_melt)
    //  Whether lighting this one would take right now. Exported so the NPC scheme that sends a
    //  stalker to light a fire can ask before it sends him, instead of him fiddling with a fire
    //  that will refuse.
    bool can_turn_on() const;
    virtual void shedule_Update(u32 dt);
};
