#pragma once

#include "xrPhysics/PhysicsShell.h"
#include "WeaponAmmo.h"
#include "PHShellCreator.h"

#include "ShootingObject.h"
#include "hud_item_object.h"
#include "Actor_Flags.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "firedeps.h"
#include "game_cl_single.h"
#include "first_bullet_controller.h"
#include "save_extension_chunk_ids.h"

#include "CameraRecoil.h"

class CEntity;
class ENGINE_API CMotionDef;
class CSE_ALifeItemWeapon;
class CSE_ALifeItemWeaponAmmo;
class CWeaponMagazined;
class CParticlesObject;
class CUIWindow;
class CBinocularsVision;
class CNightVisionEffector;

enum EWeaponConditionType : u32
{
    eWeaponConditionChamberCycle = 1u << 25,
    eWeaponConditionMagazineRemoved = 1u << 26,
    eWeaponConditionFireMode = 1u << 27,
    eWeaponConditionScopeMount = 1u << 28,
    eWeaponConditionSilencerMount = 1u << 29,
    eWeaponConditionGrenadeLauncherMount = 1u << 30,
    eWeaponConditionSidecarSaveMask = eWeaponConditionChamberCycle | eWeaponConditionMagazineRemoved,
    eWeaponConditionLegacySaveMask = eWeaponConditionFireMode | eWeaponConditionScopeMount |
        eWeaponConditionSilencerMount | eWeaponConditionGrenadeLauncherMount,
    eWeaponConditionExtendedSaveMask = eWeaponConditionSidecarSaveMask | eWeaponConditionLegacySaveMask,
};

static_assert((eWeaponConditionSidecarSaveMask & eWeaponConditionLegacySaveMask) == 0);
static_assert(eWeaponConditionSidecarSaveMask == 0x06000000u);
static_assert(eWeaponConditionLegacySaveMask == 0x78000000u);

// ---- the fault rework: fouling -> deformation -> breakage ---------------------------------------
// Fault ids (string id = bit + 1), never bits: a section's condition_avail is tested by id while
// the fault mask stores id - 1. That asymmetry is the original x86 build's and every stock mask
// was authored against it (CWeaponMagazined::AddConditionFault keeps it).
inline constexpr u32 weapon_fault_bit(u32 id) { return 1u << (id - 1); }

template <size_t N>
constexpr u32 weapon_fault_mask(const u32 (&ids)[N])
{
    u32 mask = 0;
    for (u32 id : ids)
        mask |= weapon_fault_bit(id);
    return mask;
}

// Fouling stage: the six "dirty" parts - mainspring, return spring, barrel, sear, firing pin, bolt.
inline constexpr u32 kWeaponFoulingFaultIds[] = { 3, 5, 11, 16, 19, 22 };
// Deformation stage (durability): worn receiver and mainspring, the deformed parts, split grip,
// broken stock. The mounts and the selector (28..31) join the pool only on a weapon that has them.
inline constexpr u32 kWeaponDeformFaultIds[] = { 1, 4, 6, 7, 8, 9, 12, 14, 17, 20, 23 };
inline constexpr u32 kWeaponMountFaultIds[] = { 28, 29, 30, 31 };
// Breakage stage: a deformed part that keeps being fired breaks into its counterpart.
struct SWeaponFaultBreakage
{
    u32 deformedId;
    u32 brokenId;
};
inline constexpr SWeaponFaultBreakage kWeaponFaultBreakages[] = {
    { 1, 2 }, { 9, 10 }, { 12, 13 }, { 14, 15 }, { 17, 18 }, { 20, 21 } };

inline constexpr u32 kWeaponFoulingFaultMask = weapon_fault_mask(kWeaponFoulingFaultIds);
inline constexpr u32 kWeaponDeformFaultMask =
    weapon_fault_mask(kWeaponDeformFaultIds) | weapon_fault_mask(kWeaponMountFaultIds);
