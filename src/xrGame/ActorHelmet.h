#pragma once

#include "inventory_item_object.h"

struct SBoneProtections;

struct SHelmetFilterSaveState
{
    u16 objectId{};
    u16 elapsed{};
    u32 sectionChecksum{};
};

struct SHelmetFilterSaveCaptureState
{
    xr_vector<SHelmetFilterSaveState> records;
    u32 nextObjectId{};
    bool initialized{};
    bool completed{};
};

class CHelmet : public CInventoryItemObject
{
    using inherited = CInventoryItemObject;

public:
    CHelmet();
    virtual ~CHelmet();

    virtual void Load(LPCSTR section);

    virtual void Hit(float P, ALife::EHitType hit_type);

    shared_str m_BonesProtectionSect;
    shared_str m_NightVisionSect;

    virtual void OnMoveToSlot(const SInvItemPlace& previous_place);
    virtual void OnMoveToRuck(const SInvItemPlace& previous_place);
    virtual bool net_Spawn(CSE_Abstract* DC);
    virtual void net_Export(NET_Packet& P);
    virtual void net_Import(NET_Packet& P);
    virtual void OnH_A_Chield();

    [[nodiscard]] float GetDefHitTypeProtection(ALife::EHitType hit_type) const;
    [[nodiscard]] float GetHitTypeProtection(ALife::EHitType hit_type, s16 element) const;
    [[nodiscard]] float GetBoneArmor(s16 element) const;

    float HitThroughArmor(float hit_power, s16 element, float ap, bool& add_wound, ALife::EHitType hit_type);

    float m_fPowerLoss;
    float m_fHealthRestoreSpeed;
    float m_fRadiationRestoreSpeed;
    float m_fSatietyRestoreSpeed;
    float m_fPowerRestoreSpeed;
    float m_fBleedingRestoreSpeed;

    float m_fShowNearestEnemiesDistance;

    void ReloadBonesProtection();
    void AddBonesProtection(LPCSTR bones_section);

    [[nodiscard]] u16 GetFiltersElapsed() const;
    void SetFiltersElapsed(u16 count);
    [[nodiscard]] bool UseFilters() const;

    static void CollectFilterSaveState(xr_vector<SHelmetFilterSaveState>& result);
    static void BeginFilterSaveCapture(SHelmetFilterSaveCaptureState& state);
    [[nodiscard]] static bool ContinueFilterSaveCapture(
        SHelmetFilterSaveCaptureState& state, float budgetMilliseconds);
    static void StageFilterSaveState(const xr_vector<SHelmetFilterSaveState>& state);
    static void ClearFilterSaveState();
    static void ForgetFilterSaveState(const CSE_Abstract& serverObject);

protected:
    mutable HitImmunity::HitTypeSVec m_HitTypeProtection;
    SBoneProtections* m_boneProtection;

protected:
    virtual bool install_upgrade_impl(LPCSTR section, bool test);

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CGameObject);
};
