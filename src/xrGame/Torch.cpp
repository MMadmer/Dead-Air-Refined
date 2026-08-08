#include "StdAfx.h"
#include "Torch.h"
#include "Entity.h"
#include "Actor.h"
#include "xrEngine/LightAnimLibrary.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrServer_Objects_ALife_Items.h"
#include "ai_sounds.h"

#include "Level.h"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/CameraBase.h"
#include "xrEngine/xr_collide_form.h"
#include "Inventory.h"
#include "game_base_space.h"

#include "UIGameCustom.h"
#include "ActorEffector.h"
#include "CustomOutfit.h"
#include "ActorHelmet.h"
#include "xrCommon/xr_hash_map.h"

constexpr pcstr TORCH_DEFINITION = "torch_definition";
static const float TORCH_INERTION_CLAMP = PI_DIV_6;
static const float TORCH_INERTION_SPEED_MIN = 0.5f;
static const Fvector TORCH_OFFSET = {-0.2f, +0.1f, -0.3f};
static const Fvector TORCH_OFFSET2 = {-0.2f, +0.1f, -0.3f};
static const Fvector OMNI_OFFSET = {-0.2f, +0.1f, -0.1f};
static const float OPTIMIZATION_DISTANCE = 100.f;

namespace
{
struct TorchExtendedConfig
{
    Fvector omniOffset{OMNI_OFFSET};
    Fvector torch2Offset{TORCH_OFFSET2};
    float inertionSpeedMin{TORCH_INERTION_SPEED_MIN};
    float inertionSpeedMax{};
    bool hasInertionSpeedMax{};
};

// Keep opt-in 1.0 tuning out of CTorch's native layout.
xr_flat_hash_map<const CTorch*, TorchExtendedConfig> torchExtendedConfigs;

TorchExtendedConfig& torch_extended_config(const CTorch* torch)
{
    return torchExtendedConfigs.try_emplace(torch).first->second;
}
}

CTorch::CTorch()
    : fBrightness(1.f), lanim(nullptr), guid_bone(BI_NONE),
      m_delta_h(0), m_switched_on(false),
      light_render(GEnv.Render->light_create()),
      light_render2(GEnv.Render->light_create()),
      light_omni(GEnv.Render->light_create()),
      glow_render(GEnv.Render->glow_create()),
      m_torch_inertion(5.f),
      m_bNightVisionEnabled(false), m_bNightVisionOn(false), m_night_vision(nullptr)
{
    m_prev_hp.set(0, 0);
    m_torch_offset = TORCH_OFFSET;
    m_torch_color.set(0.f, 0.f, 0.f, 0.f);
    m_torch2_color.set(0.f, 0.f, 0.f, 0.f);

    light_render->set_type(IRender_Light::SPOT);
    light_render->set_shadow(true);
    light_render2->set_type(IRender_Light::SPOT);
    light_render2->set_shadow(true);
    light_omni->set_type(IRender_Light::POINT);
    light_omni->set_shadow(false);

    // Disabling shift by x and z axes for 1st render,
    // because we don't have dynamic lighting in it.
    if (GEnv.Render->GenerationIsR1())
    {
        m_torch_offset.x = 0.f;
        m_torch_offset.z = 0.f;
    }
}

CTorch::~CTorch()
{
    torchExtendedConfigs.erase(this);
    light_render.destroy();
    light_render2.destroy();
    light_omni.destroy();
    glow_render.destroy();
    xr_delete(m_night_vision);
}

inline bool CTorch::can_use_dynamic_lights()
{
    if (!H_Parent())
        return (true);

    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(H_Parent());
    if (!owner)
        return (true);

    return (owner->can_use_dynamic_lights());
}

