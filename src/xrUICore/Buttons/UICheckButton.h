#pragma once

#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Options/UIOptionsItem.h"
#include "xrUICore/Static/UIShapeStyle.h"

class UIHint;

class XRUICORE_API CUICheckButton final : public CUI3tButton, public CUIOptionsItem
{
    typedef CUI3tButton inherited;

public:
    CUICheckButton();

    virtual void Update();

    // CUIOptionsItem
    virtual void SetCurrentOptValue(); // opt->current
    virtual void SaveBackUpOptValue(); // current->backup
    virtual void SaveOptValue(); // current->opt
    virtual void UndoOptValue(); // backup->current
    virtual bool IsChangedOptValue() const; // backup!=current

    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    virtual bool OnMouseDown(int mouse_btn);

    void InitCheckButton(Fvector2 pos, Fvector2 size, LPCSTR texture_name);

    //состояние кнопки
    IC bool GetCheck() const { return GetButtonState() == BUTTON_PUSHED; }
    IC void SetCheck(bool ch) { SetButtonState(ch ? BUTTON_PUSHED : BUTTON_NORMAL); }
    void SetDependControl(CUIWindow* pWnd);
    // the tick a skinned check box shows while it is on (the <mark> node of its layout)
    void SetCheckMark(const SUIShapeStyle& mark) { m_mark = mark; m_has_mark = true; }
    void Draw() override;

    pcstr GetDebugType() override { return "CUICheckButton"; }

private:
    bool m_opt_backup_value{};
    void InitTexture2(LPCSTR texture_name);
    CUIWindow* m_pDependControl;
    SUIShapeStyle m_mark;
    bool m_has_mark{};
};
