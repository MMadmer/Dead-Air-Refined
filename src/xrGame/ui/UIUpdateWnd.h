#pragma once

#include "UIDialogWnd.h"
#include "UpdateService.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUI3tButton;
class CUIProgressBar;
class CUIStatic;

class CUIUpdateWnd final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    CUIUpdateWnd();

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIUpdateWnd"; }

private:
    void OnAction(CUIWindow*, void*);
    void OnCancel(CUIWindow*, void*);
    void Refresh(const UpdateService::Snapshot& snapshot);
    bool CanDismiss(UpdateService::State state) const;

    CUIStatic* m_caption{};
    CUIStatic* m_message{};
    CUIStatic* m_size{};
    CUIStatic* m_progressText{};
    CUIStatic* m_error{};
    CUIProgressBar* m_progress{};
    CUI3tButton* m_action{};
    CUI3tButton* m_cancel{};
    float m_actionTwoButtonX{};
    UpdateService::State m_lastState{UpdateService::State::Idle};
    xr_string m_lastVersion;
};
