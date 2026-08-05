#include "StdAfx.h"
#include "ActorHelmet.h"
#include "Actor.h"
#include "Inventory.h"
#include "BoneProtections.h"
#include "Include/xrRender/Kinematics.h"
#include "xrCommon/xr_hash_map.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Items.h"

namespace
{
struct HelmetFilterRuntimeState
{
    CSE_Abstract* serverEntity{};
    u16 objectId{u16(-1)};
    u16 elapsed{};
    u32 sectionChecksum{};
    bool enabled{};
};

// Sidecar maps preserve the legacy helmet and server-entity layouts.
xr_flat_hash_map<const CHelmet*, HelmetFilterRuntimeState> helmetFilterStates;
xr_flat_hash_map<const CSE_Abstract*, SHelmetFilterSaveState> persistentHelmetFilterStates;
xr_flat_hash_map<u16, SHelmetFilterSaveState> stagedHelmetFilterStates;

u32 helmet_section_checksum(pcstr section)
{
    return crc32(section, static_cast<u32>(xr_strlen(section)));
}

CSE_Abstract* persistent_server_entity(u16 objectId)
{
    return ai().get_alife() ? ai().alife().objects().object(objectId, true) : nullptr;
}
}

CHelmet::CHelmet()
{
    m_flags.set(FUsingCondition, TRUE);
    m_HitTypeProtection.resize(ALife::eHitTypeMax);
    for (u32 i = 0; i < ALife::eHitTypeMax; i++)
        m_HitTypeProtection[i] = 1.0f;

    m_boneProtection = xr_new<SBoneProtections>();
}

CHelmet::~CHelmet()
{
    helmetFilterStates.erase(this);
    xr_delete(m_boneProtection);
}
void CHelmet::Load(LPCSTR section)
{
    inherited::Load(section);

    HelmetFilterRuntimeState& filterState = helmetFilterStates[this];
    filterState.enabled = pSettings->read_if_exists<bool>(section, "use_filters", false);
    filterState.elapsed = filterState.enabled ? 0 : 1;
    filterState.sectionChecksum = helmet_section_checksum(section);

    m_HitTypeProtection[ALife::eHitTypeBurn] = pSettings->r_float(section, "burn_protection");
    m_HitTypeProtection[ALife::eHitTypeStrike] = pSettings->r_float(section, "strike_protection");
    m_HitTypeProtection[ALife::eHitTypeShock] = pSettings->r_float(section, "shock_protection");
    m_HitTypeProtection[ALife::eHitTypeWound] = pSettings->r_float(section, "wound_protection");
    m_HitTypeProtection[ALife::eHitTypeRadiation] = pSettings->r_float(section, "radiation_protection");
    m_HitTypeProtection[ALife::eHitTypeTelepatic] = pSettings->r_float(section, "telepatic_protection");
    m_HitTypeProtection[ALife::eHitTypeChemicalBurn] = pSettings->r_float(section, "chemical_burn_protection");
    m_HitTypeProtection[ALife::eHitTypeExplosion] = pSettings->r_float(section, "explosion_protection");
    m_HitTypeProtection[ALife::eHitTypeFireWound] = pSettings->read_if_exists<float>(section, "fire_wound_protection", 0.0f);
    m_HitTypeProtection[ALife::eHitTypePhysicStrike] = pSettings->read_if_exists<float>(
        section, "physic_strike_protection", m_HitTypeProtection[ALife::eHitTypeStrike]);
    m_HitTypeProtection[ALife::eHitTypeLightBurn] = m_HitTypeProtection[ALife::eHitTypeBurn];

    if (pSettings->line_exist(section, "hit_fraction_actor"))
    {
        m_boneProtection->m_fHitFrac = pSettings->r_float(section, "hit_fraction_actor");

        // Since hit_fraction_actor exists both in CS and COP, but fire_wound_protection was removed in COP,
        // We can use this hacky solution to determine which damage formula to use.
        // It not robust for mods, because they can have fire_wound_protection in configs, despite that
        // original COP engine doesn't read it.
        if (pSettings->line_exist(section, "fire_wound_protection"))
            m_boneProtection->m_hitFracType = SBoneProtections::HitFractionActorCS;
        else
            m_boneProtection->m_hitFracType = SBoneProtections::HitFractionActorCOP;
    }

    if (pSettings->line_exist(section, "nightvision_sect"))
        m_NightVisionSect = pSettings->r_string(section, "nightvision_sect");
    else
        m_NightVisionSect = "";

    m_fHealthRestoreSpeed = READ_IF_EXISTS(pSettings, r_float, section, "health_restore_speed", 0.0f);
    m_fRadiationRestoreSpeed = READ_IF_EXISTS(pSettings, r_float, section, "radiation_restore_speed", 0.0f);
    m_fSatietyRestoreSpeed = READ_IF_EXISTS(pSettings, r_float, section, "satiety_restore_speed", 0.0f);
    m_fPowerRestoreSpeed = READ_IF_EXISTS(pSettings, r_float, section, "power_restore_speed", 0.0f);
    m_fBleedingRestoreSpeed = READ_IF_EXISTS(pSettings, r_float, section, "bleeding_restore_speed", 0.0f);
    m_fPowerLoss = READ_IF_EXISTS(pSettings, r_float, section, "power_loss", 1.0f);
    clamp(m_fPowerLoss, 0.0f, 1.0f);

    m_BonesProtectionSect = READ_IF_EXISTS(pSettings, r_string, section, "bones_koeff_protection", "");
    m_fShowNearestEnemiesDistance = READ_IF_EXISTS(pSettings, r_float, section, "nearest_enemies_show_dist", 0.0f);

    // Added by Axel, to enable optional condition use on any item
    m_flags.set(FUsingCondition, READ_IF_EXISTS(pSettings, r_bool, section, "use_condition", true));
}

