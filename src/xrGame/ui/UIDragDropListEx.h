#pragma once

#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Callbacks/UIWndCallback.h"
#include "xrUICore/Static/UIShapeStyle.h"

class CUICellContainer;
class CUIScrollBar;
class CUIStatic;
class CUICellItem;
class CUIDragItem;

struct CUICell
{
    CUICell()
    {
        m_item = NULL;
        Clear();
    }

    CUICellItem* m_item;
    bool m_bMainItem;

    void SetItem(CUICellItem* itm, bool bMain)
    {
        m_item = itm;
        VERIFY(m_item);
        m_bMainItem = bMain;
    }
    bool Empty() { return m_item == NULL; }
    bool MainItem() { return m_bMainItem; }
    void Clear();
    bool operator==(const CUICell& C) const { return (m_item == C.m_item); }
};

// The cells of a grid drawn by the xms_ui_shape shader (the layout's <cells> node): an empty cell,
// the plate under an item, the item selected, marked (equipped, the quick slot's own), related (the
// ammunition and addons of the selected weapon), blocked (what a trade or a script refuses), and the
// area a dragged item would take. A grid without them keeps its ui_grid texture.
// Picked and dim are drawn over the icons of any grid, textured or not: an item picked for a
// comparison, and the items that cannot join it.
struct SUIGridLooks
{
    enum Look : u8
    {
        Empty,
        Item,
        Selected,
        Marked,
        Related,
        Blocked,
        Drop,
        Picked,
        Dim,
        Count
    };
    SUIShapeStyle looks[Count];

    // the overlays of a grid without <cells>: the theme's cell_picked and cell_dim
    static const SUIGridLooks& Overlays();
};

typedef xr_vector<CUICell> UI_CELLS_VEC;
typedef UI_CELLS_VEC::iterator UI_CELLS_VEC_IT;

class CUIDragDropListEx : public CUIWindow, public CUIWndCallback
{
private:
    typedef CUIWindow inherited;

    enum
    {
        flGroupSimilar = (1 << 0),
        flAutoGrow = (1 << 1),
        flCustomPlacement = (1 << 2),
        flVerticalPlacement = (1 << 3),
        flAlwaysShowScroll = (1 << 4),
        flVirtualCells = (1 << 5),
    };
    Flags8 m_flags;
    Ivector2 m_orig_cell_capacity;
    Ivector2 m_max_cell_capacity;
    Ivector2 m_virtual_cells_alignment;
    Fvector2 m_blocker_spacing;
    Fvector2 m_highlighter_spacing;
    bool m_bConditionProgBarVisible;

protected:
    CUICellItem* m_selected_item;
    CUICellContainer* m_container;
    CUIScrollBar* m_vScrollBar;
    CUIStatic* m_highlighter{};
    CUIStatic* m_blocker{};
    CUIProgressBar* m_condition_indicator{};

    virtual void OnScrollV(CUIWindow* w, void* pData);
    virtual void OnItemStartDragging(CUIWindow* w, void* pData);
    virtual void OnItemDrop(CUIWindow* w, void* pData);
    virtual void OnItemSelected(CUIWindow* w, void* pData);
    virtual void OnItemLButtonClick(CUIWindow* w, void* pData);
    virtual void OnItemRButtonClick(CUIWindow* w, void* pData);
    virtual void OnItemDBClick(CUIWindow* w, void* pData);
    virtual void OnItemFocusReceived(CUIWindow* w, void* pData);
    virtual void OnItemFocusLost(CUIWindow* w, void* pData);
    virtual void OnItemFocusedUpdate(CUIWindow* w, void* pData);

public:
    static CUIDragItem* m_drag_item;
    CUIDragDropListEx();
    virtual ~CUIDragDropListEx();
    void InitDragDropList(Fvector2 pos, Fvector2 size);

    typedef fastdelegate::FastDelegate1<CUICellItem*, bool> DRAG_CELL_EVENT;
    typedef fastdelegate::FastDelegate2<CUIDragItem*, bool, void> DRAG_ITEM_EVENT;
    typedef fastdelegate::FastDelegate1<CUICellItem*, bool> CELL_FILTER;
    // what lies over a cell's icon: ECellState bits
    typedef fastdelegate::FastDelegate1<CUICellItem*, u8> CELL_STATE;
    enum ECellState : u8
    {
        cellPicked = 1 << 0,
        cellDimmed = 1 << 1,
    };

