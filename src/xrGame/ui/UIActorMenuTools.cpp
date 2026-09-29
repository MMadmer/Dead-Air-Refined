// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIActorMenuTools.h"

#include "UIDragDropListEx.h"
#include "UIXmlInit.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "xrUICore/ui_base.h"

#include "UIActorMenu.h"
#include "UICellItem.h"
#include "UICellItemFactory.h"
#include "UIDragDropReferenceList.h"
#include "UIInventoryUtilities.h"

#include "Actor.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "da_artefact_container.h"
#include "inventory_item.h"
#include "Inventory.h"
#include "InventoryBox.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "MPPlayersBag.h"
#include "eatable_item.h"
#include "Weapon.h"
#include "WeaponAmmo.h"
#include "WeaponBinoculars.h"
#include "Grenade.h"
#include "Scope.h"
#include "Silencer.h"
#include "GrenadeLauncher.h"
#include "CustomOutfit.h"
#include "ActorHelmet.h"
#include "ActorBackpack.h"
#include "Artefact.h"

namespace
{
constexpr pcstr tools_cfg = "dead_air_x64_inventory_tools.ltx";
constexpr pcstr sort_section = "inventory_sort";

using section_set = xr_flat_hash_map<shared_str, bool>;

bool g_loaded = false;
section_set g_stackable; // bases of numbered-section stacks
section_set g_addons; // sections some weapon names as its scope, silencer or launcher
xr_flat_hash_map<shared_str, u16> g_family_max;
xr_flat_hash_map<shared_str, EItemCategory> g_eatables;

void add_keys_of(const CInifile& ini, pcstr section, section_set& out)
{
    if (!ini.section_exist(section))
        return;
    for (const auto& item : ini.r_section(section).Data)
        out.emplace(item.first, true);
}

void load()
{
    g_loaded = true;

    // An addon is whatever a weapon section says it is. Scopes kept in another class, as the
    // scopes a script attaches, are found the same way.
    string256 name;
    for (CInifile::Sect* sect : pSettings->sections())
    {
        for (pcstr key : { "scopes", "scope_name", "silencer_name", "grenade_launcher_name" })
        {
            pcstr value = nullptr;
            if (!sect->line_exist(key, &value) || !value || !value[0])
                continue;
            const int count = _GetItemCount(value);
            for (int i = 0; i < count; ++i)
            {
                if (_GetItem(value, i, name)[0])
                    g_addons.emplace(shared_str(name), true);
            }
        }
    }

    string_path path;
    FS.update_path(path, "$game_config$", tools_cfg);
    if (!FS.exist(path))
        return;
    const CInifile ini(path, TRUE);

    // Where the installed game keeps its list of numbered-section stacks. The file and the
    // section are data because both belong to the game's own scripts.
    if (ini.line_exist(sort_section, "stack_source_file") && ini.line_exist(sort_section, "stack_source_section"))
    {
        string_path source;
        FS.update_path(source, "$game_config$", ini.r_string(sort_section, "stack_source_file"));
        if (FS.exist(source))
        {
            const CInifile from_game(source, TRUE);
            add_keys_of(from_game, ini.r_string(sort_section, "stack_source_section"), g_stackable);
        }
    }
    add_keys_of(ini, "inventory_sort_stackable", g_stackable);
}

void ensure_loaded()
{
    if (!g_loaded)
        load();
}

bool has(const section_set& set, const shared_str& section) { return set.find(section) != set.end(); }

float value(const shared_str& section, pcstr key) { return pSettings->read_if_exists<float>(section, key, 0.0f); }

// "base_2" .. "base_N" as long as they exist without a gap.
u16 family_max(const shared_str& base)
{
    if (const auto it = g_family_max.find(base); it != g_family_max.end())
        return it->second;

    string256 name;
    u16 maximum = 1;
    while (maximum < 999)
    {
        xr_sprintf(name, "%s_%u", base.c_str(), u32(maximum + 1));
        if (!pSettings->section_exist(name))
            break;
        ++maximum;
    }
    g_family_max.emplace(base, maximum);
    return maximum;
}

// The stack a section is part of: "vodka" holds one of the "vodka" stack, "vodka_3" three.
bool family_of(const shared_str& section, shared_str& base, u16& amount, u16& maximum)
{
    if (has(g_stackable, section))
    {
        base = section;
        amount = 1;
        maximum = family_max(section);
        return true;
    }

    pcstr name = section.c_str();
    pcstr tail = strrchr(name, '_');
    if (!tail || !tail[1])
        return false;
    for (pcstr c = tail + 1; *c; ++c)
    {
        if (!isdigit(u8(*c)))
            return false;
    }

    string256 head;
    const size_t length = size_t(tail - name);
    if (length >= sizeof(head))
        return false;
    std::memcpy(head, name, length);
    head[length] = 0;

    const shared_str candidate(head);
    if (!has(g_stackable, candidate) || !pSettings->section_exist(candidate.c_str()))
        return false;

    const int count = atoi(tail + 1);
    maximum = family_max(candidate);
    if (count < 1 || count > maximum)
        return false;

    base = candidate;
    amount = u16(count);
    return true;
}

shared_str family_section(const shared_str& base, u32 amount)
{
    if (amount == 1)
        return base;
    string256 name;
    xr_sprintf(name, "%s_%u", base.c_str(), amount);
    return shared_str(name);
}

EItemCategory classify_eatable(const shared_str& section)
{
    if (const auto it = g_eatables.find(section); it != g_eatables.end())
        return it->second;

    const auto result = [&section](EItemCategory category)
    {
        g_eatables.emplace(section, category);
        return category;
    };

    // Heals, stops bleeding, protects, purges a real dose of radiation or dulls hunger the way
    // drugs do: medicine, whatever food value it has on top.
    constexpr pcstr protection[] = { "boost_radiation_protection", "boost_telepat_protection",
        "boost_chemburn_protection", "boost_burn_immunity", "boost_shock_immunity", "boost_radiation_immunity",
        "boost_telepat_immunity", "boost_chemburn_immunity", "boost_strike_immunity", "boost_wound_immunity",
        "boost_explosion_immunity", "boost_fire_wound_immunity" };
    bool medical = value(section, "wounds_heal_perc") > 0.0f || value(section, "boost_health_restore") > 0.0f ||
        value(section, "boost_bleeding_restore") > 0.0f || value(section, "eat_health") >= 0.02f ||
        value(section, "boost_radiation_restore") >= 0.0002f || value(section, "eat_radiation") <= -0.05f ||
        value(section, "eat_satiety") <= -0.03f;
    for (pcstr key : protection)
        medical = medical || value(section, key) > 0.0f;
    if (medical)
        return result(EItemCategory::Medicine);

    if (value(section, "eat_satiety") > 0.0f || value(section, "eat_thirst") > 0.0f)
        return result(EItemCategory::Food);

    // Water and canteens: a sip of health and nothing a medicine does.
    if (value(section, "eat_health") > 0.0f)
        return result(EItemCategory::Food);

    // Alcohol and nothing else a drug would do: a drink.
    constexpr pcstr side_effects[] = { "eat_power", "eat_max_power", "eat_psy", "eat_sleepiness",
        "wounds_heal_perc", "boost_max_weight", "boost_power_restore" };
    bool side = false;
    for (pcstr key : side_effects)
        side = side || !fis_zero(value(section, key));
    if (value(section, "eat_alcohol") > 0.0f && !side)
        return result(EItemCategory::Food);

    constexpr pcstr other_effects[] = { "eat_health", "eat_radiation", "eat_alcohol", "eat_satiety",
        "boost_radiation_restore" };
    for (pcstr key : other_effects)
        side = side || !fis_zero(value(section, key));

    // An eatable with no effect at all is a tool some script does the work of.
    return result(side ? EItemCategory::Medicine : EItemCategory::Misc);
}

struct SStack
{
    xr_vector<CInventoryItem*> items;
    u32 total{};
    u32 maximum{};
};

using stack_map = xr_flat_hash_map<shared_str, SStack>;

void add_to(stack_map& stacks, const shared_str& key, CInventoryItem* item, u32 amount, u32 maximum)
{
    SStack& stack = stacks[key];
    stack.items.push_back(item);
    stack.total += amount;
    stack.maximum = maximum;
}

// Full stacks first, then the remainder, in the order the items were created.
template <typename Apply>
void redistribute(stack_map& stacks, Apply&& apply)
{
    for (auto& [key, stack] : stacks)
    {
        if (stack.items.size() < 2 || !stack.maximum)
            continue;

        std::sort(stack.items.begin(), stack.items.end(),
            [](CInventoryItem* left, CInventoryItem* right) { return left->object_id() < right->object_id(); });

        const u32 full = stack.total / stack.maximum;
        const u32 rest = stack.total % stack.maximum;
        const u32 keep = full + (rest ? 1 : 0);
        for (u32 i = 0; i < stack.items.size(); ++i)
            apply(stack.items[i], i < full ? stack.maximum : (i < keep ? rest : 0));
    }
}
} // namespace

