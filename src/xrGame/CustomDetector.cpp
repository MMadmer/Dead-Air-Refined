#include "StdAfx.h"
#include "CustomDetector.h"
#include "ui/ArtefactDetectorUI.h"
#include "HUDManager.h"
#include "Inventory.h"
#include "Level.h"
#include "map_manager.h"
#include "ActorEffector.h"
#include "Actor.h"
#include "xrUICore/Windows/UIWindow.h"
#include "player_hud.h"
#include "Weapon.h"
#include "ParticlesObject.h"
#include "xrEngine/LightAnimLibrary.h"
#include "Include/xrRender/Kinematics.h"
#include "xrCommon/xr_hash_map.h"

namespace
{
struct DetectorExtendedConfig
{
    bool lightEnableFromIdle{};
    bool lightDisableFromIdle{};
    bool lightHudOnly{};
    bool lightStateControlled{};
    bool idleZoom{};
};

xr_flat_hash_map<const CCustomDetector*, DetectorExtendedConfig> detectorExtendedConfigs;

const DetectorExtendedConfig& detector_extended_config(const CCustomDetector* detector)
{
    static const DetectorExtendedConfig defaults;
    const auto config = detectorExtendedConfigs.find(detector);
    return config != detectorExtendedConfigs.end() ? config->second : defaults;
}
}

ITEM_INFO::ITEM_INFO() : snd_time(0), cur_period(0)
{
    pParticle = nullptr;
    curr_ref = nullptr;
}

ITEM_INFO::~ITEM_INFO()
{
    if (pParticle)
        CParticlesObject::Destroy(pParticle);
}

bool CCustomDetector::CheckCompatibilityInt(CHudItem* itm, u16* slot_to_activate)
{
    if (!itm)
        return true;

    CInventoryItem& iitm = itm->item();
    const u32 slot = iitm.BaseSlot();
    bool bres = (slot == INV_SLOT_2 || slot == INV_SLOT_3 || slot == KNIFE_SLOT || slot == BOLT_SLOT ||
        slot == SIDEARM_SLOT || slot == ANIMATION_SLOT) && iitm.IsSingleHanded();
    if (!bres && slot_to_activate)
    {
        *slot_to_activate = NO_ACTIVE_SLOT;
        constexpr u16 compatibleSlots[] = { BOLT_SLOT, KNIFE_SLOT, SIDEARM_SLOT, INV_SLOT_3, INV_SLOT_2 };
        for (const u16 compatibleSlot : compatibleSlots)
        {
            const PIItem candidate = m_pInventory->ItemFromSlot(compatibleSlot);
            if (candidate && candidate->IsSingleHanded())
                *slot_to_activate = compatibleSlot;
        }

        if (*slot_to_activate != NO_ACTIVE_SLOT)
            bres = true;
    }

    if (itm->GetState() != CHUDState::eShowing)
        bres = bres && !itm->IsPending();

    if (bres)
    {
        CWeapon* W = smart_cast<CWeapon*>(itm);
        if (W)
            bres = bres && (W->GetState() != CHUDState::eBore) && (W->GetState() != CWeapon::eReload) &&
                (W->GetState() != CWeapon::eSwitch) && !W->IsZoomed();
    }
    return bres;
}

bool CCustomDetector::CheckCompatibility(CHudItem* itm)
{
    if (!inherited::CheckCompatibility(itm))
        return false;

    if (!CheckCompatibilityInt(itm, NULL))
    {
        HideDetector(true);
        return false;
    }
    return true;
}

void CCustomDetector::HideDetector(bool bFastMode)
{
    if (GetState() != eIdle)
        return;

    if (!bFastMode)
    {
        ToggleDetector(false);
        return;
    }

    SwitchState(eHidden);
    if (m_light)
        m_light->set_active(false);
    TurnDetectorInternal(false);
    g_player_hud->detach_item(this);
}

void CCustomDetector::ShowDetector(bool bFastMode)
{
    if (GetState() == eHidden)
        ToggleDetector(bFastMode);
}

