// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Windows/UIWindow.h"

class CUI3tButton;
class CUIStatic;
class CUIXml;
class CUIDragDropListEx;
class CInventoryItem;
class CGameObject;

// [DA] The sort and filter row of the actor menu. Every item grid of the inventory, the loot and
// the trade window gives up its top row of cells to one of these, so the row is part of the
// layout of whatever actor_menu xml is installed and covers nothing. The look is data:
// actor_menu_tools.xml; the stack lists are data too: dead_air_x64_inventory_tools.ltx.

enum class EInventoryPane : u8
{
    Actor, // the player's rucksack
    Loot, // a corpse or a box
    Trader, // the trader's goods
};

enum class EItemCategory : u8
{
    All,
    Weapons, // weapons, ammunition, grenades, weapon addons
    Armour, // outfits, helmets, backpacks
    Food, // food and drink
    Medicine, // medkits, bandages, drugs, anti-radiation
    Artefacts, // artefacts and artefact containers
    Misc, // everything else
    Count
};

// The order the item grids are laid out in: whenever a window fills them and whenever the player
// sorts. One order for every grid, kept in user.ltx (ui_inv_sort).
enum class EInventorySort : u8
{
    Default, // the bigger items first, the way the game always laid them out
    Type, // by EItemCategory, then the default order
    Name,
    Weight, // the heaviest first
    Price, // the dearest first
    Condition, // the best kept first
    Count
};

namespace inventory_tools
{
// What an item is, read from its class and its section - never from a list of names, so an
// item a mod adds lands where its data says it belongs.
EItemCategory classify(CInventoryItem& item);

struct SSortPlan
{
    xr_vector<CInventoryItem*> release; // items the merge emptied
    xr_vector<shared_str> spawn; // numbered-section items that replace a merged stack
    u32 changed{}; // counts rewritten in place
};

// Merges partial ammo boxes, numbered-section stacks and multi-use items among `items`. Counts
// are rewritten at once; releasing and spawning is left to the caller, which has to take the
// released items off the screen first. keep_sections never trades one section for another;
// merge_ammo off leaves the ammo alone.
SSortPlan consolidate(const xr_vector<CInventoryItem*>& items, bool keep_sections, bool merge_ammo);

// The two sections a numbered-section item splits into: vodka_3 is vodka_2 and vodka.
bool split_family(const shared_str& section, shared_str& first, shared_str& second);

EInventorySort sort_mode();
void set_sort_mode(EInventorySort mode);
// the string id of a mode's caption
pcstr sort_mode_caption(EInventorySort mode);
// `items` in the current order; ties fall back on the default order, then on the section
void sort_items(xr_vector<CInventoryItem*>& items);
} // namespace inventory_tools

class CUIInventoryToolbar final : public CUIWindow
{
    using inherited = CUIWindow;

public:
    CUIInventoryToolbar(EInventoryPane pane, CUIDragDropListEx* list);

    // Takes whole rows of cells off the top of the list and sits in their place. False
    // leaves the list untouched.
    bool Init(CUIXml& xml);

    void SetActiveFilter(EItemCategory category);
    // The upgrade window filters its bag but does not sort it.
    void ShowSort(bool show);
    void Update() override;

    EInventoryPane Pane() const { return m_pane; }
    CUIDragDropListEx* List() const { return m_list; }
    CUI3tButton* SortButton() const { return m_sort; }
    CUI3tButton* FilterButton(EItemCategory category) const { return m_filters[u8(category)]; }

    // The drop-down of sort orders: its button in the row and its list, a window of the menu's
    // own so it lies over the grids and takes their clicks.
    CUI3tButton* SortModeButton() const { return m_mode; }
    CUIWindow* SortModeList() const { return m_mode_list; }
    CUI3tButton* SortModeRow(EInventorySort mode) const { return m_mode_rows[u8(mode)]; }
    void ShowSortModes(bool show);
    bool SortModesShown() const { return m_mode_list && m_mode_list->IsShown(); }
    // the caption and the checked row follow the current order
    void SyncSortMode();
    // A click anywhere but on the drop-down closes it.
    void OnMouseDown(const Fvector2& cursor);

    // The buttons at the right end of the row: <action action="..." glyph="..." hint="..."/> nodes,
    // the action one a hotkey could run (toggle_panel, compare, clear_picks, script:...).
    struct SAction
    {
        CUI3tButton* button{};
        shared_str action;
        shared_str hint; // a string id; %d takes the number of picked items
    };
    const xr_vector<SAction>& Actions() const { return m_actions; }
    // the action of one of these buttons, or null
    const shared_str* ActionOf(const CUIWindow* button) const;
    void EnableAction(pcstr action, bool enable);

    pcstr GetDebugType() override { return "CUIInventoryToolbar"; }

private:
    CUI3tButton* AddButton(CUIXml& xml, pcstr node, pcstr hint_attr, const Fvector2& pos, const Fvector2& size);
    CUI3tButton* AddButtonWith(CUIXml& xml, pcstr glyph, pcstr hint, const Fvector2& pos, const Fvector2& size);
    CUIStatic* AddGlyph(CUIXml& xml, CUIWindow* owner, pcstr glyph, const Fvector2& pos, const Fvector2& size);
    void InitSortModes(CUIXml& xml, const Fvector2& size, float fit);

    EInventoryPane m_pane;
    CUIDragDropListEx* m_list;
    CUI3tButton* m_sort{};
    CUI3tButton* m_filters[u8(EItemCategory::Count)]{};
    CUI3tButton* m_mode{};
    CUIStatic* m_mode_chevron{};
    CUIWindow* m_mode_list{};
    CUI3tButton* m_mode_rows[u8(EInventorySort::Count)]{};
    xr_vector<std::pair<CUI3tButton*, CUIStatic*>> m_glyphs;
    xr_vector<SAction> m_actions;
    EItemCategory m_active{ EItemCategory::All };
    float m_step{};
    float m_sort_width{};
    float m_mode_width{};
    bool m_sort_shown{};
    EInventorySort m_synced_mode{ EInventorySort::Count };

    u32 m_color_normal{};
    u32 m_color_hover{};
    u32 m_color_active{};
    u32 m_color_disabled{};
};