EItemCategory inventory_tools::classify(CInventoryItem& item)
{
    ensure_loaded();

    const shared_str& section = item.object().cNameSect();
    if (smart_cast<CArtefact*>(&item) || da_af_container::is_container(section.c_str()))
        return EItemCategory::Artefacts;

    if (CWeapon* weapon = smart_cast<CWeapon*>(&item))
        return smart_cast<CWeaponBinoculars*>(weapon) ? EItemCategory::Misc : EItemCategory::Weapons;
    if (smart_cast<CWeaponAmmo*>(&item) || smart_cast<CGrenade*>(&item) || smart_cast<CScope*>(&item) ||
        smart_cast<CSilencer*>(&item) || smart_cast<CGrenadeLauncher*>(&item))
    {
        return EItemCategory::Weapons;
    }

    if (smart_cast<CCustomOutfit*>(&item) || smart_cast<CHelmet*>(&item) || smart_cast<CBackpack*>(&item))
        return EItemCategory::Armour;

    if (has(g_addons, section) || pSettings->line_exist(section.c_str(), "scope_texture"))
        return EItemCategory::Weapons;

    if (item.cast_eatable_item())
        return classify_eatable(section);

    return EItemCategory::Misc;
}

inventory_tools::SSortPlan inventory_tools::consolidate(
    const xr_vector<CInventoryItem*>& items, bool keep_sections, bool merge_ammo)
{
    ensure_loaded();

    SSortPlan plan;
    stack_map ammo_stacks, families, charges;
    for (CInventoryItem* item : items)
    {
        const shared_str& section = item->object().cNameSect();
        if (CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(item))
        {
            if (merge_ammo && ammo->m_boxSize > 1)
                add_to(ammo_stacks, section, item, ammo->m_boxCurr, ammo->m_boxSize);
            continue;
        }

        shared_str base;
        u16 amount, maximum;
        if (family_of(section, base, amount, maximum))
        {
            // Merging a family trades sections, which a trader's lists may not offer.
            if (!keep_sections && maximum > 1)
                add_to(families, base, item, amount, maximum);
            continue;
        }

        CEatableItem* eatable = item->cast_eatable_item();
        if (eatable && item->IsUsingCondition() && eatable->CanDelete() && eatable->GetMaxUses() > 1 &&
            eatable->GetRemainingUses() != u8(-1) && !pSettings->line_exist(section.c_str(), "next_use_item"))
        {
            add_to(charges, section, item, eatable->GetRemainingUses(), eatable->GetMaxUses());
        }
    }

    redistribute(ammo_stacks, [&plan](CInventoryItem* item, u32 target)
    {
        CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(item);
        if (ammo->m_boxCurr != target)
        {
            ammo->m_boxCurr = u16(target);
            if (item->m_pInventory)
                item->m_pInventory->InvalidateState();
            ++plan.changed;
        }
        // An emptied box goes, unless its section keeps empty boxes as things of their own.
        if (!target && !ammo->Useful())
            plan.release.push_back(item);
    });

    redistribute(charges, [&plan](CInventoryItem* item, u32 target)
    {
        CEatableItem* eatable = item->cast_eatable_item();
        if (eatable->GetRemainingUses() != target)
        {
            eatable->SetRemainingUses(u8(target));
            eatable->SyncUseCondition();
            if (item->m_pInventory)
                item->m_pInventory->InvalidateState();
            ++plan.changed;
        }
        if (!target)
            plan.release.push_back(item);
    });

    for (auto& [base, stack] : families)
    {
        if (stack.items.size() < 2)
            continue;

        xr_vector<shared_str> wanted(stack.total / stack.maximum, family_section(base, stack.maximum));
        if (const u32 rest = stack.total % stack.maximum)
            wanted.push_back(family_section(base, rest));

        xr_vector<shared_str> have;
        for (CInventoryItem* item : stack.items)
            have.push_back(item->object().cNameSect());

        const auto by_name = [](const shared_str& left, const shared_str& right) { return xr_strcmp(left, right) < 0; };
        std::sort(wanted.begin(), wanted.end(), by_name);
        std::sort(have.begin(), have.end(), by_name);
        if (wanted == have)
            continue;

        plan.release.insert(plan.release.end(), stack.items.begin(), stack.items.end());
        plan.spawn.insert(plan.spawn.end(), wanted.begin(), wanted.end());
    }

    return plan;
}

