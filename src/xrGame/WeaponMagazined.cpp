#include "pch_script.h"

#include "WeaponMagazined.h"
#include "Actor.h"
#include "ParticlesObject.h"
#include "Scope.h"
#include "Silencer.h"
#include "GrenadeLauncher.h"
#include "Inventory.h"
#include "InventoryOwner.h"
#include "xrServer_Objects_ALife_Items.h"
#include "ActorEffector.h"
#include "EffectorZoomInertion.h"
#include "xrEngine/xr_level_controller.h"
#include "UIGameCustom.h"
#include "Common/object_broker.h"
#include "MPPlayersBag.h"
#include "ui/UIXmlInit.h"
#include "xrUICore/Static/UIStatic.h"
#include "game_object_space.h"
#include "xrScriptEngine/script_callback_ex.h"
#include "script_game_object.h"
#include "HudSound.h"

CWeaponMagazined::CWeaponMagazined(ESoundTypes eSoundType) : CWeapon(), m_bStopedAfterQueueFired(false)
{
    m_eSoundShow = ESoundTypes(SOUND_TYPE_ITEM_TAKING | eSoundType);
    m_eSoundHide = ESoundTypes(SOUND_TYPE_ITEM_HIDING | eSoundType);

    m_eSoundShot = ESoundTypes(SOUND_TYPE_WEAPON_SHOOTING | eSoundType);
    m_eSoundEmptyClick = ESoundTypes(SOUND_TYPE_WEAPON_EMPTY_CLICKING | eSoundType);

    m_eSoundReload = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING | eSoundType);
    m_eSoundReloadEmpty = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING | eSoundType);

    m_sounds_enabled = true;

    m_sSndShotCurrent = nullptr;
    m_sSilencerFlameParticles = m_sSilencerSmokeParticles = nullptr;

    m_bFireSingleShot = false;
    m_iShotNum = 0;
    m_fOldBulletSpeed = 0;
    m_iQueueSize = WEAPON_ININITE_QUEUE;
    m_magazine_flags.zero();
    m_condition_coeff = 1.f;
    m_condition_available = 0;
}

CWeaponMagazined::~CWeaponMagazined()
{
    // sounds
}

void CWeaponMagazined::net_Destroy() { inherited::net_Destroy(); }

//AVO: for custom added sounds check if sound exists
bool CWeaponMagazined::WeaponSoundExist(pcstr section, pcstr sound_name) const
{
    pcstr str;
    if (process_if_exists_set(section, sound_name, &CInifile::r_string, str, true))
        return true;
#ifdef DEBUG
    Msg("~ [WARNING] ------ Sound [%s] does not exist in [%s]", sound_name, section);
#endif
    return false;
}

//-AVO

void CWeaponMagazined::Load(LPCSTR section)
{
    inherited::Load(section);

    // Sounds
    m_sounds.LoadSound(section, "snd_draw", "sndShow", true, m_eSoundShow);
    m_sounds.LoadSound(section, "snd_holster", "sndHide", true, m_eSoundHide);

    //Alundaio: LAYERED_SND_SHOOT
    m_layered_sounds.LoadSound(section, "snd_shoot", "sndShot", false, m_eSoundShot);
    //-Alundaio

    m_sounds.LoadSound(section, "snd_empty", "sndEmptyClick", true, m_eSoundEmptyClick);
    m_sounds.LoadSound(section, "snd_reload", "sndReload", true, m_eSoundReload);

    if (WeaponSoundExist(section, "snd_pump"))
        m_sounds.LoadSound(section, "snd_pump", "sndPump", false, m_eSoundReload);
    if (WeaponSoundExist(section, "snd_fire_mode"))
        m_sounds.LoadSound(section, "snd_fire_mode", "sndFireMode", true, m_eSoundReload);
    if (WeaponSoundExist(section, "snd_reload_empty"))
        m_sounds.LoadSound(section, "snd_reload_empty", "sndReloadEmpty", true, m_eSoundReloadEmpty);
    if (WeaponSoundExist(section, "snd_reload_misfire"))
        m_sounds.LoadSound(section, "snd_reload_misfire", "sndReloadMisfire", true, m_eSoundReloadMisfire);

    //звуки и партиклы глушителя, если такой есть
    if (m_eSilencerStatus == ALife::eAddonAttachable || m_eSilencerStatus == ALife::eAddonPermanent)
    {
        if (pSettings->line_exist(section, "silencer_flame_particles"))
            m_sSilencerFlameParticles = pSettings->r_string(section, "silencer_flame_particles");
        if (pSettings->line_exist(section, "silencer_smoke_particles"))
            m_sSilencerSmokeParticles = pSettings->r_string(section, "silencer_smoke_particles");

        //Alundaio: LAYERED_SND_SHOOT Silencer
        m_layered_sounds.LoadSound(section, "snd_silncer_shot", "sndSilencerShot", false, m_eSoundShot);
        if (WeaponSoundExist(section, "snd_silncer_shot_actor"))
            m_layered_sounds.LoadSound(
                section, "snd_silncer_shot_actor", "sndSilencerShotActor", false, m_eSoundShot);
        //-Alundaio
    }

    if (IsSilencerAttached() && m_layered_sounds.FindSoundItem("sndSilencerShot", false))
        m_sSndShotCurrent = "sndSilencerShot";
    else
        m_sSndShotCurrent = "sndShot";

    m_condition_coeff = READ_IF_EXISTS(pSettings, r_float, section, "condition_coeff", 1.f);
    m_condition_available = READ_IF_EXISTS(pSettings, r_u32, section, "condition_avail", 0);
    m_magazine_flags.set(
        mfDetachableMagazine, READ_IF_EXISTS(pSettings, r_bool, section, "magazined", false));
    m_magazine_flags.set(mfBoltAction, READ_IF_EXISTS(pSettings, r_bool, section, "bolt_action", false));
    m_magazine_flags.set(mfAlwaysLoad, READ_IF_EXISTS(pSettings, r_bool, section, "always_load", false));

    m_iBaseDispersionedBulletsCount = READ_IF_EXISTS(pSettings, r_u8, section, "base_dispersioned_bullets_count", 0);
    m_fBaseDispersionedBulletsSpeed =
        READ_IF_EXISTS(pSettings, r_float, section, "base_dispersioned_bullets_speed", m_fStartBulletSpeed);

    if (pSettings->line_exist(section, "fire_modes"))
    {
        shared_str FireModesList = pSettings->r_string(section, "fire_modes");
        int ModesCount = _GetItemCount(FireModesList.c_str());
        m_bHasDifferentFireModes = ModesCount > 1;
        m_aFireModes.clear();

        for (int i = 0; i < ModesCount; i++)
        {
            string16 sItem;
            _GetItem(FireModesList.c_str(), i, sItem);
            m_aFireModes.push_back((s8)atoi(sItem));
        }

        m_iCurFireMode = ModesCount - 1;
        m_iPrefferedFireMode = READ_IF_EXISTS(pSettings, r_s16, section, "preffered_fire_mode", -1);
    }
    else
    {
        m_bHasDifferentFireModes = false;
    }
    LoadSilencerKoeffs();
}