// Everything the original lists as a fault: bits 0-23 and the selector/mount bits 27-30. Bits
// 24-26 are placeholders in the stock strings, 25-26 this build's chamber and magazine state.
inline constexpr u32 kWeaponDisplayableFaultMask = 0x78FFFFFFu;
static_assert((kWeaponDisplayableFaultMask & u32(eWeaponConditionSidecarSaveMask)) == 0);
static_assert((kWeaponDeformFaultMask & u32(eWeaponConditionLegacySaveMask)) == u32(eWeaponConditionLegacySaveMask));

// Deformed parts whose counterpart can still break: not broken yet, and allowed by the section.
inline u32 weapon_breakable_deformations(u32 conditionType, u32 conditionAvailable)
{
    u32 mask = 0;
    for (const SWeaponFaultBreakage& breakage : kWeaponFaultBreakages)
    {
        if ((conditionType & weapon_fault_bit(breakage.deformedId)) &&
            !(conditionType & weapon_fault_bit(breakage.brokenId)) &&
            (conditionAvailable & (1u << breakage.brokenId)))
        {
            mask |= weapon_fault_bit(breakage.deformedId);
        }
    }
    return mask;
}

// WEX1 also accepts duplicated legacy bits from containers written before legacy ownership was restored.
inline constexpr u32 weaponExtendedSaveChunkType = SaveExtensionChunkIds::WeaponExtended;
inline constexpr u16 weaponExtendedSaveChunkVersion = 1;

struct SWeaponExtendedSaveState
{
    u16 objectId{u16(-1)};
    u16 reserved{};
    u32 sectionChecksum{};
    u32 extendedMask{};
};

// WFL1: the fault accumulators. A record is written only when something is non-zero, so a
// fresh game adds nothing to the sidecar; wear travels as a 16-bit fraction of the way to the
// next deformation, or a quickload would hand the progress back.
inline constexpr u32 weaponFoulingSaveChunkType = SaveExtensionChunkIds::WeaponFouling;
inline constexpr u16 weaponFoulingSaveChunkVersion = 1;

struct SWeaponFoulingSaveState
{
    u16 objectId{u16(-1)};
    u16 reserved{};
    u32 sectionChecksum{};
    u16 fouling{};
    u16 stress{};
    u16 wear{};
    u16 foulingStage{};
};

struct SWeaponExtendedSaveCaptureState
{
    xr_vector<SWeaponExtendedSaveState> records;
    xr_vector<SWeaponFoulingSaveState> foulingRecords;
    u32 nextObjectId{};
    bool initialized{};
    bool completed{};
};

class CWeapon : public CHudItemObject, public CShootingObject
{
    typedef CHudItemObject inherited;

public:
    CWeapon();
    virtual ~CWeapon();

    // Generic
    virtual void Load(LPCSTR section);

    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Destroy();
    virtual void net_Export(NET_Packet& P);
    virtual void net_Import(NET_Packet& P);

    virtual CWeapon* cast_weapon() { return this; }
    virtual CWeaponMagazined* cast_weapon_magazined() { return 0; }
    // serialization
    virtual void save(NET_Packet& output_packet);
    virtual void load(IReader& input_packet);
    virtual bool net_SaveRelevant() { return inherited::net_SaveRelevant(); }

    static void CollectExtendedSaveState(xr_vector<SWeaponExtendedSaveState>& result);
    static void BeginExtendedSaveCapture(SWeaponExtendedSaveCaptureState& state);
    [[nodiscard]] static bool ContinueExtendedSaveCapture(
        SWeaponExtendedSaveCaptureState& state, float budgetMilliseconds);
    static bool EncodeExtendedSaveState(
        const xr_vector<SWeaponExtendedSaveState>& state, xr_vector<u8>& payload);
    static bool DecodeExtendedSaveState(
        const xr_vector<u8>& payload, xr_vector<SWeaponExtendedSaveState>& state);
    static bool StageExtendedSaveState(const xr_vector<SWeaponExtendedSaveState>& state);
    static bool StageFoulingSaveState(const xr_vector<SWeaponFoulingSaveState>& state);
    static void ClearExtendedSaveState();
    static void ForgetExtendedSaveState(const CSE_Abstract& serverObject);