void CHelmet::ReloadBonesProtection()
{
    IGameObject* parent = H_Parent();
    if (IsGameTypeSingle())
        parent = smart_cast<IGameObject*>(Level().CurrentViewEntity());

    if (parent && parent->Visual() && m_BonesProtectionSect.size())
        m_boneProtection->reload(m_BonesProtectionSect, smart_cast<IKinematics*>(parent->Visual()));
}

bool CHelmet::net_Spawn(CSE_Abstract* DC)
{
    if (IsGameTypeSingle())
        ReloadBonesProtection();

    BOOL res = inherited::net_Spawn(DC);
    if (!res || !DC)
        return res;

    HelmetFilterRuntimeState& filterState = helmetFilterStates[this];
    filterState.objectId = DC->ID;
    filterState.serverEntity = persistent_server_entity(filterState.objectId);

    const auto staged = stagedHelmetFilterStates.find(DC->ID);
    if (staged != stagedHelmetFilterStates.end())
    {
        if (filterState.enabled && staged->second.sectionChecksum == filterState.sectionChecksum)
            filterState.elapsed = staged->second.elapsed;
        stagedHelmetFilterStates.erase(staged);
    }
    else if (filterState.enabled && filterState.serverEntity)
    {
        const auto persistent = persistentHelmetFilterStates.find(filterState.serverEntity);
        if (persistent != persistentHelmetFilterStates.end())
        {
            if (persistent->second.objectId == DC->ID &&
                persistent->second.sectionChecksum == filterState.sectionChecksum)
            {
                filterState.elapsed = persistent->second.elapsed;
            }
            else
            {
                persistentHelmetFilterStates.erase(persistent);
            }
        }
    }

    if (filterState.enabled && filterState.serverEntity)
    {
        persistentHelmetFilterStates[filterState.serverEntity] =
            { DC->ID, filterState.elapsed, filterState.sectionChecksum };
    }
    else if (!filterState.enabled)
    {
        filterState.elapsed = 1;
        if (filterState.serverEntity)
            persistentHelmetFilterStates.erase(filterState.serverEntity);
    }

    return (res);
}

void CHelmet::net_Export(NET_Packet& P)
{
    inherited::net_Export(P);
    P.w_float_q8(GetCondition(), 0.0f, 1.0f);
}

void CHelmet::net_Import(NET_Packet& P)
{
    inherited::net_Import(P);
    float _cond;
    P.r_float_q8(_cond, 0.0f, 1.0f);
    SetCondition(_cond);
}

void CHelmet::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    //	ReloadBonesProtection();
}

