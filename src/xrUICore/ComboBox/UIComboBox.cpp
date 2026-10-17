#include "pch.hpp"
#include "UIComboBox.h"
#include "ListBox/UIListBoxItem.h"
#include "Static/UIShape.h"
#include "Cursor/UICursor.h"
#include "Buttons/UIBtnHint.h"
#include "xrCore/xr_token.h"

CUIComboBox* CUIComboBox::s_open = nullptr;

namespace
{
// interface units
constexpr int MaxShownRows = 10;
constexpr float ListGap = 2.f; // between the field and its list
constexpr float ListPad = 3.f; // round the rows
constexpr float TextPad = 6.f;
constexpr float ScrollWidth = 3.f;
constexpr float ScreenMargin = 6.f;
constexpr float DefaultHeight = 20.f;

STRING_VALUE GetOptionTokenText(pcstr token)
{
    string256 displayId;
    strconcat(displayId, "st_opt_", token);

    const STRING_VALUE displayText = StringTable().translate(displayId);
    return displayText == displayId ? StringTable().translate(token) : displayText;
}

SUIShapeStyle box(u32 fill, u32 line, float border, float radius)
{
    SUIShapeStyle s;
    s.fill = fill;
    s.line = line;
    s.border = border;
    s.radius = radius;
    return s;
}

// The menus' own look, for a theme that has none: a black field with a light line, light rows.
SUIShapeSkin default_field()
{
    SUIShapeSkin skin;
    skin.looks[SUIShapeSkin::Normal] = box(color_rgba(0, 0, 0, 179), color_rgba(232, 230, 224, 153), 1.f, 2.f);
    skin.looks[SUIShapeSkin::Highlight] = box(color_rgba(0, 0, 0, 179), color_rgba(232, 230, 224, 255), 1.f, 2.f);
    skin.looks[SUIShapeSkin::Pushed] = skin.looks[SUIShapeSkin::Highlight];
    skin.looks[SUIShapeSkin::Checked] = skin.looks[SUIShapeSkin::Highlight];
    skin.looks[SUIShapeSkin::Disabled] = box(color_rgba(0, 0, 0, 120), color_rgba(232, 230, 224, 46), 1.f, 2.f);
    return skin;
}

SUIShapeSkin default_row()
{
    SUIShapeSkin skin;
    skin.looks[SUIShapeSkin::Normal] = box(0, 0, 0.f, 2.f);
    skin.looks[SUIShapeSkin::Highlight] = box(color_rgba(232, 230, 224, 51), 0, 0.f, 2.f);
    skin.text[SUIShapeSkin::Highlight] = color_rgba(255, 255, 255, 255);
    skin.set[SUIShapeSkin::Highlight] = SUIShapeSkin::TextSet;
    skin.looks[SUIShapeSkin::Pushed] = skin.looks[SUIShapeSkin::Highlight];
    skin.looks[SUIShapeSkin::Checked] = box(color_rgba(232, 230, 224, 26), 0, 0.f, 2.f);
    skin.looks[SUIShapeSkin::Disabled] = skin.looks[SUIShapeSkin::Normal];
    return skin;
}

SUIShapeSkin skin_or(pcstr name, SUIShapeSkin fallback)
{
    const SUIShapeSkin* skin = UITheme::Skin(name);
    return skin ? *skin : fallback;
}

SUIShapeStyle style_or(pcstr name, const SUIShapeStyle& fallback)
{
    const SUIShapeSkin* skin = UITheme::Skin(name);
    return skin ? skin->looks[SUIShapeSkin::Normal] : fallback;
}

// a look's own caption colour, else the drop-down's
u32 text_of(const SUIShapeSkin& skin, SUIShapeSkin::Look look, u32 fallback)
{
    return skin.HasText(look) ? skin.text[look] : fallback;
}
} // namespace

CUIComboBox::CUIComboBox() : CUIWindow("CUIComboBox")
{
    AttachChild(&m_text);
    UI().Focus().RegisterFocusable(this);
}

CUIComboBox::~CUIComboBox()
{
    if (s_open == this)
    {
        s_open = nullptr;
        if (CUIWindow* parent = GetParent())
        {
            parent->SetCapture(this, false);
            parent->SetKeyboardCapture(this, false);
        }
    }
    UI().Focus().UnregisterFocusable(this);
}

void CUIComboBox::SetListLength(int length) { m_iListHeight = length; }