void CTorch::Load(LPCSTR section)
{
    inherited::Load(section);
    light_trace_bone = pSettings->r_string(section, "light_trace_bone");
    m_torch_offset = READ_IF_EXISTS(pSettings, r_fvector3, section, "torch_offset", TORCH_OFFSET);

    TorchExtendedConfig& config = torch_extended_config(this);
    config.omniOffset = READ_IF_EXISTS(pSettings, r_fvector3, section, "omni_offset", OMNI_OFFSET);
    config.inertionSpeedMin = _max(
        0.f, READ_IF_EXISTS(pSettings, r_float, section, "torch_inertion_speed_min", TORCH_INERTION_SPEED_MIN));
    config.hasInertionSpeedMax = pSettings->line_exist(section, "torch_inertion_speed_max");
    if (config.hasInertionSpeedMax)
    {
        config.inertionSpeedMax = _max(
            config.inertionSpeedMin, pSettings->r_float(section, "torch_inertion_speed_max"));
    }

    if (GEnv.Render->GenerationIsR1())
    {
        m_torch_offset.x = 0.f;
        m_torch_offset.z = 0.f;
    }

    m_bNightVisionEnabled = !!pSettings->r_bool(section, "night_vision");

    if (pSettings->line_exist(section, "snd_turn_on"))
        m_sounds.LoadSound(section, "snd_turn_on", "sndTurnOn", false, SOUND_TYPE_ITEM_USING);
    if (pSettings->line_exist(section, "snd_turn_off"))
        m_sounds.LoadSound(section, "snd_turn_off", "sndTurnOff", false, SOUND_TYPE_ITEM_USING);
}

void CTorch::SwitchNightVision()
{
    if (OnClient())
        return;
    SwitchNightVision(!m_bNightVisionOn);
}

void CTorch::SwitchNightVision(bool vision_on, bool use_sounds)
{
    if (!m_bNightVisionEnabled)
        return;

    m_bNightVisionOn = vision_on;

    CActor* pA = smart_cast<CActor*>(H_Parent());
    if (!pA)
    {
        return;
    }
    if (!m_night_vision)
        m_night_vision = xr_new<CNightVisionEffector>(cNameSect());

    LPCSTR disabled_names = pSettings->r_string(cNameSect(), "disabled_maps");
    pcstr curr_map = Level().name().c_str();
    u32 cnt = _GetItemCount(disabled_names);
    bool b_allow = true;
    string512 tmp;
    for (u32 i = 0; i < cnt; ++i)
    {
        _GetItem(disabled_names, i, tmp);
        if (0 == xr_stricmp(tmp, curr_map))
        {
            b_allow = false;
            break;
        }
    }

    CHelmet* pHelmet = smart_cast<CHelmet*>(pA->inventory().ItemFromSlot(HELMET_SLOT));
    CCustomOutfit* pOutfit = smart_cast<CCustomOutfit*>(pA->inventory().ItemFromSlot(OUTFIT_SLOT));

    if (pHelmet && pHelmet->m_NightVisionSect.size() && !b_allow)
    {
        m_night_vision->OnDisabled(pA, use_sounds);
        return;
    }
    else if (pOutfit && pOutfit->m_NightVisionSect.size() && !b_allow)
    {
        m_night_vision->OnDisabled(pA, use_sounds);
        return;
    }

    bool bIsActiveNow = m_night_vision->IsActive();

    if (m_bNightVisionOn)
    {
        if (!bIsActiveNow)
        {
            if (pHelmet && pHelmet->m_NightVisionSect.size())
            {
                m_night_vision->Start(pHelmet->m_NightVisionSect, pA, use_sounds);
                return;
            }
            else if (pOutfit && pOutfit->m_NightVisionSect.size())
            {
                m_night_vision->Start(pOutfit->m_NightVisionSect, pA, use_sounds);
                return;
            }
            m_bNightVisionOn = false; // in case if there is no nightvision in helmet and outfit
        }
    }
    else
    {
        if (bIsActiveNow)
        {
            m_night_vision->Stop(100000.0f, use_sounds);
        }
    }
}

void CTorch::Switch()
{
    if (OnClient())
        return;
    bool bActive = !m_switched_on;
    Switch(bActive);
}

