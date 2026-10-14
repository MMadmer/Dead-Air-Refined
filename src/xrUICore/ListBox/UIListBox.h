#pragma once

#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIShapeStyle.h"

class CUIListBoxItem;

class XRUICORE_API CUIListBox final : public CUIScrollView
{
public:
    CUIListBox();
    CUIListBoxItem* AddItem();
    CUIListBoxItem* AddTextItem(LPCSTR text);
    void AddExistingItem(CUIListBoxItem*);

    CUIListBoxItem* GetItemByTAG(u32 tag_value);
    int GetIdxByTAG(u32 tag_value);
    CUIListBoxItem* GetItemByIDX(int idx);
    CUIListBoxItem* GetItemByText(LPCSTR text);
    CUIListBoxItem* GetSelectedItem();

    LPCSTR GetSelectedText();
    LPCSTR GetText(int idx);
    void MoveSelectedUp();
    void MoveSelectedDown();
    void SetSelectionTexture(LPCSTR texture);
    void SetItemHeight(float h);
    float GetItemHeight();
    float GetLongestLength();

    virtual void SetSelected(CUIWindow* w) { CUIScrollView::SetSelected(w); };
    u32 GetSelectedIDX();
    void SetSelectedIDX(u32 idx);
    void SetSelectedTAG(u32 tag_val);
    void SetSelectedText(LPCSTR txt);
    void SetImmediateSelection(bool f);

    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = 0);

    void SetTextColor(u32 color);
    u32 GetTextColor();
    void SetFont(CGameFont* pFont);
    CGameFont* GetFont();

    pcstr GetDebugType() override { return "CUIListBox"; }

protected:
    CGameFont* m_pFont;
    float m_def_item_height;
    u32 m_text_color;
    shared_str m_selection_texture;

public:
    // Every row of the list drawn by the shader: normal, highlight under the cursor, checked when
    // selected (the layout's <list><item shape="..."/></list>).
    void SetItemSkin(std::unique_ptr<SUIShapeSkin> skin) { m_item_skin = std::move(skin); }

protected:
    std::unique_ptr<SUIShapeSkin> m_item_skin;
    void SkinItem(CUIListBoxItem* item);

    bool m_bImmediateSelection;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIScrollView);
};