void CUIComboBox::SetFont(CGameFont* font)
{
    if (!font)
        return;
    m_font = font;
    m_text.SetFont(font);
    m_rowText.SetFont(font);
    m_list_box.SetFont(font);
}

void CUIComboBox::SetLooks(const SUIShapeSkin& field, const SUIShapeStyle& list, const SUIShapeSkin& row, const SUIShapeStyle& arrow)
{
    m_fieldLook = field;
    m_listLook = list;
    m_rowLook = row;
    m_arrowLook = arrow;
    m_looksSet = true;
}

void CUIComboBox::InitComboBox(Fvector2 pos, float width)
{
    m_bInited = true;
    if (!m_iListHeight)
        m_iListHeight = 4;

    // a layout's height, else the stock one
    const float height = GetHeight() > 0.f ? GetHeight() : DefaultHeight;
    CUIWindow::SetWndPos(pos);
    CUIWindow::SetWndSize({ width, height });

    // the choice at the left, the arrow in a square at the right (square on the screen)
    m_text.SetWndPos({ TextPad, 0.f });
    m_text.SetWndSize({ std::max(width - TextPad - height * UI().get_current_kx(), 1.f), height });
    m_text.SetVTextAlignment(valCenter);
    m_text.SetEllipsis(true);
    m_text.Enable(false);

    m_rowText.SetVTextAlignment(valCenter);
    m_rowText.SetEllipsis(true);

    m_list_box.SetWndSize({ width, height * m_iListHeight });
    m_list_box.InitScrollView();
    m_list_box.SetItemHeight(height);
    m_list_box.SetMessageTarget(this);

    if (!m_font)
        SetFont(m_text.GetFont() ? m_text.GetFont() : UI().Font().pFontLetterica16Russian);
    if (!m_looksSet)
    {
        SUIShapeStyle chevron;
        chevron.kind = SUIShapeStyle::Kind::Chevron;
        chevron.border = 1.5f;
        SetLooks(skin_or("combo", default_field()),
            style_or("combo_list", box(color_rgba(10, 10, 10, 255), color_rgba(232, 230, 224, 204), 1.f, 2.f)),
            skin_or("combo_row", default_row()), style_or("combo_arrow", chevron));
    }
}

CUIListBoxItem* CUIComboBox::AddItem_(LPCSTR str, int _data)
{
    R_ASSERT2(m_bInited, "Can't add item to ComboBox before Initialization");
    CUIListBoxItem* itm = m_list_box.AddTextItem(str);
    itm->SetData((void*)(__int64)_data);
    return itm;
}

void CUIComboBox::SetText(LPCSTR text)
{
    if (!text)
        return;
    m_text.SetText(text);
}

void CUIComboBox::disable_id(int id)
{
    if (m_disabled.end() == std::find(m_disabled.begin(), m_disabled.end(), id))
        m_disabled.push_back(id);
}

void CUIComboBox::enable_id(int id)
{
    const auto it = std::find(m_disabled.begin(), m_disabled.end(), id);
    if (m_disabled.end() != it)
        m_disabled.erase(it);
}

void CUIComboBox::SetCurrentOptValue()
{
    Close();
    m_list_box.Clear();
    const xr_token* tok = GetOptToken();
    R_ASSERT3(tok, "Option token doesnt exist:", m_entry.c_str());

    for (; tok->name; ++tok)
    {
        if (m_disabled.end() == std::find(m_disabled.begin(), m_disabled.end(), tok->id))
            AddItem_(GetOptionTokenText(tok->name).c_str(), tok->id);
    }

    const STRING_VALUE cur_val = GetOptionTokenText(GetOptTokenValue());
    m_text.SetText(cur_val.c_str());
    m_list_box.SetSelectedText(cur_val.c_str());

    if (CUIListBoxItem* itm = m_list_box.GetSelectedItem())
        m_itoken_id = (int)(__int64)itm->GetData();
    else
        m_itoken_id = 1; // first
}

void CUIComboBox::SaveBackUpOptValue() { m_opt_backup_value = m_itoken_id; }

void CUIComboBox::UndoOptValue()
{
    m_itoken_id = m_opt_backup_value;
    OnChangedOptValue();
    SetItemToken(m_itoken_id);
    CUIOptionsItem::UndoOptValue();
}

void CUIComboBox::SaveOptValue()
{
    CUIOptionsItem::SaveOptValue();
    if (const xr_token* tok = GetOptToken())
        SaveOptStringValue(get_token_name(tok, m_itoken_id));
}

