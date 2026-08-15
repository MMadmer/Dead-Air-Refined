#include "StdAfx.h"
#include "WeaponAutomaticShotgun.h"
#include "Entity.h"
#include "ParticlesObject.h"
#include "xrEngine/xr_level_controller.h"
#include "Inventory.h"
#include "Level.h"
#include "Actor.h"

CWeaponAutomaticShotgun::CWeaponAutomaticShotgun()
{
    m_eSoundClose = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING);
    m_eSoundAddCartridge = ESoundTypes(SOUND_TYPE_WEAPON_RECHARGING);
}

CWeaponAutomaticShotgun::~CWeaponAutomaticShotgun() {}

bool CWeaponAutomaticShotgun::net_Spawn(CSE_Abstract* DC)
{
    const bool result = inherited::net_Spawn(DC);
    if (!result)
        return false;

    CSE_ALifeItemWeaponShotGun* server_weapon = smart_cast<CSE_ALifeItemWeaponShotGun*>(DC);
    R_ASSERT2(server_weapon, make_string("Invalid server entity for automatic shotgun '%s'", cNameSect().c_str()));

    if (server_weapon->m_AmmoIDs.empty())
        return result;

    R_ASSERT3(server_weapon->m_AmmoIDs.size() == m_magazine.size(),
        "Saved mixed automatic-shotgun magazine size does not match ammo count", cNameSect().c_str());

    for (size_t i = 0; i < m_magazine.size(); ++i)
    {
        const u8 ammo_type = server_weapon->m_AmmoIDs[i];
        R_ASSERT3(
            ammo_type < m_ammoTypes.size(), "Saved automatic-shotgun cartridge type is out of range", cNameSect().c_str());
        m_magazine[i].Load(m_ammoTypes[ammo_type].c_str(), ammo_type);
    }

    return result;
}

void CWeaponAutomaticShotgun::Load(LPCSTR section)
{
    inherited::Load(section);

    if (pSettings->line_exist(section, "tri_state_reload"))
    {
        m_bTriStateReload = !!pSettings->r_bool(section, "tri_state_reload");
    };
    if (m_bTriStateReload)
    {
        m_sounds.LoadSound(section, "snd_open_weapon", "sndOpen", false, m_eSoundOpen);

        m_sounds.LoadSound(section, "snd_add_cartridge", "sndAddCartridge", false, m_eSoundAddCartridge);

        m_sounds.LoadSound(section, "snd_close_weapon", "sndClose", false, m_eSoundClose);
    };
}

bool CWeaponAutomaticShotgun::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;

    if (m_bTriStateReload && GetState() == eReload && cmd == kWPN_FIRE && flags & CMD_START &&
        m_sub_state == eSubstateReloadInProcess) //остановить перезагрузку
    {
        AddCartridge(1);
        m_sub_state = eSubstateReloadEnd;
        return true;
    }
    return false;
}

void CWeaponAutomaticShotgun::OnAnimationEnd(u32 state)
{
    if (!m_bTriStateReload || IsMisfire() || state != eReload)
    {
        m_sub_state = eSubstateReloadBegin;
        return inherited::OnAnimationEnd(state);
    }

    switch (m_sub_state)
    {
    case eSubstateReloadBegin:
    {
        m_sub_state = eSubstateReloadInProcess;
        SwitchState(eReload);
    }
    break;

    case eSubstateReloadInProcess:
    {
        if (0 != AddCartridge(1))
        {
            m_sub_state = eSubstateReloadEnd;
        }
        SwitchState(eReload);
    }
    break;

    case eSubstateReloadEnd:
    {
        m_sub_state = eSubstateReloadBegin;
        SwitchState(eIdle);
    }
    break;
    };
}

void CWeaponAutomaticShotgun::Reload()
{
    if (IsMisfire())
        inherited::Reload();
    else if (m_bTriStateReload)
    {
        TriStateReload();
    }
    else
        inherited::Reload();
}

void CWeaponAutomaticShotgun::TriStateReload()
{
    if (m_magazine.size() == (u32)iMagazineSize || !HaveCartridgeInInventory(1))
        return;
    CWeapon::Reload();
    m_sub_state = eSubstateReloadBegin;
    SwitchState(eReload);
}

void CWeaponAutomaticShotgun::OnStateSwitch(u32 S, u32 oldState)
{
    if (!m_bTriStateReload || IsMisfire() || S != eReload)
    {
        inherited::OnStateSwitch(S, oldState);
        return;
    }

    CWeapon::OnStateSwitch(S, oldState);

    if (m_magazine.size() == (u32)iMagazineSize || !HaveCartridgeInInventory(1))
    {
        switch2_EndReload();
        m_sub_state = eSubstateReloadEnd;
        return;
    };

    switch (m_sub_state)
    {
    case eSubstateReloadBegin:
        if (HaveCartridgeInInventory(1))
            switch2_StartReload();
        break;
    case eSubstateReloadInProcess:
        if (HaveCartridgeInInventory(1))
            switch2_AddCartgidge();
        break;
    case eSubstateReloadEnd: switch2_EndReload(); break;
    };
}

