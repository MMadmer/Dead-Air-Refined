// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIMajorUpdateWnd.h"

#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrCore/ProductVersion.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Buttons/UICheckButton.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
bool is_russian_language()
{
    const shared_str language = StringTable().GetCurrentLanguage();
    return language && (xr_strcmp(language.c_str(), "rus") == 0 || xr_strcmp(language.c_str(), "ru") == 0);
}
}

CUIMajorUpdateWnd::CUIMajorUpdateWnd() : CUIDialogWnd(CUIMajorUpdateWnd::GetDebugType())
{
    m_bWorkInPause = true;
}

bool CUIMajorUpdateWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_update_major.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    m_caption = UIHelper::CreateStatic(xml, "main:caption", this);
    m_message = UIHelper::CreateStatic(xml, "main:message", this);
    m_folderHint = UIHelper::CreateStatic(xml, "main:folder_hint", this);
    m_theme = UIHelper::CreateStatic(xml, "main:theme", this);
    m_changes = UIHelper::CreateScrollView(xml, "main:changes", this);
    m_changesText = xr_new<CUIStatic>("Major release changelog");
    CUIXmlInit::InitStatic(xml, "main:changes_text", 0, m_changesText);
    m_changesText->SetWidth(m_changes->GetDesiredChildWidth());
    m_changes->AddWindow(m_changesText, true);
    m_disable = UIHelper::CreateCheck(xml, "main:disable_checks", this);
    m_disableLabel = UIHelper::CreateStatic(xml, "main:disable_checks_label", this);
    m_action = UIHelper::Create3tButton(xml, "main:action", this);
    m_cancel = UIHelper::Create3tButton(xml, "main:cancel", this);

    m_action->SetWindowName("action");
    m_cancel->SetWindowName("cancel");
    m_disable->SetWindowName("disable_checks");
    Register(m_action);
    Register(m_cancel);
    Register(m_disable);
    AddCallback(m_action, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIMajorUpdateWnd::OnOpenPage));
    AddCallback(m_cancel, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIMajorUpdateWnd::OnClose));
    AddCallback(m_disable, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIMajorUpdateWnd::OnDisableChecks));
    return true;
}

void CUIMajorUpdateWnd::Show(bool status)
{
    inherited::Show(status);
    if (!status)
        return;

    const UpdateService::Snapshot snapshot = UpdateService::GetSnapshot();
    const bool russian = is_russian_language();
    const xr_string& changes = russian ? snapshot.majorChangesRu : snapshot.majorChangesEn;
    const xr_string& theme = russian ? snapshot.majorThemeRu : snapshot.majorThemeEn;

    string512 text{};
    xr_sprintf(text, sizeof(text), StringTable().translate("st_update_major_message").c_str(),
        DeadAirRefined::Version, snapshot.majorVersion.c_str());
    m_message->SetText(text);
    m_theme->SetText(theme.c_str());
    m_theme->Show(!theme.empty());
    m_changesText->SetText(changes.c_str());
    m_changesText->AdjustHeightToText();
    m_changes->ScrollToBegin();
    m_changes->Show(!changes.empty());
    // The switch reflects the live setting rather than the last click: the same option lives
    // in the Game tab and may have been changed there.
    m_disable->SetCheck(!UpdateService::MajorNoticeEnabled());
}

void CUIMajorUpdateWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIMajorUpdateWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik))
    {
        OnClose(nullptr, nullptr);
        return true;
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIMajorUpdateWnd::OnOpenPage(CUIWindow*, void*) { UpdateService::OpenMajorReleasePage(); }

void CUIMajorUpdateWnd::OnClose(CUIWindow*, void*)
{
    UpdateService::DismissMajor();
    HideDialog();
}

void CUIMajorUpdateWnd::OnDisableChecks(CUIWindow*, void*)
{
    UpdateService::SetMajorNoticeEnabled(!m_disable->GetCheck());
}