bool CUIComboBox::IsChangedOptValue() const { return m_opt_backup_value != m_itoken_id; }
LPCSTR CUIComboBox::GetText() const { return m_text.GetText(); }
u32 CUIComboBox::GetSize() const { return m_list_box.GetSize(); }

LPCSTR CUIComboBox::GetTextOf(int index)
{
    if (u32(index) >= GetSize())
        return "";
    return m_list_box.GetText(index);
}

void CUIComboBox::SetItemIDX(int idx)
{
    m_list_box.SetSelectedIDX(idx);
    CUIListBoxItem* itm = m_list_box.GetSelectedItem();
    if (!itm)
        return;

    m_itoken_id = (int)(__int64)itm->GetData();
    m_text.SetText(m_list_box.GetSelectedText());
    OnChangedOptValue();
}

// The row whose data is the token; none such leaves the choice as it is (the list box's search by tag answers
// its last row then).
void CUIComboBox::SetItemToken(int tok_id)
{
    for (int i = 0, count = int(GetSize()); i < count; ++i)
    {
        if (CUIListBoxItem* itm = m_list_box.GetItemByIDX(i); itm && int(__int64(itm->GetData())) == tok_id)
        {
            SetItemIDX(i);
            return;
        }
    }
}
void CUIComboBox::SetSelectedIDX(u32 idx) { m_list_box.SetSelectedIDX(idx); }

u32 CUIComboBox::GetSelectedIDX()
{
    // the list box counts past its end when nothing is chosen
    const u32 idx = m_list_box.GetSelectedIDX();
    return idx < GetSize() ? idx : u32(-1);
}

// A choice made by the player, from the list or by the arrows: the option changes (applied at once when
// it is applied on change), and the owner hears of it when it differs.
void CUIComboBox::Choose(int row)
{
    if (row < 0 || row >= int(GetSize()))
        return;
    const int before = m_itoken_id;
    SetItemIDX(row);
    if (before != m_itoken_id)
    {
        if (CUIWindow* target = GetMessageTarget())
            target->SendMessage(this, LIST_ITEM_SELECT, nullptr);
    }
}

bool CUIComboBox::SetNextItemSelected(bool next, bool loop)
{
    const int last = int(GetSize()) - 1;
    if (last < 0)
        return false;

    int idx = int(GetSelectedIDX());
    if (idx < 0)
        idx = next ? -1 : last + 1;
    if (next)
        idx = idx < last ? idx + 1 : (loop ? 0 : -1);
    else
        idx = idx > 0 ? idx - 1 : (loop ? last : -1);
    if (idx < 0)
        return false;
    Choose(idx);
    return true;
}

void CUIComboBox::ClearList()
{
    Close();
    m_list_box.Clear();
    m_text.SetText("");
    m_itoken_id = 0;
    m_disabled.clear();
}

// ---------------------------------------------------------------------------------------------------
// the open list

void CUIComboBox::Open()
{
    if (IsOpen() || !GetSize())
        return;
    if (s_open)
        s_open->Close();
    s_open = this;
    if (g_btnHint && g_btnHint->Owner() == this)
        g_btnHint->Discard();

    const int current = int(GetSelectedIDX());
    m_hoverRow = current;
    LayoutList();
    // the choice in sight, in the middle when it can
    m_firstRow = 0;
    ScrollTo(current - m_visibleRows / 2);

    // everything the mouse and the keys do goes here while the list is open
    if (CUIWindow* parent = GetParent())
    {
        parent->SetCapture(this, true);
        parent->SetKeyboardCapture(this, true);
    }
}

void CUIComboBox::Close()
{
    if (!IsOpen())
        return;
    s_open = nullptr;
    m_hoverRow = -1;
    m_draggingThumb = false;
    if (CUIWindow* parent = GetParent())
    {
        parent->SetCapture(this, false);
        parent->SetKeyboardCapture(this, false);
    }
}

void CUIComboBox::CloseOpenList()
{
    if (s_open)
        s_open->Close();
}