    virtual void UpdateCL();
    virtual void shedule_Update(u32 dt);

    void renderable_Render(u32 context_id, IRenderable* root) override;
    void render_hud_mode() override;
    float GetHudFov();
    bool need_renderable() override;

    virtual void render_item_ui();
    virtual bool render_item_ui_query();

    virtual void OnH_B_Chield();
    virtual void OnH_A_Chield();
    virtual void OnH_B_Independent(bool just_before_destroy);
    virtual void OnH_A_Independent();
    virtual void OnEvent(NET_Packet& P, u16 type); // {inherited::OnEvent(P,type);}

    virtual void Hit(SHit* pHDS);

    virtual void reinit();
    virtual void reload(LPCSTR section);
    virtual void create_physic_shell();
    virtual void activate_physic_shell();
    virtual void setup_physic_shell();

    virtual void SwitchState(u32 S);

    virtual void OnActiveItem();
    virtual void OnHiddenItem();
    virtual void SendHiddenItem(); // same as OnHiddenItem but for client... (sends message to a server)...

public:
    virtual bool can_kill() const;
    virtual CInventoryItem* can_kill(CInventory* inventory) const;
    virtual const CInventoryItem* can_kill(const xr_vector<const CGameObject*>& items) const;
    virtual bool ready_to_kill() const;
    virtual bool NeedToDestroyObject() const;
    virtual ALife::_TIME_ID TimePassedAfterIndependant() const;

protected:
    //время удаления оружия
    ALife::_TIME_ID m_dwWeaponRemoveTime;
    ALife::_TIME_ID m_dwWeaponIndependencyTime;

    virtual bool IsHudModeNow();
    void LoadScope(const shared_str& section);
    shared_str GetScopeTextureName(pcstr section) const;

public:
    void signal_HideComplete();
    virtual bool Action(u16 cmd, u32 flags);

    enum EWeaponStates
    {
        eFire = eLastBaseState + 1,
        eFire2,
        eReload,
        eMisfire,
        eMagEmpty,
        eSwitch,
    };
    enum EWeaponSubStates
    {
        eSubstateReloadBegin = 0,
        eSubstateReloadInProcess,
        eSubstateReloadEnd,
    };
    enum
    {
        undefined_ammo_type = u8(-1)
    };

    IC BOOL IsValid() const { return iAmmoElapsed; }
    // Does weapon need's update?
    BOOL IsUpdating();

    BOOL IsMisfire() const;
    BOOL CheckForMisfire();
    u32 GetConditionType() const { return m_condition_type; }
    void SetConditionType(u32 condition_type)
    {
        if (m_condition_type == condition_type)
            return;

        m_condition_type = condition_type;
        m_BriefInfo_CalcFrame = 0;
    }
    bool HasConditionType(EWeaponConditionType condition_type) const
    {
        return (m_condition_type & condition_type) != 0;
    }

    // ---- fault rework accumulators ----
    u16 GetFouling() const { return m_fouling; }
    u16 GetStress() const { return m_stress; }
    u16 GetFoulingStage() const { return m_foulingStage; }
    float GetWearProgress() const { return m_wearProgress; }
    // 0..1 of the way to the next dirty fault: what the tooltip and the QA probes read.
    float GetFoulingRatio() const;
    // Rounds to the next dirty fault of this weapon, before the per-interval jitter.
    virtual float FoulingInterval() const { return 0.f; }
    // The bits a script cleared (a kit, the mechanic): the accumulators of that stage start over.
    void OnConditionFaultsCleared(u32 clearedMask);
    bool IsAmmoSuitable(const shared_str& item_section) { return IsNecessaryItem(item_section); }
    LPCSTR GetAmmoName() const
    {
        return m_ammoType < m_ammoTypes.size() ? m_ammoTypes[m_ammoType].c_str() : nullptr;
    }

