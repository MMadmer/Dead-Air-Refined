// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "UIDialogWnd.h"
#include "UpdateService.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUIStatic;
class CUI3tButton;
class CUICheckButton;
class CUIScrollView;

// Announcement of a release from a HIGHER MAJOR line. Deliberately not the update dialog: a
// new major is not compatible with the installed saves and mods, so nothing is downloaded or
// replaced in place - the player is pointed at the release page and asked to install it into
// a separate folder. Shown on every launch whose check succeeds, until either the checkbox
// here or the Game options switch turns update checking off.
class CUIMajorUpdateWnd final : public CUIDialogWnd, public CUIWndCallback
{
    typedef CUIDialogWnd inherited;

public:
    CUIMajorUpdateWnd();

    bool Init();
    void Show(bool status) override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;

    pcstr GetDebugType() override { return "CUIMajorUpdateWnd"; }

private:
    void OnOpenPage(CUIWindow*, void*);
    void OnClose(CUIWindow*, void*);
    void OnDisableChecks(CUIWindow*, void*);

    CUIStatic* m_caption{};
    CUIStatic* m_message{};
    CUIStatic* m_folderHint{};
    // Optional release headline above the change list; hidden when the notes carry none.
    CUIStatic* m_theme{};
    CUIScrollView* m_changes{};
    CUIStatic* m_changesText{};
    CUIStatic* m_disableLabel{};
    CUICheckButton* m_disable{};
    CUI3tButton* m_action{};
    CUI3tButton* m_cancel{};
};
