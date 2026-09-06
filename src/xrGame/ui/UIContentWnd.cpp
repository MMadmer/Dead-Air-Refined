#include "StdAfx.h"
#include "UIContentWnd.h"

#include "ContentService.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

#include <algorithm>

namespace
{
float mebibytes(u64 bytes) { return static_cast<float>(bytes) / (1024.f * 1024.f); }
}

CUIContentWnd::CUIContentWnd() : CUIDialogWnd(CUIContentWnd::GetDebugType()) { m_bWorkInPause = true; }

bool CUIContentWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_content.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    m_caption = UIHelper::CreateStatic(xml, "main:caption", this);
    m_message = UIHelper::CreateStatic(xml, "main:message", this);
    m_problems = UIHelper::CreateScrollView(xml, "main:problems", this);
    m_problemsText = xr_new<CUIStatic>("Content problems");
    CUIXmlInit::InitStatic(xml, "main:problems_text", 0, m_problemsText);
    m_problemsText->SetWidth(m_problems->GetDesiredChildWidth());
    m_problems->AddWindow(m_problemsText, true);
    m_progressText = UIHelper::CreateStatic(xml, "main:progress_text", this);
    m_progress = UIHelper::CreateProgressBar(xml, "main:progress", this);
    m_action = UIHelper::Create3tButton(xml, "main:action", this);
    m_exit = UIHelper::Create3tButton(xml, "main:exit", this);

    m_action->SetWindowName("action");
    m_exit->SetWindowName("exit");
    Register(m_action);
    Register(m_exit);
    AddCallback(m_action, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIContentWnd::OnAction));
    AddCallback(m_exit, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIContentWnd::OnExit));
    return true;
}

void CUIContentWnd::Show(bool status)
{
    inherited::Show(status);
    if (status)
        Refresh();
}

void CUIContentWnd::Update()
{
    inherited::Update();

    // A repair moves; the dialog has to move with it, or the player is left staring at a frozen
    // window through a download that can take an hour.
    if (ContentService::RepairRunning() || m_repairFinished != (ContentService::GetSnapshot().state ==
        ContentService::State::Repaired))
    {
        Refresh();
    }
}

void CUIContentWnd::Refresh()
{
    const ContentService::Snapshot snapshot = ContentService::GetSnapshot();
    m_repairFinished = snapshot.state == ContentService::State::Repaired;

    pcstr message = "st_content_message";
    switch (snapshot.state)
    {
    // Recovery means the installation cannot even say what content it should have, which is a
    // different repair from "three bundles are missing" - name it separately so a bug report
    // carries the distinction.
    case ContentService::State::Recovery: message = "st_content_message_recovery"; break;
    case ContentService::State::Repairing: message = "st_content_message_repairing"; break;
    case ContentService::State::Repaired: message = "st_content_message_repaired"; break;
    default: break;
    }
    m_message->SetText(StringTable().translate(message).c_str());

    // The same progress line and bar as the update window, so a download reads the same
    // wherever it runs. Before the first byte and after a failure the line carries the stage
    // name or the reason instead, which is how a failed repair stays explained on screen.
    const bool repairing = snapshot.state == ContentService::State::Repairing;
    m_progress->Show(repairing);
    m_progressText->Show(repairing || !snapshot.activity.empty());
    if (repairing && snapshot.repairTotal)
    {
        string128 line{};
        xr_sprintf(line, sizeof(line), StringTable().translate("st_update_progress").c_str(),
            mebibytes(snapshot.repairDone), mebibytes(snapshot.repairTotal));
        m_progressText->SetText(line);
        m_progress->ForceSetProgressPos(std::clamp(
            100.f * static_cast<float>(snapshot.repairDone) / static_cast<float>(snapshot.repairTotal), 0.f, 100.f));
    }
    else
    {
        m_progressText->SetText(snapshot.activity.c_str());
        m_progress->ForceSetProgressPos(0.f);
    }

    // The text control takes its line breaks as the two-character sequence, not the control
    // character: joined with a real newline the list came out as one run-on line.
    string4096 text{};
    for (const auto& problem : snapshot.problems)
    {
        if (text[0])
            xr_strcat(text, sizeof(text), "\\n");
        xr_strcat(text, sizeof(text), problem.c_str());
    }
    m_problemsText->SetText(text);
    m_problemsText->AdjustHeightToText();
    m_problems->ScrollToBegin();

    // One primary action, and what it is depends on where the repair got to. Recovery has none:
    // without a manifest there is nothing to repair against, and offering a button that cannot
    // work would be worse than not offering one.
    pcstr caption = "st_content_repair";
    bool enabled = true;
    if (snapshot.state == ContentService::State::Repaired)
        caption = "st_content_restart";
    else if (repairing)
    {
        caption = "st_content_repairing";
        enabled = false;
    }
    else if (snapshot.state == ContentService::State::Recovery)
    {
        enabled = false;
    }

    m_action->SetText(StringTable().translate(caption).c_str());
    m_action->Enable(enabled);
}

void CUIContentWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIContentWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    // Escape does not dismiss this one. The dialog has exactly two ways out and both of them
    // leave the installation in a state the game can explain.
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik))
        return true;
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIContentWnd::OnAction(CUIWindow*, void*)
{
    if (m_repairFinished)
    {
        // The filesystem indexed its archives at startup, so bundles that arrived since are not
        // usable in this session. The relaunch is not a courtesy - it is the only way to use
        // what was just installed.
        if (ContentService::Relaunch())
            Console->Execute("quit");
        return;
    }

    if (!ContentService::RepairRunning())
    {
        ContentService::StartRepair();
        Refresh();
    }
}

void CUIContentWnd::OnExit(CUIWindow*, void*) { Console->Execute("quit"); }
