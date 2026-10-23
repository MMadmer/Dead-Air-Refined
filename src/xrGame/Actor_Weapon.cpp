// Actor_Weapon.cpp:	 для работы с оружием
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#include "Actor.h"
#include "ActorEffector.h"
#include "Missile.h"
#include "Inventory.h"
#include "Weapon.h"
#include "map_manager.h"
#include "Level.h"
#include "CharacterPhysicsSupport.h"
#include "EffectorShot.h"
#include "player_hud.h"
#include "WeaponMagazined.h"
#include "Grenade.h"
#include "game_base_space.h"
#include "Artefact.h"

static const float VEL_MAX = 10.f;
static const float VEL_A_MAX = 10.f;

#define GetWeaponParam(pWeapon, func_name, def_value) ((pWeapon) ? (pWeapon->func_name) : def_value)

//возвращает текуший разброс стрельбы (в радианах)с учетом движения
namespace
{
// A lamp, a lighter or a detector in the left hand leaves the weapon to one hand (the item stays in the hand while
// aiming, CCustomDetector::UpdateVisibility). Without the support hand the hold sways more - a two-handed grip lowers
// the vertical and lateral tremor of aiming and shoots measurably tighter (Kelleran, ODU 2018), shooters report
// groups about half the size - and the loose wrist lets the gun rotate further and come back slower after each shot
// (high-speed footage: much more rotation, most of it after the bullet is out).
constexpr float kOneHandDispersion = 1.6f;
constexpr float kOneHandRecoil = 1.5f;
constexpr float kOneHandRelax = 0.75f;

bool left_hand_busy() { return g_player_hud && g_player_hud->attached_item(1); }
} // namespace

float CActor::GetWeaponAccuracy() const
{
    CWeapon* W = smart_cast<CWeapon*>(inventory().ActiveItem());
    const float hand_k = left_hand_busy() ? kOneHandDispersion : 1.f;

    if (IsZoomAimingMode() && W && !GetWeaponParam(W, IsRotatingToZoom(), false))
    {
        return m_fDispAim * hand_k;
    }
    float dispersion = m_fDispBase * GetWeaponParam(W, Get_PDM_Base(), 1.0f);

    CEntity::SEntityState state;
    if (g_State(state))
    {
        // fAVelocity = angle velocity
        dispersion *=
            (1.0f + (state.fAVelocity / VEL_A_MAX) * m_fDispVelFactor * GetWeaponParam(W, Get_PDM_Vel_F(), 1.0f));
        // fVelocity = linear velocity
        dispersion *=
            (1.0f + (state.fVelocity / VEL_MAX) * m_fDispVelFactor * GetWeaponParam(W, Get_PDM_Vel_F(), 1.0f));

        bool bAccelerated = isActorAccelerated(mstate_real, IsZoomAimingMode());
        if (bAccelerated || !state.bCrouch)
        {
            dispersion *= (1.0f + m_fDispAccelFactor * GetWeaponParam(W, Get_PDM_Accel_F(), 1.0f));
        }

        if (state.bCrouch)
        {
            dispersion *= (1.0f + m_fDispCrouchFactor * GetWeaponParam(W, Get_PDM_Crouch(), 1.0f));
            if (!bAccelerated)
            {
                dispersion *= (1.0f + m_fDispCrouchNoAccelFactor * GetWeaponParam(W, Get_PDM_Crouch_NA(), 1.0f));
            }
        }
    }
    return dispersion * hand_k;
}

void CActor::g_fireParams(const CHudItem* pHudItem, Fvector& fire_pos, Fvector& fire_dir)
{
    fire_pos = Cameras().Position();
    fire_dir = Cameras().Direction();

    const CMissile* pMissile = smart_cast<const CMissile*>(pHudItem);
    if (pMissile)
    {
        Fvector offset;
        XFORM().transform_dir(offset, pMissile->throw_point_offset());
        fire_pos.add(offset);
    }
}

void CActor::g_WeaponBones(int& L, int& R1, int& R2)
{
    R1 = m_r_hand;
    R2 = m_r_finger2;
    L = m_l_finger1;
}

