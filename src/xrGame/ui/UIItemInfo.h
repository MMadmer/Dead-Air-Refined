#pragma once
#include "xrUICore/Windows/UIWindow.h"

class CInventoryItem;
class CUIStatic;
class CUIScrollView;
class CUIProgressBar;
class CUIConditionParams;
class CUIWpnParams;
class CUIArtefactParams;
class CUIFrameWindow;
class UIInvUpgPropertiesWnd;
class CUIOutfitInfo;
class CUIBoosterInfo;
class CUICellItem;

extern const char* const fieldsCaptionColor;

class CUIItemInfo final : public CUIWindow
{
private:
    typedef CUIWindow inherited;
    struct _desc_info
    {
        CGameFont* pDescFont;
        u32 uDescClr;
        bool bShowDescrText;
    };
    _desc_info m_desc_info;
    CInventoryItem* m_pInvItem;
    // The id of the object that pointer belongs to. The panel is handed a raw item and
    // then shown for as long as the window is up; the item can go away underneath it -
    // sold, taken, or released by a script finishing a task while the trade window is
    // open - and nothing tells the panel. Re-resolving the id is the one way to find out
    // that does not read through the pointer being checked.
    u16 m_inv_item_id;

public:
    CUIItemInfo();
    ~CUIItemInfo() override;

    pcstr GetDebugType() override { return "CUIItemInfo"; }

    void Update() override;

    CInventoryItem* CurrentItem() const { return m_pInvItem; }
    void InitItemInfo(Fvector2 pos, Fvector2 size, LPCSTR xml_name);
    bool InitItemInfo(cpcstr xml_name);
    void InitItem(CUICellItem* pCellItem, CInventoryItem* pCompareItem = nullptr,
        u32 item_price = u32(-1), pcstr trade_tip = nullptr);

    void TryAddConditionInfo(CInventoryItem& pInvItem, CInventoryItem* pCompareItem);
    bool TryAddWpnInfo(CInventoryItem& pInvItem, CInventoryItem* pCompareItem);
    bool TryAddArtefactInfo(CInventoryItem& pInvItem);
    bool TryAddOutfitInfo(CInventoryItem& pInvItem, CInventoryItem* pCompareItem);
    void TryAddUpgradeInfo(CInventoryItem& pInvItem);
    void TryAddBoosterInfo(CInventoryItem& pInvItem);

    virtual void Draw();
    bool m_b_FitToHeight;
    u32 delay;

    CUIFrameWindow* UIBackground;
    CUIStatic* UIName;
    CUIStatic* UIWeight;
    CUIStatic* UICost;
    CUIStatic* UITradeTip;
    //	CUIStatic*			UIDesc_line;
    CUIScrollView* UIDesc;
    bool m_complex_desc;

    CUIConditionParams* UIConditionWnd;
    CUIWpnParams* UIWpnParams;
    CUIArtefactParams* UIArtefactParams;
    UIInvUpgPropertiesWnd* UIProperties;
    CUIOutfitInfo* UIOutfitInfo;
    CUIBoosterInfo* UIBoosterInfo;

    Fvector2 UIItemImageSize;
    CUIStatic* UIItemImage;
};