void CWeaponMagazined::FireStart()
{
    if (HasConditionType(eWeaponConditionChamberCycle) && iAmmoElapsed)
    {
        bMisfire = true;
        SwitchState(eReload);
        return;
    }

    if (!IsMisfire())
    {
        if (IsValid())
        {
            if (!IsWorking() || AllowFireWhileWorking())
            {
                if (GetState() == eReload)
                    return;
                if (GetState() == eShowing)
                    return;
                if (GetState() == eHiding)
                    return;
                if (GetState() == eMisfire)
                    return;

                inherited::FireStart();

                if (iAmmoElapsed == 0)
                    OnMagazineEmpty();
                else
                {
                    R_ASSERT(H_Parent());
                    SwitchState(eFire);
                }
            }
        }
        else
        {
            if (eReload != GetState())
                OnMagazineEmpty();
        }
    }
    else // misfire
    {
        // Alundaio
        if (const auto object = smart_cast<CGameObject*>(H_Parent()))
        {
            object->callback(GameObject::eOnWeaponJammed)(object->lua_game_object(), this->lua_game_object());
        }

        if (smart_cast<CActor*>(this->H_Parent()) && (Level().CurrentViewEntity() == H_Parent()))
            CurrentGameUI()->AddCustomStatic("gun_jammed", true);

        OnEmptyClick();
    }
}

void CWeaponMagazined::FireEnd()
{
    inherited::FireEnd();
}

void CWeaponMagazined::Reload()
{
    inherited::Reload();
    TryReload();
}

bool CWeaponMagazined::TryReload()
{
    if (m_pInventory)
    {
        if (!ParentIsActor())
        {
            SetPending(true);
            SwitchState(eReload);
            return true;
        }

        if (IsGameTypeSingle() && ParentIsActor())
        {
            int AC = GetSuitableAmmoTotal();
            Actor()->callback(GameObject::eWeaponNoAmmoAvailable)(lua_game_object(), AC);
        }

        m_pCurrentAmmo = GetAmmoForReload(m_ammoTypes[m_ammoType].c_str());

        if (IsMisfire() && iAmmoElapsed)
        {
            SetPending(true);
            SwitchState(eReload);
            return true;
        }

        if (m_pCurrentAmmo || unlimited_ammo())
        {
            SetPending(true);
            SwitchState(eReload);
            return true;
        }
        else
            for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
            {
                m_pCurrentAmmo = GetAmmoForReload(m_ammoTypes[i].c_str());
                if (m_pCurrentAmmo)
                {
                    m_set_next_ammoType_on_reload = i;
                    SetPending(true);
                    SwitchState(eReload);
                    return true;
                }
            }
    }

    if (GetState() != eIdle)
        SwitchState(eIdle);

    return false;
}

bool CWeaponMagazined::IsAmmoAvailable()
{
    if (GetAmmoForReload(m_ammoTypes[m_ammoType].c_str()))
        return (true);
    else
        for (u32 i = 0; i < m_ammoTypes.size(); ++i)
            if (GetAmmoForReload(m_ammoTypes[i].c_str()))
                return (true);
    return (false);
}

void CWeaponMagazined::OnMagazineEmpty()
{
    if (IsGameTypeSingle() && ParentIsActor())
    {
        int AC = GetSuitableAmmoTotal();
        Actor()->callback(GameObject::eOnWeaponMagazineEmpty)(lua_game_object(), AC);
    }

    if (ParentIsActor() && m_magazine_flags.test(mfDetachableMagazine) &&
        !m_magazine_flags.test(mfBoltAction) && !m_magazine_flags.test(mfAlwaysLoad))
        SetConditionType(GetConditionType() | eWeaponConditionChamberCycle);

    if (GetState() == eIdle)
    {
        OnEmptyClick();
        return;
    }

    if (GetNextState() != eMagEmpty && GetNextState() != eReload)
    {
        SwitchState(eMagEmpty);
    }

    inherited::OnMagazineEmpty();
}