// =================================================================================================

CUIInventoryToolbar::CUIInventoryToolbar(EInventoryPane pane, CUIDragDropListEx* list)
    : CUIWindow("inventory_tools"), m_pane(pane), m_list(list)
{
}

bool CUIInventoryToolbar::Init(CUIXml& xml)
{
    constexpr pcstr root = "inventory_tools";
    if (!m_list || !xml.NavigateToNode(root, 0))
        return false;

    const float size = xml.ReadAttribFlt(root, 0, "size", 26.0f);
    const float spacing = xml.ReadAttribFlt(root, 0, "spacing", 3.0f);
    const float gap = xml.ReadAttribFlt(root, 0, "gap", 12.0f);
    const float lift = xml.ReadAttribFlt(root, 0, "lift", 8.0f);
    const float margin = xml.ReadAttribFlt(root, 0, "margin", 2.0f);
    const float gap_below = xml.ReadAttribFlt(root, 0, "gap_below", 4.0f);

    const Fvector2 list_pos = m_list->GetWndPos();
    const Fvector2 list_size = m_list->GetWndSize();
    const float row = float(m_list->CellSize().y + m_list->CellsSpacing().y);
    if (row < 1.0f || size < 1.0f)
        return false;

    // Square on screen: a widescreen layout is squeezed horizontally.
    const float kx = UI().get_current_kx();
    constexpr u32 filters = u32(EItemCategory::Count);
    const float width = (size + gap + filters * (size + spacing) - spacing) * kx;
    const float fit = width > list_size.x ? list_size.x / width : 1.0f;
    const Fvector2 button{ size * kx * fit, size * fit };

    // The row rises into the free space above the grid by up to `lift`, never onto a window that
    // ends above the grid (the header, a trade button). Windows reaching across the grid's top
    // edge - backgrounds, the trash area, the trade bar's frame - are not in the way.
    float ceiling = -flt_max;
    if (CUIWindow* parent = m_list->GetParent())
    {
        for (CUIWindow* w : parent->GetChildWndList())
        {
            if (w == m_list)
                continue;
            const Frect& r = w->GetWndRect();
            const bool beside = r.x2 <= list_pos.x || r.x1 >= list_pos.x + list_size.x;
            if (!beside && r.y2 <= list_pos.y + EPS_L)
                ceiling = std::max(ceiling, r.y2);
        }
    }
    const float top = std::max(list_pos.y - lift, ceiling + margin);

    // The grid starts just under the buttons and keeps whole rows of cells.
    const float grid_top = std::max(list_pos.y, top + button.y + gap_below);
    const int rows_before = iFloor(list_size.y / row + EPS_L);
    const int rows_after = iFloor((list_pos.y + list_size.y - grid_top) / row + EPS_L);
    if (rows_after < 1)
        return false;

    m_list->InitDragDropList({ list_pos.x, grid_top }, { list_size.x, rows_after * row });
    const Ivector2 capacity = m_list->CellsCapacity();
    const int lost = rows_before - rows_after;
    if (m_list->IsAutoGrow() && !m_list->ItemsCount() && lost > 0 && capacity.y > lost)
        m_list->SetStartCellsCapacity({ capacity.x, capacity.y - lost });

    SetWndPos({ list_pos.x, top });
    SetWndSize({ list_size.x, button.y });

    m_color_normal = CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color", 0, color_rgba(240, 217, 182, 255));
    m_color_hover = CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color_hover", 0, color_rgba(0, 0, 0, 255));
    m_color_active = CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color_active", 0, color_rgba(238, 155, 23, 255));

    m_step = (size + spacing) * kx * fit;
    m_sort_width = button.x + gap * kx * fit;

    constexpr pcstr sort_hints[] = { "hint_actor", "hint_loot", "hint_trader" };
    m_sort = AddButton(xml, "sort", sort_hints[u8(m_pane)], {}, button);

    constexpr pcstr filter_nodes[filters] = { "filter_all", "filter_weapons", "filter_armour", "filter_food",
        "filter_medicine", "filter_artefacts", "filter_misc" };
    for (u32 i = 0; i < filters; ++i)
    {
        m_filters[i] = AddButton(xml, filter_nodes[i], "hint", {}, button);
        m_filters[i]->SetButtonAsSwitch(true);
    }

    m_sort_shown = false;
    ShowSort(true);
    SetActiveFilter(EItemCategory::All);
    return true;
}

