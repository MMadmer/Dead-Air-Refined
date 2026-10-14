// UICheckButton.cpp: класс кнопки, имеющей 2 состояния:
// с галочкой и без
//////////////////////////////////////////////////////////////////////

#include "pch.hpp"
#include "UICheckButton.h"
#include "Hint/UIHint.h"
#include "xrEngine/xr_input.h"
#include "Static/UIShape.h"

CUICheckButton::CUICheckButton()
{
    TextItemControl()->SetTextAlignment(CGameFont::alLeft);
    SetButtonAsSwitch(true);
    m_pDependControl = NULL;
}

void CUICheckButton::SetDependControl(CUIWindow* pWnd) { m_pDependControl = pWnd; }

void CUICheckButton::Draw()
{
    inherited::Draw();
    if (!m_has_mark || !GetCheck() || !IsShown())
        return;
    // the tick in the box at the left of the button, square on screen
    Frect rect;
    GetAbsoluteRect(rect);
    const float side = rect.height() * 0.62f;
    const float w = side * UI().get_current_kx();
    const float box = std::min(rect.width(), rect.height() * UI().get_current_kx());
    const float x = rect.x1 + (box - w) * 0.5f;
    const float y = rect.y1 + (rect.height() - side) * 0.5f;
    UIShape::Draw(Frect().set(x, y, x + w, y + side), m_mark);
}
void CUICheckButton::Update()
{
    CUI3tButton::Update();
    if (m_pDependControl)
        m_pDependControl->Enable(GetCheck());
}

void CUICheckButton::SetCurrentOptValue()
{
    SetCheck(GetOptBoolValue());
}

void CUICheckButton::SaveOptValue()
{
    CUIOptionsItem::SaveOptValue();
    SaveOptBoolValue(GetCheck());
}

void CUICheckButton::SaveBackUpOptValue()
{
    m_opt_backup_value = GetCheck();
}

bool CUICheckButton::IsChangedOptValue() const { return m_opt_backup_value != GetCheck(); }
void CUICheckButton::UndoOptValue()
{
    SetCheck(m_opt_backup_value);
    CUIOptionsItem::UndoOptValue();
}

void CUICheckButton::InitCheckButton(Fvector2 pos, Fvector2 size, LPCSTR texture_name)
{
    InitButton(pos, size);
    // a skinned box draws its own frame and tick: the old texture would lie beside them
    if (GetShapeSkin())
    {
        TextItemControl()->m_wndPos.set(pos);
        TextItemControl()->m_wndSize.set(size);
        return;
    }
    InitTexture2(texture_name);
    TextItemControl()->m_wndPos.set(pos);
    TextItemControl()->m_wndSize.set(
        Fvector2().set(size.x, m_background->Get(S_Enabled)->GetStaticItem()->GetSize().y));
}

void CUICheckButton::InitTexture2(LPCSTR texture_name)
{
    CUI3tButton::InitTexture(texture_name); // "ui_checker"
    Frect r = m_background->Get(S_Enabled)->GetStaticItem()->GetTextureRect();
    TextItemControl()->m_TextOffset.x = TextItemControl()->m_TextOffset.x + r.width();
}

bool CUICheckButton::OnMouseDown(int mouse_btn)
{
    if (mouse_btn == MOUSE_1)
    {
        if (GetButtonState() == BUTTON_NORMAL)
        {
            SetButtonState(BUTTON_PUSHED);
            GetMessageTarget()->SendMessage(this, CHECK_BUTTON_SET, nullptr);
        }
        else
        {
            SetButtonState(BUTTON_NORMAL);
            GetMessageTarget()->SendMessage(this, CHECK_BUTTON_RESET, nullptr);
        }
    }
    GetMessageTarget()->SendMessage(this, BUTTON_CLICKED, nullptr);
    OnChangedOptValue();
    return true;
}

bool CUICheckButton::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    return CUIWindow::OnMouseAction(x, y, mouse_action);
}
