// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// A module's own PDA interface (XmsPda): the screens a .pdaasset declares, drawn inside
// the device's screen, with a live map in a slot of them.
//
// The layout arrives as a flat item list from xms_pda.script (canvas units, colours
// resolved, texts in cp1251); this class only draws it and makes it react. Every item is
// one CUIPdaItem window under its parent's window, so the stock window tree does the
// clipping, the input and the drawing order. Three roots: the chrome and the pages go
// under the stock section a stock page shows, the overlay stays above everything.

#include "xrUICore/Windows/UIWindow.h"
#include "xrScriptEngine/script_space_forward.hpp"

class CUIPdaWnd;
class CUIMapWnd;
class CUIPdaItem;

namespace XMS
{
struct Module;
}

class CUIPdaLayout
{
public:
    enum ELayer : u8
    {
        lrChrome,
        lrPage,
        lrOverlay,
    };

    // One item of the layout, as xms_pda_layout.normalize describes it.
    struct SItem
    {
        xr_string kind; // rect image text button list group map row
        ELayer layer{lrChrome};
        xr_string page;
        int parent{0}; // 1-based index into the same list; 0 = the layer (or the list for rows)
        xr_string id;
        Frect rect{}; // canvas units, relative to the parent
        bool visible{true};

        u32 color{0}, hover{0}, press{0}, selected{0}, border{0}, tint{0xFFFFFFFF};
        bool has_hover{false}, has_press{false}, has_selected{false}, has_border{false};
        float radius{0}, border_w{0};

        xr_string texture;
        bool stretch{true};

        xr_string text, font, hint;
        u32 text_color{0xFFFFFFFF}, text_hover{0}, text_selected{0};
        bool has_text_hover{false}, has_text_selected{false}, has_text{false};
        char align{'l'}, valign{'c'};
        bool wrap{false};

        xr_string icon;
        float icon_size{0};
        u32 icon_color{0xFFFFFFFF}, icon_selected{0};
        bool has_icon_selected{false};

        xr_vector<xr_string> action;
        xr_vector<xr_string> selected_on;

        float row_h{0}, gap{0};
        xr_string empty;
        int row{0};

        xr_string zoom;
    };

    explicit CUIPdaLayout(CUIPdaWnd& pda);
    ~CUIPdaLayout();

    // Reads the owner's layout (xms_pda.build) and builds every window under `host`, inside
    // `screen` (host coordinates). False: the layout could not be read - the stock PDA stays.
    bool Build(const XMS::Module& owner, CUIWindow* host, const Frect& screen);

    void OnShow();
    void OnHide();
    void Update();
    void DrawHint();

    // map / tasks / contacts: a page or a stock section, as the layout's keys say
    void OpenKey(pcstr key);
    // Stock code asking the PDA for a section while a layout is up: the map key of Dead Air
    // asks for eptTasks, the contacts key for eptContacts, a tutorial for anything. The two
    // screens a layout replaces go where its keys say; any other section is shown as is,
    // with the overlay above it.
    void OpenStock(pcstr section);
    bool Page(pcstr id);
    // the page the PDA shows, or opens on when it has not been shown yet
    pcstr CurrentPage() const { return m_page.empty() ? m_start.c_str() : m_page.c_str(); }
    // 1 show, 0 hide, -1 toggle
    bool ShowItem(pcstr id, int mode);
    bool SetText(pcstr id, pcstr text);
    bool SetRows(pcstr id, const luabind::adl::object& items);
    // the action of an item, as if it was clicked (QA and scripts); a list's `row` (1-based)
    // is its row: the row's actions run and it becomes the list's selection
    bool Click(pcstr id, int row = 0);
    void Describe(xr_string& out) const;

    pcstr Id() const { return m_id.c_str(); }
    bool KeepNoise() const { return m_noise; }
    bool KeepClose() const { return m_close; }
    bool ShowsStock() const { return !m_stock.empty(); }