void CUIInventoryToolbar::ShowSort(bool show)
{
    if (show == m_sort_shown)
        return;
    m_sort_shown = show;
    m_sort->Show(show);
    m_sort->Enable(show);

    // Flush with the grid's first column, whether the sort button is there or not.
    float x = 0.0f;
    const float y = 0.0f;

    m_sort->SetWndPos({ x, y });
    if (show)
        x += m_sort_width;
    for (CUI3tButton* filter : m_filters)
    {
        filter->SetWndPos({ x, y });
        x += m_step;
    }
}

CUI3tButton* CUIInventoryToolbar::AddButton(
    CUIXml& xml, pcstr node, pcstr hint_attr, const Fvector2& pos, const Fvector2& size)
{
    string256 path;
    xr_sprintf(path, "inventory_tools:%s", node);

    auto* button = xr_new<CUI3tButton>();
    button->SetAutoDelete(true);
    AttachChild(button);
    button->InitButton(pos, size);
    button->InitTexture(xml.Read("inventory_tools:button_texture", 0, "ui_inGame2_inventory_button"), false);
    button->m_hint_text = xml.ReadAttrib(path, 0, hint_attr, "");

    // A white mask over the button, tinted by the button's state.
    if (cpcstr glyph = xml.ReadAttrib(path, 0, "glyph", nullptr))
    {
        const float inset = xml.ReadAttribFlt("inventory_tools", 0, "glyph_inset", 0.14f);
        const Fvector2 margin{ size.x * inset, size.y * inset };

        auto* icon = xr_new<CUIStatic>("glyph");
        icon->SetAutoDelete(true);
        button->AttachChild(icon);
        icon->SetWndPos(margin);
        icon->SetWndSize({ size.x - 2.0f * margin.x, size.y - 2.0f * margin.y });
        icon->InitTexture(glyph, false);
        icon->SetStretchTexture(true);
        // Clicks belong to the button.
        icon->Enable(false);
        m_glyphs.emplace_back(button, icon);
    }
    return button;
}