BOOL CActor::g_State(SEntityState& state) const
{
    state.bJump = !!(mstate_real & mcJump);
    state.bCrouch = !!(mstate_real & mcCrouch);
    state.bFall = !!(mstate_real & mcFall);
    state.bSprint = !!(mstate_real & mcSprint);
    state.fVelocity = character_physics_support()->movement()->GetVelocityActual();
    state.fAVelocity = fCurAVelocity;
    return TRUE;
}

void CActor::SetCantRunState(bool bDisable)
{
    if (g_Alive() && this == Level().CurrentControlEntity())
    {
        NET_Packet P;
        u_EventGen(P, GEG_PLAYER_DISABLE_SPRINT, ID());
        P.w_s8(bDisable ? 1 : -1);
        u_EventSend(P);
    };
}
void CActor::SetWeaponHideState(u16 State, bool bSet)
{
    if (g_Alive() && this == Level().CurrentControlEntity())
    {
        NET_Packet P;
        u_EventGen(P, GEG_PLAYER_WEAPON_HIDE_STATE, ID());
        P.w_u16(State);
        P.w_u8(u8(bSet));
        u_EventSend(P);
    };
}
static constexpr u16 BestWeaponSlots[]{INV_SLOT_3, INV_SLOT_2, GRENADE_SLOT, KNIFE_SLOT};

void CActor::SelectBestWeapon(IGameObject* object)
{
    if (!object || IsGameTypeSingle())
        return;

    auto* weapon = smart_cast<CWeapon*>(object);
    auto* grenade = smart_cast<CGrenade*>(object);
    auto* artefact = smart_cast<CArtefact*>(object);
    auto* inventoryItem = smart_cast<CInventoryItem*>(object);

    if (artefact && artefact->H_Parent())
        return;
    if ((!weapon && !grenade && !artefact) || !inventoryItem)
        return;

    // Artifact modes keep an occupied primary slot instead of replacing it on pickup.
    if (GameID() == eGameIDArtefactHunt || GameID() == eGameIDCaptureTheArtefact)
    {
        const auto baseSlot = inventoryItem->BaseSlot();
        if (baseSlot == INV_SLOT_2 || baseSlot == INV_SLOT_3)
        {
            CInventoryItem* slottedItem = inventory().ItemFromSlot(baseSlot);
            if (slottedItem && slottedItem != inventoryItem)
                return;
        }
    }

    for (const u16 slot : BestWeaponSlots)
    {
        PIItem candidate = inventory().ItemFromSlot(slot);
        if (!candidate)
            continue;
        if (inventory().GetActiveSlot() == slot)
            return;
        if (!candidate->can_kill())
            continue;

#ifdef DEBUG
        Msg("--- Selecting best weapon [%d], Frame[%d]", slot, Device.dwFrame);
#endif
        inventory().Activate(slot);
        return;
    }
}

#define ENEMY_HIT_SPOT "mp_hit_sector_location"
BOOL g_bShowHitSectors = TRUE;

void CActor::HitSector(IGameObject* who, IGameObject* weapon)
{
    if (!g_bShowHitSectors)
        return;
    if (!g_Alive())
        return;

    bool bShowHitSector = true;

    CEntityAlive* pEntityAlive = smart_cast<CEntityAlive*>(who);

    if (!pEntityAlive || this == who)
        bShowHitSector = false;

    if (weapon)
    {
        CWeapon* pWeapon = smart_cast<CWeapon*>(weapon);
        if (pWeapon)
        {
            if (pWeapon->IsSilencerAttached())
            {
                bShowHitSector = false;
            }
        }
    }

    if (!bShowHitSector)
        return;
    Level().MapManager().AddMapLocation(ENEMY_HIT_SPOT, who->ID());
}

