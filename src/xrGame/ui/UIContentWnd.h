#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUIStatic;
class CUI3tButton;
class CUIScrollView;
class CUIProgressBar;

// Shown when the installation is missing content it declares.
//
// Two ways out: repair, or quit. There is no "play anyway" - with content unmounted a level
// load is a missing-asset crash, so offering it would only trade a clear message for an
// unexplained one. The window lists what is actually wrong, in the same words as the log and
// the diagnostic report, so a player can paste something useful into a bug report.
//
// A completed repair ends in a relaunch rather than resuming play: the filesystem indexes its
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

    pcstr GetDebugType() override { return "CUIContentWnd"; }

private:
    void Refresh();
    void OnAction(CUIWindow*, void*);
    void OnExit(CUIWindow*, void*);

    CUIStatic* m_caption{};
    CUIStatic* m_message{};
    CUIScrollView* m_problems{};
    CUIStatic* m_problemsText{};
    CUIProgressBar* m_progress{};
    CUI3tButton* m_action{};
    CUI3tButton* m_exit{};
    bool m_repairFinished{};
};