void CUIInventoryToolbar::SetActiveFilter(EItemCategory category)
{
    m_active = category;
    for (u32 i = 0; i < u32(EItemCategory::Count); ++i)
    {
        if (m_filters[i])
            m_filters[i]->SetButtonState(i == u32(category) ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
    }
}

void CUIInventoryToolbar::Update()
{
    inherited::Update();

    // A switch pressed and let go somewhere else stays down; the active filter decides. The one
    // under the cursor is left alone, its click may still be on its way.
    for (u32 i = 0; i < u32(EItemCategory::Count); ++i)
    {
        CUI3tButton* button = m_filters[i];
        if (button && !button->CursorOverWindow())
            button->SetButtonState(i == u32(m_active) ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
    }

    for (auto& [button, glyph] : m_glyphs)
    {
        u32 color = m_color_normal;
        if (button->GetButtonState() == CUIButton::BUTTON_PUSHED)
            color = m_color_active;
        else if (button->CursorOverWindow())
            color = m_color_hover;
        glyph->SetTextureColor(color);
    }
}

// =================================================================================================
// The actor menu's side of the row

bool RemoveItemFromList(CUIDragDropListEx* lst, PIItem pItem); // UIActorMenuInventory.cpp
bool is_item_in_list(CUIDragDropListEx* pList, PIItem item); // UIActorMenuTrade.cpp

void CUIActorMenu::InitInventoryTools()
{
    CUIXml xml;
    if (!xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "actor_menu_tools.xml", false))
        return;

    // The inventory and the loot window share the player's bag.
    const std::pair<EInventoryPane, eActorMenuListType> panes[] =
    {
        { EInventoryPane::Actor,  eInventoryBagList },
        { EInventoryPane::Actor,  eTradeActorBagList },
        { EInventoryPane::Loot,   eSearchLootBagList },
        { EInventoryPane::Trader, eTradePartnerBagList },
    };
    const CUIWndCallback::void_function on_click(this, &CUIActorMenu::OnInventoryToolClicked);
    for (const auto& [pane, id] : panes)
    {
        if (!m_pLists[id])
            continue;

        auto* toolbar = xr_new<CUIInventoryToolbar>(pane, m_pLists[id]);
        if (!toolbar->Init(xml))
        {
            xr_delete(toolbar);
            continue;
        }
        toolbar->SetAutoDelete(true);
        AttachChild(toolbar);
        toolbar->Show(false);

        RegisterCallback(toolbar->SortButton(), BUTTON_CLICKED, on_click);
        for (u32 i = 0; i < u32(EItemCategory::Count); ++i)
            RegisterCallback(toolbar->FilterButton(EItemCategory(i)), BUTTON_CLICKED, on_click);
        m_inventory_tools.push_back(toolbar);
    }
}

CUIDragDropListEx* CUIActorMenu::GetInventoryPaneList(EInventoryPane pane)
{
    switch (m_currMenuMode)
    {
    case mmInventory:
    case mmUpgrade:
        return pane == EInventoryPane::Actor ? m_pLists[eInventoryBagList] : nullptr;
    case mmTrade:
        if (pane == EInventoryPane::Actor)
            return m_pLists[eTradeActorBagList];
        return pane == EInventoryPane::Trader ? m_pLists[eTradePartnerBagList] : nullptr;
    case mmDeadBodySearch:
        if (pane == EInventoryPane::Actor)
            return m_pLists[eSearchLootActorBagList];
        return pane == EInventoryPane::Loot ? m_pLists[eSearchLootBagList] : nullptr;
    default:
        return nullptr;
    }
}

void CUIActorMenu::SyncInventoryTools()
{
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        CUIDragDropListEx* list = GetInventoryPaneList(toolbar->Pane());
        toolbar->Show(list == toolbar->List() && list->IsShown());
        toolbar->ShowSort(m_currMenuMode != mmUpgrade);
    }
}