    DRAG_CELL_EVENT m_f_item_drop;
    DRAG_CELL_EVENT m_f_item_start_drag;
    DRAG_CELL_EVENT m_f_item_db_click;
    DRAG_CELL_EVENT m_f_item_selected;
    DRAG_CELL_EVENT m_f_item_lbutton_click;
    DRAG_CELL_EVENT m_f_item_rbutton_click;
    DRAG_CELL_EVENT m_f_item_focus_received;
    DRAG_CELL_EVENT m_f_item_focus_lost;
    DRAG_CELL_EVENT m_f_item_focused_update;
    DRAG_ITEM_EVENT m_f_drag_event;

    u32 back_color;

    const Ivector2& CellsCapacity();
    void SetCellsCapacity(const Ivector2 c);
    void SetStartCellsCapacity(const Ivector2 c)
    {
        m_orig_cell_capacity = c;
        SetCellsCapacity(c);
    };
    void ResetCellsCapacity()
    {
        VERIFY(ItemsCount() == 0);
        SetCellsCapacity(m_orig_cell_capacity);
    };

    void SetMaxCellsCapacity(const Ivector2& c);
    const Ivector2& MaxCellsCapacity() const
    {
        return m_max_cell_capacity;
    }

    const Ivector2& CellSize();
    void SetCellSize(const Ivector2 new_sz);
    const Ivector2& CellsSpacing();
    void SetCellsSpacing(const Ivector2& new_sz);
    void SetCellsVertAlignment(xr_string alignment);
    void SetCellsHorizAlignment(xr_string alignment);

    const Ivector2 GetVirtualCellsAlignment() { return m_virtual_cells_alignment; };
    int ScrollPos();
    void ReinitScroll();
    void GetClientArea(Frect& r);
    Fvector2 GetDragItemPosition();

    void SetAutoGrow(bool b);
    bool IsAutoGrow();
    void SetGrouping(bool b);
    bool IsGrouping();
    void SetCustomPlacement(bool b);
    bool GetCustomPlacement();
    void SetVerticalPlacement(bool b);
    bool GetVerticalPlacement();
    void SetAlwaysShowScroll(bool b);
    bool GetVirtualCells();
    void SetVirtualCells(bool b);

    void SetHighlighter(CUIStatic* highlighter, Fvector2 spacing, bool convertPosToOurs = true);
    CUIStatic* GetHighlighter() const
    {
        return m_highlighter;
    }
    const Fvector2& GetHighlighterSpacing() const
    {
        return m_highlighter_spacing;
    }
    void Highlight(bool highlight);

    void SetBlocker(CUIStatic* blocker, Fvector2 spacing, bool convertPosToOurs = true);
    CUIStatic* GetBlocker() const
    {
        return m_blocker;
    }
    const Fvector2& GetBlockerSpacing() const
    {
        return m_blocker_spacing;
    }

    void SetConditionIndicator(CUIProgressBar* indicator, bool convertPosToOurs = true);
    CUIProgressBar* GetConditionIndicator() const
    {
        return m_condition_indicator;
    }

    Ivector2 CalculateCapacity(int desiredCells);

    bool GetConditionProgBarVisibility() { return m_bConditionProgBarVisible; };
    void SetConditionProgBarVisibility(bool b) { m_bConditionProgBarVisible = b; };

    void SetGridLooks(std::unique_ptr<SUIGridLooks> looks) { m_grid_looks = std::move(looks); }
    const SUIGridLooks* GetGridLooks() const { return m_grid_looks.get(); }
    // the scroll_bar.xml (or scroll_bar_<name>.xml) profile of the grid's scroll bar
    void SetScrollProfile(pcstr profile) { m_scroll_profile = profile; }

private:
    std::unique_ptr<SUIGridLooks> m_grid_looks;
    shared_str m_scroll_profile{ "default" };

public:
public:
    // items management
    virtual void SetItem(CUICellItem* itm); // auto
    virtual void SetItem(CUICellItem* itm, Fvector2 abs_pos); // start at cursor pos
    virtual void SetItem(CUICellItem* itm, Ivector2 cell_pos); // start at cell
    bool CanSetItem(CUICellItem* itm);

    u32 ItemsCount();
    CUICellItem* GetItemIdx(u32 idx);
    virtual CUICellItem* RemoveItem(CUICellItem* itm, bool force_root);
    void CreateDragItem(CUICellItem* itm);