    BOOL AutoSpawnAmmo() const { return m_bAutoSpawnAmmo; };
    bool IsTriStateReload() const { return m_bTriStateReload; }
    EWeaponSubStates GetReloadState() const { return (EWeaponSubStates)m_sub_state; }
protected:
    bool m_bTriStateReload;

    // a misfire happens, you'll need to rearm weapon
    bool bMisfire;
    u32 m_condition_type;

    // Fault rework accumulators, persisted in the WFL1 sidecar chunk and never in the frozen
    // 0.98b streams: rounds since the last cleaning, shots fired with a breakable deformation,
    // progress to the next deformation, dirty faults produced in this cleaning cycle, and the
    // fouling interval in force (0 = not drawn yet).
    u16 m_fouling{};
    u16 m_stress{};
    u16 m_foulingStage{};
    float m_wearProgress{};
    float m_foulingTarget{};

    void PackFoulingState(SWeaponFoulingSaveState& record) const;
    void ApplyFoulingState(const SWeaponFoulingSaveState& record);
    void RestoreFoulingState(u16 objectId, u32 sectionChecksum, pcstr sectionName);
    void ParkFoulingState();

    BOOL m_bAutoSpawnAmmo;
    virtual bool AllowBore();

public:
    u8 m_sub_state; // Alundaio: made public

    bool IsGrenadeLauncherAttached() const;
    bool IsScopeAttached() const;
    bool IsSilencerAttached() const;

    virtual bool GrenadeLauncherAttachable();
    virtual bool ScopeAttachable();
    virtual bool SilencerAttachable();

    ALife::EWeaponAddonStatus get_GrenadeLauncherStatus() const;
    ALife::EWeaponAddonStatus get_ScopeStatus() const;
    ALife::EWeaponAddonStatus get_SilencerStatus() const;
    virtual bool UseScopeTexture() { return true; };
    //обновление видимости для косточек аддонов
    void UpdateAddonsVisibility();
    void UpdateHUDAddonsVisibility();
    //инициализация свойств присоединенных аддонов
    virtual void InitAddons();

    void on_b_hud_detach() override;

    //для отоброажения иконок апгрейдов в интерфейсе
    int GetScopeX() { return pSettings->r_s32(m_scopes[m_cur_scope], "scope_x"); }
    int GetScopeY() { return pSettings->r_s32(m_scopes[m_cur_scope], "scope_y"); }
    int GetSilencerX() { return m_iSilencerX; }
    int GetSilencerY() { return m_iSilencerY; }
    int GetGrenadeLauncherX() { return m_iGrenadeLauncherX; }
    int GetGrenadeLauncherY() { return m_iGrenadeLauncherY; }
    const shared_str& GetGrenadeLauncherName() const { return m_sGrenadeLauncherName; }
    const shared_str GetScopeName() const { return pSettings->r_string(m_scopes[m_cur_scope], "scope_name"); }
    const shared_str& GetSilencerName() const { return m_sSilencerName; }
    IC void ForceUpdateAmmo() { m_BriefInfo_CalcFrame = 0; }
    u8 GetAddonsState() const { return m_flagsAddOnState; };
    void SetAddonsState(u8 st) { m_flagsAddOnState = st; } // dont use!!! for buy menu only!!!
protected:
    //состояние подключенных аддонов
    u8 m_flagsAddOnState;

    //возможность подключения различных аддонов
    ALife::EWeaponAddonStatus m_eScopeStatus;
    ALife::EWeaponAddonStatus m_eSilencerStatus;
    ALife::EWeaponAddonStatus m_eGrenadeLauncherStatus;

    //названия секций подключаемых аддонов
    shared_str m_sScopeName;
    shared_str m_sSilencerName;
    shared_str m_sGrenadeLauncherName;