void CUIActorMenu::ResetInventoryTools()
{
    m_pending_sort = {};
    if (m_item_filter != EItemCategory::All)
        SetItemFilter(EItemCategory::All);
}

void CUIActorMenu::OnInventoryToolClicked(CUIWindow* w, void* d)
{
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        if (w == toolbar->SortButton())
        {
            SortInventoryPane(toolbar->Pane());
            return;
        }
        for (u32 i = 0; i < u32(EItemCategory::Count); ++i)
        {
            if (w != toolbar->FilterButton(EItemCategory(i)))
                continue;
            // The active filter pressed again lets everything back.
            const EItemCategory category = EItemCategory(i);
            SetItemFilter(category == m_item_filter ? EItemCategory::All : category);
            return;
        }
    }
}

bool CUIActorMenu::CellPassesItemFilter(CUICellItem* itm)
{
    const PIItem item = static_cast<PIItem>(itm->m_pData);
    return !item || inventory_tools::classify(*item) == m_item_filter;
}

void CUIActorMenu::SetItemFilter(EItemCategory category)
{
    if (category >= EItemCategory::Count)
        category = EItemCategory::All;
    m_item_filter = category;

    // A selection that stays on screen stays selected (the upgrade target among them); only a
    // cell about to leave the grid lets it go.
    if (m_pCurrentCellItem && category != EItemCategory::All && !CellPassesItemFilter(m_pCurrentCellItem))
        SetCurrentItem(nullptr);

    const CUIDragDropListEx::CELL_FILTER filter = category == EItemCategory::All ?
        CUIDragDropListEx::CELL_FILTER() :
        CUIDragDropListEx::CELL_FILTER(this, &CUIActorMenu::CellPassesItemFilter);

    // Every grid of every window; the slots, the belt and the quick slots are never filtered.
    for (const eActorMenuListType id : { eInventoryBagList, eTradeActorBagList, eTradeActorList,
             eTradePartnerBagList, eTradePartnerList, eSearchLootBagList })
    {
        if (m_pLists[id])
            m_pLists[id]->SetCellFilter(filter);
    }

    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
        toolbar->SetActiveFilter(category);
}