void CTorch::Switch(bool light_on)
{
    m_switched_on = light_on;
    if (can_use_dynamic_lights())
    {
        light_render->set_active(light_on);
        light_omni->set_active(smart_cast<CActor*>(H_Parent()) ? false : light_on);
    }
    glow_render->set_active(light_on);

    if (light_trace_bone.c_str())
    {
        IKinematics* pVisual = smart_cast<IKinematics*>(Visual());
        VERIFY(pVisual);
        u16 bi = pVisual->LL_BoneID(light_trace_bone);

        pVisual->LL_SetBoneVisible(bi, light_on, TRUE);
        pVisual->CalculateBones(TRUE);
    }
}

void CTorch::Switch2(bool light_on)
{
    m_switched_on = light_on;
    if (can_use_dynamic_lights())
    {
        light_render2->set_active(light_on);
        light_omni->set_active(light_on);
    }
    glow_render->set_active(light_on);

    if (light_trace_bone.c_str())
    {
        IKinematics* visual = smart_cast<IKinematics*>(Visual());
        VERIFY(visual);
        const u16 bone = visual->LL_BoneID(light_trace_bone);
        visual->LL_SetBoneVisible(bone, light_on, TRUE);
        visual->CalculateBones(TRUE);
    }
}

void CTorch::SetTorchSpot(bool spot)
{
    if (OnClient())
        return;

    light_render->set_type(spot ? IRender_Light::SPOT : IRender_Light::POINT);
}

void CTorch::SetTorchRadius(float value)
{
    if (OnClient())
        return;

    light_render->set_cone(deg2rad(value));
    glow_render->set_radius(value);
}

void CTorch::SetTorchRange(float value)
{
    if (!OnClient())
        light_render->set_range(value);
}

void CTorch::SetTorchInertion(float value)
{
    if (!OnClient())
        m_torch_inertion = value;
}

void CTorch::SetTorchColorR(float value)
{
    if (OnClient())
        return;

    m_torch_color.r = value;
    light_render->set_color(m_torch_color);
    glow_render->set_color(m_torch_color);
}

void CTorch::SetTorchColorG(float value)
{
    if (OnClient())
        return;

    m_torch_color.g = value;
    light_render->set_color(m_torch_color);
    glow_render->set_color(m_torch_color);
}

void CTorch::SetTorchColorB(float value)
{
    if (OnClient())
        return;

    m_torch_color.b = value;
    light_render->set_color(m_torch_color);
    glow_render->set_color(m_torch_color);
}

void CTorch::SetTorchColorA(float value)
{
    if (OnClient())
        return;

    m_torch_color.a = value;
    light_render->set_color(m_torch_color);
}

void CTorch::SetTorchOffsetX(float value)
{
    if (!OnClient())
        m_torch_offset.x = value;
}

void CTorch::SetTorchOffsetY(float value)
{
    if (!OnClient())
        m_torch_offset.y = value;
}

void CTorch::SetTorchOffsetZ(float value)
{
    if (!OnClient())
        m_torch_offset.z = value;
}

void CTorch::SetTorchAnimation(LPCSTR value)
{
    if (!OnClient())
        lanim = LALib.FindItem(value);
}

void CTorch::SetTorchTexture(LPCSTR value)
{
    if (OnClient())
        return;

    light_render->set_texture(value);
    glow_render->set_texture(value);
}

void CTorch::SetTorch2Radius(float value)
{
    if (!OnClient())
        light_render2->set_cone(deg2rad(value));
}

void CTorch::SetTorch2Range(float value)
{
    if (!OnClient())
        light_render2->set_range(value);
}

void CTorch::SetTorch2ColorR(float value)
{
    if (OnClient())
        return;

    m_torch2_color.r = value;
    light_render2->set_color(m_torch2_color);
    light_omni->set_color(m_torch2_color);
}

void CTorch::SetTorch2ColorG(float value)
{
    if (OnClient())
        return;

    m_torch2_color.g = value;
    light_render2->set_color(m_torch2_color);
    light_omni->set_color(m_torch2_color);
}

