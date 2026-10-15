// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIContentWnd.h"

#include "ContentService.h"
#include "UIHelper.h"
#include "UIXmlInit.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/ProgressBar/UIProgressBar.h"
#include "xrUICore/Static/UIStatic.h"

#include <algorithm>

namespace
{
float mebibytes(u64 bytes) { return static_cast<float>(bytes) / (1024.f * 1024.f); }
float gibibytes(u64 bytes) { return static_cast<float>(bytes) / (1024.f * 1024.f * 1024.f); }
}

CUIContentWnd::CUIContentWnd() : CUIDialogWnd(CUIContentWnd::GetDebugType()) { m_bWorkInPause = true; }

bool CUIContentWnd::Init()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_content.xml", false))
        return false;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    // the layout's own plates, shades and separators (auto_static), under everything else
    CUIXmlInit::InitAutoStaticGroup(xml, "main", 0, this);
    UIHelper::CreateStatic(xml, "main:background", this);
    m_caption = UIHelper::CreateStatic(xml, "main:caption", this);
    m_message = UIHelper::CreateStatic(xml, "main:message", this);
    m_size = UIHelper::CreateStatic(xml, "main:size", this);
    m_progressText = UIHelper::CreateStatic(xml, "main:progress_text", this);
    m_progress = UIHelper::CreateProgressBar(xml, "main:progress", this);
    m_error = UIHelper::CreateStatic(xml, "main:error", this);
    m_action = UIHelper::Create3tButton(xml, "main:action", this);
    m_exit = UIHelper::Create3tButton(xml, "main:exit", this);
    m_exitX = m_exit->GetWndPos().x;

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
    else
        m_preview.reset();
}

void CUIContentWnd::Preview(const ContentService::Snapshot& snapshot)
{
    m_preview = snapshot;
    Refresh(snapshot);
}

void CUIContentWnd::Update()
{
    inherited::Update();
    // A download moves; the window has to move with it, or the player is left staring at a frozen
    // window through a download that can take an hour.
    if (!m_preview)
        Refresh();
}

void CUIContentWnd::Refresh() { Refresh(m_preview ? *m_preview : ContentService::GetSnapshot()); }

