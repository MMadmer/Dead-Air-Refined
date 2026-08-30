#include "StdAfx.h"
#include "PdaAnimatorItem.h"

#include "da_pda3d.h"
#include "Level.h"
#include "UIGameCustom.h"
#include "UIGameSP.h"
#include "player_hud.h"
#include "ui/UIPdaWnd.h"

void CPdaAnimatorItem::attach_ui()
{
    if (m_ui_attached)
        return;
    CUIGameCustom* ui = CurrentGameUI();
    if (!ui)
        return;
    CUIPdaWnd* pda = ui->GetPdaMenuPtr();
    if (!pda)
        return;
    // Render-only: the dialog is drawn (into $user$ui via the RT pass) but takes NO input -
    // it never enters the dialog stack here, so the player keeps full movement and combat
    // control, and the PDA time dilation stays off. AddDialogToRender fires Show(true)
    // itself - the same info portions the 2D dialog always fired, exactly once.
    da_pda3d::set_presenter_active(true);
    da_pda3d::on_shown();
    ui->AddDialogToRender(pda);
    m_ui_attached = true;
}

void CPdaAnimatorItem::detach_ui()
{
    if (!m_ui_attached)
        return;
    m_ui_attached = false;
    da_pda3d::set_presenter_active(false);
    CUIGameCustom* ui = CurrentGameUI();
    if (!ui)
        return;
    CUIPdaWnd* pda = ui->GetPdaMenuPtr();
    if (!pda)
        return;
    if (da_pda3d::ui_focused())
    {
        ui->UnfocusHeldDialog(pda);
        TimeDilator()->SetCurrentMode(UITimeDilator::None);
    }
    da_pda3d::set_ui_focused(false);
    // RemoveDialogToRender fires Show(false) itself - the ui_pda_hide portion, once.
    ui->RemoveDialogToRender(pda);
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
    case eShowing: attach_ui(); break;
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

void CPdaAnimatorItem::OnZoomIn()
{
    inherited::OnZoomIn();
    // Second stage: the ALREADY SHOWN dialog gains input focus (StartDialog would assert on
    // it), the cursor comes alive, the UI eats the mouse. WASD still reaches the actor -
    // CUIPdaWnd::StopAnyMove() is false. Time dilation matches the 2D dialog's behaviour:
    // on while the player is actually looking at the screen.
    if (CUIGameCustom* ui = CurrentGameUI())
        if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
        {
            if (!m_ui_attached)
                attach_ui();
            ui->FocusHeldDialog(pda, true);
            TimeDilator()->SetCurrentMode(UITimeDilator::Pda);
            da_pda3d::set_ui_focused(true);
        }
}

void CPdaAnimatorItem::OnZoomOut()
{
    inherited::OnZoomOut();
    if (da_pda3d::ui_focused())
        if (CUIGameCustom* ui = CurrentGameUI())
            if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
            {
                ui->UnfocusHeldDialog(pda);
                TimeDilator()->SetCurrentMode(UITimeDilator::None);
            }
    da_pda3d::set_ui_focused(false);
}

void CPdaAnimatorItem::UpdateCL()
{
    inherited::UpdateCL();

    if (!m_ui_attached)
        return;

    // ESC inside the focused stage asks only to lower the device from the face.
    if (da_pda3d::consume_unzoom_request() && IsZoomed())
        OnZoomOut();

    // Someone force-hid the dialog under us (a tutorial, a script): a device in hands with
    // a dead feed makes no sense - ask the activation script to put it away.
    CUIGameCustom* ui = CurrentGameUI();
    CUIPdaWnd* pda = ui ? ui->GetPdaMenuPtr() : nullptr;
    if ((!pda || !pda->IsShown()) && GetState() != eHiding && GetState() != eHidden)
        da_pda3d::request_deactivate();
}
