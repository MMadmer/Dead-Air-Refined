#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUIStatic;
class CUI3tButton;
class CUIScrollView;

// Shown when the installation is missing content it declares.
//
// There is no "play anyway": with content unmounted a level load is a missing-asset crash,
// so offering it would only trade a clear message for an unexplained one. The dialog lists
// what is actually wrong - the same lines that go to the log and the diagnostic report - so
// a player can paste something useful into a bug report.
class CUIContentWnd final : public CUIDialogWnd, public CUIWndCallback
{
    typedef CUIDialogWnd inherited;

public:
    CUIContentWnd();

    bool Init();
    void Show(bool status) override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;

    pcstr GetDebugType() override { return "CUIContentWnd"; }

private:
    void OnExit(CUIWindow*, void*);

    CUIStatic* m_caption{};
    CUIStatic* m_message{};
    CUIScrollView* m_problems{};
    CUIStatic* m_problemsText{};
    CUI3tButton* m_exit{};
};