void CUIContentWnd::Refresh(const ContentService::Snapshot& snapshot)
{
    using ContentService::Failure;
    using ContentService::Stage;
    using ContentService::State;

    m_repairFinished = snapshot.state == State::Repaired;
    const bool repairing = snapshot.state == State::Repairing;
    // A download cannot help an installation without a manifest, nor one whose archives the
    // engine reads from somewhere else: both are a reinstall.
    const bool recovery = snapshot.state == State::Recovery || (snapshot.state == State::Incomplete && !snapshot.repairable);
    const bool checking = !repairing && !m_repairFinished && !recovery && snapshot.checking;
    const bool failed = snapshot.state == State::Incomplete && snapshot.failure != Failure::None && !checking && !recovery;

    pcstr caption = "st_content_caption";
    pcstr message = "st_content_message";
    if (recovery)
    {
        caption = "st_content_caption_recovery";
        message = "st_content_message_recovery";
    }
    else if (repairing)
    {
        caption = "st_content_caption_repairing";
        message = "st_content_message_repairing";
    }
    else if (m_repairFinished)
    {
        caption = "st_content_caption_repaired";
        message = "st_content_message_repaired";
    }
    else if (checking)
    {
        caption = "st_content_caption_checking";
        message = "st_content_message_checking";
    }
    m_caption->SetText(StringTable().translate(caption).c_str());
    m_message->SetText(StringTable().translate(message).c_str());

    // The most the download can cost: what the missing bundles weigh whole. Rebuilt bundles cost
    // less, which the progress line shows once the download knows it.
    // Not beside a disk-space error: that one names what the download and the install need
    // together, and two different sizes one above the other read as a contradiction.
    const bool offer = !repairing && !m_repairFinished && !recovery && !checking && snapshot.missingBytes &&
        !(failed && snapshot.failure == Failure::Space);
    m_size->Show(offer);
    if (offer)
    {
        string128 line{};
        xr_sprintf(line, sizeof(line), StringTable().translate("st_content_size").c_str(), mebibytes(snapshot.missingBytes));
        m_size->SetText(line);
    }

    // The same progress line and bar as the update window, so a download reads the same wherever
    // it runs; around the download itself the line names the stage instead.
    string128 progress{};
    float position = 0.f;
    bool bar = false;
    if (repairing)
    {
        switch (snapshot.stage)
        {
        case Stage::Planning: xr_strcpy(progress, StringTable().translate("st_content_stage_plan").c_str()); break;
        case Stage::Downloading:
            xr_sprintf(progress, sizeof(progress), StringTable().translate("st_update_progress").c_str(),
                mebibytes(snapshot.repairDone), mebibytes(std::max(snapshot.repairTotal, snapshot.repairDone)));
            position = snapshot.repairTotal ? 100.f * static_cast<float>(snapshot.repairDone) / static_cast<float>(snapshot.repairTotal) : 0.f;
            bar = true;
            break;
        case Stage::Installing:
        case Stage::Done:
            xr_strcpy(progress, StringTable().translate("st_content_stage_install").c_str());
            position = 100.f;
            bar = true;
            break;
        default: xr_strcpy(progress, StringTable().translate("st_content_stage_check").c_str()); break;
        }
    }
    m_progressText->Show(repairing);
    m_progressText->SetText(progress);
    m_progress->Show(bar);
    m_progress->ForceSetProgressPos(std::clamp(position, 0.f, 100.f));

    // What went wrong, in what the player can do about it. The reason itself is in the log.
    m_error->Show((failed && !repairing) || (m_repairFinished && m_relaunchFailed));
    if (m_repairFinished && m_relaunchFailed)
        m_error->SetText(StringTable().translate("st_content_fail_restart").c_str());
    else if (failed && !repairing)
    {
        string256 line{};
        switch (snapshot.failure)
        {
        case Failure::Space:
            xr_sprintf(line, sizeof(line), StringTable().translate("st_content_fail_space").c_str(),
                gibibytes(snapshot.spaceRequired), gibibytes(snapshot.spaceFree));
            break;
        case Failure::Check: xr_strcpy(line, StringTable().translate("st_content_fail_check").c_str()); break;
        case Failure::Install: xr_strcpy(line, StringTable().translate("st_content_fail_install").c_str()); break;
        default: xr_strcpy(line, StringTable().translate("st_content_fail_download").c_str()); break;
        }
        m_error->SetText(line);
    }

    // One primary action, and what it is depends on where the repair got to. Recovery has none:
    // without a manifest there is nothing to download against, and a button that cannot work is
    // worse than none. Exit stays throughout - a download left halfway resumes next time.
    const bool primary = !repairing && !recovery && !checking;
    pcstr action = "st_content_repair";
    if (m_repairFinished)
        action = "st_content_restart";
    else if (failed)
        action = "st_content_retry";
    m_action->SetText(StringTable().translate(action).c_str());
    m_action->Show(primary);
    m_action->Enable(primary);

    // Exit stays except while the files are being moved into place: quitting then would only make
    // the game wait for the move with no window on screen.
    const bool installing = repairing && (snapshot.stage == Stage::Installing || snapshot.stage == Stage::Done);
    m_exit->Show(!installing);
    m_exit->Enable(!installing);
    Fvector2 position2 = m_exit->GetWndPos();
    position2.x = primary ? m_exitX : (GetWidth() - m_exit->GetWidth()) * 0.5f;
    m_exit->SetWndPos(position2);
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
    {
        if (m_preview)
            HideDialog();
        return true;
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIContentWnd::OnAction(CUIWindow*, void*)
{
    if (m_preview)
    {
        HideDialog();
        return;
    }
    if (m_repairFinished)
    {
        // The filesystem indexed its archives at startup, so bundles that arrived since are not
        // usable in this session. The relaunch is not a courtesy - it is the only way to use
        // what was just installed.
        if (ContentService::Relaunch())
            Console->Execute("quit");
        else
            m_relaunchFailed = true;
        return;
    }

    if (!ContentService::RepairRunning())
    {
        ContentService::StartRepair();
        Refresh();
    }
}

void CUIContentWnd::OnExit(CUIWindow*, void*)
{
    // a preview only closes
    if (m_preview)
    {
        HideDialog();
        return;
    }
    Console->Execute("quit");
}
