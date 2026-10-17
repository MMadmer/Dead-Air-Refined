// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrUICore/XML/xrUIXmlParser.h"

class CUI3tButton;
class CUIComboBox;
class CUIColorPicker;

// The interface colours (Options -> Game -> Interface): the two seeds of the game windows' palette - main and
// accent, ui_theme.xml - set with a picker after Unreal's while a template of a game window shows them, the
// templates chosen from a drop-down (ui_interface_colors*.xml). Apply keeps them (ui_color_main, ui_color_accent)
// and rebuilds the game's windows; Cancel and Escape put back the ones the window opened with. The window and the
// menus keep their own palette.
class CUIInterfaceColorsWnd final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    CUIInterfaceColorsWnd();
    ~CUIInterfaceColorsWnd() override;

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIInterfaceColorsWnd"; }

private:
    // the seeds the window opened with, back in the theme
    void Restore();
    void SelectTarget(u8 target);
    void OnPicked(u32 color);
    void BuildPreview();
    void Close(bool apply);
    void OnTemplate(CUIWindow*, void*);
    void OnTarget(CUIWindow* window, void*);
    void OnReset(CUIWindow*, void*);
    void OnCancel(CUIWindow*, void*);
    void OnOk(CUIWindow*, void*);

    CUIXml m_xml;
    CUIComboBox* m_template{};
    CUIWindow* m_preview{};
    CUIWindow* m_shown{}; // the template on show
    CUI3tButton* m_targets[2]{};
    CUIWindow* m_swatches[2]{};
    CUIColorPicker* m_picker{};
    CUI3tButton* m_ok{};
    CUI3tButton* m_cancel{};
    CUI3tButton* m_reset{};

    u32 m_seed[2]{};
    u32 m_before[2]{};
    bool m_chosenBefore[2]{};
    // colours are being tried: they leave with the window unless applied
    bool m_trying{};
    u8 m_target{};
    bool m_previewDirty{};
    u32 m_previewBuiltAt{};
};
