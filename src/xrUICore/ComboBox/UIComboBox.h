#pragma once
#include "xrUICore/ListBox/UIListBox.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/Options/UIOptionsItem.h"
#include "xrUICore/Static/UIShapeStyle.h"

class CUIListBoxItem;
class CGameFont;

// A drop-down: a field that shows the choice and a list that opens over everything under it - over
// the window's other controls, past the edge of a scrolling page, upwards when there is no room below.
// The list is drawn by the cursor (CUICursor::OnRender), before the hints; one is open at a time.
//
// The list opens and closes on a click of the field; a click on a row chooses it, a click anywhere
// else closes the list and does nothing more. The wheel scrolls the list, the arrows move over its
// rows, Enter chooses, Escape closes. Closed, a focused drop-down takes the left and right arrows as
// the previous and the next choice, and shows its hint (SetHint) under a resting cursor.
// A choice made any way is the same: the option's value changes (an option applied on change is
// applied) and the owner hears LIST_ITEM_SELECT when it changed.
//
// Its looks are theme styles (ui_theme.xml): the field "combo" (normal, highlight under the cursor,
// pushed while open, disabled), the list "combo_list", a row "combo_row" (highlight under the cursor,
// checked for the current choice) and the arrow "combo_arrow"; a layout may name others - shape= on
// the drop-down's node, <list shape>, <row shape>, <arrow shape>.
class XRUICORE_API CUIComboBox final : public CUIWindow, public CUIOptionsItem
{
    friend class CUIXmlInitBase;

public:
    CUIComboBox();
    ~CUIComboBox() override;

    // CUIOptionsItem
    void SetCurrentOptValue() override; // opt->current
    void SaveBackUpOptValue() override; // current->backup
    void SaveOptValue() override; // current->opt
    void UndoOptValue() override; // backup->current
    bool IsChangedOptValue() const override; // backup!=current

    LPCSTR GetText() const;
    LPCSTR GetTextOf(int index);
    void SetText(LPCSTR text);

    // the rows the list shows before it scrolls, at least (it shows up to ten, or what fits the screen)
    void SetListLength(int length);
    void SetVertScroll(bool) {} // the list scrolls when it has more rows than it shows
    CUIListBoxItem* AddItem_(LPCSTR str, int _data);
    void InitComboBox(Fvector2 pos, float width);
    void SetItemIDX(int idx);
    void SetItemToken(int tok);
    u32 GetSelectedIDX();
    void SetSelectedIDX(u32 idx);

    bool SetNextItemSelected(bool next, bool loop);

    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = nullptr) override;
    void OnFocusLost() override;
    void OnFocusReceive() override;
    int CurrentID() { return m_itoken_id; }
    void disable_id(int id);
    void enable_id(int id);

    void ClearList();
    u32 GetSize() const;

    void SetTextColor(u32 color) { m_textColor[0] = color; }
    void SetTextColorD(u32 color) { m_textColor[1] = color; }
    void SetFont(CGameFont* font);
    // a string id, the hint of the closed field
    void SetHint(pcstr hint) { m_hint = hint; }

    // the looks: the field's states, the list, a row's states, the arrow
    void SetLooks(const SUIShapeSkin& field, const SUIShapeStyle& list, const SUIShapeSkin& row, const SUIShapeStyle& arrow);

    bool IsOpen() const { return s_open == this; }
    static bool AnyOpen() { return s_open != nullptr; }
    // The open list, if any: drawn over everything, or closed once its drop-down is gone from the screen.
    static void DrawOpenList();
    static void CloseOpenList();

    pcstr GetDebugType() override { return "CUIComboBox"; }

    // The rows: the list box is the store of the texts and their tags, never shown.
    CUIListBox m_list_box;

protected:
    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override;
    bool OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action) override;
    void Update() override;
    void Draw() override;

private:
    void Open();
    void Close();
    // the list's rectangle and its visible rows, absolute interface units, laid out for the screen
    void LayoutList();
    int RowAt(const Fvector2& cursor) const;
    void ScrollTo(int row);
    void Choose(int row);
    bool OnScrollBar(const Fvector2& cursor) const;
    void DrawList();

    static CUIComboBox* s_open;

    xr_vector<int> m_disabled;
    int m_iListHeight{};
    int m_itoken_id{};
    int m_opt_backup_value{};
    bool m_bInited{};

    CUIStatic m_text{ "Text" };
    CUIStatic m_rowText{ "Row text" };
    CGameFont* m_font{};
    u32 m_textColor[2]{ 0xFFE8E6E0, 0xFF5E5B55 };

    SUIShapeSkin m_fieldLook;
    SUIShapeStyle m_listLook;
    SUIShapeSkin m_rowLook;
    SUIShapeStyle m_arrowLook;
    bool m_looksSet{};
    shared_str m_hint;

    // the open list
    Frect m_listRect{};
    float m_rowHeight{};
    int m_visibleRows{};
    int m_firstRow{};
    int m_hoverRow{ -1 };
    bool m_draggingThumb{};
    float m_dragOffset{};

    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIWindow);
};
