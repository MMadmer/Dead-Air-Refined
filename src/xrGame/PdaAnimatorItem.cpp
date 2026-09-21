// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "PdaAnimatorItem.h"

#include "da_pda3d.h"
#include "Level.h"
#include "Actor.h"
#include "xrEngine/xr_level_controller.h"
#include "Inventory.h"
#include "Torch.h"
#include "UIGameCustom.h"
#include "UIGameSP.h"
#include "player_hud.h"
#include "ui/UIPdaWnd.h"

void CPdaAnimatorItem::attach_ui()
{
    if (m_ui_attached)
        return;
    // Window-led: the window is normally ALREADY shown (opening it is what raised this
    // item); show_window here covers the savegame-restore path, where the item comes back
    // on its own and brings the window up with it. AddDialogToRender dedups.
    da_pda3d::set_presenter_active(true);
    da_pda3d::show_window();
    // The screen turns on at screen_on_mark of the show motion (the thumb hits the power
    // button); the timings of the just-started anm_show are in the base members.
    da_pda3d::on_shown(m_dwMotionStartTm, m_dwMotionEndTm);
    m_ui_attached = true;
}

void CPdaAnimatorItem::detach_ui()
{
    if (!m_ui_attached)
        return;
    m_ui_attached = false;
    da_pda3d::set_presenter_active(false);
    // hide_window drops the focus stage (input stack + time dilation) before removing the
    // dialog from the render list; both are idempotent.
    da_pda3d::hide_window();
}

void CPdaAnimatorItem::OnStateSwitch(u32 S, u32 oldState)
{
    // The hands must be the animation set's native rig BEFORE the show state resolves and
    // plays its motions: this item's hud MotionIDs bind against the CURRENT hands model on
    // first attach, and the previous weapon is already hidden at this point, so nothing
    // else gets re-bound mid-life. Covers every activation path, savegame restore included.
    if (S == eShowing)
        da_pda3d::swap_hands_in();
    inherited::OnStateSwitch(S, oldState);
    switch (S)
    {
    case eShowing:
        m_aim_started = false;
        m_oneshot_playing = false;
        m_torch_nv_valid = false;
        m_want_focus = false;
        attach_ui();
        break;
    case eHiding:
        // Drop the focus as soon as the hide starts, so the holster animation plays with
        // the player back in control.
        if (IsZoomed())
            OnZoomOut();
        break;
    case eHidden:
        detach_ui();
        // Hand the DA hands back BEFORE the previous weapon starts its show (the actor tick
        // attaches it right after the slot switch lands). Detach self first so the hands
        // reload touches no attached item; the weapon then binds to the restored rig, where
        // its MotionIDs resolved originally. Without this the weapon raised on the pda rig.
        if (g_player_hud)
            g_player_hud->detach_item(this);
        da_pda3d::swap_hands_out();
        break;
    default: break;
    }
}

void CPdaAnimatorItem::net_Destroy()
{
    // Safety net for every abrupt end - death with the device up, a script releasing the
    // object outright. Without this the presenter flag stayed raised forever: hands stuck
    // on the pda rig, the 2D dialog suppressed, every toggle swallowed. During level
    // teardown (net_Stop clears bReady before remove_objects) the UI widgets are already
    // half-dead - poking Show(false) into the map window crashed on quit - so only the
    // flags are cleared there; the whole UI goes down right after us anyway.
    if (g_pGameLevel && Level().bReady)
        detach_ui();
    else
    {
        m_ui_attached = false;
        da_pda3d::set_presenter_active(false);
        da_pda3d::set_ui_focused(false);
    }
    inherited::net_Destroy();
}

void CPdaAnimatorItem::enter_focus()
{
    // The ALREADY SHOWN dialog gains input focus (StartDialog would assert on it), the
    // cursor comes alive, the UI eats the mouse. WASD still reaches the actor -
    // CUIPdaWnd::StopAnyMove() is false. Time dilation matches the 2D dialog's behaviour:
    // on while the player is actually driving the screen. set_ui_focused BEFORE
    // FocusHeldDialog so any re-entrant zoom-out sees the guard armed.
    if (CUIGameCustom* ui = CurrentGameUI())
        if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
        {
            if (!m_ui_attached)
                attach_ui();
            da_pda3d::set_ui_focused(true);
            ui->FocusHeldDialog(pda, true);
            TimeDilator()->SetCurrentMode(UITimeDilator::Pda);
        }
}

void CPdaAnimatorItem::OnZoomIn()
{
    // Skip CWeaponBinoculars: its zoom path plays binocular sounds and VERIFYs the
    // m_binoc_vision it never created (vision_present = false on animator items).
    CWeaponMagazined::OnZoomIn();
    // LMB look-zoom raises the device to the eyes and stops there - the mouse stays on
    // the camera; only the RMB focus toggle brings the cursor in.
    if (m_want_focus)
        enter_focus();
}