void CCustomDetector::ToggleDetector(bool bFastMode)
{
    m_bNeedActivation = false;
    m_bFastAnimMode = bFastMode;

    if (GetState() == eHidden)
    {
        PIItem iitem = m_pInventory->ActiveItem();
        CHudItem* itm = (iitem) ? iitem->cast_hud_item() : NULL;
        u16 slot_to_activate = NO_ACTIVE_SLOT;

        if (CheckCompatibilityInt(itm, &slot_to_activate))
        {
            if (slot_to_activate != NO_ACTIVE_SLOT)
            {
                m_pInventory->Activate(slot_to_activate);
                m_bNeedActivation = true;
            }
            else
            {
                SwitchState(eShowing);
                TurnDetectorInternal(true);
            }
        }
    }
    else if (GetState() == eIdle)
        SwitchState(eHiding);
}

void CCustomDetector::OnStateSwitch(u32 S, u32 oldState)
{
    inherited::OnStateSwitch(S, oldState);
    UpdateHudParticles(S == eIdle);

    const DetectorExtendedConfig& config = detector_extended_config(this);
    const auto enableLight = [this]()
    {
        if (!m_light || m_light->get_active())
            return;

        m_light->set_active(true);
        if (m_sounds.FindSoundItem("sndSwitch", false))
            m_sounds.PlaySound("sndSwitch", Fvector().set(0.f, 0.f, 0.f), this, true, false);
    };

    switch (S)
    {
    case eShowing:
    {
        if (config.lightStateControlled && !config.lightEnableFromIdle)
            enableLight();
        g_player_hud->attach_item(this);
        m_sounds.PlaySound("sndShow", Fvector().set(0, 0, 0), this, true, false);
        PlayHUDMotion(m_bFastAnimMode ? "anm_show_fast" : "anm_show", "anim_show", FALSE /*TRUE*/, this, GetState());
        SetPending(TRUE);
    }
    break;
    case eHiding:
    {
        if (config.lightStateControlled && config.lightDisableFromIdle && m_light)
            m_light->set_active(false);
        if (oldState != eHiding)
        {
            m_sounds.PlaySound("sndHide", Fvector().set(0, 0, 0), this, true, false);
            PlayHUDMotion(m_bFastAnimMode ? "anm_hide_fast" : "anm_hide", "anim_show", FALSE/*TRUE*/, this, GetState());
            SetPending(TRUE);
        }
    }
    break;
    case eIdle:
    {
        PlayAnimIdle();
        SetPending(FALSE);
    }
    break;
    case eIdleZoom:
    {
        if (config.idleZoom)
        {
            if (config.lightStateControlled && config.lightEnableFromIdle)
                enableLight();
            PlayHUDMotion("anm_zoom", "anim_idle", TRUE, nullptr, GetState());
            SetPending(FALSE);
        }
        else
        {
            SwitchState(eIdle);
        }
    }
    break;
    }
}

void CCustomDetector::OnAnimationEnd(u32 state)
{
    inherited::OnAnimationEnd(state);
    switch (state)
    {
    case eShowing:
    {
        const DetectorExtendedConfig& config = detector_extended_config(this);
        if (config.lightStateControlled && config.lightEnableFromIdle && m_light)
        {
            const bool wasActive = m_light->get_active();
            m_light->set_active(true);
            if (!wasActive && m_sounds.FindSoundItem("sndSwitch", false))
                m_sounds.PlaySound("sndSwitch", Fvector().set(0.f, 0.f, 0.f), this, true, false);
        }
        SwitchState(eIdle);
        if (IsUsingCondition() && m_fDecayRate > 0.f)
            ChangeCondition(-m_fDecayRate);
    }
    break;
    case eHiding:
    {
        SwitchState(eHidden);
        if (m_light)
            m_light->set_active(false);
        TurnDetectorInternal(false);
        g_player_hud->detach_item(this);
    }
    break;
    }
}