    //смещение иконов апгрейдов в инвентаре
    int m_iScopeX, m_iScopeY;
    int m_iSilencerX, m_iSilencerY;
    int m_iGrenadeLauncherX, m_iGrenadeLauncherY;

protected:
    struct SZoomParams
    {
        bool m_bZoomEnabled; //разрешение режима приближения
        bool m_bHideCrosshairInZoom;
        bool m_bZoomDofEnabled;

        bool m_bIsZoomModeNow; //когда режим приближения включен
        float m_fCurrentZoomFactor; //текущий фактор приближения
        float m_fZoomRotateTime; //время приближения

        float m_fIronSightZoomFactor; //коэффициент увеличения прицеливания
        float m_fScopeZoomFactor; //коэффициент увеличения прицела

        float m_fZoomRotationFactor;

        Fvector m_ZoomDof;
        Fvector4 m_ReloadDof;
        Fvector4 m_ReloadEmptyDof;

        bool m_bUseDynamicZoom;
        shared_str m_sUseZoomPostprocess;
        shared_str m_sUseBinocularVision;
        CBinocularsVision* m_pVision;
        CNightVisionEffector* m_pNight_vision;

    } m_zoom_params;

    float m_fRTZoomFactor; // run-time zoom factor
    CUIWindow* m_UIScope;

public:
    IC bool IsZoomEnabled() const { return m_zoom_params.m_bZoomEnabled; }
    virtual void ZoomInc();
    virtual void ZoomDec();
    virtual void OnZoomIn();
    virtual void OnZoomOut();
    IC bool IsZoomed() const { return m_zoom_params.m_bIsZoomModeNow; };
    CUIWindow* ZoomTexture();

    bool ZoomHideCrosshair() { return m_zoom_params.m_bHideCrosshairInZoom || ZoomTexture(); }
    IC float GetZoomFactor() const { return m_zoom_params.m_fCurrentZoomFactor; }
    IC void SetZoomFactor(float f) { m_zoom_params.m_fCurrentZoomFactor = f; }
    virtual float CurrentZoomFactor();
    //показывает, что оружие находится в соостоянии поворота для приближенного прицеливания
    bool IsRotatingToZoom() const { return (m_zoom_params.m_fZoomRotationFactor < 1.f); }
    virtual u8 GetCurrentHudOffsetIdx();

    virtual float Weight() const;
    virtual u32 Cost() const;

public:
    virtual EHandDependence HandDependence() const { return eHandDependence; }
    bool IsSingleHanded() const { return m_bIsSingleHanded; }
public:
    IC LPCSTR strap_bone0() const { return m_strap_bone0; }
    IC LPCSTR strap_bone1() const { return m_strap_bone1; }
    IC void strapped_mode(bool value) { m_strapped_mode = value; }
    IC bool strapped_mode() const { return m_strapped_mode; }
protected:
    LPCSTR m_strap_bone0;
    LPCSTR m_strap_bone1;
    Fmatrix m_StrapOffset;
    bool m_strapped_mode;
    bool m_can_be_strapped;

    Fmatrix m_Offset;
    // 0-используется без участия рук, 1-одна рука, 2-две руки
    EHandDependence eHandDependence;
    bool m_bIsSingleHanded;

public:
    //загружаемые параметры
    Fvector vLoadedFirePoint;
    Fvector vLoadedFirePoint2;

private:
    firedeps m_current_firedeps;

protected:
    virtual void UpdateFireDependencies_internal();
    virtual void UpdatePosition(const Fmatrix& transform); //.
    virtual void UpdateXForm();
    virtual void UpdateHudAdditonal(Fmatrix&);
    IC void UpdateFireDependencies()
    {
        if (dwFP_Frame == Device.dwFrame)
            return;
        UpdateFireDependencies_internal();
    };

