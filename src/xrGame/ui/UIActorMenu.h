#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrServerEntities/inventory_space.h"
#include "xrUICore/Hint/UIHint.h"
#include "UIActorMenuTools.h"

class CUICharacterInfo;
class CUIDragDropListEx;
class CUIDragDropReferenceList;
class CUICellItem;
class CUIDragItem;
class ui_actor_state_wnd;
class CUIItemInfo;
class CUIOutfitInfo;
class CUIFrameLineWnd;
class CUIStatic;
class CUI3tButton;
class CInventoryOwner;
class CInventoryBox;
class CUIInventoryUpgradeWnd;
class UIInvUpgradeInfo;
class CUIMessageBoxEx;
class StaticDrawableWrapper;
class CUIPropertiesBox;
class CTrade;
class CUIProgressBar;
class CUITradeBar;
class CUIWeightBar;
class CUIInventoryToolbar;
class CUIInventoryHotkeysPanel;
class CUIItemCompareWnd;
struct SInventoryHotkey;
enum class EHotkeyTrigger : u8;

namespace inventory
{
namespace upgrade
{
class Upgrade;
}
} // namespace upgrade, inventory

enum EDDListType
{
    iInvalid,
    iActorSlot,
    iActorBag,
    iActorBelt,

    iActorTrade,
    iPartnerTradeBag,
    iPartnerTrade,
    iDeadBodyBag,
    iQuickSlot,
    iTrashSlot,
    iListTypeMax
};

enum EMenuMode
{
    mmUndefined,
    mmInventory,
    mmTrade,
    mmUpgrade,
    mmDeadBodySearch,
};

class CUIActorMenu final : public CUIDialogWnd, public CUIWndCallback
{
    typedef CUIDialogWnd inherited;
    typedef inventory::upgrade::Upgrade Upgrade_type;

protected:
    enum eActorMenuSndAction
    {
        eSndOpen = 0,
        eSndClose,
        eItemToSlot,
        eItemToBelt,
        eItemToRuck,
        eProperties,
        eDropItem,
        eAttachAddon,
        eDetachAddon,
        eItemUse,
        eSndMax
    };

    enum eActorMenuListType
    {
        eInventoryKnifeList,
        eInventoryPistolList,
        eInventoryAutomaticList,
        eInventorySidearmList,
        eInventoryBinocularList,

        eInventoryBackpackList,
        eInventoryOutfitList,
        eInventoryHelmetList,

        eInventoryBeltList,
        eInventoryDetectorList,

        eInventoryBagList,

        eTradeActorList,
        eTradeActorBagList,

        eTradePartnerList,
        eTradePartnerBagList,

        eSearchLootBagList,
        eSearchLootActorBagList,

        eTrashList,

        eListCount
    };

    EMenuMode m_currMenuMode;
    ref_sound sounds[eSndMax];
    void PlaySnd(eActorMenuSndAction a);

    CInventoryOwner* m_pActorInvOwner{};
    CInventoryOwner* m_pPartnerInvOwner{};
    CInventoryBox* m_pInvBox{};

    CTrade* m_actor_trade{};
    CTrade* m_partner_trade{};

    CUICellItem* m_pCurrentCellItem{};

    // Messages
    CUIPropertiesBox* m_UIPropertiesBox{};
    CUIMessageBoxEx* m_message_box_yes_no{};
    CUIMessageBoxEx* m_message_box_ok{};
    StaticDrawableWrapper* m_message_static{};

    // Windows
    CUIWindow* m_pInventoryWnd{};
    CUIWindow* m_pTradeWnd{};
    CUIWindow* m_pSearchLootWnd{};
    CUIInventoryUpgradeWnd* m_pUpgradeWnd{};
    UIInvUpgradeInfo* m_upgrade_info{};
    CUICellItem* m_upgrade_selected{};

    // Left side
    CUIStatic* m_LeftBackground{};

    // Character infos
    CUICharacterInfo* m_ActorCharacterInfo{};
    CUICharacterInfo* m_PartnerCharacterInfo{};
    CUICharacterInfo* m_TradeActorCharacterInfo{};
    CUICharacterInfo* m_TradePartnerCharacterInfo{};
    CUICharacterInfo* m_SearchLootActorCharacterInfo{};
    CUICharacterInfo* m_SearchLootPartnerCharacterInfo{};

