#include "StdAfx.h"
#include "UIUpdateWnd.h"

#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrCore/ProductVersion.h"
#include "xrEngine/xr_input.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
double mebibytes(u64 bytes)
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

bool is_russian_language()
{
    const shared_str language = StringTable().GetCurrentLanguage();
    return language && (xr_strcmp(language.c_str(), "rus") == 0 || xr_strcmp(language.c_str(), "ru") == 0);
}
}

CUIUpdateWnd::CUIUpdateWnd() : CUIDialogWnd(CUIUpdateWnd::GetDebugType())
{
    m_bWorkInPause = true;
}

bool CUIUpdateWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_update.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    m_caption = UIHelper::CreateStatic(xml, "main:caption", this);
    m_message = UIHelper::CreateStatic(xml, "main:message", this);
    m_size = UIHelper::CreateStatic(xml, "main:size", this);
    m_changes = UIHelper::CreateScrollView(xml, "main:changes", this);
    m_changesText = xr_new<CUIStatic>("Update changelog");
    CUIXmlInit::InitStatic(xml, "main:changes_text", 0, m_changesText);
    m_changesText->SetWidth(m_changes->GetDesiredChildWidth());
    m_changes->AddWindow(m_changesText, true);
    m_progressText = UIHelper::CreateStatic(xml, "main:progress_text", this);
    m_error = UIHelper::CreateStatic(xml, "main:error", this);
    m_progress = UIHelper::CreateProgressBar(xml, "main:progress", this);
    m_action = UIHelper::Create3tButton(xml, "main:action", this);
    m_cancel = UIHelper::Create3tButton(xml, "main:cancel", this);
    m_actionTwoButtonX = m_action->GetWndPos().x;

    m_action->SetWindowName("action");
    m_cancel->SetWindowName("cancel");
    Register(m_action);
    Register(m_cancel);
    AddCallback(m_action, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIUpdateWnd::OnAction));
    AddCallback(m_cancel, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIUpdateWnd::OnCancel));
    return true;
}

void CUIUpdateWnd::Show(bool status)
{
    inherited::Show(status);
    if (status)
    {
        m_lastState = UpdateService::State::Idle;
        m_lastVersion.clear();
        m_lastChanges.clear();
        Refresh(UpdateService::GetSnapshot());
    }
}

void CUIUpdateWnd::Update()
{
    inherited::Update();
    Refresh(UpdateService::GetSnapshot());
}

void CUIUpdateWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIUpdateWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    const UpdateService::State state = UpdateService::GetSnapshot().state;
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik) && CanDismiss(state))
    {
        OnCancel(nullptr, nullptr);
        return true;
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIUpdateWnd::OnAction(CUIWindow*, void*)
{
    const UpdateService::State state = UpdateService::GetSnapshot().state;
    if (state == UpdateService::State::Available || state == UpdateService::State::DownloadFailed)
        UpdateService::StartDownload();
    else if (state == UpdateService::State::Ready || state == UpdateService::State::ApplyFailed)
        UpdateService::RestartAndApply();
}

void CUIUpdateWnd::OnCancel(CUIWindow*, void*)
{
    const UpdateService::State state = UpdateService::GetSnapshot().state;
    if (!CanDismiss(state))
        return;
    UpdateService::Dismiss();
    HideDialog();
}

void CUIUpdateWnd::Refresh(const UpdateService::Snapshot& snapshot)
{
    const bool available = snapshot.state == UpdateService::State::Available;
    const bool downloading = snapshot.state == UpdateService::State::Downloading;
    const bool ready = snapshot.state == UpdateService::State::Ready;
    const bool failed = snapshot.state == UpdateService::State::DownloadFailed ||
        snapshot.state == UpdateService::State::ApplyFailed;
    const xr_string& changes = is_russian_language() ? snapshot.changesRu : snapshot.changesEn;

    if (snapshot.state != m_lastState || snapshot.version != m_lastVersion || changes != m_lastChanges)
    {
        m_lastState = snapshot.state;
        m_lastVersion = snapshot.version;
        m_lastChanges = changes;

        string512 text{};
        if (available)
        {
            m_caption->SetText(StringTable().translate("st_update_available_caption").c_str());
            xr_sprintf(text, sizeof(text), StringTable().translate("st_update_available_message").c_str(),
                DeadAirRefined::Version, snapshot.version.c_str());
            m_message->SetText(text);
            xr_sprintf(text, sizeof(text), StringTable().translate("st_update_size").c_str(),
                mebibytes(snapshot.totalBytes));
            m_size->SetText(text);
            m_action->SetText(StringTable().translate("st_update_download").c_str());
            m_changesText->SetText(changes.c_str());
            m_changesText->AdjustHeightToText();
            m_changes->ScrollToBegin();
        }
        else
        {
            m_caption->SetText(StringTable().translate("st_update_progress_caption").c_str());
            m_message->SetText(ready ? StringTable().translate("st_update_ready").c_str() : "");
            m_action->SetText(failed ? StringTable().translate("st_update_retry").c_str() :
                StringTable().translate("st_update_restart").c_str());
        }

        m_error->SetText(failed ? StringTable().translate("st_update_failed").c_str() : "");
        m_size->Show(available);
        m_changes->Show(available && !changes.empty());
        m_progressText->Show(downloading || ready);
        m_progress->Show(downloading || ready);
        m_error->Show(failed);
        m_action->Show(available || ready || failed);
        m_action->Enable(available || ready || failed);
        m_cancel->Show(CanDismiss(snapshot.state));
        m_cancel->Enable(CanDismiss(snapshot.state));

        if (m_action->IsShown())
        {
            Fvector2 position = m_action->GetWndPos();
            position.x = m_cancel->IsShown() ? m_actionTwoButtonX : (GetWidth() - m_action->GetWidth()) * 0.5f;
            m_action->SetWndPos(position);
        }
    }

    if (downloading || ready)
    {
        string128 text{};
        xr_sprintf(text, sizeof(text), StringTable().translate("st_update_progress").c_str(),
            mebibytes(snapshot.downloadedBytes), mebibytes(snapshot.totalBytes));
        m_progressText->SetText(text);
        const float progress = snapshot.totalBytes ?
            100.f * static_cast<float>(snapshot.downloadedBytes) / static_cast<float>(snapshot.totalBytes) : 0.f;
        m_progress->ForceSetProgressPos(std::clamp(progress, 0.f, 100.f));
        m_action->Enable(ready);
    }
}

bool CUIUpdateWnd::CanDismiss(UpdateService::State state) const
{
    return state == UpdateService::State::Available || state == UpdateService::State::DownloadFailed ||
        state == UpdateService::State::ApplyFailed;
}