void CWeaponMagazined::UnloadMagazine(bool spawn_ammo)
{
    const bool detachable_magazine = m_magazine_flags.test(mfDetachableMagazine);
    if (detachable_magazine)
        SetConditionType(GetConditionType() | eWeaponConditionMagazineRemoved);

    const size_t cartridges_to_keep =
        detachable_magazine && !HasConditionType(eWeaponConditionChamberCycle) &&
            !m_magazine_flags.test(mfAlwaysLoad) ?
        1 :
        0;
    xr_map<LPCSTR, u16> l_ammo;

    while (m_magazine.size() > cartridges_to_keep)
    {
        CCartridge& l_cartridge = m_magazine.back();
        xr_map<LPCSTR, u16>::iterator l_it;
        for (l_it = l_ammo.begin(); l_ammo.end() != l_it; ++l_it)
        {
            if (!xr_strcmp(l_cartridge.m_ammoSect.c_str(), l_it->first))
            {
                ++(l_it->second);
                break;
            }
        }

        if (l_it == l_ammo.end())
            l_ammo[l_cartridge.m_ammoSect.c_str()] = 1;
        m_magazine.pop_back();
        --iAmmoElapsed;
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    if (IsGameTypeSingle() && ParentIsActor())
    {
        int AC = GetSuitableAmmoTotal();
        Actor()->callback(GameObject::eOnWeaponMagazineEmpty)(lua_game_object(), AC);
    }

    if (!spawn_ammo)
        return;

    xr_map<LPCSTR, u16>::iterator l_it;
    for (l_it = l_ammo.begin(); l_ammo.end() != l_it; ++l_it)
    {
        if (m_pInventory && !UsesAmmoBelt())
        {
            CWeaponAmmo* l_pA = smart_cast<CWeaponAmmo*>(m_pInventory->GetAny(l_it->first));
            if (l_pA)
            {
                u16 l_free = l_pA->m_boxSize - l_pA->m_boxCurr;
                l_pA->m_boxCurr = l_pA->m_boxCurr + (l_free < l_it->second ? l_free : l_it->second);
                l_it->second = l_it->second - (l_free < l_it->second ? l_free : l_it->second);
            }
        }
        if (l_it->second && !unlimited_ammo())
            SpawnAmmo(l_it->second, l_it->first);
    }
}

bool CWeaponMagazined::HaveAmmoInMagazine() const
{
    const size_t minimumAmmo =
        HasConditionType(eWeaponConditionChamberCycle) ||
            !m_magazine_flags.test(mfDetachableMagazine) || m_magazine_flags.test(mfAlwaysLoad) ?
        0 :
        1;
    return m_magazine.size() > minimumAmmo;
}

void CWeaponMagazined::ReloadMagazine()
{
    m_BriefInfo_CalcFrame = 0;

    if (IsMisfire())
    {
        if (!m_magazine_flags.test(mfBoltAction) &&
            !HasConditionType(eWeaponConditionChamberCycle) && !m_magazine.empty())
        {
            m_magazine.pop_back();
            --iAmmoElapsed;
            VERIFY((u32)iAmmoElapsed == m_magazine.size());
        }

        SetConditionType(GetConditionType() & ~u32(eWeaponConditionChamberCycle));
        bMisfire = false;
        return;
    }

    if (!m_magazine_flags.test(mfLockAmmoType))
    {
        m_pCurrentAmmo = nullptr;
    }

    if (!m_pInventory)
        return;

    if (m_set_next_ammoType_on_reload != undefined_ammo_type)
    {
        m_ammoType = m_set_next_ammoType_on_reload;
        m_set_next_ammoType_on_reload = undefined_ammo_type;
    }

    if (!unlimited_ammo())
    {
        if (m_ammoTypes.size() <= m_ammoType)
            return;

        LPCSTR tmp_sect_name = m_ammoTypes[m_ammoType].c_str();

        if (!tmp_sect_name)
            return;

        //попытаться найти в инвентаре патроны текущего типа
        m_pCurrentAmmo = GetAmmoForReload(tmp_sect_name);

        if (!m_pCurrentAmmo && !m_magazine_flags.test(mfLockAmmoType))
        {
            for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
            {
                //проверить патроны всех подходящих типов
                m_pCurrentAmmo = GetAmmoForReload(m_ammoTypes[i].c_str());
                if (m_pCurrentAmmo)
                {
                    m_ammoType = i;
                    break;
                }
            }
        }
    }

    //нет патронов для перезарядки
    if (!m_pCurrentAmmo && !unlimited_ammo())
        return;

    u32 condition_type = GetConditionType() & ~eWeaponConditionMagazineRemoved;
    if (!m_magazine_flags.test(mfBoltAction) || m_magazine_flags.test(mfAlwaysLoad))
        condition_type &= ~eWeaponConditionChamberCycle;
    SetConditionType(condition_type);

    //разрядить магазин, если загружаем патронами другого типа
    if (!m_magazine_flags.test(mfLockAmmoType) && !m_magazine.empty() &&
        (!m_pCurrentAmmo || xr_strcmp(m_pCurrentAmmo->cNameSect(), m_magazine.back().m_ammoSect.c_str())))
        UnloadMagazine();

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    if (m_DefaultCartridge.m_LocalAmmoType != m_ammoType)
        m_DefaultCartridge.Load(m_ammoTypes[m_ammoType].c_str(), m_ammoType);
    CCartridge l_cartridge = m_DefaultCartridge;
    while (iAmmoElapsed < iMagazineSize)
    {
        if (!unlimited_ammo())
        {
            if (!m_pCurrentAmmo->Get(l_cartridge))
                break;
        }
        ++iAmmoElapsed;
        l_cartridge.m_LocalAmmoType = m_ammoType;
        m_magazine.push_back(l_cartridge);
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
    SetConditionType(GetConditionType() & ~eWeaponConditionMagazineRemoved);

    //выкинуть коробку патронов, если она пустая
    if (m_pCurrentAmmo && !m_pCurrentAmmo->m_boxCurr && OnServer())
        m_pCurrentAmmo->SetDropManual(true);

    if (iMagazineSize > iAmmoElapsed)
    {
        m_magazine_flags.set(mfLockAmmoType, true);
        ReloadMagazine();
        m_magazine_flags.set(mfLockAmmoType, false);
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());
}

void CWeaponMagazined::OnStateSwitch(u32 S, u32 oldState)
{
    inherited::OnStateSwitch(S, oldState);
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(this->H_Parent());
    switch (S)
    {
    case eIdle: switch2_Idle(); break;
    case eFire: switch2_Fire(); break;
    case eMisfire:
        if (smart_cast<CActor*>(this->H_Parent()) && (Level().CurrentViewEntity() == H_Parent()))
            CurrentGameUI()->AddCustomStatic("gun_jammed", true);
        break;
    case eMagEmpty: switch2_Empty(); break;
    case eReload:
        if (owner)
            m_sounds_enabled = owner->CanPlayShHdRldSounds();
        switch2_Reload();
        break;
    case eShowing:
        if (owner)
            m_sounds_enabled = owner->CanPlayShHdRldSounds();
        switch2_Showing();
        break;
    case eHiding:
        if (owner)
            m_sounds_enabled = owner->CanPlayShHdRldSounds();
        if (oldState != eHiding)
            switch2_Hiding();
        break;
    case eHidden: switch2_Hidden(); break;
    }
}

void CWeaponMagazined::UpdateCL()
{
    inherited::UpdateCL();
    float dt = Device.fTimeDelta;

    //когда происходит апдейт состояния оружия
    //ничего другого не делать
    if (GetNextState() == GetState())
    {
        switch (GetState())
        {
        case eShowing:
        case eHiding:
        case eReload:
        case eIdle:
        {
            fShotTimeCounter -= dt;
            clamp(fShotTimeCounter, 0.0f, flt_max);
        }
        break;
        case eFire: { state_Fire(dt);
        }
        break;
        case eMisfire: state_Misfire(dt); break;
        case eMagEmpty: state_MagEmpty(dt); break;
        case eHidden: break;
        }
    }

    UpdateSounds();
}

void CWeaponMagazined::UpdateSounds()
{
    if (Device.dwFrame == dwUpdateSounds_Frame)
        return;

    dwUpdateSounds_Frame = Device.dwFrame;

    Fvector P = get_LastFP();
    m_sounds.SetPosition("sndShow", P);
    m_sounds.SetPosition("sndHide", P);
    //. nah	m_sounds.SetPosition("sndShot", P);
    m_sounds.SetPosition("sndReload", P);

    if (m_sounds.FindSoundItem("sndReloadEmpty", false))
        m_sounds.SetPosition("sndReloadEmpty", P);
    if (m_sounds.FindSoundItem("sndPump", false))
        m_sounds.SetPosition("sndPump", P);
    if (m_sounds.FindSoundItem("sndFireMode", false))
        m_sounds.SetPosition("sndFireMode", P);

    //. nah	m_sounds.SetPosition("sndEmptyClick", P);
}

void CWeaponMagazined::state_Fire(float dt)
{
    if (iAmmoElapsed > 0)
    {
        VERIFY(fOneShotTime > 0.f);

        Fvector p1, d;
        p1.set(get_LastFP());
        d.set(get_LastFD());

        if (!H_Parent())
        {
            StopShooting();
            return;
        }
        if (smart_cast<CMPPlayersBag*>(H_Parent()) != nullptr)
        {
            Msg("! WARNING: state_Fire of object [%d][%s] while parent is CMPPlayerBag...", ID(), cNameSect().c_str());
            return;
        }

        CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
        if (nullptr == io->inventory().ActiveItem())
        {
            Log("current_state", GetState());
            Log("next_state", GetNextState());
            Log("item_sect", cNameSect().c_str());
            Log("H_Parent", H_Parent()->cNameSect().c_str());
            StopShooting();
            return; //Alundaio: This is not supposed to happen but it does. GSC was aware but why no return here? Known to cause crash on game load if NPC immediately enters combat.
        }

        CEntity* E = smart_cast<CEntity*>(H_Parent());
        E->g_fireParams(this, p1, d);

        if (!E->g_stateFire())
            StopShooting();

        if (m_iShotNum == 0)
        {
            m_vStartPos = p1;
            m_vStartDir = d;
        };

        VERIFY(!m_magazine.empty());

        while (!m_magazine.empty() && fShotTimeCounter < 0 && (IsWorking() || m_bFireSingleShot) &&
            (m_iQueueSize < 0 || m_iShotNum < m_iQueueSize))
        {
            if (CheckForMisfire())
            {
                StopShooting();
                OnEmptyClick();
                return;
            }

            m_bFireSingleShot = false;

            //Alundaio: Use fModeShotTime instead of fOneShotTime if current fire mode is 2-shot burst
            //Alundaio: Cycle down RPM after two shots; used for Abakan/AN-94
            if (GetCurrentFireMode() == 2 || GetCurrentFireMode() == 3 || (bCycleDown && m_iShotNum <= 1))
                fShotTimeCounter = fModeShotTime;
            else if (ParentIsActor())
                fShotTimeCounter = GetActorShotTime();
            else
                fShotTimeCounter = fOneShotTime;

            float delay_penalty = 0.f;
            if (m_condition_type & 0x4)
                delay_penalty += 0.3f;
            if (m_condition_type & 0x8)
                delay_penalty += 0.6f;
            if (GetCurrentFireMode() == -1)
            {
                if (m_condition_type & 0x10)
                    delay_penalty += 0.2f;
                if (m_condition_type & 0x20)
                    delay_penalty += 0.4f;
            }
            fShotTimeCounter *= 1.f + ::Random.randF(0.5f, 2.5f) * delay_penalty;
            //Alundaio: END

            ++m_iShotNum;
            TryAddConditionFailure();

            OnShot();

            if (m_iShotNum > m_iBaseDispersionedBulletsCount)
                FireTrace(p1, d);
            else
                FireTrace(m_vStartPos, m_vStartDir);
        }

        if (m_iShotNum == m_iQueueSize)
            m_bStopedAfterQueueFired = true;

        UpdateSounds();
    }

    if (fShotTimeCounter < 0)
    {
        /*
                if(bDebug && H_Parent() && (H_Parent()->ID() != Actor()->ID()))
                {
                    Msg("stop shooting w=[%s] magsize=[%d] sshot=[%s] qsize=[%d] shotnum=[%d]",
                            IsWorking()?"true":"false",
                            m_magazine.size(),
                            m_bFireSingleShot?"true":"false",
                            m_iQueueSize,
                            m_iShotNum);
                }
        */
        if (iAmmoElapsed == 0)
            OnMagazineEmpty();

        if (m_dwMotionCurrTm >= m_dwMotionEndTm)
            StopShooting();
    }
    else
    {
        fShotTimeCounter -= dt;
    }
}

void CWeaponMagazined::TryAddConditionFailure()
{
    if (!ParentIsActor())
        return;

    const float scaled_deterioration = (GetWeaponDeterioration() + 0.0001f) * 1000.f;
    const int first_divisor = _max(1, int(m_condition_coeff * 50.f / scaled_deterioration));
    const int second_divisor =
        _max(1, int(m_condition_coeff * 100.f * _max(GetCondition(), 0.01f) / scaled_deterioration + 20.f));
    const int third_divisor =
        _max(1, int(m_condition_coeff * 50.f * _max(GetCondition(), 0.01f) / scaled_deterioration + 1.f));

    u32 condition_id = 0;
    if (::Random.randI(first_divisor) == 0)
    {
        static constexpr u32 failures[] = {1, 3, 5, 11, 16, 19};
        condition_id = failures[::Random.randI(std::size(failures))];
    }

    if (::Random.randI(second_divisor) == 0)
    {
        static constexpr u32 failures[] = {4, 6, 9, 12, 14, 17, 20};
        condition_id = failures[::Random.randI(std::size(failures))];
    }

    if (::Random.randI(third_divisor) == 0)
    {
        static constexpr u32 prerequisites[] = {0x100, 0x800, 0x2000, 0x10000, 0x80000};
        static constexpr u32 failures[] = {10, 13, 15, 18, 21};
        const u32 index = ::Random.randI(std::size(failures));
        if (m_condition_type & prerequisites[index])
            condition_id = failures[index];
    }

    if (condition_id && (m_condition_available & (1u << condition_id)))
        m_condition_type |= 1u << (condition_id - 1);
}

void CWeaponMagazined::state_Misfire(float dt)
{
    SwitchState(eIdle);

    bMisfire = true;

    UpdateSounds();
}

void CWeaponMagazined::state_MagEmpty(float dt) {}
void CWeaponMagazined::SetDefaults() { CWeapon::SetDefaults(); }
void CWeaponMagazined::OnShot()
{
    // Sound
    //Alundaio: LAYERED_SND_SHOOT
    m_layered_sounds.PlaySound(m_sSndShotCurrent.c_str(), get_LastFP(), H_Root(), !!GetHUDmode(), false, (u8)-1);
    //-Alundaio

    if (m_sounds.FindSoundItem("sndPump", false))
        PlaySound("sndPump", get_LastFP());

    // Camera
    AddShotEffector();

    // Animation
    PlayAnimShoot();

    // Shell Drop
    Fvector vel;
    PHGetLinearVell(vel);
    OnShellDrop(get_LastSP(), vel);

    // Огонь из ствола
    StartFlameParticles();

    //дым из ствола
    ForceUpdateFireParticles();
    StartSmokeParticles(get_LastFP(), vel);

    if (m_magazine_flags.test(mfBoltAction))
    {
        if (ParentIsActor())
            SetConditionType(GetConditionType() | eWeaponConditionChamberCycle);
        else if (m_sounds.FindSoundItem("sndReloadMisfire", false))
            PlaySound("sndReloadMisfire", get_LastFP());
    }
}

void CWeaponMagazined::OnEmptyClick() { PlaySound("sndEmptyClick", get_LastFP()); }
void CWeaponMagazined::OnAnimationEnd(u32 state)
{
    switch (state)
    {
    case eFire:
        SwitchState(eIdle);
        break;
    case eReload:
        ReloadMagazine();
        SwitchState(eIdle);
        break; // End of reload animation
    case eHiding:
        SwitchState(eHidden);
        break; // End of Hide
    case eShowing:
        SwitchState(eIdle);
        break; // End of Show
    case eIdle:
        switch2_Idle();
        break; // Keep showing idle
    }
    inherited::OnAnimationEnd(state);
}

void CWeaponMagazined::switch2_Idle()
{
    m_iShotNum = 0;
    if (m_fOldBulletSpeed != 0.f)
        SetBulletSpeed(m_fOldBulletSpeed);

    SetPending(false);
    PlayAnimIdle();
}

#ifdef DEBUG
#include "ai/stalker/ai_stalker.h"
#include "object_handler_planner.h"
#endif
void CWeaponMagazined::switch2_Fire()
{
    if (GetState() == eFire)
        return;

    CInventoryOwner* io = smart_cast<CInventoryOwner*>(H_Parent());
    if (!io)
        return;

    CInventoryItem* ii = smart_cast<CInventoryItem*>(this);
    if (ii != io->inventory().ActiveItem())
    {
        Msg("~ WARNING: Not an active item, item %s, owner %s, active item %s",
            cName().c_str(), H_Parent()->cName().c_str(),
            io->inventory().ActiveItem() ? io->inventory().ActiveItem()->object().cName().c_str() : "no_active_item");
        return;
    }

#ifdef DEBUG
    if (ii != io->inventory().ActiveItem())
        Msg("! not an active item, item %s, owner %s, active item %s", cName().c_str(), H_Parent()->cName().c_str(),
            io->inventory().ActiveItem() ? io->inventory().ActiveItem()->object().cName().c_str() : "no_active_item");

    if (!(io && (ii == io->inventory().ActiveItem())))
    {
        if (const auto stalker = smart_cast<CAI_Stalker*>(H_Parent()))
        {
            stalker->planner().show();
            stalker->planner().show_current_world_state();
            stalker->planner().show_target_world_state();
        }
    }
#endif // DEBUG

    m_bStopedAfterQueueFired = false;
    m_bFireSingleShot = true;
    m_iShotNum = 0;

    if ((OnClient() || Level().IsDemoPlay()) && !IsWorking())
        FireStart();
}

void CWeaponMagazined::switch2_Empty()
{
}
void CWeaponMagazined::PlayReloadSound()
{
    if (m_sounds_enabled)
    {
        if (bMisfire)
        {
            //TODO: make sure correct sound is loaded in CWeaponMagazined::Load(LPCSTR section)
            if (m_sounds.FindSoundItem("sndReloadMisfire", false))
                PlaySound("sndReloadMisfire", get_LastFP());
            else if (m_sounds.FindSoundItem("sndReloadEmpty", false))
                PlaySound("sndReloadEmpty", get_LastFP());
            else
                PlaySound("sndReload", get_LastFP());
        }
        else
        {
            if (iAmmoElapsed == 0 || HasConditionType(eWeaponConditionChamberCycle))
            {
                if (m_sounds.FindSoundItem("sndReloadEmpty", false))
                    PlaySound("sndReloadEmpty", get_LastFP());
                else
                    PlaySound("sndReload", get_LastFP());
            }
            else
                PlaySound("sndReload", get_LastFP());
        }
    }
}

void CWeaponMagazined::switch2_Reload()
{
    CWeapon::FireEnd();
    OnZoomOut();

    PlayReloadSound();
    PlayAnimReload();
    SetPending(true);
}
void CWeaponMagazined::switch2_Hiding()
{
    OnZoomOut();
    CWeapon::FireEnd();

    if (m_sounds_enabled)
        PlaySound("sndHide", get_LastFP());

    PlayAnimHide();
    SetPending(true);
}

void CWeaponMagazined::switch2_Hidden()
{
    CWeapon::FireEnd();

    StopCurrentAnimWithoutCallback();

    signal_HideComplete();
    RemoveShotEffector();
}
void CWeaponMagazined::switch2_Showing()
{
    if (m_sounds_enabled)
        PlaySound("sndShow", get_LastFP());

    SetPending(true);
    PlayAnimShow();
}

bool CWeaponMagazined::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    //если оружие чем-то занято, то ничего не делать
    if (IsPending())
        return false;

    switch (cmd)
    {
    case kWPN_RELOAD:
    {
        if (flags & CMD_START)
            if (iAmmoElapsed < iMagazineSize || IsMisfire() ||
                HasConditionType(eWeaponConditionChamberCycle) ||
                HasConditionType(eWeaponConditionMagazineRemoved))
                Reload();
    }
        return true;
    case kWPN_FIREMODE_PREV:
    {
        if (flags & CMD_START)
        {
            OnPrevFireMode();
            return true;
        };
    }
    break;
    case kWPN_FIREMODE_NEXT:
    {
        if (flags & CMD_START)
        {
            OnNextFireMode();
            return true;
        };
    }
    break;
    }
    return false;
}

bool CWeaponMagazined::CanAttach(PIItem pIItem)
{
    CScope* pScope = smart_cast<CScope*>(pIItem);
    CSilencer* pSilencer = smart_cast<CSilencer*>(pIItem);
    CGrenadeLauncher* pGrenadeLauncher = smart_cast<CGrenadeLauncher*>(pIItem);

    if (pScope && ScopeAttachable() &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) == 0 &&
        !HasConditionType(eWeaponConditionScopeMount) /*&&
                (m_scopes[cur_scope]->m_sScopeName == pIItem->object().cNameSect())*/)
    {
        auto it = m_scopes.begin();
        for (; it != m_scopes.end(); ++it)
        {
            if (pSettings->r_string((*it), "scope_name") == pIItem->object().cNameSect())
                return true;
        }
        return false;
    }
    else if (pSilencer && SilencerAttachable() &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) == 0 &&
        !HasConditionType(eWeaponConditionSilencerMount) &&
        (m_sSilencerName == pIItem->object().cNameSect()))
        return true;
    else if (pGrenadeLauncher && GrenadeLauncherAttachable() &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) == 0 &&
        !HasConditionType(eWeaponConditionGrenadeLauncherMount) &&
        (m_sGrenadeLauncherName == pIItem->object().cNameSect()))
        return true;
    else
        return inherited::CanAttach(pIItem);
}