void CCustomDetector::UpdateXForm() { CInventoryItem::UpdateXForm(); }
void CCustomDetector::OnActiveItem() { return; }
void CCustomDetector::OnHiddenItem() {}
CCustomDetector::CCustomDetector()
{
    m_ui = NULL;
    m_bFastAnimMode = false;
    m_bNeedActivation = false;
    m_light_anim = nullptr;
    m_light_bone = BI_NONE;
    m_light_range = 0.0f;
    m_light_brightness = 1.0f;
    m_light_angle = 0.0f;
    m_light_enabled = false;
    m_light_volumetric = false;
    m_light_shadow = false;
    m_light_spot = false;
    m_light_color.set(1.0f, 1.0f, 1.0f, 1.0f);
    m_particles_enabled = false;
    m_hud_particles_enabled = false;
    m_particles_bone = BI_NONE;
    m_hud_particles = nullptr;
}

CCustomDetector::~CCustomDetector()
{
    detectorExtendedConfigs.erase(this);
    m_artefacts.destroy();
    TurnDetectorInternal(false);
    CParticlesObject::Destroy(m_hud_particles);
    m_light.destroy();
    xr_delete(m_ui);
}

bool CCustomDetector::net_Spawn(CSE_Abstract* DC)
{
    if (!inherited::net_Spawn(DC))
        return false;

    IKinematics* visual = smart_cast<IKinematics*>(Visual());
    R_ASSERT(visual);

    const DetectorExtendedConfig& config = detector_extended_config(this);
    if (m_light_enabled)
    {
        m_light_bone = visual->LL_BoneID(m_light_bone_name);
        m_light = GEnv.Render->light_create();
        m_light->set_shadow(m_light_shadow);
        m_light->set_type(m_light_spot ? IRender_Light::SPOT : IRender_Light::POINT);
        m_light->set_range(m_light_range);
        m_light->set_color(m_light_color);
        m_light->set_cone(m_light_angle);
        m_light->set_texture(m_light_texture.c_str());
        m_light->set_volumetric(m_light_volumetric);
        m_light->set_active(!config.lightHudOnly);
    }

    if (m_particles_enabled)
    {
        CParticlesPlayer::LoadParticles(visual);
        m_particles_bone = visual->LL_BoneID(m_particles_bone_name);
        if (m_particles_bone == BI_NONE)
            Msg("! Detector '%s' has no particle bone '%s'", cNameSect().c_str(), m_particles_bone_name.c_str());
        else if (m_particles_name.size() && xr_strcmp(m_particles_name.c_str(), "none"))
            StartParticles(m_particles_name, m_particles_bone, Fvector().set(0.0f, 1.0f, 0.0f), ID(), -1, false);
    }

    TurnDetectorInternal(false);
    return true;
}

void CCustomDetector::net_Destroy()
{
    CParticlesObject::Destroy(m_hud_particles);
    m_light.destroy();
    inherited::net_Destroy();
}

void CCustomDetector::Load(LPCSTR section)
{
    m_animation_slot = 7;
    inherited::Load(section);

    m_fAfDetectRadius = pSettings->r_float(section, "af_radius");
    m_fAfVisRadius = pSettings->r_float(section, "af_vis_radius");
    m_fDecayRate = READ_IF_EXISTS(pSettings, r_float, section, "decay_rate", 0.f); //Alundaio
    m_artefacts.load(section, "af");

    m_sounds.LoadSound(section, "snd_draw", "sndShow");
    m_sounds.LoadSound(section, "snd_holster", "sndHide");
    if (pSettings->line_exist(section, "snd_switch"))
        m_sounds.LoadSound(section, "snd_switch", "sndSwitch");

    const bool lightEnabled = pSettings->r_bool(section, "light_enabled");
    DetectorExtendedConfig& config = detectorExtendedConfigs[this];
    config.lightEnableFromIdle = pSettings->read_if_exists<bool>(section, "light_enable_from_idle", lightEnabled);
    config.lightDisableFromIdle = pSettings->read_if_exists<bool>(section, "light_disable_from_idle", lightEnabled);
    config.lightHudOnly = pSettings->read_if_exists<bool>(section, "light_hud_only", false);
    config.lightStateControlled = lightEnabled || pSettings->line_exist(section, "light_enable_from_idle") ||
        pSettings->line_exist(section, "light_disable_from_idle");
    config.idleZoom = HudSection().size() && pSettings->line_exist(HudSection().c_str(), "anm_zoom");

    m_light_enabled = lightEnabled;
    m_light_range = pSettings->r_float(section, "light_range");
    m_light_brightness = pSettings->r_float(section, "light_brightness");
    m_light_angle = pSettings->r_float(section, "light_angle");
    m_light_shadow = pSettings->r_bool(section, "light_shadow");
    m_light_volumetric = pSettings->r_bool(section, "light_volumetric");
    m_light_spot = pSettings->r_bool(section, "light_spot");
    m_light_texture = pSettings->r_string(section, "light_texture");
    m_light_bone_name = pSettings->r_string(section, "light_bone");
    m_light_color = pSettings->r_fcolor(section, "light_color");
    m_light_color.a = 1.0f;
    m_light_color.mul_rgb(m_light_brightness);

    m_light_anim = LALib.FindItem(pSettings->r_string(section, "light_color_animmator"));

    m_particles_enabled = pSettings->r_bool(section, "particles_enabled");
    m_hud_particles_enabled = pSettings->r_bool(section, "hud_particles_enabled");
    m_particles_name = pSettings->r_string(section, "particles");
    m_particles_bone_name = pSettings->r_string(section, "particles_bone");
}