void CTorch::SetTorch2ColorB(float value)
{
    if (OnClient())
        return;

    m_torch2_color.b = value;
    light_render2->set_color(m_torch2_color);
    light_omni->set_color(m_torch2_color);
}

void CTorch::SetTorch2ColorA(float value)
{
    if (OnClient())
        return;

    m_torch2_color.a = value;
    light_render2->set_color(m_torch2_color);
    light_omni->set_color(m_torch2_color);
}

void CTorch::SetTorch2OffsetX(float value)
{
    if (!OnClient())
        torch_extended_config(this).torch2Offset.x = value;
}

void CTorch::SetTorch2OffsetY(float value)
{
    if (!OnClient())
        torch_extended_config(this).torch2Offset.y = value;
}

void CTorch::SetTorch2OffsetZ(float value)
{
    if (!OnClient())
        torch_extended_config(this).torch2Offset.z = value;
}

bool CTorch::torch_active() const { return (m_switched_on); }
bool CTorch::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeItemTorch* torch = smart_cast<CSE_ALifeItemTorch*>(e);
    R_ASSERT(torch);
    cNameVisual_set(torch->get_visual());

    R_ASSERT(!GetCForm());
    R_ASSERT(smart_cast<IKinematics*>(Visual()));
    CForm = xr_new<CCF_Skeleton>(this);

    if (!inherited::net_Spawn(DC))
        return (FALSE);

    bool b_r2 = GEnv.Render->GenerationIsR2OrHigher();

    IKinematics* K = smart_cast<IKinematics*>(Visual());
    CInifile* pUserData = K->LL_UserData();
    R_ASSERT3(pUserData, "Empty Torch user data!", torch->get_visual());
    lanim = LALib.FindItem(pUserData->r_string(TORCH_DEFINITION, "color_animator"));
    guid_bone = K->LL_BoneID(pUserData->r_string(TORCH_DEFINITION, "guide_bone"));
    VERIFY(guid_bone != BI_NONE);

    Fcolor clr = pUserData->r_fcolor(TORCH_DEFINITION, (b_r2) ? "color_r2" : "color");
    fBrightness = clr.intensity();
    float range = pUserData->r_float(TORCH_DEFINITION, (b_r2) ? "range_r2" : "range");
    m_torch_color = clr;
    light_render->set_color(clr);
    light_render->set_range(range);
    light_render2->set_color(clr);
    light_render2->set_range(range);

    if (b_r2)
    {
        bool useVolumetric = pUserData->read_if_exists<bool>(TORCH_DEFINITION, "volumetric_enabled", false);
        light_render->set_volumetric(useVolumetric);
        if (useVolumetric)
        {
            float volQuality = pUserData->read_if_exists<float>(TORCH_DEFINITION, "volumetric_quality", 1.f);
            clamp(volQuality, 0.f, 1.f);
            light_render->set_volumetric_quality(volQuality);

            float volIntensity = pUserData->read_if_exists<float>(TORCH_DEFINITION, "volumetric_intensity", 1.f);
            clamp(volIntensity, 0.f, 10.f);
            light_render->set_volumetric_intensity(volIntensity);

            float volDistance = pUserData->read_if_exists<float>(TORCH_DEFINITION, "volumetric_distance", 1.f);
            clamp(volDistance, 0.f, 1.f);
            light_render->set_volumetric_distance(volDistance);
        }
    }

    Fcolor clr_o = pUserData->r_fcolor(TORCH_DEFINITION, (b_r2) ? "omni_color_r2" : "omni_color");
    float range_o = pUserData->r_float(TORCH_DEFINITION, (b_r2) ? "omni_range_r2" : "omni_range");
    light_omni->set_color(clr_o);
    light_omni->set_range(range_o);

    light_render->set_cone(deg2rad(pUserData->r_float(TORCH_DEFINITION, "spot_angle")));
    light_render->set_texture(pUserData->r_string(TORCH_DEFINITION, "spot_texture"));
    light_render2->set_cone(deg2rad(pUserData->r_float(TORCH_DEFINITION, "spot_angle")));
    light_render2->set_texture(pUserData->r_string(TORCH_DEFINITION, "spot_texture"));

    glow_render->set_texture(pUserData->r_string(TORCH_DEFINITION, "glow_texture"));
    glow_render->set_color(clr);
    glow_render->set_radius(pUserData->r_float(TORCH_DEFINITION, "glow_radius"));

    //включить/выключить фонарик
    Switch(torch->m_active);
    VERIFY(!torch->m_active || (torch->ID_Parent != 0xffff));

    if (torch->ID_Parent == 0)
        SwitchNightVision(torch->m_nightvision_active, false);
    // else
    //	SwitchNightVision	(false, false);

    m_delta_h = fis_zero(m_torch_offset.x) ?
        0.f :
        PI_DIV_2 - atan((range * 0.5f) / _abs(m_torch_offset.x));

    return (TRUE);
}