void CPdaAnimatorItem::OnZoomOut()
{
    m_want_focus = false;
    CWeaponMagazined::OnZoomOut();
    if (da_pda3d::ui_focused())
        if (CUIGameCustom* ui = CurrentGameUI())
            if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
            {
                ui->UnfocusHeldDialog(pda);
                TimeDilator()->SetCurrentMode(UITimeDilator::None);
            }
    da_pda3d::set_ui_focused(false);
}

float CPdaAnimatorItem::GetInertionFactor()
{
    // player_hud::update_inertion lerps hip->aim as: value = aim - (aim - hip) * factor.
    // CHudItem returns a constant 1.0, i.e. the hip numbers win even while aiming - the
    // aim keys are dead for every item in the fork. Here the zoom rotation drives it, so
    // the device smoothly takes the aim inertion (damped, see the ltx) as it comes up to
    // the face and the hip feel back as it lowers. Nothing else reads this value.
    return 1.f - clampr(m_zoom_params.m_fZoomRotationFactor, 0.f, 1.f);
}

bool CPdaAnimatorItem::Action(u16 cmd, u32 flags)
{
    switch (cmd)
    {
    case kWPN_FIRE:
        // LMB = hold-to-look (the Anomaly convention the player asked for): press raises
        // the device to the eyes with the mouse STAYING on the camera - read on the move,
        // sprint drops via the normal zoom rules - release lowers it back. In the focused
        // stage the window owns the input, so this never fires there and LMB stays a UI
        // click. Also overrides CWeaponBinoculars' kWPN_FIRE->kWPN_ZOOM remap.
        if (flags & CMD_START)
        {
            if (!IsZoomed() && !IsPending())
            {
                m_want_focus = false;
                if (GetState() != eIdle)
                    SwitchState(eIdle);
                OnZoomIn();
            }
        }
        else if (IsZoomed() && !da_pda3d::ui_focused())
            OnZoomOut();
        return true;

    case kWPN_ZOOM:
        // RMB = focus TOGGLE (holding it would fight the cursor): press with the device
        // down raises straight into focus; press during a look-zoom promotes it to focus;
        // leaving focus happens inside the UI (RMB/ESC there -> request_unzoom). The
        // release is eaten - the stock hold-to-zoom path must not lower the device.
        if (!(flags & CMD_START))
            return true;
        if (!IsZoomed())
        {
            if (!IsPending())
            {
                m_want_focus = true;
                if (GetState() != eIdle)
                    SwitchState(eIdle);
                OnZoomIn();
            }
        }
        else if (!da_pda3d::ui_focused())
        {
            m_want_focus = true;
            enter_focus();
        }
        return true;

    default: break;
    }
    return inherited::Action(cmd, flags);
}

bool CPdaAnimatorItem::play_first_existing(std::initializer_list<pcstr> names, bool mix_in)
{
    for (pcstr n : names)
        if (n && isHUDAnimationExist(n, true))
        {
            PlayHUDMotion(n, mix_in ? TRUE : FALSE, this, GetState());
            return true;
        }
    return false;
}

void CPdaAnimatorItem::play_composed_idle()
{
    // Movement axis, matching what the base TryPlayAnimIdle distinguishes (sprint is
    // handled by the caller before we get here).
    pcstr move = "";
    if (CActor* actor = smart_cast<CActor*>(CHudItem::object().H_Parent()))
    {
        CEntity::SEntityState st;
        actor->g_State(st);
        if (actor->AnyMove())
            move = st.bCrouch ? "_moving_crouch" : "_moving";
        else if (st.bCrouch)
            move = "_crouch";
    }

    const bool aim = IsZoomed() || m_aim_started;
    pcstr joy = aim ? da_pda3d::joystick_suffix() : "";

    string128 buf;
    // Fallback ladder, most specific first: aim+joystick+move -> aim+joystick ->
    // aim+move -> aim -> joystick-less plain chain. Every rung is optional data.
    if (aim)
    {
        if (joy[0])
        {
            xr_sprintf(buf, "anm_idle_aim%s%s", joy, move);
            if (isHUDAnimationExist(buf, true))
            {
                PlayHUDMotion(buf, TRUE, this, GetState());
                return;
            }
            xr_sprintf(buf, "anm_idle_aim%s", joy);
            if (isHUDAnimationExist(buf, true))
            {
                PlayHUDMotion(buf, TRUE, this, GetState());
                return;
            }
        }
        xr_sprintf(buf, "anm_idle_aim%s", move);
        if (isHUDAnimationExist(buf, true))
        {
            PlayHUDMotion(buf, TRUE, this, GetState());
            return;
        }
        if (play_first_existing({"anm_idle_aim"}, true))
            return;
        // no aim set at all - fall through to the plain chain
    }
    xr_sprintf(buf, "anm_idle%s", move);
    if (isHUDAnimationExist(buf, true))
    {
        PlayHUDMotion(buf, TRUE, this, GetState());
        return;
    }
    PlayHUDMotion("anm_idle", "anim_idle", TRUE, this, GetState());
}