    // Item infos
    CUIItemInfo* m_ItemInfo{};
    CUIItemInfo* m_ItemInfoInventoryMode{};
    CUIItemInfo* m_ItemInfoTradeMode{};
    CUIItemInfo* m_ItemInfoSearchLootMode{};
    CUIOutfitInfo* m_OutfitInfo{};

    // Money
    CUIStatic* m_ActorMoney{};
    CUIStatic* m_TradeActorMoney{};
    CUIStatic* m_PartnerMoney{};

    // Trade bar
    CUITradeBar* m_ActorTradeBar;
    CUITradeBar* m_PartnerTradeBar;

    // Weight bar
    CUIWeightBar* m_ActorWeightBar;
    CUIWeightBar* m_PartnerWeightBar;

    // Drag&Drop lists
    CUIDragDropListEx* m_pLists[eListCount]{};

public:
    CUIDragDropReferenceList* m_pQuickSlot{};

protected:
    // Condition bars
    ui_actor_state_wnd* m_ActorStateInfo{};

    // Buttons
    CUI3tButton* m_trade_button{};
    CUI3tButton* m_trade_buy_button{};
    CUI3tButton* m_trade_sell_button{};
    CUI3tButton* m_takeall_button{};
    CUI3tButton* m_exit_button{};

    // Helpers
    CUIStatic* m_clock_value{};
    UIHint* m_hint_wnd{};

    // Sort and filter rows under the item grids
    xr_vector<CUIInventoryToolbar*> m_inventory_tools;
    EItemCategory m_item_filter{ EItemCategory::All };

    // A sorted pane waits for the items the merge released before it is laid out again.
    struct SPendingSort
    {
        xr_vector<u16> released;
        u32 started{};
        u32 settled_frames{};
        u8 panes{}; // 1 << EInventoryPane
        bool active{};
    } m_pending_sort;

    // The hotkeys of the item grids, their panel, the comparison (UIActorMenuHotkeys.cpp)
    CUIInventoryHotkeysPanel* m_hotkeys_panel{};
    CUIItemCompareWnd* m_compare_wnd{};
    // the hint of the item as it would be new, beside the item's own
    CUIItemInfo* m_ItemInfoPristine{};
    bool m_pristine_shown{};
    // A hover hotkey fires again only once the cursor has moved: the grid closing up under a still
    // cursor would carry off the next item as well.
    Fvector2 m_hover_cursor{ -1.0f, -1.0f };
    // the release of a press a hotkey took goes nowhere
    EUIMessages m_swallow_up{};
    bool m_swallow_armed{};
    // A hover hotkey acts after the update: the cell it moves is one the grid is walking through.
    struct SPendingHover
    {
        shared_str action;
        u16 item{ u16(-1) };
    } m_pending_hover;
    // the item a hover hotkey could not move, left alone until the cursor is over another
    u16 m_hover_refused{ u16(-1) };
    // items picked for a comparison; the first one's kind is the only one the others may be
    xr_vector<u16> m_picks;
    u8 m_pick_kind{};

protected:
    u32 m_last_time;
    bool m_repair_mode;
    bool m_item_info_view;
    bool m_highlight_clear;
    u32 m_trade_partner_inventory_state;

public:
    void SetMenuMode(EMenuMode mode);
    EMenuMode GetMenuMode() { return m_currMenuMode; };
    void SetActor(CInventoryOwner* io);
    void SetPartner(CInventoryOwner* io);
    CInventoryOwner* GetPartner() { return m_pPartnerInvOwner; };
    void SetInvBox(CInventoryBox* box);
    CInventoryBox* GetInvBox() { return m_pInvBox; };
    // dar_ui_open trade: the partner may stand anywhere until the menu closes
    void SetDistanceExempt(bool exempt) { m_distanceExempt = exempt; }
private:
    bool m_distanceExempt{};
    void PropertiesBoxForSlots(PIItem item, bool& b_show);
    void PropertiesBoxForWeapon(CUICellItem* cell_item, PIItem item, bool& b_show);
    void PropertiesBoxForAddon(PIItem item, bool& b_show);
    void PropertiesBoxForUsing(PIItem item, bool& b_show);
    void PropertiesBoxForPlaying(PIItem item, bool& b_show);
    void PropertiesBoxForDrop(CUICellItem* cell_item, PIItem item, bool& b_show);
    void PropertiesBoxForRepair(PIItem item, bool& b_show);
    void PropertiesBoxForDonate(PIItem item, bool& b_show); //Alundaio

private:
    void clear_highlight_lists();
    void set_highlight_item(CUICellItem* cell_item);
    void highlight_item_slot(CUICellItem* cell_item);
    void highlight_armament(PIItem item, CUIDragDropListEx* ddlist);
    void highlight_ammo_for_weapon(PIItem weapon_item, CUIDragDropListEx* ddlist);
    void highlight_weapons_for_ammo(PIItem ammo_item, CUIDragDropListEx* ddlist);
    bool highlight_addons_for_weapon(PIItem weapon_item, CUICellItem* ci);
    void highlight_weapons_for_addon(PIItem addon_item, CUIDragDropListEx* ddlist);
    void highlight_equipped() const;

protected:
    void Construct();
    void InitializeUniversal(CUIXml& uiXml);