void CTorch::net_Destroy()
{
    Switch(false);
    SwitchNightVision(false);

    inherited::net_Destroy();
}

void CTorch::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
}

void CTorch::OnH_B_Independent(bool just_before_destroy)
{
    inherited::OnH_B_Independent(just_before_destroy);

    Switch(false);
    SwitchNightVision(false);

    m_sounds.StopAllSounds();
}

void CTorch::OnMoveToRuck(const SInvItemPlace& prev)
{
    inherited::OnMoveToRuck(prev);
    if (prev.type != eItemPlaceSlot)
        return;

    Switch(false);
    SwitchNightVision(false);
}

void CTorch::UpdateCL()
{
    inherited::UpdateCL();

    // Only the light in the actor's hands bypasses the local shadow budget: with a small
    // budget the slot would go to the nearest wall lamp and the hand light would shine
    // through walls. NPC and dropped torches compete like any other light. Refreshed every
    // frame because ownership changes on pickup/drop.
    const bool actorOwned = !!smart_cast<CActor*>(H_Parent());
    light_render->set_never_demote(actorOwned);
    light_render2->set_never_demote(actorOwned);
    light_omni->set_never_demote(actorOwned);

    if (!m_switched_on)
        return;

    CBoneInstance& BI = smart_cast<IKinematics*>(Visual())->LL_GetBoneInstance(guid_bone);
    Fmatrix M;

    if (H_Parent())
    {
        const TorchExtendedConfig& config = torch_extended_config(this);
        CActor* actor = smart_cast<CActor*>(H_Parent());
        if (actor)
        {
            DrainCondition(Device.fTimeDelta);
            if (GetCondition() <= 0.f)
            {
                Switch(false);
                return;
            }
            smart_cast<IKinematics*>(H_Parent()->Visual())->CalculateBones_Invalidate();
        }

        if (H_Parent()->XFORM().c.distance_to_sqr(Device.vCameraPosition) < _sqr(OPTIMIZATION_DISTANCE) ||
            GameID() != eGameIDSingle)
        {
            // near camera
            smart_cast<IKinematics*>(H_Parent()->Visual())->CalculateBones();
            M.mul_43(XFORM(), BI.mTransform);
        }
        else
        {
            // approximately the same
            M = H_Parent()->XFORM();
            H_Parent()->Center(M.c);
            M.c.y += H_Parent()->Radius() * 2.f / 3.f;
        }

        if (actor)
        {
            const CCameraBase* camera = actor->active_cam() == eacLookAt ? actor->cam_Active() : actor->cam_FirstEye();
            const float inertionSpeedMax = config.hasInertionSpeedMax ?
                config.inertionSpeedMax : _max(config.inertionSpeedMin, m_torch_inertion + config.inertionSpeedMin);
            m_prev_hp.x = angle_inertion_var(m_prev_hp.x, -camera->yaw, config.inertionSpeedMin,
                inertionSpeedMax, TORCH_INERTION_CLAMP, Device.fTimeDelta);
            m_prev_hp.y = angle_inertion_var(m_prev_hp.y, -camera->pitch, config.inertionSpeedMin,
                inertionSpeedMax, TORCH_INERTION_CLAMP, Device.fTimeDelta);

            Fvector dir, right, up;
            dir.setHP(m_prev_hp.x + m_delta_h, m_prev_hp.y);
            Fvector::generate_orthonormal_basis_normalized(dir, up, right);

            Fvector offset = M.c;
            offset.mad(M.i, m_torch_offset.x);
            offset.mad(M.j, m_torch_offset.y);
            offset.mad(M.k, m_torch_offset.z);
            light_render->set_position(offset);

            offset = M.c;
            offset.mad(M.i, config.torch2Offset.x);
            offset.mad(M.j, config.torch2Offset.y);
            offset.mad(M.k, config.torch2Offset.z);
            light_render2->set_position(offset);

            offset = M.c;
            offset.mad(M.i, config.omniOffset.x);
            offset.mad(M.j, config.omniOffset.y);
            offset.mad(M.k, config.omniOffset.z);
            light_omni->set_position(offset);
            glow_render->set_position(M.c);

            light_render->set_rotation(dir, right);
            light_render2->set_rotation(dir, right);
            light_omni->set_rotation(dir, right);
            glow_render->set_direction(dir);

        } // if(actor)
        else
        {
            if (can_use_dynamic_lights())
            {
                light_render->set_position(M.c);
                light_render->set_rotation(M.k, M.i);
                light_render2->set_position(M.c);
                light_render2->set_rotation(M.k, M.i);
                light_render2->set_active(false);

                Fvector offset = M.c;
                offset.mad(M.i, config.omniOffset.x);
                offset.mad(M.j, config.omniOffset.y);
                offset.mad(M.k, config.omniOffset.z);
                light_omni->set_position(offset);
                light_omni->set_rotation(M.k, M.i);
            } // if (can_use_dynamic_lights())

            glow_render->set_position(M.c);
            glow_render->set_direction(M.k);
        }
    } // if(HParent())
    else
    {
        if (getVisible() && m_pPhysicsShell)
        {
            M.mul(XFORM(), BI.mTransform);

            m_switched_on = false;
            light_render->set_active(false);
            light_render2->set_active(false);
            light_omni->set_active(false);
            glow_render->set_active(false);
        } // if (getVisible() && m_pPhysicsShell)
    }

    if (!m_switched_on)
        return;

    // calc color animator
    if (!lanim)
        return;

    int frame;
    // возвращает в формате BGR
    u32 clr = lanim->CalculateBGR(Device.fTimeGlobal, frame);

    Fcolor fclr;
    fclr.set((float)color_get_B(clr), (float)color_get_G(clr), (float)color_get_R(clr), 1.f);
    fclr.mul_rgb(fBrightness / 255.f);
    if (can_use_dynamic_lights())
    {
        light_render->set_color(fclr);
        light_omni->set_color(fclr);
    }
    glow_render->set_color(fclr);
}