    // ---- for the items ----------------------------------------------------------
    void OnItemClick(CUIPdaItem* item);
    bool IsSelected(const CUIPdaItem* item) const;
    // the text with every {placeholder} filled in
    void Format(pcstr raw, xr_string& out) const;
    // a var's value from the layout's own `vars`, for a name no script has set
    bool DefaultVar(pcstr name, xr_string& out) const;
    float Scale() const { return m_scale.y; }
    // virtual units per canvas unit, per axis
    const Fvector2& Scale2() const { return m_scale; }

    static bool ReadItems(const luabind::adl::object& list, xr_vector<SItem>& out, xr_string& err);

private:
    struct SPage
    {
        xr_string id, title, stock;
        CUIWindow* root{};
    };

    CUIPdaWnd& m_pda;
    xr_string m_id, m_title, m_module;
    Fvector2 m_canvas{960.f, 680.f};
    Frect m_screen{};
    Fvector2 m_scale{1.f, 1.f};
    bool m_noise{true}, m_close{true};

    CUIWindow* m_host{};
    // not auto-deleted: the overlay is re-attached above a stock section, and a detached
    // auto-delete window would be destroyed by DetachChild
    CUIWindow* m_under{};
    CUIWindow* m_chrome{};
    CUIWindow* m_over{};
    xr_vector<SPage> m_pages;
    CUIMapWnd* m_map{};
    xr_string m_map_page;
    bool m_map_global{false};  // the map widget's zoom = "global": the whole zone at first
    bool m_map_opened{false};

    xr_vector<CUIPdaItem*> m_items;           // every item window, rows included
    xr_vector<CUIPdaItem*> m_dynamic;         // items whose text has {placeholders}
    xr_string m_page, m_prev_page, m_start, m_stock;
    xr_string m_key_map, m_key_tasks, m_key_contacts;
    xr_map<xr_string, xr_string> m_defaults;
    u32 m_next_format{0};
    bool m_shown{false};

    CUIWindow* CreateItems(const xr_vector<SItem>& items, CUIWindow* list_parent);
    CUIPdaItem* CreateItem(const SItem& item, CUIWindow* parent);
    CUIMapWnd* CreateMap(const SItem& item, CUIWindow* parent);
    CUIPdaItem* FindItem(pcstr id) const;
    void Scaled(const Frect& canvas, Frect& out) const;
    void RunAction(const xr_string& action, CUIPdaItem* item);
    void ApplyPage();
    void RefreshTexts(bool force);
    void ShowStock(pcstr section);
    void HideStock();
    // stock:<section> of the layout itself: its page for that section, or the section
    void StockAction(pcstr section);
    void RaiseOverlay();
    void ShowMap(bool on);
    void Forget(CUIWindow* w);
};

// One item of a layout: a (rounded) rectangle, an image, a text, a button, a list, a row,
// a group, or the frame a map sits in.
class CUIPdaItem final : public CUIWindow
{
    using inherited = CUIWindow;

public:
    CUIPdaItem(CUIPdaLayout& owner, const CUIPdaLayout::SItem& item);

    const CUIPdaLayout::SItem& Item() const { return m_item; }
    CUIPdaLayout::SItem& Item() { return m_item; }

    void SetLabel(pcstr text); // cp1251, placeholders already filled in
    void SetRowSelected(bool v) { m_row_selected = v; }
    bool RowSelected() const { return m_row_selected; }

    // list: where the rows go, and the "nothing here" line
    class CUIScrollView* Rows() const { return m_rows; }
    void SetEmptyShown(bool v);

    // the item's own parts, made once by the layout
    void InitParts();

    void Draw() override;
    void Update() override;
    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    void OnFocusLost() override;
    void Show(bool status) override;

    pcstr GetDebugType() override { return "CUIPdaItem"; }

private:
    CUIPdaLayout& m_owner;
    CUIPdaLayout::SItem m_item;
    class CUIStatic* m_image{};
    class CUIStatic* m_label{};
    class CUIScrollView* m_rows{};
    class CUIStatic* m_empty{};
    bool m_pressed{false};
    bool m_row_selected{false};

    bool Clickable() const;
    u32 BackColor() const;
    // the fill in `color`, or (border) the outline the item declares
    void DrawShape(u32 color, bool border) const;
};