void CHelmet::OnMoveToSlot(const SInvItemPlace& previous_place)
{
    inherited::OnMoveToSlot(previous_place);
    if (m_pInventory && (previous_place.type == eItemPlaceSlot))
    {
        CActor* pActor = smart_cast<CActor*>(H_Parent());
        if (pActor)
        {
            if (pActor->GetNightVisionStatus())
                pActor->SwitchNightVision(true, false);
        }
    }
}

void CHelmet::OnMoveToRuck(const SInvItemPlace& previous_place)
{
    inherited::OnMoveToRuck(previous_place);
    if (m_pInventory && (previous_place.type == eItemPlaceSlot))
    {
        CActor* pActor = smart_cast<CActor*>(H_Parent());
        if (pActor)
            pActor->SwitchNightVision(false);
    }
}

void CHelmet::Hit(float hit_power, ALife::EHitType hit_type)
{
    hit_power *= GetHitImmunity(hit_type);
    ChangeCondition(-hit_power);
}

float CHelmet::GetDefHitTypeProtection(ALife::EHitType hit_type) const
{
    float condition = GetCondition();
    if (UseFilters() &&
        (hit_type == ALife::eHitTypeChemicalBurn || hit_type == ALife::eHitTypeRadiation) &&
        (condition < 0.25f || !GetFiltersElapsed()))
    {
        condition = 0.0f;
    }

    const float base = m_HitTypeProtection[hit_type] * condition;

    if (m_boneProtection->m_hitFracType == SBoneProtections::HitFraction)
        return 1.0f - base; // SOC

    return base; // CS/COP
}

u16 CHelmet::GetFiltersElapsed() const
{
    const auto state = helmetFilterStates.find(this);
    return state != helmetFilterStates.end() && state->second.enabled ? state->second.elapsed : 1;
}

void CHelmet::SetFiltersElapsed(u16 count)
{
    HelmetFilterRuntimeState& state = helmetFilterStates[this];
    state.elapsed = state.enabled ? count : 1;

    if (!state.serverEntity && state.objectId != u16(-1))
        state.serverEntity = persistent_server_entity(state.objectId);
    if (state.enabled && state.serverEntity)
    {
        persistentHelmetFilterStates[state.serverEntity] =
            { state.serverEntity->ID, state.elapsed, state.sectionChecksum };
    }
}

bool CHelmet::UseFilters() const
{
    const auto state = helmetFilterStates.find(this);
    return state != helmetFilterStates.end() && state->second.enabled;
}

void CHelmet::CollectFilterSaveState(xr_vector<SHelmetFilterSaveState>& result)
{
    SHelmetFilterSaveCaptureState state;
    BeginFilterSaveCapture(state);
    while (!ContinueFilterSaveCapture(state, flt_max))
    {
    }
    result = std::move(state.records);
}

void CHelmet::BeginFilterSaveCapture(SHelmetFilterSaveCaptureState& state)
{
    state = {};
    state.initialized = true;
    state.completed = !ai().get_alife() ||
        (persistentHelmetFilterStates.empty() && stagedHelmetFilterStates.empty());
}

bool CHelmet::ContinueFilterSaveCapture(
    SHelmetFilterSaveCaptureState& state, float budgetMilliseconds)
{
    if (!state.initialized)
        return false;
    if (state.completed)
        return true;
    if (!(budgetMilliseconds > 0.0f))
        return false;
    if (!ai().get_alife())
    {
        state.records.clear();
        state.completed = true;
        return true;
    }

    const bool unlimitedBudget = budgetMilliseconds == flt_max;
    CTimer budgetTimer;
    if (!unlimitedBudget)
        budgetTimer.Start();

    u32 batchObjects = 0;
    while (CSE_ALifeDynamicObject* object = ai().alife().objects().next_save_extension_object(
        ESaveExtensionObjectType::Helmet, state.nextObjectId))
    {
        CSE_ALifeItemHelmet* serverHelmet = static_cast<CSE_ALifeItemHelmet*>(object);
        const u16 objectId = serverHelmet->ID;

        if (objectId != u16(-1))
        {
            const u32 sectionChecksum = helmet_section_checksum(serverHelmet->s_name.c_str());
            const auto persistent = persistentHelmetFilterStates.find(serverHelmet);
            auto staged = stagedHelmetFilterStates.find(objectId);
            bool hasPersistentRecord = false;
            if (persistent != persistentHelmetFilterStates.end())
            {
                const SHelmetFilterSaveState& record = persistent->second;
                if (record.objectId == objectId && record.elapsed && record.sectionChecksum == sectionChecksum)
                {
                    state.records.push_back(record);
                    hasPersistentRecord = true;
                }
                else
                {
                    persistentHelmetFilterStates.erase(persistent);
                }
            }

            if (!hasPersistentRecord && staged != stagedHelmetFilterStates.end())
            {
                const SHelmetFilterSaveState& record = staged->second;
                if (record.objectId == objectId && record.elapsed && record.sectionChecksum == sectionChecksum)
                {
                    state.records.push_back(record);
                }
                else
                {
                    stagedHelmetFilterStates.erase(staged);
                }
            }
        }

        if (!unlimitedBudget && ++batchObjects == 16)
        {
            batchObjects = 0;
            if (budgetTimer.GetElapsed_sec() * 1000.0f >= budgetMilliseconds)
                return false;
        }
    }

    state.completed = true;
    return true;
}