bool CUIActorMenu::SortInventoryPane(EInventoryPane pane)
{
    CUIDragDropListEx* list = GetInventoryPaneList(pane);
    // The upgrade window holds a pointer to its target cell; a rebuilt grid would drop it.
    if (!list || m_currMenuMode == mmUpgrade || !m_pActorInvOwner || m_pending_sort.active ||
        CUIDragDropListEx::m_drag_item)
    {
        return false;
    }

    // New stacks are created in the inventory the pane shows. A box or a body the player may not
    // take from is left as it is.
    CGameObject* owner = nullptr;
    switch (pane)
    {
    case EInventoryPane::Actor:
        owner = smart_cast<CGameObject*>(m_pActorInvOwner);
        break;
    case EInventoryPane::Loot:
        if (m_pPartnerInvOwner)
        {
            if (m_pPartnerInvOwner->deadbody_can_take_status())
                owner = smart_cast<CGameObject*>(m_pPartnerInvOwner);
        }
        else if (m_pInvBox && m_pInvBox->can_take())
            owner = m_pInvBox;
        break;
    case EInventoryPane::Trader:
        owner = smart_cast<CGameObject*>(m_pPartnerInvOwner);
        break;
    }
    if (!owner)
        return false;

    // What the pane holds, filtered-out cells included; quest items are never touched.
    xr_vector<PIItem> items;
    for (CUIWindow* w : list->GetContainer()->GetChildWndList())
    {
        CUICellItem* cell = static_cast<CUICellItem*>(w);
        for (u32 i = 0; i <= cell->ChildsCount(); ++i)
        {
            const PIItem item = static_cast<PIItem>((i ? cell->Child(i - 1) : cell)->m_pData);
            if (item && !item->IsQuestItem())
                items.push_back(item);
        }
    }

    SetCurrentItem(nullptr);
    // A trader keeps his sections: his trade lists decide what of his stock he shows at all. A living
    // NPC keeps his ammo as it is: with infinite ammo (CObjectHandler) every emptied box that leaves
    // him is replaced by a full one, and a merge would print rounds.
    const bool living_partner = pane != EInventoryPane::Actor && m_pPartnerInvOwner && m_pPartnerInvOwner->is_alive();
    const inventory_tools::SSortPlan plan =
        inventory_tools::consolidate(items, pane == EInventoryPane::Trader, !living_partner);

    m_pending_sort = {};
    for (const PIItem item : plan.release)
    {
        // Off the screen first: a box and a body do not tell the menu when an item of theirs goes.
        for (CUIDragDropListEx* l : m_pLists)
        {
            while (l && RemoveItemFromList(l, item)) {}
        }
        m_pending_sort.released.push_back(item->object_id());
        item->object().DestroyObject();
    }

    if (!plan.spawn.empty())
    {
        u32 vertex = owner->ai_location().level_vertex_id();
        const bool valid = ai().get_level_graph() && ai().level_graph().valid_vertex_id(vertex);
        if (!valid && Actor())
            vertex = Actor()->ai_location().level_vertex_id();
        for (const shared_str& section : plan.spawn)
            Level().spawn_item(section.c_str(), owner->Position(), vertex, owner->ID());
    }

    // Laid out now without the released items, and once more when the new stacks have arrived.
    RefillInventoryPane(pane, m_pending_sort.released);
    if (!plan.release.empty() || !plan.spawn.empty())
    {
        m_pending_sort.pane = pane;
        m_pending_sort.started = Device.dwTimeContinual;
        m_pending_sort.active = true;
    }

    PlaySnd(eItemToRuck);
    return true;
}

