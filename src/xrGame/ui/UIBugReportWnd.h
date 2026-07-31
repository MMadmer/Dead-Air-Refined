#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUI3tButton;
class CUICheckButton;
class CUIEditBox;
class CUIMessageBoxEx;
class CUIStatic;

class CUIBugReportWnd final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    CUIBugReportWnd();
    ~CUIBugReportWnd() override;

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIBugReportWnd"; }

private:
    void OnSubmit(CUIWindow*, void*);
    void OnCancel(CUIWindow*, void*);
    void OnMessageOk(CUIWindow*, void*);
    void ShowResult(bool success, pcstr detail = nullptr);
    bool InputIsValid() const;
    void UpdateCounters();

    CUIEditBox* m_title{};
    CUIEditBox* m_description{};
    CUICheckButton* m_attachDump{};
    CUI3tButton* m_submit{};
    CUI3tButton* m_cancel{};
    CUIStatic* m_titleCounter{};
    CUIStatic* m_descriptionCounter{};
    CUIStatic* m_status{};
    CUIMessageBoxEx* m_messageBox{};
    bool m_closeAfterMessage{};
};