    virtual void LoadFireParams(LPCSTR section);

public:
    IC const Fvector& get_LastFP()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFP;
    }
    IC const Fvector& get_LastFP2()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFP2;
    }
    IC const Fvector& get_LastFD()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastFD;
    }
    IC const Fvector& get_LastSP()
    {
        UpdateFireDependencies();
        return m_current_firedeps.vLastSP;
    }

    virtual const Fvector& get_CurrentFirePoint() { return get_LastFP(); }
    virtual const Fvector& get_CurrentFirePoint2() { return get_LastFP2(); }
    virtual const Fmatrix& get_ParticlesXFORM()
    {
        UpdateFireDependencies();
        return m_current_firedeps.m_FireParticlesXForm;
    }
    virtual void ForceUpdateFireParticles();

protected:
    virtual void SetDefaults();

    virtual bool MovingAnimAllowedNow();
    virtual void OnStateSwitch(u32 S, u32 oldState);
    virtual void OnAnimationEnd(u32 state);

    //трассирование полета пули
    virtual void FireTrace(const Fvector& P, const Fvector& D);
    virtual float GetWeaponDeterioration();

    virtual void FireStart() { CShootingObject::FireStart(); }
    virtual void FireEnd();

    virtual void Reload();
    void StopShooting();

    // обработка визуализации выстрела
    virtual void OnShot(){};
    virtual void AddShotEffector();
    virtual void RemoveShotEffector();
    virtual void ClearShotEffector();
    virtual void StopShotEffector();

public:
    float GetBaseDispersion(float cartridge_k);
    float GetFireDispersion(bool with_cartridge, bool for_crosshair = false);
    virtual float GetFireDispersion(float cartridge_k, bool for_crosshair = false);
    virtual int ShotsFired() { return 0; }
    virtual int GetCurrentFireMode() { return 1; }
    //параметы оружия в зависимоти от его состояния исправности
    float GetConditionDispersionFactor() const;
    float GetConditionMisfireProbability() const;
    virtual float GetConditionToShow() const;

public:
    CameraRecoil cam_recoil; // simple mode (walk, run)
    CameraRecoil zoom_cam_recoil; // using zoom =(ironsight or scope)

protected:
    //фактор увеличения дисперсии при максимальной изношености
    //(на сколько процентов увеличится дисперсия)
    float fireDispersionConditionFactor;

    //вероятность осечки при максимальной изношености
    float misfireProbability;
    float misfireConditionK;
    // modified by Peacemaker [17.10.08]
    bool  misfireUseOldFormula{};
    float misfireStartCondition; //изношенность, при которой появляется шанс осечки
    float misfireEndCondition; //изношеность при которой шанс осечки становится константным
    float misfireStartProbability; //шанс осечки при изношености больше чем misfireStartCondition
    float misfireEndProbability; //шанс осечки при изношености больше чем misfireEndCondition
    //износ, выше которого осечек не бывает вообще (misfire_condition_ceiling)
    float misfireConditionCeiling{ 1.f };
    float conditionDecreasePerQueueShot; //увеличение изношености при выстреле очередью
    float conditionDecreasePerShot; //увеличение изношености при одиночном выстреле

    // Fault rework tuning (fault_* keys: engine default, then [inventory], then the weapon section).
    float faultFoulingRounds{ 50.f };
    float faultFoulingRepeat{ 0.5f };
    float faultDeformRounds{ 100.f };
    float faultBreakRounds{ 250.f };
    // (condition_shot_dec + 0.0001) * 1000, the wear scale every fault interval divides by. The
    // single-shot rate on purpose: the burst rate would move the threshold inside a burst.
    float ScaledDeterioration() const { return (_max(0.f, conditionDecreasePerShot) + 0.0001f) * 1000.f; }

public:
    // Capped like GetConditionMisfireProbability does, so the HUD warning appears exactly
    // when misfires actually become possible.
    float GetMisfireStartCondition() const { return _min(misfireStartCondition, misfireConditionCeiling); }
    float GetMisfireEndCondition() const { return _min(misfireEndCondition, GetMisfireStartCondition()); }