void CTorch::create_physic_shell() { CPhysicsShellHolder::create_physic_shell(); }
void CTorch::activate_physic_shell() { CPhysicsShellHolder::activate_physic_shell(); }
void CTorch::setup_physic_shell() { CPhysicsShellHolder::setup_physic_shell(); }
void CTorch::net_Export(NET_Packet& P)
{
    inherited::net_Export(P);
    //	P.w_u8						(m_switched_on ? 1 : 0);

    u8 F = 0;
    F |= (m_switched_on ? eTorchActive : 0);
    F |= (m_bNightVisionOn ? eNightVisionActive : 0);
    const CActor* pA = smart_cast<const CActor*>(H_Parent());
    if (pA)
    {
        if (pA->attached(this))
            F |= eAttached;
    }
    P.w_u8(F);
    //	Msg("CTorch::net_export - NV[%d]", m_bNightVisionOn);
}

void CTorch::net_Import(NET_Packet& P)
{
    inherited::net_Import(P);

    u8 F = P.r_u8();
    bool new_m_switched_on = !!(F & eTorchActive);
    bool new_m_bNightVisionOn = !!(F & eNightVisionActive);

    if (new_m_switched_on != m_switched_on)
        Switch(new_m_switched_on);
    if (new_m_bNightVisionOn != m_bNightVisionOn)
    {
        //		Msg("CTorch::net_Import - NV[%d]", new_m_bNightVisionOn);

        const CActor* pA = smart_cast<const CActor*>(H_Parent());
        if (pA)
        {
            SwitchNightVision(new_m_bNightVisionOn);
        }
    }
}
bool CTorch::can_be_attached() const
{
    const CActor* pA = smart_cast<const CActor*>(H_Parent());
    if (pA)
        return pA->inventory().InSlot(this);
    else
        return true;
}