    // Old style UI
    void InitializeInventoryMode(CUIXml& uiXml);
    void InitializeTradeMode(CUIXml& uiXml);
    void InitializeSearchLootMode(CUIXml& uiXml);
    // Old style UI

    void InitSounds(CUIXml& uiXml);
    void InitAllowedDrops();
    void InitCallbacks();
    void InitInventoryTools();
    void InitHotkeys(CUIXml& uiXml);

    void SyncInventoryTools();
    void ResetInventoryTools();
    void OnInventoryToolClicked(CUIWindow* w, void* d);
    // the order every grid is laid out in (ui_inv_sort), applied to the grids on screen at once
    void SetSortMode(EInventorySort mode);
    // true when a drop-down of orders was open
    bool CloseSortModes();
    bool CellPassesItemFilter(CUICellItem* itm);
    // the pane a grid shows, false for a slot, the belt, a trade table
    bool PaneOfList(CUIDragDropListEx* list, EInventoryPane& pane);
    // the inventory a pane's new items go to, null when the player may not change it
    CGameObject* PaneOwner(EInventoryPane pane);

    // hotkeys
    u8 HotkeyWindow() const;
    void SyncHotkeys();
    CUICellItem* CellUnderCursor();
    bool OverHotkeysPanel() const;
    bool TryHotkey(EHotkeyTrigger trigger, int key = 0);
    bool TryHoverHotkey(CUICellItem* itm);
    void RunPendingHover();
    // the cell of a grid holding this item, itself or grouped under it
    CUICellItem* CellOfItem(u16 id);
    // a window over the grids takes the mouse: the comparison, a drop-down of orders
    bool GridsCovered() const;
    // a hotkey's action, an action button's: the window's own, or one for the item of `cell`
    bool RunWindowAction(const shared_str& action, CUICellItem* cell);
    void OnHotkeysPanelClose(CUIWindow*, void*);
    // one item of a cell to where a double click would send it, the way the window does it
    bool MoveToOtherSide(CUICellItem* itm, bool equip);
    bool MoveCell(CUICellItem* cell, bool whole_stack);
    bool SplitCell(CUICellItem* cell);
    bool DropCell(CUICellItem* cell, bool whole_stack);
    // the comparison
    void TogglePick(CUICellItem* cell);
    void ClearPicks();
    bool IsPicked(CUICellItem* cell) const;
    u8 CellPickState(CUICellItem* cell);
    void PrunePicks();
    void OpenCompare();
    u32 ComparePrice(CInventoryItem& item);
    void UpdatePristineInfo(CUICellItem* cell_item);
    void UpdatePendingSort();
    void RefillInventoryPane(EInventoryPane pane, const xr_vector<u16>& skip);
    CUIDragDropListEx* GetInventoryPaneList(EInventoryPane pane);

    void InitCellForSlot(u16 slot_idx);
    void InitInventoryContents(CUIDragDropListEx* pBagList, bool onlyBagList = false);
    void ClearAllLists();

    void BindDragDropListEvents(CUIDragDropListEx* lst);
    void RegisterCallback(CUIWindow* window, s16 event, const CUIWndCallback::void_function& callback);