bool CWeaponMagazined::CanDetach(const char* item_section_name)
{
    if (m_eScopeStatus == ALife::eAddonAttachable &&
        0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) &&
        !HasConditionType(eWeaponConditionScopeMount)) /* &&
           (m_scopes[cur_scope]->m_sScopeName	== item_section_name))*/
    {
        auto it = m_scopes.begin();
        for (; it != m_scopes.end(); ++it)
        {
            if (pSettings->r_string((*it), "scope_name") == item_section_name)
                return true;
        }
        return false;
    }
    //	   return true;
    else if (m_eSilencerStatus == ALife::eAddonAttachable &&
        0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) &&
        !HasConditionType(eWeaponConditionSilencerMount) && (m_sSilencerName == item_section_name))
        return true;
    else if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable &&
        0 != (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) &&
        !HasConditionType(eWeaponConditionGrenadeLauncherMount) &&
        (m_sGrenadeLauncherName == item_section_name))
        return true;
    else
        return inherited::CanDetach(item_section_name);
}

bool CWeaponMagazined::Attach(PIItem pIItem, bool b_send_event)
{
    bool result = false;

    CScope* pScope = smart_cast<CScope*>(pIItem);
    CSilencer* pSilencer = smart_cast<CSilencer*>(pIItem);
    CGrenadeLauncher* pGrenadeLauncher = smart_cast<CGrenadeLauncher*>(pIItem);

    if (pScope && ScopeAttachable() && !HasConditionType(eWeaponConditionScopeMount) &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) == 0 /*&&
       (m_scopes[cur_scope]->m_sScopeName == pIItem->object().cNameSect())*/)
    {
        auto it = m_scopes.begin();
        for (; it != m_scopes.end(); ++it)
        {
            if (pSettings->r_string((*it), "scope_name") == pIItem->object().cNameSect())
                m_cur_scope = u8(it - m_scopes.begin());
        }
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonScope;
        result = true;
    }
    else if (pSilencer && SilencerAttachable() && !HasConditionType(eWeaponConditionSilencerMount) &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) == 0 &&
        (m_sSilencerName == pIItem->object().cNameSect()))
    {
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonSilencer;
        result = true;
    }
    else if (pGrenadeLauncher && GrenadeLauncherAttachable() &&
        !HasConditionType(eWeaponConditionGrenadeLauncherMount) &&
        (m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) == 0 &&
        (m_sGrenadeLauncherName == pIItem->object().cNameSect()))
    {
        m_flagsAddOnState |= CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher;
        result = true;
    }

    if (result)
    {
        if (b_send_event && OnServer())
        {
            //уничтожить подсоединенную вещь из инвентаря
            //.			pIItem->Drop					();
            pIItem->object().DestroyObject();
        };

        UpdateAddonsVisibility();
        InitAddons();

        return true;
    }
    else
        return inherited::Attach(pIItem, b_send_event);
}