void CWeaponAutomaticShotgun::switch2_StartReload()
{
    PlaySound("sndOpen", get_LastFP());
    PlayAnimOpenWeapon();
    SetPending(TRUE);
}

void CWeaponAutomaticShotgun::switch2_AddCartgidge()
{
    PlaySound("sndAddCartridge", get_LastFP());
    PlayAnimAddOneCartridgeWeapon();
    SetPending(TRUE);
}

void CWeaponAutomaticShotgun::switch2_EndReload()
{
    SetPending(FALSE);
    if (isHUDAnimationExist("anm_close") || isHUDAnimationExist("anim_close"))
    {
        PlaySound("sndClose", get_LastFP());
        PlayAnimCloseWeapon();
    }
    else
        SwitchState(eIdle);
}

void CWeaponAutomaticShotgun::PlayAnimOpenWeapon()
{
    VERIFY(GetState() == eReload);
    PlayHUDMotion("anm_open", "anim_open", TRUE, this, GetState());
}
void CWeaponAutomaticShotgun::PlayAnimAddOneCartridgeWeapon()
{
    VERIFY(GetState() == eReload);
    PlayHUDMotion("anm_add_cartridge", "anim_add_cartridge", TRUE, this, GetState());
}
void CWeaponAutomaticShotgun::PlayAnimCloseWeapon()
{
    VERIFY(GetState() == eReload);

    PlayHUDMotion("anm_close", "anim_close", TRUE, this, GetState());
}

bool CWeaponAutomaticShotgun::HaveCartridgeInInventory(u8 cnt)
{
    if (unlimited_ammo())
        return true;
    if (!m_pInventory)
        return false;

    u32 ac = GetAmmoCount(m_ammoType);
    if (ac < cnt)
    {
        for (u8 i = 0; i < u8(m_ammoTypes.size()); ++i)
        {
            if (m_ammoType == i)
                continue;
            ac += GetAmmoCount(i);
            if (ac >= cnt)
            {
                m_ammoType = i;
                break;
            }
        }
    }
    return ac >= cnt;
}

u8 CWeaponAutomaticShotgun::AddCartridge(u8 cnt)
{
    if (IsMisfire())
        bMisfire = false;

    if (m_set_next_ammoType_on_reload != undefined_ammo_type)
    {
        m_ammoType = m_set_next_ammoType_on_reload;
        m_set_next_ammoType_on_reload = undefined_ammo_type;
    }

    if (!HaveCartridgeInInventory(1))
        return 0;

    m_pCurrentAmmo = GetAmmoForReload(m_ammoTypes[m_ammoType].c_str());
    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    if (m_DefaultCartridge.m_LocalAmmoType != m_ammoType)
        m_DefaultCartridge.Load(m_ammoTypes[m_ammoType].c_str(), m_ammoType);
    CCartridge l_cartridge = m_DefaultCartridge;
    while (cnt)
    {
        if (!unlimited_ammo())
        {
            if (!m_pCurrentAmmo->Get(l_cartridge))
                break;
        }
        --cnt;
        ++iAmmoElapsed;
        l_cartridge.m_LocalAmmoType = m_ammoType;
        m_magazine.push_back(l_cartridge);
        //		m_fCurrentCartirdgeDisp = l_cartridge.m_kDisp;
    }

    VERIFY((u32)iAmmoElapsed == m_magazine.size());

    //выкинуть коробку патронов, если она пустая
    if (m_pCurrentAmmo && !m_pCurrentAmmo->m_boxCurr && OnServer())
        m_pCurrentAmmo->SetDropManual(TRUE);

    return cnt;
}

void CWeaponAutomaticShotgun::net_Export(NET_Packet& P)
{
    inherited::net_Export(P);
    P.w_u8(u8(m_magazine.size()));
    for (u32 i = 0; i < m_magazine.size(); i++)
    {
        CCartridge& l_cartridge = *(m_magazine.begin() + i);
        P.w_u8(l_cartridge.m_LocalAmmoType);
    }
}

void CWeaponAutomaticShotgun::net_Import(NET_Packet& P)
{
    inherited::net_Import(P);
    u8 AmmoCount = P.r_u8();
    for (u32 i = 0; i < AmmoCount; i++)
    {
        u8 LocalAmmoType = P.r_u8();
        if (i >= m_magazine.size())
            continue;
        if (LocalAmmoType >= m_ammoTypes.size())
        {
            Msg("! Automatic shotgun '%s' received invalid cartridge type %u", cNameSect().c_str(), LocalAmmoType);
            continue;
        }
        CCartridge& l_cartridge = *(m_magazine.begin() + i);
        if (LocalAmmoType == l_cartridge.m_LocalAmmoType)
            continue;
#ifdef DEBUG
        Msg("! %s reload to %s", l_cartridge.m_ammoSect.c_str(), m_ammoTypes[LocalAmmoType].c_str());
#endif
        l_cartridge.Load(m_ammoTypes[LocalAmmoType].c_str(), LocalAmmoType);
    }
}