    static bool ShowIfExist(CUIWindow* window, bool status); // sorry

    EDDListType GetListType(CUIDragDropListEx* l);

public:
    CUIDragDropListEx* GetListByType(EDDListType t); //Alundaio: Made public

protected:
    CUIDragDropListEx* GetSlotList(u16 slot_idx);
    bool CanSetItemToList(PIItem item, CUIDragDropListEx* l, u16& ret_slot);

    xr_vector<EDDListType> m_allowed_drops[iListTypeMax];
    bool AllowItemDrops(EDDListType from, EDDListType to);

    bool OnItemDrop(CUICellItem* itm);
    bool DropItemOnAnotherItem(EDDListType t_old, EDDListType t_new, CUIDragDropListEx* old_owner, CUIDragDropListEx* new_owner);
    bool OnItemStartDrag(CUICellItem* itm);
    bool OnItemDbClick(CUICellItem* itm);
    bool OnItemSelected(CUICellItem* itm);
    bool OnItemRButtonClick(CUICellItem* itm);
    bool OnItemFocusReceive(CUICellItem* itm);
    bool OnItemFocusLost(CUICellItem* itm);
    bool OnItemFocusedUpdate(CUICellItem* itm);
    void OnDragItemOnTrash(CUIDragItem* item, bool b_receive);
    bool OnItemDropped(PIItem itm, CUIDragDropListEx* new_owner, CUIDragDropListEx* old_owner);

    void ResetMode();
    void InitInventoryMode();
    void DeInitInventoryMode();
    void InitTradeMode();
    void DeInitTradeMode();
    void InitUpgradeMode();
    void DeInitUpgradeMode();
    void InitDeadBodySearchMode();
    void DeInitDeadBodySearchMode() const;

    void InitActorInfo();
    void InitPartnerInfo();

    void CurModeToScript();
    void RepairEffect_CurItem();

public:
    //Alundaio: Made public
    void SetCurrentItem(CUICellItem* itm);
    CUICellItem* CurrentItem();

protected:
    PIItem CurrentIItem();

    void InfoCurItem(CUICellItem* cell_item); // on update item
    CUIItemInfo* GetModeSpecificItemInfo();
    CUICharacterInfo* GetModeSpecificActorInfo() const;
    CUICharacterInfo* GetModeSpecificPartnerInfo(EMenuMode fallback) const;

    void ActivatePropertiesBox();
    void TryHidePropertiesBox();
    void ProcessPropertiesBoxClicked(CUIWindow* w, void* d);

    void CheckDistance();
    void UpdateItemsPlace();

    void SetupUpgradeItem();
    void UpdateUpgradeItem();
    void TrySetCurUpgrade();
    void UpdateButtonsLayout();

    // inventory
    bool ToSlot(CUICellItem* itm, bool force_place, u16 slot_id);
    bool ToBag(CUICellItem* itm, bool b_use_cursor_pos);
    bool ToBelt(CUICellItem* itm, bool b_use_cursor_pos);
    bool TryUseItem(CUICellItem* cell_itm);
    bool ToQuickSlot(CUICellItem* itm);

    void UpdateActorMP();
    void UpdateOutfit();
    void MoveArtefactsToBag();
    bool TryActiveSlot(CUICellItem* itm);
    void TryRepairItem(CUIWindow* w, void* d);
    bool CanUpgradeItem(PIItem item);

    bool ToActorTrade(CUICellItem* itm, bool b_use_cursor_pos);
    bool ToPartnerTrade(CUICellItem* itm, bool b_use_cursor_pos);
    bool ToPartnerTradeBag(CUICellItem* itm, bool b_use_cursor_pos);
    bool ToDeadBodyBag(CUICellItem* itm, bool b_use_cursor_pos);

    void AttachAddon(PIItem item_to_upgrade);
    void DetachAddon(LPCSTR addon_name, PIItem itm = NULL);

    void SendEvent_Item2Slot(PIItem pItem, u16 parent, u16 slot_id);
    void SendEvent_Item2Belt(PIItem pItem, u16 parent);
    void SendEvent_Item2Ruck(PIItem pItem, u16 parent);
    void SendEvent_Item_Drop(PIItem pItem, u16 parent);
    void SendEvent_Item_Eat(PIItem pItem, u16 parent);
    void SendEvent_ActivateSlot(u16 slot, u16 recipient);
    void DropAllCurrentItem();
    void OnPressUserKey(bool take);

