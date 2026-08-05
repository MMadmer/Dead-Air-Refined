#pragma once

#include "hud_item_object.h"
#include "hit_immunity.h"
#include "xrPhysics/PHUpdateObject.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path.h"

class SArtefactActivation;
struct SArtefactDetectorsSupport;

inline constexpr u32 artefactOverrideValueCount = 16;

struct SArtefactOverrideSaveState
{
    u32 sectionChecksum{};
    u32 changedMask{};
    float values[artefactOverrideValueCount]{};
    u16 objectId{u16(-1)};
};

struct SArtefactOverrideSaveCaptureState
{
    xr_vector<SArtefactOverrideSaveState> records;
    u32 nextObjectId{};
    bool initialized{};
    bool completed{};
};

class CArtefact : public CHudItemObject, public CPHUpdateObject
{
    typedef CHudItemObject inherited;

public:
    CArtefact();
    virtual ~CArtefact();

    virtual void Load(LPCSTR section);
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();

    virtual void OnH_A_Chield();
    virtual void OnH_B_Independent(bool just_before_destroy);
    virtual void OnActiveItem();
    virtual void OnHiddenItem();

    virtual void UpdateCL();
    virtual void shedule_Update(u32 dt);
    void UpdateWorkload(u32 dt);

    virtual bool CanTake() const;

    virtual bool renderable_ShadowGenerate() { return false; }
    virtual bool renderable_ShadowReceive() { return true; }
    virtual void create_physic_shell();

    virtual CArtefact* cast_artefact() { return this; }

    float GetHealthPower() const { return m_fHealthRestoreSpeed; }
    float GetRadiationPower() const { return m_fRadiationRestoreSpeed; }
    float GetSatietyPower() const { return m_fSatietyRestoreSpeed; }
    float GetPowerPower() const { return m_fPowerRestoreSpeed; }
    float GetBleedingPower() const { return m_fBleedingRestoreSpeed; }

    void SetRuntimeWeight(float value);
    void SetHealthPower(float value);
    void SetRadiationPower(float value);
    void SetSatietyPower(float value);
    void SetPowerPower(float value);
    void SetBleedingPower(float value);

    static void CollectOverrideSaveState(xr_vector<SArtefactOverrideSaveState>& result);
    static void BeginOverrideSaveCapture(SArtefactOverrideSaveCaptureState& state);
    [[nodiscard]] static bool ContinueOverrideSaveCapture(
        SArtefactOverrideSaveCaptureState& state, float budgetMilliseconds);
    static void StageOverrideSaveState(const xr_vector<SArtefactOverrideSaveState>& state);
    static void ClearOverrideSaveState();
    static void ForgetOverrideSaveState(const CSE_Abstract& serverObject);
    void ConsumeOverrideSaveState(u16 objectId);

protected:
    virtual void UpdateCLChild(){};
    virtual void CreateArtefactActivation();

    SArtefactActivation* m_activationObj{};
    SArtefactDetectorsSupport* m_detectorObj{};

    u16 m_CarringBoneID;
    shared_str m_sParticlesName;

    ref_light m_pTrailLight;
    Fcolor m_TrailLightColor;
    float m_fTrailLightRange;
    u8 m_af_rank;
    bool m_bLightsEnabled;
    float m_additional_weight{};

    virtual void UpdateLights();

public:
    IC u8 GetAfRank() const { return m_af_rank; }
    IC bool CanBeActivated() { return m_bCanSpawnZone; };
    void ActivateArtefact();
    void FollowByPath(LPCSTR path_name, int start_idx, Fvector magic_force);
    bool CanBeInvisible();
    void SwitchVisibility(bool);

    void SwitchAfParticles(bool bOn);
    virtual void StartLights();
    virtual void StopLights();

    virtual void PhDataUpdate(float step);
    virtual void PhTune(float step){};

    float AdditionalInventoryWeight() const { return m_additional_weight; }
    void SetAdditionalInventoryWeight(float value);
    float ArtefactHitImmunity(ALife::EHitType hitType) const
    {
        return m_ArtefactHitImmunities.GetHitImmunity(hitType);
    }
    void SetArtefactHitImmunity(ALife::EHitType hitType, float value);
    bool m_bCanSpawnZone;
    float m_fHealthRestoreSpeed;
    float m_fRadiationRestoreSpeed;
    float m_fSatietyRestoreSpeed;
    float m_fPowerRestoreSpeed;
    float m_fBleedingRestoreSpeed;
    CHitImmunity m_ArtefactHitImmunities;

public:
    enum EAFHudStates
    {
        eActivating = eLastBaseState + 1,
    };
    virtual void Interpolate();

    virtual void PlayAnimIdle();
    virtual void MoveTo(Fvector const& position);
    virtual void StopActivation();

    virtual void ForceTransform(const Fmatrix& m);

    virtual void Hide();
    virtual void Show();
    virtual void UpdateXForm();
    virtual bool Action(u16 cmd, u32 flags);
    virtual void OnStateSwitch(u32 S, u32 oldState);
    virtual void OnAnimationEnd(u32 state);
    virtual bool IsHidden() const { return GetState() == eHidden; }
    // optimization FAST/SLOW mode
    u32 o_render_frame;
    BOOL o_fastmode;
    IC void o_switch_2_fast()
    {
        if (o_fastmode)
            return;
        o_fastmode = TRUE;
        // processing_activate		();
    }
    IC void o_switch_2_slow()
    {
        if (!o_fastmode)
            return;
        o_fastmode = FALSE;
        // processing_deactivate		();
    }

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};

struct SArtefactDetectorsSupport
{
    CArtefact* m_parent;
    ref_sound m_sound;

    Fvector m_path_moving_force;
    u32 m_switchVisTime{};
    const CPatrolPath* m_currPatrolPath{};
    const CPatrolPath::CVertex* m_currPatrolVertex{};
    Fvector m_destPoint;

    SArtefactDetectorsSupport(CArtefact* A);
    ~SArtefactDetectorsSupport();
    void SetVisible(bool);
    void FollowByPath(LPCSTR path_name, int start_idx, Fvector force);
    void UpdateOnFrame();
    void Blink();
};