void CHelmet::StageFilterSaveState(const xr_vector<SHelmetFilterSaveState>& state)
{
    persistentHelmetFilterStates.clear();
    stagedHelmetFilterStates.clear();
    stagedHelmetFilterStates.reserve(state.size());
    for (const SHelmetFilterSaveState& record : state)
        stagedHelmetFilterStates.insert_or_assign(record.objectId, record);
}

void CHelmet::ClearFilterSaveState()
{
    helmetFilterStates.clear();
    persistentHelmetFilterStates.clear();
    stagedHelmetFilterStates.clear();
}

void CHelmet::ForgetFilterSaveState(const CSE_Abstract& serverObject)
{
    persistentHelmetFilterStates.erase(&serverObject);
    stagedHelmetFilterStates.erase(serverObject.ID);

    for (auto& [helmet, state] : helmetFilterStates)
    {
        if (state.serverEntity != &serverObject)
            continue;

        state.serverEntity = nullptr;
        state.objectId = u16(-1);
    }
}

float CHelmet::GetHitTypeProtection(ALife::EHitType hit_type, s16 element) const
{
    const float base = m_HitTypeProtection[hit_type] * GetCondition();
    const float bone = m_boneProtection->getBoneProtection(element);

    if (m_boneProtection->m_hitFracType == SBoneProtections::HitFraction)
        return 1.0f - base * bone; // SOC

    return base * bone; // CS/COP
}

float CHelmet::GetBoneArmor(s16 element) const
{
    return m_boneProtection->getBoneArmor(element);
}

