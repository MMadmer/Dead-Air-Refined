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

    pcstr GetDebugType() override { return "CUIInventoryToolbar"; }

private:
    CUI3tButton* AddButton(CUIXml& xml, pcstr node, pcstr hint_attr, const Fvector2& pos, const Fvector2& size);

    EInventoryPane m_pane;
    CUIDragDropListEx* m_list;
    CUI3tButton* m_sort{};
    CUI3tButton* m_filters[u8(EItemCategory::Count)]{};
    xr_vector<std::pair<CUI3tButton*, CUIStatic*>> m_glyphs;
    EItemCategory m_active{ EItemCategory::All };
    float m_step{};
    float m_sort_width{};
    bool m_sort_shown{};

    u32 m_color_normal{};
    u32 m_color_hover{};
    u32 m_color_active{};
};
