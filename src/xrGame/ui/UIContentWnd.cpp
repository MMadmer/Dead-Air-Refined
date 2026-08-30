#include "StdAfx.h"
#include "UIContentWnd.h"

#include "ContentService.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

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
    m_exit = UIHelper::Create3tButton(xml, "main:exit", this);

    m_exit->SetWindowName("exit");
    Register(m_exit);
    AddCallback(m_exit, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIContentWnd::OnExit));
    return true;
}

void CUIContentWnd::Show(bool status)
{
    inherited::Show(status);
    if (!status)
        return;

    const ContentService::Snapshot snapshot = ContentService::GetSnapshot();

    // Recovery means the installation cannot even say what content it should have, which is a
    // different repair from "three bundles are missing" - name it separately so a bug report
    // carries the distinction.
    const pcstr message = snapshot.state == ContentService::State::Recovery ? "st_content_message_recovery"
                                                                           : "st_content_message";
    m_message->SetText(StringTable().translate(message).c_str());

    string4096 text{};
    for (const auto& problem : snapshot.problems)
    {
        if (text[0])
            xr_strcat(text, sizeof(text), "\n");
        xr_strcat(text, sizeof(text), problem.c_str());
    }
    m_problemsText->SetText(text);
    m_problemsText->AdjustHeightToText();
    m_problems->ScrollToBegin();
}

void CUIContentWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIContentWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    // Escape does not dismiss this one. The dialog has exactly one way out, because there is
    // exactly one thing a player can usefully do with a broken installation.
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik))
        return true;
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIContentWnd::OnExit(CUIWindow*, void*) { Console->Execute("quit"); }