    // trade
    void InitPartnerInventoryContents();
    void ColorizeItem(CUICellItem* itm, bool colorize);
    float CalcItemsWeight(CUIDragDropListEx* pList);
    u32 CalcItemsPrice(CUIDragDropListEx* pList, CTrade* pTrade, bool bBuying);
    void UpdatePrices();
    bool CanMoveToPartner(PIItem pItem);
    void TransferItems(
        CUIDragDropListEx* pSellList, CUIDragDropListEx* pBuyList, CTrade* pTrade, bool bBuying, bool bFree = false);

public:
    CUIActorMenu();
    virtual ~CUIActorMenu();

    virtual bool StopAnyMove();
    bool NeedCenterCursor() const override;
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);
    virtual void Draw();
    virtual void Update();
    virtual void Show(bool status);
    void ShowDialog(bool bDoHideIndicators) override;

    virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);

    void ShowMessage(pcstr message, pcstr staticMessage = nullptr, float staticMsgTtl = -1.0f);

    void CallMessageBoxYesNo(LPCSTR text);
    void CallMessageBoxOK(LPCSTR text);
    void OnMesBoxYes(CUIWindow*, void*);
    void OnMesBoxNo(CUIWindow*, void*);

    void OnInventoryAction(PIItem pItem, u16 action_type);
    void ShowRepairButton(bool status);
    bool SetInfoCurUpgrade(Upgrade_type* upgrade_type, CInventoryItem* inv_item);
    void SeparateUpgradeItem();
    PIItem get_upgrade_item();
    bool DropAllItemsFromRuck(bool quest_force = false); // debug func

    void UpdateActor();
    void UpdatePartnerBag();
    void UpdateDeadBodyBag();

    void OnBtnPerformTrade(CUIWindow* w, void* d);
    void OnBtnPerformTradeBuy(CUIWindow* w, void* d);
    void OnBtnPerformTradeSell(CUIWindow* w, void* d);
    void OnBtnExitClicked(CUIWindow* w, void* d);
    void TakeAllFromPartner(CUIWindow* w, void* d);
    void StoreAllToPartner(CUIWindow* w, void* d);
    void TakeAllFromInventoryBox();
    void StoreAllToInventoryBox();

    IC UIHint* get_hint_wnd() { return m_hint_wnd; }

    // the panel of hotkeys (ui_inv_hotkeys keeps whether it is up)
    void ShowHotkeysPanel(bool show);
    bool HotkeysPanelShown() const;
    // the console's and the scripts' way to the hotkeys and the comparison
    bool RunHotkeyAction(pcstr action);
    // an item in or out of the comparison; false when its kind is not the picked one's or it is full
    bool PickItem(CInventoryItem* item);
    u32 PickedCount() const { return u32(m_picks.size()); }
    void ClearComparison() { ClearPicks(); }
    void OpenComparison() { OpenCompare(); }
    // the centre of the first cell on screen holding an item of `section` (dar_ui_cursor_item)
    bool FindCellCenter(pcstr section, Fvector2& at);

    // The sort and filter row: what its buttons do, open to scripts as well.
    bool SortInventoryPane(EInventoryPane pane);
    void SetItemFilter(EItemCategory category);
    EItemCategory GetItemFilter() const { return m_item_filter; }

	//AxelDominator && Alundaio consumable use condition
	void RefreshCurrentItemCell();

	CScriptGameObject* GetCurrentItemAsGameObject();
	void HighlightSectionInSlot(pcstr section, EDDListType type, u16 slot_id = 0);
	void HighlightForEachInSlot(const luabind::functor<bool>& functor, EDDListType type, u16 slot_id);
    bool ToSlotScript(CScriptGameObject* GO, bool force_place, u16 slot_id);
    bool ToBeltScript(CScriptGameObject* GO, bool b_use_cursor_pos);

	//-AxelDominator && Alundaio consumable use condition
	void DonateCurrentItem(CUICellItem* cell_item); //Alundaio: Donate item via context menu while in trade menu

    pcstr GetDebugType() override { return "CUIActorMenu"; }

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIDialogWnd);
}; // class CUIActorMenu