void CUIActorMenu::UpdatePendingSort()
{
    // Rebuilding the grid would pull the cell from under an item being dragged.
    if (!m_pending_sort.active || CUIDragDropListEx::m_drag_item)
        return;

    bool gone = true;
    for (const u16 id : m_pending_sort.released)
    {
        if (Level().Objects.net_Find(id))
        {
            gone = false;
            break;
        }
    }
    if (gone)
        ++m_pending_sort.settled_frames;

    // The new stacks come a frame or two after the old items leave.
    constexpr u32 settle_frames = 3;
    constexpr u32 give_up_ms = 3000;
    if ((!gone || m_pending_sort.settled_frames < settle_frames) &&
        Device.dwTimeContinual - m_pending_sort.started < give_up_ms)
    {
        return;
    }

    const SPendingSort pending = std::move(m_pending_sort);
    m_pending_sort = {};
    RefillInventoryPane(pending.pane, pending.released);
}

void CUIActorMenu::RefillInventoryPane(EInventoryPane pane, const xr_vector<u16>& skip)
{
    CUIDragDropListEx* list = GetInventoryPaneList(pane);
    if (!list)
        return;

    SetCurrentItem(nullptr);

    // The same sources the windows fill these grids from when they open.
    TIItemContainer items;
    switch (pane)
    {
    case EInventoryPane::Actor:
        items = m_pActorInvOwner->inventory().m_ruck;
        break;
    case EInventoryPane::Loot:
        if (m_pPartnerInvOwner)
            m_pPartnerInvOwner->inventory().AddAvailableItems(items, false, m_pPartnerInvOwner->is_alive());
        else if (m_pInvBox)
            m_pInvBox->AddAvailableItems(items);
        break;
    case EInventoryPane::Trader:
        if (m_pPartnerInvOwner)
            m_pPartnerInvOwner->inventory().AddAvailableItems(items, true);
        break;
    }
    std::sort(items.begin(), items.end(), InventoryUtilities::GreaterRoomInRuck);

    list->ClearAll(true);
    for (const PIItem item : items)
    {
        if (std::find(skip.begin(), skip.end(), item->object_id()) != skip.end())
            continue;
        if (pane == EInventoryPane::Actor && (!item->CanShow() || smart_cast<CMPPlayersBag*>(&item->object())))
            continue;
        // What is on the trade table stays there.
        if (m_currMenuMode == mmTrade &&
            (is_item_in_list(m_pLists[eTradeActorList], item) || is_item_in_list(m_pLists[eTradePartnerList], item)))
        {
            continue;
        }

        CUICellItem* cell = create_cell_item(item);
        list->SetItem(cell);
        if (m_currMenuMode == mmTrade && pane == EInventoryPane::Actor && m_pPartnerInvOwner)
            ColorizeItem(cell, !CanMoveToPartner(item));
    }

    if (pane == EInventoryPane::Trader && m_pPartnerInvOwner)
        m_trade_partner_inventory_state = m_pPartnerInvOwner->inventory().ModifyFrame();
    if (pane == EInventoryPane::Actor && m_pQuickSlot)
        m_pQuickSlot->ReloadReferences(m_pActorInvOwner);

    UpdateItemsPlace();
}