void CPdaAnimatorItem::PlayAnimIdle()
{
    // Raise-to-face edge latch: entering zoom plays the one-shot transition first, the
    // looping aim idle comes on its OnAnimationEnd. Leaving zoom mirrors it.
    if (IsZoomed() && !m_aim_started)
    {
        m_aim_started = true;
        if (play_first_existing({"anm_idle_aim_start"}, true))
        {
            m_oneshot_playing = true;
            return;
        }
    }
    else if (!IsZoomed() && m_aim_started)
    {
        m_aim_started = false;
        if (GetState() == eIdle && play_first_existing({"anm_idle_aim_end"}, true))
        {
            m_oneshot_playing = true;
            return;
        }
    }

    // Sprint keeps the stock behaviour (anm_idle_sprint already resolves for us).
    if (MovingAnimAllowedNow() && !IsZoomed())
        if (CActor* actor = smart_cast<CActor*>(CHudItem::object().H_Parent()))
        {
            CEntity::SEntityState st;
            actor->g_State(st);
            if (st.bSprint && isHUDAnimationExist("anm_idle_sprint", true))
            {
                PlayHUDMotion("anm_idle_sprint", TRUE, this, GetState());
                return;
            }
        }

    play_composed_idle();
}

void CPdaAnimatorItem::PlayAnimHide()
{
    // Holstering straight out of the raised-to-face pose gets its own motion (the hands
    // are near the head, the plain holster starts from the hip and pops).
    if ((m_aim_started || IsZoomed()) && isHUDAnimationExist("anm_hide_from_aim", true))
    {
        m_aim_started = false;
        PlayHUDMotion("anm_hide_from_aim", TRUE, this, GetState());
        return;
    }
    m_aim_started = false;
    inherited::PlayAnimHide();
}

void CPdaAnimatorItem::OnAnimationEnd(u32 state)
{
    // One-shot overlays (aim start/end, headlamp ack) end inside eIdle, where the base
    // handler does nothing - re-enter the composed idle explicitly.
    if (state == eIdle && m_oneshot_playing)
    {
        m_oneshot_playing = false;
        PlayAnimIdle();
        return;
    }
    inherited::OnAnimationEnd(state);
}

void CPdaAnimatorItem::UpdateCL()
{
    inherited::UpdateCL();

    if (!m_ui_attached)
        return;

    // ESC (or P/M) inside the focused stage asks only to lower the device from the face.
    // Consume only when it can be acted on - a swallowed request must not vanish.
    if (da_pda3d::ui_focused() && da_pda3d::consume_unzoom_request())
        OnZoomOut();

    const bool idle_ready = GetState() == eIdle && !IsPending();

    // Joystick: in the focused stage the cursor motion drives the thumb. Re-enter idle on
    // a direction change; the quantizer itself rate-limits to the configured period.
    if (da_pda3d::ui_focused() && da_pda3d::joystick_step() && idle_ready && !m_oneshot_playing)
        PlayAnimIdle();

    // Headlamp / night vision acknowledgement: the toggles live on the actor (kTORCH /
    // kNIGHT_VISION reach CTorch directly), the device just visibly reacts - the free hand
    // flicks to the headgear. The light itself is NOT gated on the motion; fidelity note
    // in docs/dead-air/pda-1to1-plan.md.
    if (idle_ready && !m_oneshot_playing)
        if (CActor* actor = smart_cast<CActor*>(CHudItem::object().H_Parent()))
            if (CTorch* torch = smart_cast<CTorch*>(actor->inventory().ItemFromSlot(TORCH_SLOT)))
            {
                const bool t = torch->torch_active();
                const bool nv = torch->GetNightVisionStatus();
                if (!m_torch_nv_valid)
                {
                    m_torch_seen = t;
                    m_nv_seen = nv;
                    m_torch_nv_valid = true;
                }
                else if (t != m_torch_seen || nv != m_nv_seen)
                {
                    m_torch_seen = t;
                    m_nv_seen = nv;
                    const bool aim = IsZoomed() || m_aim_started;
                    if (play_first_existing(
                            {aim ? "anm_headlamp_aim" : "anm_headlamp", "anm_headlamp"}, true))
                        m_oneshot_playing = true;
                }
            }
}