void CTorch::afterDetach()
{
    inherited::afterDetach();
    Switch(false);
}

void CTorch::enable(bool value)
{
    inherited::enable(value);

    if (!enabled() && m_switched_on)
        Switch(false);
}

CNightVisionEffector::CNightVisionEffector(const shared_str& section)
    : m_pActor(nullptr)
{
    m_sounds.LoadSound(section.c_str(), "snd_night_vision_on", "NightVisionOnSnd", false, SOUND_TYPE_ITEM_USING);
    m_sounds.LoadSound(section.c_str(), "snd_night_vision_off", "NightVisionOffSnd", false, SOUND_TYPE_ITEM_USING);
    m_sounds.LoadSound(section.c_str(), "snd_night_vision_idle", "NightVisionIdleSnd", true, SOUND_TYPE_ITEM_USING);
    m_sounds.LoadSound(
        section.c_str(), "snd_night_vision_broken", "NightVisionBrokenSnd", false, SOUND_TYPE_ITEM_USING);
}

void CNightVisionEffector::Start(const shared_str& sect, CActor* pA, bool play_sound)
{
    m_pActor = pA;
    if (!m_pActor)
        return;

    if (!IsActive())
        AddEffector(m_pActor, effNightvision, sect);

    if (play_sound)
    {
        PlaySounds(eStartSound);
        PlaySounds(eIdleSound);
    }
}

void CNightVisionEffector::Stop(const float factor, bool play_sound)
{
    m_sounds.StopSound("NightVisionOnSnd");
    m_sounds.StopSound("NightVisionIdleSnd");

    if (!m_pActor)
        return;
    CEffectorPP* pp = m_pActor->Cameras().GetPPEffector((EEffectorPPType)effNightvision);
    if (pp)
    {
        pp->Stop(factor);
        if (play_sound)
            PlaySounds(eStopSound);
    }
}

bool CNightVisionEffector::IsActive()
{
    if (!m_pActor)
        return false;
    CEffectorPP* pp = m_pActor->Cameras().GetPPEffector((EEffectorPPType)effNightvision);
    return pp;
}

void CNightVisionEffector::OnDisabled(CActor* pA, bool play_sound)
{
    m_pActor = pA;
    m_sounds.StopSound("NightVisionOnSnd");
    m_sounds.StopSound("NightVisionIdleSnd");
    if (play_sound)
        PlaySounds(eBrokeSound);
}

void CNightVisionEffector::PlaySounds(EPlaySounds which)
{
    if (!m_pActor)
        return;

    bool bPlaySoundFirstPerson = !!m_pActor->HUDview();
    switch (which)
    {
    case eStartSound: { m_sounds.PlaySound("NightVisionOnSnd", m_pActor->Position(), NULL, bPlaySoundFirstPerson);
    }
    break;
    case eStopSound: { m_sounds.PlaySound("NightVisionOffSnd", m_pActor->Position(), NULL, bPlaySoundFirstPerson);
    }
    break;
    case eIdleSound:
    {
        m_sounds.PlaySound("NightVisionIdleSnd", m_pActor->Position(), NULL, bPlaySoundFirstPerson, true);
    }
    break;
    case eBrokeSound: { m_sounds.PlaySound("NightVisionBrokenSnd", m_pActor->Position(), NULL, bPlaySoundFirstPerson);
    }
    break;
    default: NODEFAULT;
    }
}
