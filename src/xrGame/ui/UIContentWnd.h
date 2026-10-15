// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "UIDialogWnd.h"
#include "ContentService.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUIStatic;
class CUI3tButton;
class CUIProgressBar;

// Shown when the installation is missing content it declares.
//
// Two ways out: download what is missing, or quit. There is no "play anyway" - with content
// unmounted a level load is a missing-asset crash, so offering it would only trade a clear
// message for an unexplained one. The window says what to do and how much it costs; which
// bundle is missing and why goes to the log and the diagnostic report, where it helps.
//
// A completed download ends in a relaunch rather than resuming play: the filesystem indexes its
// archives once, at startup, so bundles that arrive during a session are not usable in it.
class CUIContentWnd final : public CUIDialogWnd, public CUIWndCallback
{
    typedef CUIDialogWnd inherited;

public:
    CUIContentWnd();

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    // The window over a made-up repair instead of the content service's (dar_ui_dialog): for its
    // layout. Its buttons only close it.
    void Preview(const ContentService::Snapshot& snapshot);

    pcstr GetDebugType() override { return "CUIContentWnd"; }

private:
    void Refresh();
    void Refresh(const ContentService::Snapshot& snapshot);
    void OnAction(CUIWindow*, void*);
    void OnExit(CUIWindow*, void*);

    CUIStatic* m_caption{};
    CUIStatic* m_message{};
    CUIStatic* m_size{};
    CUIStatic* m_progressText{};
    CUIProgressBar* m_progress{};
    CUIStatic* m_error{};
    CUI3tButton* m_action{};
    CUI3tButton* m_exit{};
    float m_exitX{};
    bool m_repairFinished{};
    // The relaunch after a finished download could not start: the player is told to start the game.
    bool m_relaunchFailed{};
    std::optional<ContentService::Snapshot> m_preview;
};