protected:
    struct SPDM
    {
        float m_fPDM_disp_base;
        float m_fPDM_disp_vel_factor;
        float m_fPDM_disp_accel_factor;
        float m_fPDM_disp_crouch;
        float m_fPDM_disp_crouch_no_acc;
    };
    SPDM m_pdm;

    float m_crosshair_inertion;
    first_bullet_controller m_first_bullet_controller;

protected:
    //для отдачи оружия
    Fvector m_vRecoilDeltaAngle;

protected:
    //для второго ствола
    void StartFlameParticles2();
    void StopFlameParticles2();
    void UpdateFlameParticles2();

protected:
    shared_str m_sFlameParticles2;
    //объект партиклов для стрельбы из 2-го ствола
    CParticlesObject* m_pFlameParticles2;

protected:
    int GetAmmoCount(u8 ammo_type) const;
    bool UsesAmmoBelt() const;
    CWeaponAmmo* GetAmmoForReload(LPCSTR ammo_section) const;

public:
    IC int GetAmmoElapsed() const { return /*int(m_magazine.size())*/ iAmmoElapsed; }
    IC int GetAmmoMagSize() const { return iMagazineSize; }
    int GetSuitableAmmoTotal(bool use_item_to_spawn = false) const;

    void SetAmmoElapsed(int ammo_count);

    virtual void OnMagazineEmpty();
    void SpawnAmmo(u32 boxCurr = 0xffffffff, LPCSTR ammoSect = NULL, u32 ParentID = 0xffffffff);
    bool SwitchAmmoType(u32 flags);

    virtual float Get_PDM_Base() const { return m_pdm.m_fPDM_disp_base; };
    virtual float Get_PDM_Vel_F() const { return m_pdm.m_fPDM_disp_vel_factor; };
    virtual float Get_PDM_Accel_F() const { return m_pdm.m_fPDM_disp_accel_factor; };
    virtual float Get_PDM_Crouch() const { return m_pdm.m_fPDM_disp_crouch; };
    virtual float Get_PDM_Crouch_NA() const { return m_pdm.m_fPDM_disp_crouch_no_acc; };
    virtual float GetCrosshairInertion() const { return m_crosshair_inertion; };
    float GetFirstBulletDisp() const { return m_first_bullet_controller.get_fire_dispertion(); };
protected:
    int iAmmoElapsed; // ammo in magazine, currently
    int iMagazineSize; // size (in bullets) of magazine

    //для подсчета в GetSuitableAmmoTotal
    mutable int m_iAmmoCurrentTotal;
    mutable u32 m_BriefInfo_CalcFrame; //кадр на котором просчитали кол-во патронов
    bool m_bAmmoWasSpawned;

    virtual bool IsNecessaryItem(const shared_str& item_sect);