void CCustomDetector::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);

    if (!IsWorking())
        return;

    Position().set(H_Parent()->Position());

    Fvector P;
    P.set(H_Parent()->Position());

    const float detection_radius = IsUsingCondition() && GetCondition() <= 0.01f ? 0.f : m_fAfDetectRadius;
    m_artefacts.feel_touch_update(P, detection_radius);
}

bool CCustomDetector::IsWorking() { return m_bWorking && H_Parent() && H_Parent() == Level().CurrentViewEntity(); }
void CCustomDetector::UpfateWork()
{
    UpdateAf();
    if (m_ui)
        m_ui->update();
}

void CCustomDetector::UpdateVisibility()
{
    // check visibility
    attachable_hud_item* i0 = g_player_hud->attached_item(0);
    if (i0 && HudItemData())
    {
        bool bClimb = ((Actor()->MovingState() & mcClimb) != 0);
        if (bClimb)
        {
            HideDetector(true);
            m_bNeedActivation = true;
        }
        else
        {
            CWeapon* wpn = smart_cast<CWeapon*>(i0->m_parent_hud_item);
            if (wpn)
            {
                const u32 weaponState = wpn->GetState();
                const DetectorExtendedConfig& config = detector_extended_config(this);
                if (weaponState == CWeapon::eReload || weaponState == CWeapon::eSwitch)
                {
                    HideDetector(true);
                    m_bNeedActivation = true;
                }
                else if (wpn->IsZoomed())
                {
                    if (config.idleZoom)
                    {
                        if (GetState() != eIdleZoom)
                            SwitchState(eIdleZoom);
                    }
                    else
                    {
                        HideDetector(true);
                        m_bNeedActivation = true;
                    }
                }
                else if (GetState() == eIdleZoom)
                {
                    SwitchState(eIdle);
                }
            }
        }
    }
    else if (m_bNeedActivation)
    {
        attachable_hud_item* i0 = g_player_hud->attached_item(0);
        bool bClimb = ((Actor()->MovingState() & mcClimb) != 0);
        if (!bClimb)
        {
            CHudItem* huditem = (i0) ? i0->m_parent_hud_item : NULL;
            bool bChecked = !huditem || CheckCompatibilityInt(huditem, 0);

            if (bChecked)
                ShowDetector(true);
        }
    }
}

void CCustomDetector::UpdateCL()
{
    inherited::UpdateCL();
    UpdateDeviceEffects();

    if (H_Parent() != Level().CurrentEntity())
        return;

    UpdateVisibility();
    if (!IsWorking())
        return;

    DrainCondition(Device.fTimeDelta);
    UpfateWork();
}

void CCustomDetector::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    if (m_light)
        m_light->set_active(false);
    if (m_particles_enabled && m_particles_name.size())
        StopParticles(m_particles_name, BI_NONE, true);
}

