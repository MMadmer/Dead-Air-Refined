#pragma once

#include "dx113DFluidEmitters.h"

namespace xray::render::RENDER_NAMESPACE
{
class dx113DFluidData
{
public:
    enum eVolumePrivateRT
    {
        VP_VELOCITY0 = 0,
        VP_PRESSURE,
        VP_COLOR, //	Swap with global after update
        VP_NUM_TARGETS
    };

    enum SimulationType
    {
        ST_FOG = 0,
        ST_FIRE,
        //	The campfire: a combusting field (temperature, fuel, burn, soot) whose fuel comes
        //	off the surfaces the fire actually stands on, and its own ray-cast.
        ST_DA_FIRE,
    };

    //	Everything the campfire's passes need, in grid units: one simulation step is one unit
    //	of time and a velocity of 1 is one cell per step. CDaFireEffect fills this in.
    struct DaFireParams
    {
        Fvector m_vSource{};    // the fire's centre, in cells
        float m_fRadius{6.f};   // its radius, in cells
        float m_fBedRange{6.f}; // how far above and below the centre a surface still burns

        float m_fIgnition{0.1f};
        float m_fBurnPerT{4.f};
        float m_fTPerBurn{5.f};
        float m_fCooling{0.035f};
        float m_fFuelPerBurn{0.25f};
        float m_fSmokePerBurn{1.5f};
        float m_fExpansion{0.6f};
        float m_fFuelTarget{0.8f};

        Fvector m_vWind{};      // cells per step, on the grid's axes
        float m_fWindRelax{0.05f};

        float m_fBuoyancy{0.16f};
        float m_fInject{0.35f};
        float m_fTime{};        // seconds, for the noise that breaks the bed into tongues
        float m_fCouple{0.5f};

        float m_fSmokeFade{0.995f};
        float m_fVelDamp{0.995f};
        float m_fPuff{2.f};     // the pool-fire puffing rate, Hz

        //	Rendering
        float m_fEmission{1.f};
        float m_fAbsorb{2.5f};
        float m_fAlbedo{0.55f};
        float m_fEmber{0.5f};
        Fvector m_vFireLight{};
        float m_fCoreT{1.6f};
        float m_fSmokeGain{1.f};
        float m_fHotAbsorb{};   // how much the soot that is still glowing swallows as well
        Fvector m_vSunStep{};   // one step toward the sun, in the grid's texture space
        Fvector m_vSunColor{};
        float m_fShadow{};      // how hard the plume shades itself against the sun
        float m_fSmokeLift{};   // buoyancy the soot keeps once the flame in it has gone out
        float m_fGroundJet{};   // how hard a blast runs outward along the ground it sits on
        float m_fGroundDust{};  // and how much dust it tears off it
        float m_fRingEnv{};     // how much of that ground run is left
        float m_fBlastDiv{};    // the divergence the blast makes this step, signed
        float m_fEmisPow{3.f};  // how steeply emission climbs with temperature
        float m_fWindFloor{};   // how much wind reaches the gas still down at the source
        float m_fEdgeFade{10.f};// cells before a face of the box over which everything fades out
        float m_fDrainBand{1.f};// cells at the walls where the field drains rather than piles up
        float m_fDrainRate{1.f};
        //  Soot laid straight on the bed with no fuel and no heat behind it: a fire that has
        //  been put out steams for a while. Zero while it burns.
        float m_fSmoulder{};
        float m_fFade{1.f};     // crossover with the marched flame

        //	A blast: the charge thrown into a sphere over the first moments instead of a bed
        float m_fBlastInject{};
        float m_fBlastRadius{};
        float m_fBlastSpeed{};
        float m_fBlastPilot{};  // and the temperature its charge starts at; 0 = not a blast

        int m_iIterations{16};
    };

    struct Settings
    {
        float m_fHemi;
        float m_fConfinementScale;
        float m_fDecay;
        float m_fGravityBuoyancy;
        SimulationType m_SimulationType;
        DaFireParams m_DaFire;
    };

public:
    dx113DFluidData();
    ~dx113DFluidData();

    void Load(IReader* data);

    // A volume built by code (the shader fire): no profile, no level data. The private
    // textures already exist from the constructor; this sets the frame and the settings.
    void InitProcedural(const Fmatrix& transform, const Settings& settings);
    void SetTransform(const Fmatrix& m) { m_Transform = m; }
    void SetSettings(const Settings& s) { m_Settings = s; }
    void ClearEmitters() { m_Emitters.clear(); }
    //	The campfire hands the manager a volume it voxelised from the level itself, so the
    //	per-frame obstacle rasterisation is skipped entirely for it.
    void SetStaticObstacles(ID3DTexture3D* pT) { m_pStaticObstacles = pT; }
    ID3DTexture3D* GetStaticObstacles() const { return m_pStaticObstacles; }
    void AddEmitter(const dx113DFluidEmitters::CEmitter& e) { m_Emitters.push_back(e); }

    void SetTexture(eVolumePrivateRT id, ID3DTexture3D* pT)
    {
        pT->AddRef();
        m_pRTTextures[id]->Release();
        m_pRTTextures[id] = pT;
    }
    void SetView(eVolumePrivateRT id, ID3DRenderTargetView* pV)
    {
        pV->AddRef();
        m_pRenderTargetViews[id]->Release();
        m_pRenderTargetViews[id] = pV;
    }

    ID3DTexture3D* GetTexture(eVolumePrivateRT id) const
    {
        m_pRTTextures[id]->AddRef();
        return m_pRTTextures[id];
    }
    ID3DRenderTargetView* GetView(eVolumePrivateRT id) const
    {
        m_pRenderTargetViews[id]->AddRef();
        return m_pRenderTargetViews[id];
    }
    const Fmatrix& GetTransform() const { return m_Transform; }
    const xr_vector<Fmatrix>& GetObstaclesList() const { return m_Obstacles; }
    const xr_vector<dx113DFluidEmitters::CEmitter>& GetEmittersList() const { return m_Emitters; }
    const Settings& GetSettings() const { return m_Settings; }

#ifndef MASTER_GOLD
    //	Allow real-time config reload
    void ReparseProfile(const xr_string& Profile);
#endif

private:
    typedef dx113DFluidEmitters::CEmitter CEmitter;

private:
    void CreateRTTextureAndViews(int rtIndex, D3D_TEXTURE3D_DESC TexDesc);
    void DestroyRTTextureAndViews(int rtIndex);

    void ParseProfile(const xr_string& Profile);

private:
    Fmatrix m_Transform;

    xr_vector<Fmatrix> m_Obstacles;
    xr_vector<CEmitter> m_Emitters;

    Settings m_Settings;

    static DXGI_FORMAT m_VPRenderTargetFormats[VP_NUM_TARGETS];

    ID3DRenderTargetView* m_pRenderTargetViews[VP_NUM_TARGETS];
    ID3DTexture3D* m_pRTTextures[VP_NUM_TARGETS];

    //	Not owned: the effect that built it keeps it alive.
    ID3DTexture3D* m_pStaticObstacles{};
};
} // namespace xray::render::RENDER_NAMESPACE