public:
    xr_vector<shared_str> m_ammoTypes;
    /*
        struct SScopes
        {
            shared_str			m_sScopeName;
            int					m_iScopeX;
            int					m_iScopeY;
        };
        DEFINE_VECTOR(SScopes*, SCOPES_VECTOR, SCOPES_VECTOR_IT);
        SCOPES_VECTOR			m_scopes;

        u8						cur_scope;
    */

    using SCOPES_VECTOR = xr_vector<shared_str>;
    SCOPES_VECTOR m_scopes;
    u8 m_cur_scope;

    // HUD bones driven by the scope keys, resolved from the ltx once: UpdateHUDAddonsVisibility
    // runs every frame, and set_bone_visible takes a shared_str, so parsing there would intern a
    // string per bone per frame. Parallel to m_scopes, rebuilt when an upgrade grows that list.
    struct SScopeBones
    {
        xr_vector<shared_str> hide;
        xr_vector<shared_str> show;
    };

    shared_str m_scopes_hide_bone;
    xr_vector<SScopeBones> m_scope_bones;
    bool m_scope_bones_used{};
    // Last applied state, so the per-frame path costs three comparisons.
    const void* m_scope_bones_target{};
    u8 m_scope_bones_scope{ u8(-1) };
    bool m_scope_bones_attached{};

    void CacheScopeBones();
    void UpdateScopeBonesVisibility();
    void ResetScopeBonesVisibility();
    void ShowScopeBones();
    void SetScopeBoneVisible(const shared_str& bone_name, BOOL visible);

    CWeaponAmmo* m_pCurrentAmmo;
    u8 m_ammoType;
    //-	shared_str				m_ammoName; <== deleted
    bool m_bHasTracers;
    u8 m_u8TracerColorID;
    u8 m_set_next_ammoType_on_reload;
    // Multitype ammo support
    xr_vector<CCartridge> m_magazine;
    CCartridge m_DefaultCartridge;
    float m_fCurrentCartirdgeDisp;
    float m_hud_fov_add_mod;
    // Per-item hud fov multipliers (Gunslinger's hud_fov_factor / hud_fov_zoom_factor):
    // hip and aimed values, lerped by the zoom rotation factor. Both default to 1.0, so
    // items without the keys render bit-exactly as before.
    float m_hud_fov_factor;
    float m_hud_fov_zoom_factor;
    float m_nearwall_dist_max;
    float m_nearwall_dist_min;
    float m_nearwall_last_hud_fov;
    float m_nearwall_target_hud_fov;
    float m_nearwall_speed_mod;

    bool unlimited_ammo();
    IC bool can_be_strapped() const { return m_can_be_strapped; };

    float GetMagazineWeight(const decltype(m_magazine)& mag) const;

protected:
    u32 m_ef_main_weapon_type;
    u32 m_ef_weapon_type;

public:
    virtual u32 ef_main_weapon_type() const;
    virtual u32 ef_weapon_type() const;

    //Alundaio
    int GetAmmoCount_forType(shared_str const& ammo_type) const;
    virtual void set_ef_main_weapon_type(u32 type) { m_ef_main_weapon_type = type; };
    virtual void set_ef_weapon_type(u32 type) { m_ef_weapon_type = type; };
    virtual void SetAmmoType(u8 type) { m_ammoType = type; };
    u8 GetAmmoType() { return m_ammoType; }
    //-Alundaio

protected:
    // This is because when scope is attached we can't ask scope for these params
    // therefore we should hold them by ourself :-((
    float m_addon_holder_range_modifier;
    float m_addon_holder_fov_modifier;

public:
    virtual void modify_holder_params(float& range, float& fov) const;
    virtual bool use_crosshair() const { return true; }
    bool show_crosshair();
    bool show_indicators();
    virtual BOOL ParentMayHaveAimBullet();
    virtual BOOL ParentIsActor();

private:
    void UpdateScopeDofRadius();
    void ResetScopeDofRadius();

    virtual bool install_upgrade_ammo_class(LPCSTR section, bool test);
    bool install_upgrade_disp(LPCSTR section, bool test);
    bool install_upgrade_hit(LPCSTR section, bool test);
    bool install_upgrade_addon(LPCSTR section, bool test);

protected:
    virtual bool install_upgrade_impl(LPCSTR section, bool test);

private:
    float m_hit_probability[egdCount];

public:
    const float& hit_probability() const;

private:
    Fvector m_overriden_activation_speed;
    bool m_activation_speed_is_overriden;
    virtual bool ActivationSpeedOverriden(Fvector& dest, bool clear_override);

    bool m_bRememberActorNVisnStatus;

    Lock render_lock{};

public:
    virtual void SetActivationSpeedOverride(Fvector const& speed);
    bool HasActivationSpeedOverride() const;
    bool GetRememberActorNVisnStatus() { return m_bRememberActorNVisnStatus; };
    virtual void EnableActorNVisnAfterZoom();

    virtual void DumpActiveParams(shared_str const& section_name, CInifile& dst_ini) const;
    virtual shared_str const GetAnticheatSectionName() const { return cNameSect(); };

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};