void CCustomDetector::OnH_B_Independent(bool just_before_destroy)
{
    inherited::OnH_B_Independent(just_before_destroy);

    m_artefacts.clear();
    if (m_light)
        m_light->set_active(m_light_enabled && !detector_extended_config(this).lightHudOnly);
    if (m_particles_enabled && m_particles_bone != BI_NONE && m_particles_name.size() &&
        xr_strcmp(m_particles_name.c_str(), "none"))
        StartParticles(m_particles_name, m_particles_bone, Fvector().set(0.0f, 1.0f, 0.0f), ID(), -1, false);
    CParticlesObject::Destroy(m_hud_particles);

	if (GetState() != eHidden)
	{
		// Detaching hud item and animation stop in OnH_A_Independent
		TurnDetectorInternal(false);
		SwitchState(eHidden);
	}
}

void CCustomDetector::OnMoveToRuck(const SInvItemPlace& prev)
{
    inherited::OnMoveToRuck(prev);
    if (prev.type == eItemPlaceSlot)
    {
        SwitchState(eHidden);
        g_player_hud->detach_item(this);
    }
    if (m_light)
        m_light->set_active(false);
    TurnDetectorInternal(false);
    StopCurrentAnimWithoutCallback();
}

void CCustomDetector::OnMoveToSlot(const SInvItemPlace& prev) { inherited::OnMoveToSlot(prev); }
void CCustomDetector::TurnDetectorInternal(bool b)
{
    m_bWorking = b;
    if (b && !m_ui)
    {
        CreateUI();
    }
    else
    {
        //.		xr_delete			(m_ui);
    }

    UpdateNightVisionMode(b);
}

#include "game_base_space.h"
void CCustomDetector::UpdateNightVisionMode(bool b_on) {}

void CCustomDetector::UpdateHudParticles(bool active)
{
    if (!m_hud_particles_enabled)
        return;

    if (!active || !m_particles_name.size() || !xr_strcmp(m_particles_name.c_str(), "none"))
    {
        CParticlesObject::Destroy(m_hud_particles);
        return;
    }

    if (!m_hud_particles)
    {
        m_hud_particles = CParticlesObject::Create(m_particles_name.c_str(), FALSE);
        if (m_hud_particles)
            m_hud_particles->Play(true);
    }
}

void CCustomDetector::UpdateDeviceEffects()
{
    if (m_light && m_light->get_active())
    {
        if (!m_light_enabled)
            m_light->set_active(false);
        else
        {
            IKinematics* visual = smart_cast<IKinematics*>(Visual());
            if (visual && m_light_bone != BI_NONE)
            {
                visual->CalculateBones();
                Fmatrix transform;
                transform.mul_43(XFORM(), visual->LL_GetTransform(m_light_bone));
                m_light->set_rotation(transform.k, transform.i);
                m_light->set_position(transform.c);

                if (m_light_anim)
                {
                    int frame = 0;
                    const u32 color = m_light_anim->CalculateBGR(Device.fTimeGlobal, frame);
                    Fcolor animated;
                    animated.set(
                        float(color_get_B(color)) / 255.f,
                        float(color_get_G(color)) / 255.f,
                        float(color_get_R(color)) / 255.f,
                        1.f);
                    animated.mul_rgb(m_light_brightness);
                    m_light->set_color(animated);
                }
            }

            attachable_hud_item* hudItem = HudItemData();
            if (hudItem)
            {
                firedeps dependencies;
                hudItem->setup_firedeps(dependencies);
                m_light->set_position(dependencies.vLastFP2);
                m_light->set_rotation(
                    dependencies.m_FireParticlesXForm.k, dependencies.m_FireParticlesXForm.i);
            }
        }
    }

    if (m_hud_particles)
    {
        attachable_hud_item* hud_item = HudItemData();
        if (hud_item)
        {
            firedeps deps;
            hud_item->setup_firedeps(deps);
            Fmatrix transform = deps.m_FireParticlesXForm;
            transform.c.set(deps.vLastFP);
            m_hud_particles->UpdateParent(transform, Fvector().set(0.0f, 0.0f, 0.0f));
        }
    }
}

bool CAfList::feel_touch_contact(IGameObject* O)
{
    TypesMapIt it = m_TypesMap.find(O->cNameSect());

    bool res = (it != m_TypesMap.end());
    if (res)
    {
        CArtefact* pAf = smart_cast<CArtefact*>(O);

        if (pAf->GetAfRank() > m_af_rank)
            res = false;
    }
    return res;
}