    void DestroyDragItem();
    void ClearAll(bool bDestroy);
    void Compact();
    bool IsOwner(CUICellItem* itm);
    void clear_select_armament();
    Ivector2 PickCell(const Fvector2& abs_pos);
    CUICell& GetCellIdx(size_t idx);
    CUICell& GetCellAt(const Ivector2& pos);
    CUICellContainer* GetContainer() { return m_container; }; //Alundaio

    // A visual filter. Rejected cells stay in the list - counted, priced, weighed and removable
    // like any other - but leave the grid, and the accepted ones are packed from the top. An
    // empty filter brings the layout the list had before filtering back as far as it still fits.
    void SetCellFilter(const CELL_FILTER& filter);
    bool HasCellFilter() const { return !m_cell_filter.empty(); }
    void SetCellState(const CELL_STATE& state) { m_cell_state = state; }
    u8 CellState(CUICellItem* itm) const { return m_cell_state.empty() ? 0 : m_cell_state(itm); }

    // The root cells a bulk move takes: every one, or under a filter only those on screen.
    xr_vector<CUICellItem*> GetShownItems();
    // Takes these root cells out of the list and deletes them with their grouped children.
    void DestroyItems(const xr_vector<CUICellItem*>& cells);

private:
    CELL_FILTER m_cell_filter;
    CELL_STATE m_cell_state;
    xr_vector<std::pair<CUICellItem*, Ivector2>> m_unfiltered_layout;

    bool PassesFilter(CUICellItem* itm) const { return m_cell_filter.empty() || m_cell_filter(itm); }
    void ParkItem(CUICellItem* itm);
    void Relayout(bool restore_layout);
    void ForgetLayout(CUICellItem* itm);

public:
    // UIWindow overriding
    virtual void Draw();
    virtual void Update();
    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);

    void OnDragEvent(CUIDragItem* drag_item, bool b_receive);

    pcstr GetDebugType() override { return "CUIDragDropListEx"; }
};

class CUICellContainer final : public CUIWindow
{
    friend class CUIDragDropListEx;
    friend class CUIDragDropReferenceList;

private:
    typedef CUIWindow inherited;
    ui_shader hShader;
    UI_CELLS_VEC m_cells_to_draw;

protected:
    CUIDragDropListEx* m_pParentDragDropList;

    Ivector2 m_cellsCapacity;        // count      (col, row)
    Ivector2 m_cellSize;             // pixels (width, height)
    Ivector2 m_cellSpacing;          // pixels (width, height)

    UI_CELLS_VEC m_cells;

    void GetTexUVLT(Fvector2& uv, u32 col, u32 row, u8 select_mode);
    void ReinitSize();
    u32 GetCellsInRange(const Irect& rect, UI_CELLS_VEC& res);

public:
    CUICellContainer(CUIDragDropListEx* parent);

    Ivector2 PickCell(const Fvector2& abs_pos); //Alundaio made public
    bool ValidCell(const Ivector2& pos) const; //Alundaio made public
    CUICell& GetCellAt(const Ivector2& pos); //Alundaio made public

    pcstr GetDebugType() override { return "CUICellContainer"; }

protected:
    virtual void Draw();
    void DrawBlocker() const;
    // the cells as shapes, in place of the ui_grid texture
    void DrawShapeCells(const Irect& visible, bool dragArea, const Ivector2& dragCell, const Ivector2& dragSize);
    // picked and dimmed items, over their icons
    void DrawCellStates();
    xr_vector<Fvector2> m_shape_centres;
    xr_vector<CUICellItem*> m_shape_items;

    IC const Ivector2& CellsCapacity() { return m_cellsCapacity; };
    void SetCellsCapacity(const Ivector2& c);
    IC const Ivector2& CellSize() { return m_cellSize; };
    void SetCellSize(const Ivector2& new_sz);
    IC const Ivector2& CellsSpacing() { return m_cellSpacing; };
    void SetCellsSpacing(const Ivector2& new_sz);
    Ivector2 TopVisibleCell();
    CUICell& GetCellIdx(size_t idx);
    Ivector2 GetItemPos(CUICellItem* itm);
    Ivector2 FindFreeCell(const Ivector2& size);
    bool HasFreeSpace(const Ivector2& size);
    bool IsRoomFree(const Ivector2& pos, const Ivector2& size);

    bool AddSimilar(CUICellItem* itm);
    CUICellItem* FindSimilar(CUICellItem* itm);

    void PlaceItemAtPos(CUICellItem* itm, Ivector2& cell_pos);
    CUICellItem* RemoveItem(CUICellItem* itm, bool force_root);

    void Grow();
    void Shrink();
    void ClearAll(bool bDestroy);
    void clear_select_armament();
};