void CUIComboBox::LayoutList()
{
    Frect field;
    GetAbsoluteRect(field);
    const int count = int(GetSize());
    m_rowHeight = field.height();

    // all of a short list, up to ten rows of a long one (a layout asking more gets more), within the screen
    const int wanted = std::max(1, std::min(count, std::max(m_iListHeight, MaxShownRows)));
    const float below = UI_BASE_HEIGHT - ScreenMargin - (field.bottom + ListGap);
    const float above = field.top - ListGap - ScreenMargin;
    const float wantedHeight = wanted * m_rowHeight + 2.f * ListPad;
    const bool upwards = wantedHeight > below && above > below;
    const float room = upwards ? above : below;
    m_visibleRows = std::clamp(int((room - 2.f * ListPad) / m_rowHeight), 1, wanted);
    const float height = m_visibleRows * m_rowHeight + 2.f * ListPad;

    // as wide as the field, or as the longest row
    float width = field.width();
    const bool scrolls = count > m_visibleRows;
    if (CGameFont* font = m_font)
    {
        for (int i = 0; i < count; ++i)
        {
            float w = font->SizeOf_(m_list_box.GetText(i));
            UI().ClientToScreenScaledWidth(w);
            width = std::max(width, w + 2.f * (TextPad + ListPad) + (scrolls ? ScrollWidth + 3.f : 0.f));
        }
    }
    width = std::min(width, UI_BASE_WIDTH - 2.f * ScreenMargin);
    const float x = std::clamp(field.left, ScreenMargin, UI_BASE_WIDTH - ScreenMargin - width);
    const float y = upwards ? field.top - ListGap - height : field.bottom + ListGap;
    m_listRect.set(x, y, x + width, y + height);
    m_firstRow = std::clamp(m_firstRow, 0, std::max(count - m_visibleRows, 0));
}

void CUIComboBox::ScrollTo(int row) { m_firstRow = std::clamp(row, 0, std::max(int(GetSize()) - m_visibleRows, 0)); }

int CUIComboBox::RowAt(const Fvector2& cursor) const
{
    if (!m_listRect.in(cursor.x, cursor.y) || OnScrollBar(cursor))
        return -1;
    const int row = m_firstRow + int(std::floor((cursor.y - m_listRect.top - ListPad) / m_rowHeight));
    return row >= m_firstRow && row < std::min(m_firstRow + m_visibleRows, int(GetSize())) ? row : -1;
}

bool CUIComboBox::OnScrollBar(const Fvector2& cursor) const
{
    return int(GetSize()) > m_visibleRows && cursor.x >= m_listRect.right - ListPad - ScrollWidth - 3.f &&
        cursor.x <= m_listRect.right && cursor.y >= m_listRect.top && cursor.y <= m_listRect.bottom;
}

void CUIComboBox::DrawOpenList()
{
    if (!s_open)
        return;
    // gone from the screen - the drop-down or a window it lies in hidden - the list goes too
    for (CUIWindow* w = s_open; w; w = w->GetParent())
    {
        if (!w->IsShown())
        {
            s_open->Close();
            return;
        }
    }
    s_open->LayoutList();
    s_open->DrawList();
}

void CUIComboBox::DrawList()
{
    UIShape::Draw(m_listRect, m_listLook);

    const int count = int(GetSize());
    const int current = int(GetSelectedIDX());
    const bool scrolls = count > m_visibleRows;
    const float rowRight = m_listRect.right - ListPad - (scrolls ? ScrollWidth + 3.f : 0.f);
    for (int i = 0; i < m_visibleRows && m_firstRow + i < count; ++i)
    {
        const int row = m_firstRow + i;
        Frect r;
        r.set(m_listRect.left + ListPad, m_listRect.top + ListPad + i * m_rowHeight, rowRight,
            m_listRect.top + ListPad + (i + 1) * m_rowHeight);
        const SUIShapeSkin::Look look =
            row == m_hoverRow ? SUIShapeSkin::Highlight : (row == current ? SUIShapeSkin::Checked : SUIShapeSkin::Normal);
        UIShape::Draw(r, m_rowLook.looks[look]);

        m_rowText.SetWndPos({ r.left + TextPad, r.top });
        m_rowText.SetWndSize({ r.width() - 2.f * TextPad, r.height() });
        m_rowText.SetText(m_list_box.GetText(row));
        m_rowText.SetTextColor(text_of(m_rowLook, look, m_textColor[0]));
        m_rowText.Draw();
    }

    if (scrolls)
    {
        // a thin bar: the track, and the thumb over the part shown
        const float x1 = m_listRect.right - ListPad - ScrollWidth;
        const float top = m_listRect.top + ListPad, bottom = m_listRect.bottom - ListPad;
        const float span = bottom - top;
        const float thumb = std::max(span * m_visibleRows / count, 8.f);
        const float at = top + (span - thumb) * m_firstRow / float(std::max(count - m_visibleRows, 1));
        SUIShapeStyle track = style_or("combo_scroll_track", box(color_rgba(232, 230, 224, 20), 0, 0.f, 1.f));
        SUIShapeStyle bar = style_or("combo_scroll_thumb", box(color_rgba(232, 230, 224, 128), 0, 0.f, 1.f));
        UIShape::Draw(Frect().set(x1, top, x1 + ScrollWidth, bottom), track);
        UIShape::Draw(Frect().set(x1, at, x1 + ScrollWidth, at + thumb), bar);
    }
    // the rows' texts over the list, under the hints the cursor draws next
    UI().RenderFont();
}

