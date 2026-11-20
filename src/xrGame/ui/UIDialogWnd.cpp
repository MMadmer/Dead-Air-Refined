#include "StdAfx.h"
#include "UIDialogWnd.h"

CUIDialogWnd::CUIDialogWnd(pcstr window_name) : CUIWindow(window_name)
{
    m_pParentHolder = NULL;
    m_bWorkInPause = false;
    m_bShowMe = false;
}

CUIDialogWnd::~CUIDialogWnd()
{
    // A window may die while a holder still lists it: Lua frees script windows at its GC and at the script engine
    // restart of a load, shown or not, and a holder kept the freed pointer for its next frame.
    SetHolder(nullptr);
    CDialogHolder::OnDialogDestroyed(this);
}

void CUIDialogWnd::SetHolder(CDialogHolder* h)
{
    if (m_pParentHolder == h)
        return;

    if (m_pParentHolder)
    {
        auto& linked = m_pParentHolder->m_linked_dialogs;
        const auto it = std::find(linked.begin(), linked.end(), this);
        if (it != linked.end())
            linked.erase(it);
    }

    m_pParentHolder = h;

    if (h)
        h->m_linked_dialogs.push_back(this);
}

void CUIDialogWnd::Show(bool status)
{
    inherited::Show(status);

    if (status)
        ResetAll();
}

bool CUIDialogWnd::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (!IR_process())
        return false;
    if (inherited::OnKeyboardAction(dik, keyboard_action))
        return true;
    return false;
}

bool CUIDialogWnd::OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action)
{
    if (!IR_process())
        return false;
    if (inherited::OnControllerAction(axis, state, controller_action))
        return true;
    return false;
}

bool CUIDialogWnd::IR_process()
{
    if (!IsEnabled())
        return false;

    if (GetHolder() && GetHolder()->IgnorePause())
        return true;

    if (Device.Paused() && !WorkInPause())
        return false;

    return true;
}

void CUIDialogWnd::FillDebugInfo()
{
#ifndef MASTER_GOLD
    CUIWindow::FillDebugInfo();

    if (ImGui::CollapsingHeader(CUIDialogWnd::GetDebugType()))
    {
        ImGui::LabelText("Current holder", "%s", m_pParentHolder ? m_pParentHolder->GetDebugType() : "none");
        ImGui::LabelText("Work in pause", m_bWorkInPause ? "true" : "false");
    }
#endif
}

CDialogHolder* CurrentDialogHolder();

void CUIDialogWnd::ShowOrHideDialog(bool bDoHideIndicators)
{
    if (IsShown())
        GetHolder()->StopDialog(this);
    else
        CurrentDialogHolder()->StartDialog(this, bDoHideIndicators);
}

void CUIDialogWnd::ShowDialog(bool bDoHideIndicators)
{
    if (!IsShown())
        CurrentDialogHolder()->StartDialog(this, bDoHideIndicators);
}

void CUIDialogWnd::HideDialog()
{
    if (GetHolder() && IsShown())
        GetHolder()->StopDialog(this);
}