bool CHelmet::install_upgrade_impl(LPCSTR section, bool test)
{
    bool result = inherited::install_upgrade_impl(section, test);

    result |= process_if_exists(
        section, "burn_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeBurn], test);
    result |= process_if_exists(
        section, "shock_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeShock], test);
    result |= process_if_exists(
        section, "strike_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeStrike], test);
    result |= process_if_exists(
        section, "wound_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeWound], test);
    result |= process_if_exists(
        section, "radiation_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeRadiation], test);
    result |= process_if_exists(
        section, "telepatic_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeTelepatic], test);
    result |= process_if_exists(section, "chemical_burn_protection", &CInifile::r_float,
                                m_HitTypeProtection[ALife::eHitTypeChemicalBurn], test);
    result |= process_if_exists(
        section, "explosion_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeExplosion], test);
    result |= process_if_exists(
        section, "fire_wound_protection", &CInifile::r_float, m_HitTypeProtection[ALife::eHitTypeFireWound], test);

    LPCSTR str{};
    bool result2 = process_if_exists_set(section, "nightvision_sect", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_NightVisionSect._set(str);
    }
    result |= result2;

    result |= process_if_exists(section, "health_restore_speed", &CInifile::r_float, m_fHealthRestoreSpeed, test);
    result |= process_if_exists(section, "radiation_restore_speed", &CInifile::r_float, m_fRadiationRestoreSpeed, test);
    result |= process_if_exists(section, "satiety_restore_speed", &CInifile::r_float, m_fSatietyRestoreSpeed, test);
    result |= process_if_exists(section, "power_restore_speed", &CInifile::r_float, m_fPowerRestoreSpeed, test);
    result |= process_if_exists(section, "bleeding_restore_speed", &CInifile::r_float, m_fBleedingRestoreSpeed, test);

    result |= process_if_exists(section, "power_loss", &CInifile::r_float, m_fPowerLoss, test);
    clamp(m_fPowerLoss, 0.0f, 1.0f);

    result |= process_if_exists(
        section, "nearest_enemies_show_dist", &CInifile::r_float, m_fShowNearestEnemiesDistance, test);

    result2 = process_if_exists_set(section, "bones_koeff_protection", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_BonesProtectionSect = str;
        ReloadBonesProtection();
    }
    result2 = process_if_exists_set(section, "bones_koeff_protection_add", &CInifile::r_string, str, test);
    if (result2 && !test)
        AddBonesProtection(str);

    if (m_boneProtection->m_hitFracType == SBoneProtections::HitFractionActorCS ||
        m_boneProtection->m_hitFracType == SBoneProtections::HitFractionActorCOP)
    {
        result |= process_if_exists(section, "hit_fraction_actor", &CInifile::r_float, m_boneProtection->m_fHitFrac, test);
    }

    return result;
}

void CHelmet::AddBonesProtection(LPCSTR bones_section)
{
    IGameObject* parent = H_Parent();
    if (IsGameTypeSingle())
        parent = smart_cast<IGameObject*>(Level().CurrentViewEntity());

    if (parent && parent->Visual() && m_BonesProtectionSect.size())
        m_boneProtection->add(bones_section, smart_cast<IKinematics*>(parent->Visual()));
}

float CHelmet::HitThroughArmor(float hit_power, s16 element, float ap, bool& add_wound, ALife::EHitType hit_type)
{
    float NewHitPower = hit_power;

    switch (m_boneProtection->m_hitFracType)
    {
    default:
    case SBoneProtections::HitFractionActorCOP:
    case SBoneProtections::HitFractionActorCS:
    {
        const float ba = element == static_cast<s16>(BI_NONE) ? -1.0f : GetBoneArmor(element);
        if (element != static_cast<s16>(BI_NONE) && ba <= 0.0f)
            return NewHitPower;

        if (hit_type == ALife::eHitTypeFireWound)
        {
            if (ba < 0.0f)
                return NewHitPower;

            float BoneArmor = ba * GetCondition();
            if (/*!fis_zero(ba, EPS) && */ ap > BoneArmor)
            {
                //пуля пробила бронь
                if (!IsGameTypeSingle())
                {
                    float hit_fraction = (ap - BoneArmor) / ap;
                    if (hit_fraction < m_boneProtection->m_fHitFrac)
                        hit_fraction = m_boneProtection->m_fHitFrac;

                    NewHitPower *= hit_fraction;
                    NewHitPower *= m_boneProtection->getBoneProtection(element);
                }

                VERIFY(NewHitPower >= 0.0f);
            }
            else
            {
                //пуля НЕ пробила бронь
                NewHitPower *= m_boneProtection->m_fHitFrac;
                add_wound = false; //раны нет
            }
        }
        else
        {
            // Original Dead Air applies the full condition-scaled helmet protection to non-bullet hits.
            NewHitPower -= GetDefHitTypeProtection(hit_type);
            if (NewHitPower < 0.0f)
                NewHitPower = 0.0f;
        }

        //увеличить изношенность шлема
        Hit(hit_power, hit_type);
        break;
    }
    case SBoneProtections::HitFraction:
    {
        if (hit_type == ALife::eHitTypeFireWound)
        {
            const float BoneArmor = m_boneProtection->getBoneArmor(element) * GetCondition() * (1 - ap);
            NewHitPower -= BoneArmor;
            if (NewHitPower < hit_power * m_boneProtection->m_fHitFrac)
                NewHitPower = hit_power * m_boneProtection->m_fHitFrac;
        }
        else
        {
            NewHitPower -= GetHitTypeProtection(hit_type, element);
        }

        //увеличить изношенность шлема
        Hit(hit_power, hit_type);
        break;
    }
    } // switch (m_boneProtection->m_hitFracType)

    return NewHitPower;
}