// ---------------------------------------------------------------------------------------------------

void CUIComboBox::Update()
{
    CUIWindow::Update();
    if (IsOpen() && !IsEnabled())
        Close();

    // the hint of the closed field, as a button's
    if (g_btnHint && m_hint.size() && !IsOpen() && CursorOverWindow() && g_btnHint->Vacant() &&
        Device.dwTimeContinual > m_dwFocusReceiveTime + 700)
        g_btnHint->ShowFor(this, m_hint.c_str());
}

void CUIComboBox::Draw()
{
    Frect field;
    GetAbsoluteRect(field);
    const bool enabled = IsEnabled();
    const bool hover = CursorOverWindow() || UI().Focus().GetFocused() == this;
    const SUIShapeSkin::Look look = !enabled ? SUIShapeSkin::Disabled :
        (IsOpen() ? SUIShapeSkin::Pushed : (hover ? SUIShapeSkin::Highlight : SUIShapeSkin::Normal));
    UIShape::Draw(field, m_fieldLook.looks[look]);

    const u32 text = text_of(m_fieldLook, look, m_textColor[enabled ? 0 : 1]);
    m_text.SetTextColor(text);
    CUIWindow::Draw();

    // the arrow in the square at the right, in the caption's colour unless its look has one
    SUIShapeStyle arrow = m_arrowLook;
    if (!(arrow.fill >> 24))
        arrow.fill = text;
    const float kx = UI().get_current_kx();
    const float side = field.height() * 0.42f;
    const float cx = field.right - field.height() * 0.5f * kx;
    const float cy = (field.top + field.bottom) * 0.5f;
    UIShape::Draw(Frect().set(cx - side * 0.5f * kx, cy - side * 0.5f, cx + side * 0.5f * kx, cy + side * 0.5f), arrow);

    if (g_btnHint && g_btnHint->Owner() == this)
        g_btnHint->Draw_();
}

void CUIComboBox::OnFocusLost()
{
    CUIWindow::OnFocusLost();
    if (g_btnHint && g_btnHint->Owner() == this)
        g_btnHint->Discard();
    if (IsOpen() && pInput->IsCurrentInputTypeController())
        Close();
}

void CUIComboBox::OnFocusReceive() { CUIWindow::OnFocusReceive(); }

bool CUIComboBox::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (!IsOpen())
    {
        if ((mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK) && IsEnabled())
        {
            Open();
            return true;
        }
        return CUIWindow::OnMouseAction(x, y, mouse_action);
    }

    // open: every action of the mouse is the list's
    const Fvector2 cursor = GetUICursor().GetCursorPosition();
    switch (mouse_action)
    {
    case WINDOW_MOUSE_MOVE:
        if (m_draggingThumb)
        {
            const int count = int(GetSize());
            const float span = m_listRect.height() - 2.f * ListPad;
            const float thumb = std::max(span * m_visibleRows / count, 8.f);
            const float along = (cursor.y - m_dragOffset - m_listRect.top - ListPad) / std::max(span - thumb, 1.f);
            ScrollTo(int(std::round(along * (count - m_visibleRows))));
        }
        else
            m_hoverRow = RowAt(cursor);
        break;
    case WINDOW_MOUSE_WHEEL_UP:
        ScrollTo(m_firstRow - 1);
        m_hoverRow = RowAt(cursor);
        break;
    case WINDOW_MOUSE_WHEEL_DOWN:
        ScrollTo(m_firstRow + 1);
        m_hoverRow = RowAt(cursor);
        break;
    case WINDOW_LBUTTON_DOWN:
    case WINDOW_LBUTTON_DB_CLICK:
        if (OnScrollBar(cursor))
        {
            // the thumb is dragged from where it was taken; a click on the track pages
            const int count = int(GetSize());
            const float span = m_listRect.height() - 2.f * ListPad;
            const float thumb = std::max(span * m_visibleRows / count, 8.f);
            const float at = m_listRect.top + ListPad + (span - thumb) * m_firstRow / float(std::max(count - m_visibleRows, 1));
            if (cursor.y >= at && cursor.y <= at + thumb)
            {
                m_draggingThumb = true;
                m_dragOffset = cursor.y - at;
            }
            else
                ScrollTo(m_firstRow + (cursor.y < at ? -m_visibleRows : m_visibleRows));
        }
        else if (RowAt(cursor) < 0)
            Close(); // anywhere else, the field too: the list closes and nothing more
        break;
    case WINDOW_LBUTTON_UP:
        if (m_draggingThumb)
            m_draggingThumb = false;
        else if (const int row = RowAt(cursor); row >= 0)
        {
            Close();
            Choose(row);
        }
        break;
    case WINDOW_RBUTTON_DOWN: Close(); break;
    default: break;
    }
    return true;
}