void CActor::on_weapon_shot_start(CWeapon* weapon)
{
    CameraRecoil camera_recoil(IsZoomAimingMode() ? weapon->zoom_cam_recoil : weapon->cam_recoil);
    const bool one_hand = left_hand_busy();
    if (one_hand)
    {
        camera_recoil.Dispersion *= kOneHandRecoil;
        camera_recoil.DispersionInc *= kOneHandRecoil;
        camera_recoil.MaxAngleVert *= kOneHandRecoil;
        camera_recoil.MaxAngleHorz *= kOneHandRecoil;
        camera_recoil.StepAngleHorz *= kOneHandRecoil;
        camera_recoil.RelaxSpeed *= kOneHandRelax;
    }

    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (!effector)
    {
        effector = (CCameraShotEffector*)Cameras().AddCamEffector(xr_new<CCameraShotEffector>(camera_recoil));
    }
    else if (effector->m_WeaponID != weapon->ID() || m_shot_one_hand != one_hand)
    {
        effector->Initialize(camera_recoil);
    }
    m_shot_one_hand = one_hand;

    effector->m_WeaponID = weapon->ID();
    R_ASSERT(effector);

    effector->SetRndSeed(GetShotRndSeed());
    effector->SetActor(this);
    effector->Shot(weapon);
}

void CActor::on_weapon_shot_update()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector)
    {
        update_camera(effector);
    }
}

void CActor::on_weapon_shot_remove(CWeapon* weapon) { Cameras().RemoveCamEffector(eCEShot); }
void CActor::on_weapon_shot_stop()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector && effector->IsActive())
    {
        effector->StopShoting();
    }
}

void CActor::on_weapon_hide(CWeapon* weapon)
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    if (effector && effector->IsActive())
        effector->Reset();
}

Fvector CActor::weapon_recoil_delta_angle()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    Fvector result = {0.f, 0.f, 0.f};

    if (effector)
        effector->GetDeltaAngle(result);

    return (result);
}

Fvector CActor::weapon_recoil_last_delta()
{
    CCameraShotEffector* effector = smart_cast<CCameraShotEffector*>(Cameras().GetCamEffector(eCEShot));
    Fvector result = {0.f, 0.f, 0.f};

    if (effector)
        effector->GetLastDelta(result);

    return (result);
}
//////////////////////////////////////////////////////////////////////////

void CActor::SpawnAmmoForWeapon(CInventoryItem* pIItem)
{
    if (OnClient())
        return;
    if (!pIItem)
        return;

    CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*>(pIItem);
    if (!pWM || !pWM->AutoSpawnAmmo())
        return;

    ///	CWeaponAmmo* pAmmo = smart_cast<CWeaponAmmo*>(inventory().GetAny( (pWM->m_ammoTypes[0].c_str()) ));
    //	if (!pAmmo)
    pWM->SpawnAmmo(0xffffffff, NULL, ID());
};

void CActor::RemoveAmmoForWeapon(CInventoryItem* pIItem)
{
    if (OnClient())
        return;
    if (!pIItem)
        return;

    CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*>(pIItem);
    if (!pWM || !pWM->AutoSpawnAmmo())
        return;

    CWeaponAmmo* pAmmo = smart_cast<CWeaponAmmo*>(inventory().GetAny(pWM->m_ammoTypes[0].c_str()));
    if (!pAmmo)
        return;
    //--- мы нашли патроны к текущему оружию
    /*
    //--- проверяем не подходят ли они к чему-то еще
    bool CanRemove = true;
    TIItemContainer::const_iterator I = inventory().m_all.begin();//, B = I;
    TIItemContainer::const_iterator E = inventory().m_all.end();
    for ( ; I != E; ++I)
    {
    CInventoryItem* pItem = (*I);//->m_pIItem;
    CWeaponMagazined* pWM = smart_cast<CWeaponMagazined*> (pItem);
    if (!pWM || !pWM->AutoSpawnAmmo()) continue;
    if (pWM == pIItem) continue;
    if (pWM->m_ammoTypes[0] != pAmmo->CInventoryItem::object().cNameSect()) continue;
    CanRemove = false;
    break;
    };

    if (!CanRemove) return;
    */
    pAmmo->DestroyObject();
    //	NET_Packet			P;
    //	u_EventGen			(P,GE_DESTROY,pAmmo->ID());
    //	u_EventSend			(P);
};