bool CWeaponMagazined::DetachScope(const char* item_section_name, bool b_spawn_item)
{
    bool detached = false;
    auto it = m_scopes.begin();
    for (; it != m_scopes.end(); ++it)
    {
        LPCSTR iter_scope_name = pSettings->r_string((*it), "scope_name");
        if (!xr_strcmp(iter_scope_name, item_section_name))
        {
            m_cur_scope = 0;
            detached = true;
        }
    }
    return detached;
}

bool CWeaponMagazined::Detach(const char* item_section_name, bool b_spawn_item)
{
    if (m_eScopeStatus == ALife::eAddonAttachable && !HasConditionType(eWeaponConditionScopeMount) &&
        DetachScope(item_section_name, b_spawn_item))
    {
        if ((m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonScope) == 0)
        {
            Msg("ERROR: scope addon already detached.");
            return true;
        }
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonScope;

        UpdateAddonsVisibility();
        InitAddons();

        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else if (m_eSilencerStatus == ALife::eAddonAttachable &&
        !HasConditionType(eWeaponConditionSilencerMount) && (m_sSilencerName == item_section_name))
    {
        if ((m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonSilencer) == 0)
        {
            Msg("ERROR: silencer addon already detached.");
            return true;
        }
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonSilencer;

        UpdateAddonsVisibility();
        InitAddons();
        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else if (m_eGrenadeLauncherStatus == ALife::eAddonAttachable &&
        !HasConditionType(eWeaponConditionGrenadeLauncherMount) &&
        (m_sGrenadeLauncherName == item_section_name))
    {
        if ((m_flagsAddOnState & CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher) == 0)
        {
            Msg("ERROR: grenade launcher addon already detached.");
            return true;
        }
        m_flagsAddOnState &= ~CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher;

        UpdateAddonsVisibility();
        InitAddons();
        return CInventoryItemObject::Detach(item_section_name, b_spawn_item);
    }
    else
        return inherited::Detach(item_section_name, b_spawn_item);
    ;
}
/*
void CWeaponMagazined::LoadAddons()
{
    m_zoom_params.m_fIronSightZoomFactor = READ_IF_EXISTS( pSettings, r_float, cNameSect(), "ironsight_zoom_factor",
50.0f );

}
*/
void CWeaponMagazined::InitAddons()
{
    m_zoom_params.m_fIronSightZoomFactor =
        READ_IF_EXISTS(pSettings, r_float, cNameSect(), "ironsight_zoom_factor", 50.0f);
    if (IsScopeAttached())
    {
        shared_str scope_tex_name;
        if (m_eScopeStatus == ALife::eAddonAttachable)
        {
            // m_scopes[cur_scope]->m_sScopeName = pSettings->r_string(cNameSect(), "scope_name");
            // m_scopes[cur_scope]->m_iScopeX	 = pSettings->r_s32(cNameSect(),"scope_x");
            // m_scopes[cur_scope]->m_iScopeY	 = pSettings->r_s32(cNameSect(),"scope_y");

            scope_tex_name = GetScopeTextureName(GetScopeName().c_str());
            m_zoom_params.m_fScopeZoomFactor = pSettings->r_float(GetScopeName(), "scope_zoom_factor");
            m_zoom_params.m_sUseZoomPostprocess =
                READ_IF_EXISTS(pSettings, r_string, GetScopeName(), "scope_nightvision", 0);
            m_zoom_params.m_bUseDynamicZoom =
                READ_IF_EXISTS(pSettings, r_bool, GetScopeName(), "scope_dynamic_zoom", false);
            m_zoom_params.m_sUseBinocularVision =
                READ_IF_EXISTS(pSettings, r_string, GetScopeName(), "scope_alive_detector", 0);
            m_fRTZoomFactor = m_zoom_params.m_fScopeZoomFactor;
            if (m_UIScope)
            {
                xr_delete(m_UIScope);
            }

            if (!GEnv.isDedicatedServer && scope_tex_name.size())
            {
                m_UIScope = xr_new<CUIWindow>("Scope UI");
                LoadScope(scope_tex_name);
            }
        }
    }
    else
    {
        if (m_UIScope)
        {
            xr_delete(m_UIScope);
        }

        if (IsZoomEnabled())
        {
            m_zoom_params.m_fIronSightZoomFactor = pSettings->r_float(cNameSect(), "scope_zoom_factor");
        }
    }

    const bool silencer = IsSilencerAttached();

    if (silencer && m_layered_sounds.FindSoundItem("sndSilencerShot", false))
    {
        m_sFlameParticlesCurrent = m_sSilencerFlameParticles;
        m_sSmokeParticlesCurrent = m_sSilencerSmokeParticles;
        m_sSndShotCurrent = "sndSilencerShot";

        //подсветка от выстрела
        LoadLights(cNameSect().c_str(), "silencer_");
    }
    else
    {
        m_sFlameParticlesCurrent = m_sFlameParticles;
        m_sSmokeParticlesCurrent = m_sSmokeParticles;
        m_sSndShotCurrent = "sndShot";

        //подсветка от выстрела
        LoadLights(cNameSect().c_str(), "");
    }

    if (silencer)
        ApplySilencerKoeffs();
    else
        ResetSilencerKoeffs();

    inherited::InitAddons();
}

void CWeaponMagazined::LoadSilencerKoeffs()
{
    if (m_eSilencerStatus == ALife::eAddonAttachable)
    {
        LPCSTR sect = m_sSilencerName.c_str();
        m_silencer_koef.hit_power = READ_IF_EXISTS(pSettings, r_float, sect, "bullet_hit_power_k", 1.0f);
        m_silencer_koef.hit_impulse = READ_IF_EXISTS(pSettings, r_float, sect, "bullet_hit_impulse_k", 1.0f);
        m_silencer_koef.bullet_speed = READ_IF_EXISTS(pSettings, r_float, sect, "bullet_speed_k", 1.0f);
        m_silencer_koef.fire_dispersion = READ_IF_EXISTS(pSettings, r_float, sect, "fire_dispersion_base_k", 1.0f);
        m_silencer_koef.cam_dispersion = READ_IF_EXISTS(pSettings, r_float, sect, "cam_dispersion_k", 1.0f);
        m_silencer_koef.cam_disper_inc = READ_IF_EXISTS(pSettings, r_float, sect, "cam_dispersion_inc_k", 1.0f);
    }

    clamp(m_silencer_koef.hit_power, 0.0f, 1.0f);
    clamp(m_silencer_koef.hit_impulse, 0.0f, 1.0f);
    clamp(m_silencer_koef.bullet_speed, 0.0f, 1.0f);
    clamp(m_silencer_koef.fire_dispersion, 0.0f, 3.0f);
    clamp(m_silencer_koef.cam_dispersion, 0.0f, 1.0f);
    clamp(m_silencer_koef.cam_disper_inc, 0.0f, 1.0f);
}

void CWeaponMagazined::ApplySilencerKoeffs() { cur_silencer_koef = m_silencer_koef; }
void CWeaponMagazined::ResetSilencerKoeffs() { cur_silencer_koef.Reset(); }
void CWeaponMagazined::PlayAnimShow()
{
    VERIFY(GetState() == eShowing);
    PlayHUDMotion("anm_show", "anim_draw", false, this, GetState());
}

void CWeaponMagazined::PlayAnimHide()
{
    VERIFY(GetState() == eHiding);
    PlayHUDMotion("anm_hide", "anim_holster", true, this, GetState());
}

void CWeaponMagazined::PlayAnimReload()
{
    const auto state = GetState();
    VERIFY(state == eReload);
    if (bMisfire)
    {
        if (cpcstr anim_name = WhichHUDAnimationExist("anm_reload_misfire", "anim_reload_misfire"))
            PlayHUDMotion(anim_name, true, this, state);
        else if (cpcstr anim_name = WhichHUDAnimationExist("anm_reload_empty", "anim_reload_empty"))
            PlayHUDMotion(anim_name, true, this, state);
        else
            PlayHUDMotion("anm_reload", "anim_reload", true, this, state);
    }
    else
    {
        if (cpcstr anim_name =
                iAmmoElapsed == 0 || HasConditionType(eWeaponConditionChamberCycle) ?
            WhichHUDAnimationExist("anm_reload_empty", "anim_reload_empty") :
            nullptr)
            PlayHUDMotion(anim_name, true, this, state);
        else
            PlayHUDMotion("anm_reload", "anim_reload", true, this, state);
    }
}

void CWeaponMagazined::PlayAnimAim() { PlayHUDMotion("anm_idle_aim", "anim_idle_aim", true, nullptr, GetState()); }
void CWeaponMagazined::PlayAnimIdle()
{
    if (GetState() != eIdle)
        return;
    if (IsZoomed())
    {
        PlayAnimAim();
    }
    else
        inherited::PlayAnimIdle();
}

void CWeaponMagazined::PlayAnimShoot()
{
    VERIFY(GetState() == eFire);
    PlayHUDMotion("anm_shots", "anim_shoot", true, this, GetState());
}

void CWeaponMagazined::OnZoomIn()
{
    inherited::OnZoomIn();

    if (GetState() == eIdle)
        PlayAnimIdle();

    // Alundaio
    if (const auto object = smart_cast<CGameObject*>(H_Parent()))
    {
        object->callback(GameObject::eOnWeaponZoomIn)(object->lua_game_object(), this->lua_game_object());
    }

    if (CActor* pActor = smart_cast<CActor*>(H_Parent()))
    {
        if (pActor->Cameras().GetCamEffector(eCEActorMoving))
            pActor->Cameras().RemoveCamEffector(eCEActorMoving);

        CEffectorZoomInertion* S = smart_cast<CEffectorZoomInertion*>(pActor->Cameras().GetCamEffector(eCEZoom));
        if (!S)
        {
            S = (CEffectorZoomInertion*)pActor->Cameras().AddCamEffector(xr_new<CEffectorZoomInertion>());
            S->Init(this);
        };
        S->SetRndSeed(pActor->GetZoomRndSeed());
        R_ASSERT(S);
    }
}
void CWeaponMagazined::OnZoomOut()
{
    if (!IsZoomed())
        return;

    inherited::OnZoomOut();

    if (GetState() == eIdle)
        PlayAnimIdle();

    //Alundaio
    if (const auto object = smart_cast<CGameObject*>(H_Parent()))
    {
        object->callback(GameObject::eOnWeaponZoomOut)(object->lua_game_object(), this->lua_game_object());
    }

    if (CActor* pActor = smart_cast<CActor*>(H_Parent()))
        pActor->Cameras().RemoveCamEffector(eCEZoom);
}

//переключение режимов стрельбы одиночными и очередями
bool CWeaponMagazined::SwitchMode()
{
    if (eIdle != GetState() || IsPending())
        return false;

    if (SingleShotMode())
        m_iQueueSize = WEAPON_ININITE_QUEUE;
    else
        m_iQueueSize = 1;

    PlaySound("sndEmptyClick", get_LastFP());

    return true;
}

void CWeaponMagazined::OnNextFireMode()
{
    if (!m_bHasDifferentFireModes)
        return;
    if (GetState() != eIdle)
        return;
    if (HasConditionType(eWeaponConditionFireMode))
        return;
    m_iCurFireMode = (m_iCurFireMode + 1 + m_aFireModes.size()) % m_aFireModes.size();
    SetQueueSize(GetCurrentFireMode());
    OnFireModeChanged();
};

void CWeaponMagazined::OnPrevFireMode()
{
    if (!m_bHasDifferentFireModes)
        return;
    if (GetState() != eIdle)
        return;
    if (HasConditionType(eWeaponConditionFireMode))
        return;
    m_iCurFireMode = (m_iCurFireMode - 1 + m_aFireModes.size()) % m_aFireModes.size();
    SetQueueSize(GetCurrentFireMode());
    OnFireModeChanged();
};

void CWeaponMagazined::OnFireModeChanged()
{
    if (m_sounds.FindSoundItem("sndFireMode", false))
        PlaySound("sndFireMode", get_LastFP());

    const float condition = _max(GetCondition(), 0.2f);
    if (::Random.randF(0.f, 0.9f) > condition)
        SetConditionType(GetConditionType() | eWeaponConditionFireMode);
}

void CWeaponMagazined::OnH_A_Chield()
{
    if (m_bHasDifferentFireModes)
    {
        CActor* actor = smart_cast<CActor*>(H_Parent());
        if (!actor)
            SetQueueSize(-1);
        else
            SetQueueSize(GetCurrentFireMode());
    }
    else if (!m_aFireModes.empty())
        SetQueueSize(GetCurrentFireMode());
    inherited::OnH_A_Chield();
}

void CWeaponMagazined::SetQueueSize(int size) { m_iQueueSize = size; };
float CWeaponMagazined::GetWeaponDeterioration()
{
    // modified by Peacemaker [17.10.08]
    //	if (!m_bHasDifferentFireModes || m_iPrefferedFireMode == -1 || u32(GetCurrentFireMode()) <=
    // u32(m_iPrefferedFireMode))
    //		return inherited::GetWeaponDeterioration();
    //	return m_iShotNum*conditionDecreasePerShot;
    return (m_iShotNum == 1) ? conditionDecreasePerShot : conditionDecreasePerQueueShot;
};

void CWeaponMagazined::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    save_data(m_iQueueSize, output_packet);
    save_data(m_iShotNum, output_packet);
    save_data(m_iCurFireMode, output_packet);
}

void CWeaponMagazined::load(IReader& input_packet)
{
    inherited::load(input_packet);
    load_data(m_iQueueSize, input_packet);
    SetQueueSize(m_iQueueSize);
    load_data(m_iShotNum, input_packet);
    load_data(m_iCurFireMode, input_packet);
}

void CWeaponMagazined::net_Export(NET_Packet& P)
{
    inherited::net_Export(P);

    P.w_u8(u8(m_iCurFireMode & 0x00ff));
}

void CWeaponMagazined::net_Import(NET_Packet& P)
{
    inherited::net_Import(P);

    m_iCurFireMode = P.r_u8();
    SetQueueSize(GetCurrentFireMode());
}

bool CWeaponMagazined::GetBriefInfo(II_BriefInfo& info)
{
    VERIFY(m_pInventory);
    string32 int_str;

    const int ae = GetAmmoElapsed();
    xr_sprintf(int_str, "%d", ae);
    info.cur_ammo = int_str;

    if (HasFireModes())
    {
        if (m_iQueueSize == WEAPON_ININITE_QUEUE)
        {
            info.fire_mode = "A";
        }
        else
        {
            xr_sprintf(int_str, "%d", m_iQueueSize);
            info.fire_mode = int_str;
        }
    }
    else
        info.fire_mode = "";

    if (HasConditionType(eWeaponConditionChamberCycle) || bMisfire)
        info.fire_mode = "X";

    if (m_pInventory->ModifyFrame() <= m_BriefInfo_CalcFrame)
    {
        return false;
    }
    GetSuitableAmmoTotal(); // update m_BriefInfo_CalcFrame

    info.grenade = "";

    const u32 at_size = m_ammoTypes.size();
    if (unlimited_ammo() || at_size == 0)
    {
        info.fmj_ammo._set("--");
        info.ap_ammo._set("--");
        info.third_ammo._set("--"); //Alundaio
        info.total_ammo = "--";
    }
    else
    {
        // GetSuitableAmmoTotal(); //mp = all type
        //Alundaio: Added third ammo type and cleanup
        info.fmj_ammo._set("");
        info.ap_ammo._set("");
        info.third_ammo._set("");

        int total = 0;
        if (at_size >= 1)
        {
            const int fmj = GetAmmoCount(0);
            xr_sprintf(int_str, "%d", fmj);
            info.fmj_ammo._set(int_str);
            total += fmj;
        }
        if (at_size >= 2)
        {
            const int ap = GetAmmoCount(1);
            xr_sprintf(int_str, "%d", ap);
            info.ap_ammo._set(int_str);
            total += ap;
        }
        if (at_size >= 3)
        {
            const int third = GetAmmoCount(2);
            xr_sprintf(int_str, "%d", third);
            info.third_ammo._set(int_str);
            total += third;
        }

        xr_sprintf(int_str, "%d", total);
        info.total_ammo = int_str;
        //-Alundaio
    }

    if (ae != 0 && m_magazine.size() != 0)
    {
        LPCSTR ammo_type = m_ammoTypes[m_magazine.back().m_LocalAmmoType].c_str();
        info.name = StringTable().translate(pSettings->r_string(ammo_type, "inv_name_short"));
        info.icon = ammo_type;
    }
    else
    {
        LPCSTR ammo_type = m_ammoTypes[m_ammoType].c_str();
        info.name = StringTable().translate(pSettings->r_string(ammo_type, "inv_name_short"));
        info.icon = ammo_type;
    }
    return true;
}

bool CWeaponMagazined::install_upgrade_impl(LPCSTR section, bool test)
{
    bool result = inherited::install_upgrade_impl(section, test);

    LPCSTR str;
    // fire_modes = 1, 2, -1
    bool result2 = process_if_exists_set(section, "fire_modes", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        int ModesCount = _GetItemCount(str);
        m_aFireModes.clear();
        for (int i = 0; i < ModesCount; ++i)
        {
            string16 sItem;
            _GetItem(str, i, sItem);
            m_aFireModes.push_back((s8)atoi(sItem));
        }
        m_iCurFireMode = ModesCount - 1;
    }
    result |= result2;

    result |= process_if_exists_set(
        section, "base_dispersioned_bullets_count", &CInifile::r_s32, m_iBaseDispersionedBulletsCount, test);
    result |= process_if_exists_set(
        section, "base_dispersioned_bullets_speed", &CInifile::r_float, m_fBaseDispersionedBulletsSpeed, test);

    // sounds (name of the sound, volume (0.0 - 1.0), delay (sec))
    result2 = process_if_exists_set(section, "snd_draw", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_sounds.LoadSound(section, "snd_draw", "sndShow", false, m_eSoundShow);
    }
    result |= result2;

    result2 = process_if_exists_set(section, "snd_holster", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_sounds.LoadSound(section, "snd_holster", "sndHide", false, m_eSoundHide);
    }
    result |= result2;

    result2 = process_if_exists_set(section, "snd_shoot", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_layered_sounds.LoadSound(section, "snd_shoot", "sndShot", false, m_eSoundShot);
    }
    result |= result2;

    result2 = process_if_exists_set(section, "snd_empty", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_sounds.LoadSound(section, "snd_empty", "sndEmptyClick", false, m_eSoundEmptyClick);
    }
    result |= result2;

    result2 = process_if_exists_set(section, "snd_reload", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_sounds.LoadSound(section, "snd_reload", "sndReload", true, m_eSoundReload);
    }
    result |= result2;

    result2 = process_if_exists_set(section, "snd_reload_empty", &CInifile::r_string, str, test);
    if (result2 && !test)
    {
        m_sounds.LoadSound(section, "snd_reload_empty", "sndReloadEmpty", true, m_eSoundReloadEmpty);
    }
    result |= result2;

    // snd_shoot1     = weapons\ak74u_shot_1 ??
    // snd_shoot2     = weapons\ak74u_shot_2 ??
    // snd_shoot3     = weapons\ak74u_shot_3 ??

    if (m_eSilencerStatus == ALife::eAddonAttachable || m_eSilencerStatus == ALife::eAddonPermanent)
    {
        result |= process_if_exists_set(
            section, "silencer_flame_particles", &CInifile::r_string, m_sSilencerFlameParticles, test);
        result |= process_if_exists_set(
            section, "silencer_smoke_particles", &CInifile::r_string, m_sSilencerSmokeParticles, test);

        result2 = process_if_exists_set(section, "snd_silncer_shot", &CInifile::r_string, str, test);
        if (result2 && !test)
        {
            m_layered_sounds.LoadSound(section, "snd_silncer_shot", "sndSilencerShot", false, m_eSoundShot);
        }
        result |= result2;
    }

    // fov for zoom mode
    result |= process_if_exists(
        section, "ironsight_zoom_factor", &CInifile::r_float, m_zoom_params.m_fIronSightZoomFactor, test);

    if (IsScopeAttached())
    {
        // if ( m_eScopeStatus == ALife::eAddonAttachable )
        {
            result |= process_if_exists(
                section, "scope_zoom_factor", &CInifile::r_float, m_zoom_params.m_fScopeZoomFactor, test);
        }
    }
    else
    {
        if (IsZoomEnabled())
        {
            result |= process_if_exists(
                section, "scope_zoom_factor", &CInifile::r_float, m_zoom_params.m_fIronSightZoomFactor, test);
        }
    }

    return result;
}
//текущая дисперсия (в радианах) оружия с учетом используемого патрона и недисперсионных пуль
float CWeaponMagazined::GetFireDispersion(float cartridge_k, bool for_crosshair)
{
    float fire_disp = GetBaseDispersion(cartridge_k);
    if (for_crosshair || !m_iBaseDispersionedBulletsCount || !m_iShotNum ||
        m_iShotNum > m_iBaseDispersionedBulletsCount)
    {
        fire_disp = inherited::GetFireDispersion(cartridge_k);
    }
    return fire_disp;
}
void CWeaponMagazined::FireBullet(const Fvector& pos, const Fvector& shot_dir, float fire_disp,
    const CCartridge& cartridge, u16 parent_id, u16 weapon_id, bool send_hit)
{
    if (m_iBaseDispersionedBulletsCount)
    {
        if (m_iShotNum <= 1)
        {
            m_fOldBulletSpeed = GetBulletSpeed();
            SetBulletSpeed(m_fBaseDispersionedBulletsSpeed);
        }
        else if (m_iShotNum > m_iBaseDispersionedBulletsCount)
        {
            SetBulletSpeed(m_fOldBulletSpeed);
        }
    }
    inherited::FireBullet(pos, shot_dir, fire_disp, cartridge, parent_id, weapon_id, send_hit, GetAmmoElapsed());
}