bool CUIComboBox::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (!IsOpen())
    {
        if (CUIWindow::OnKeyboardAction(dik, keyboard_action))
            return true;
        // closed: a focused drop-down steps through its choices, or opens
        if (keyboard_action != WINDOW_KEY_PRESSED || UI().Focus().GetFocused() != this || !IsEnabled())
            return false;
        switch (GetBindedAction(dik, EKeyContext::UI))
        {
        case kUI_MOVE_LEFT: SetNextItemSelected(false, false); return true;
        case kUI_MOVE_RIGHT: SetNextItemSelected(true, false); return true;
        case kUI_ACCEPT: Open(); return true;
        default: return false;
        }
    }

    // open: the keys are the list's
    if (keyboard_action != WINDOW_KEY_PRESSED)
        return true;
    const int count = int(GetSize());
    const auto move = [&](int row)
    {
        m_hoverRow = std::clamp(row, 0, count - 1);
        if (m_hoverRow < m_firstRow)
            ScrollTo(m_hoverRow);
        else if (m_hoverRow >= m_firstRow + m_visibleRows)
            ScrollTo(m_hoverRow - m_visibleRows + 1);
    };
    const int from = m_hoverRow >= 0 ? m_hoverRow : int(GetSelectedIDX());
    switch (dik)
    {
    case SDL_SCANCODE_HOME: move(0); return true;
    case SDL_SCANCODE_END: move(count - 1); return true;
    case SDL_SCANCODE_PAGEUP: move(from - m_visibleRows); return true;
    case SDL_SCANCODE_PAGEDOWN: move(from + m_visibleRows); return true;
    default: break;
    }
    switch (GetBindedAction(dik, EKeyContext::UI))
    {
    case kUI_MOVE_UP: move(from < 0 ? count - 1 : from - 1); break;
    case kUI_MOVE_DOWN: move(from + 1); break;
    case kUI_ACCEPT:
    {
        const int row = m_hoverRow;
        Close();
        Choose(row);
        break;
    }
    case kUI_BACK: Close(); break;
    default:
        if (IsBinded(kQUIT, dik))
            Close();
        break;
    }
    return true;
}

bool CUIComboBox::OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action)
{
    if (CUIWindow::OnControllerAction(axis, state, controller_action))
        return true;
    if (!IsBinded(kUI_MOVE, axis, EKeyContext::UI))
        return IsOpen();

    if (IsOpen())
    {
        if (std::abs(state.y) > 0.5f && std::abs(state.x) < 0.2f)
        {
            const int count = int(GetSize());
            const int from = m_hoverRow >= 0 ? m_hoverRow : int(GetSelectedIDX());
            m_hoverRow = std::clamp(from + (state.y > 0 ? 1 : -1), 0, count - 1);
            if (m_hoverRow < m_firstRow)
                ScrollTo(m_hoverRow);
            else if (m_hoverRow >= m_firstRow + m_visibleRows)
                ScrollTo(m_hoverRow - m_visibleRows + 1);
        }
        return true;
    }
    if (UI().Focus().GetFocused() == this && std::abs(state.x) > 0.5f && std::abs(state.y) < 0.2f)
    {
        SetNextItemSelected(state.x > 0, false);
        return true;
    }
    return false;
}

void CUIComboBox::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    CUIWindow::SendMessage(pWnd, msg, pData);
    // the parent took the mouse elsewhere while the list was open
    if (msg == WINDOW_MOUSE_CAPTURE_LOST && IsOpen())
        Close();
}
