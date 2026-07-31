#include "StdAfx.h"
#include "UIBugReportWnd.h"
#include "BugReportConfig.h"
#include "BugReportService.h"
#include "UIHelper.h"
#include "UIMessageBoxEx.h"
#include "UIXmlInit.h"
#include "xrCore/Debug/CrashReport.h"
#include "xrEngine/xr_input.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Buttons/UICheckButton.h"
#include "xrUICore/EditBox/UIEditBox.h"
#include "xrUICore/Static/UIStatic.h"

namespace
{
size_t trimmed_length(pcstr value)
{
    if (!value)
        return 0;

    const unsigned char* begin = reinterpret_cast<const unsigned char*>(value);
    while (*begin && std::isspace(*begin))
        ++begin;

    const unsigned char* end = begin + xr_strlen(reinterpret_cast<pcstr>(begin));
    while (end != begin && std::isspace(end[-1]))
        --end;
    return static_cast<size_t>(end - begin);
}
}

CUIBugReportWnd::CUIBugReportWnd() : CUIDialogWnd(CUIBugReportWnd::GetDebugType())
{
    m_bWorkInPause = true;
}

CUIBugReportWnd::~CUIBugReportWnd()
{
    if (m_messageBox && m_messageBox->IsShown())
        m_messageBox->HideDialog();
    xr_delete(m_messageBox);
}

bool CUIBugReportWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_bug_report.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    UIHelper::CreateStatic(xml, "main:caption", this);
    UIHelper::CreateStatic(xml, "main:title_label", this);
    UIHelper::CreateStatic(xml, "main:description_label", this);
    UIHelper::CreateStatic(xml, "main:attach_label", this);

    m_title = UIHelper::CreateEditBox(xml, "main:title", this);
    m_description = UIHelper::CreateEditBox(xml, "main:description", this);
    m_attachDump = UIHelper::CreateCheck(xml, "main:attach_dump", this);
    m_submit = UIHelper::Create3tButton(xml, "main:submit", this);
    m_cancel = UIHelper::Create3tButton(xml, "main:cancel", this);
    m_titleCounter = UIHelper::CreateStatic(xml, "main:title_counter", this);
    m_descriptionCounter = UIHelper::CreateStatic(xml, "main:description_counter", this);
    m_status = UIHelper::CreateStatic(xml, "main:status", this);

    m_submit->SetWindowName("submit");
    m_cancel->SetWindowName("cancel");
    Register(m_submit);
    Register(m_cancel);
    AddCallback(m_submit, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIBugReportWnd::OnSubmit));
    AddCallback(m_cancel, BUTTON_CLICKED, CUIWndCallback::void_function(this, &CUIBugReportWnd::OnCancel));

    m_messageBox = xr_new<CUIMessageBoxEx>();
    m_messageBox->func_on_ok = CUIWndCallback::void_function(this, &CUIBugReportWnd::OnMessageOk);
    m_title->SetNextFocusCapturer(m_description);
    m_description->SetNextFocusCapturer(m_title);
    return true;
}

void CUIBugReportWnd::Show(bool status)
{
    if (status && BugReportService::GetState() != BugReportService::State::Sending)
    {
        BugReportService::Reset();
        m_title->ClearText();
        m_description->ClearText();
        m_attachDump->SetCheck(true);
        m_status->SetText("");
        m_closeAfterMessage = false;
        m_title->CaptureFocus(true);
    }
    inherited::Show(status);
}

void CUIBugReportWnd::Update()
{
    inherited::Update();
    UpdateCounters();

    const BugReportService::State state = BugReportService::GetState();
    const bool sending = state == BugReportService::State::Sending;
    m_submit->Enable(!sending && InputIsValid());
    m_cancel->Enable(!sending);
    m_title->Enable(!sending);
    m_description->Enable(!sending);
    m_attachDump->Enable(!sending);
    m_status->SetText(sending ? StringTable().translate("st_bug_report_sending").c_str() : "");

    if (state == BugReportService::State::Succeeded)
    {
        BugReportService::Reset();
        ShowResult(true);
    }
    else if (state == BugReportService::State::Failed)
    {
        const xr_string detail = BugReportService::GetMessage();
        BugReportService::Reset();
        ShowResult(false, detail.c_str());
    }
}

void CUIBugReportWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIBugReportWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik) &&
        BugReportService::GetState() != BugReportService::State::Sending)
    {
        HideDialog();
        return true;
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIBugReportWnd::OnSubmit(CUIWindow*, void*)
{
    if (!InputIsValid() || BugReportService::GetState() == BugReportService::State::Sending)
        return;

    string_path attachment{};
    if (m_attachDump->GetCheck())
    {
        m_status->SetText(StringTable().translate("st_bug_report_collecting").c_str());
        if (!CrashReporter::WriteSession())
        {
            ShowResult(false, "Diagnostic report creation failed");
            return;
        }
        xr_strcpy(attachment, CrashReporter::LatestReportPath());
    }

    if (!BugReportService::Submit(m_title->GetText(), m_description->GetText(), attachment))
        ShowResult(false, "The upload could not be started");
}

void CUIBugReportWnd::OnCancel(CUIWindow*, void*)
{
    if (BugReportService::GetState() != BugReportService::State::Sending)
        HideDialog();
}

void CUIBugReportWnd::OnMessageOk(CUIWindow*, void*)
{
    if (m_closeAfterMessage)
        HideDialog();
    m_closeAfterMessage = false;
}

void CUIBugReportWnd::ShowResult(bool success, pcstr detail)
{
    m_closeAfterMessage = success;
    if (!m_messageBox->InitMessageBox("message_box_ok"))
        return;

    xr_string text = StringTable().translate(success ? "st_bug_report_success" : "st_bug_report_failed").c_str();
    if (!success && detail && *detail)
    {
        text.append("\n");
        text.append(detail);
    }
    m_messageBox->SetText(text.c_str());
    m_messageBox->ShowDialog(true);
}

bool CUIBugReportWnd::InputIsValid() const
{
    const size_t titleLength = trimmed_length(m_title->GetText());
    const size_t descriptionLength = trimmed_length(m_description->GetText());
    return titleLength >= 5 && titleLength <= BugReportConfig::TitleMaximum &&
        descriptionLength >= 20 && descriptionLength <= BugReportConfig::DescriptionMaximum;
}

void CUIBugReportWnd::UpdateCounters()
{
    string64 counter{};
    xr_sprintf(counter, sizeof(counter), "%zu/%u", xr_strlen(m_title->GetText()), BugReportConfig::TitleMaximum);
    m_titleCounter->SetText(counter);
    xr_sprintf(counter, sizeof(counter), "%zu/%u", xr_strlen(m_description->GetText()), BugReportConfig::DescriptionMaximum);
    m_descriptionCounter->SetText(counter);
}
