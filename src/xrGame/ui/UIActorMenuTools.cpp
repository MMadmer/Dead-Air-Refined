// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "UIActorMenuTools.h"

#include "UIDragDropListEx.h"
#include "UIXmlInit.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "xrUICore/ui_base.h"
#include "xrUICore/Static/UIShape.h"

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
// The order of the grids

// ui_inv_sort: an EInventorySort, saved in user.ltx (console_commands_ui.cpp)
u32 g_inventory_sort = u32(EInventorySort::Default);

bool inventory_tools::split_family(const shared_str& section, shared_str& first, shared_str& second)
{
    ensure_loaded();
    shared_str base;
    u16 amount, maximum;
    if (!family_of(section, base, amount, maximum) || amount < 2)
        return false;
    first = family_section(base, amount - amount / 2);
    second = family_section(base, amount / 2);
    return true;
}

EInventorySort inventory_tools::sort_mode()
{
    return g_inventory_sort < u32(EInventorySort::Count) ? EInventorySort(g_inventory_sort) : EInventorySort::Default;
}

void inventory_tools::set_sort_mode(EInventorySort mode) { g_inventory_sort = u32(mode); }

pcstr inventory_tools::sort_mode_caption(EInventorySort mode)
{
    constexpr pcstr captions[u8(EInventorySort::Count)] = { "st_da_inv_order_default", "st_da_inv_order_type",
        "st_da_inv_order_name", "st_da_inv_order_weight", "st_da_inv_order_price", "st_da_inv_order_condition" };
    return captions[u8(mode) < u8(EInventorySort::Count) ? u8(mode) : 0];
}

void inventory_tools::sort_items(xr_vector<CInventoryItem*>& items)
{
    // The default order breaks every tie, then the section, so equal items keep their place.
    const auto fallback = [](CInventoryItem* left, CInventoryItem* right)
    {
        if (InventoryUtilities::GreaterRoomInRuck(left, right))
            return true;
        if (InventoryUtilities::GreaterRoomInRuck(right, left))
            return false;
        return xr_strcmp(left->object().cNameSect(), right->object().cNameSect()) < 0;
    };
    const auto by = [&fallback](auto key, bool descending)
    {
        return [key, descending, &fallback](CInventoryItem* left, CInventoryItem* right)
        {
            const auto a = key(left);
            const auto b = key(right);
            if (a != b)
                return descending ? b < a : a < b;
            return fallback(left, right);
        };
    };

    switch (sort_mode())
    {
    case EInventorySort::Type:
        std::stable_sort(items.begin(), items.end(), by([](CInventoryItem* i) { return u8(classify(*i)); }, false));
        break;
    case EInventorySort::Name:
        std::stable_sort(items.begin(), items.end(), [&fallback](CInventoryItem* left, CInventoryItem* right)
        {
            const int order = xr_strcmp(left->NameItem(), right->NameItem());
            return order ? order < 0 : fallback(left, right);
        });
        break;
    case EInventorySort::Weight:
        std::stable_sort(items.begin(), items.end(), by([](CInventoryItem* i) { return i->Weight(); }, true));
        break;
    case EInventorySort::Price:
        std::stable_sort(items.begin(), items.end(), by([](CInventoryItem* i) { return i->Cost(); }, true));
        break;
    case EInventorySort::Condition:
        std::stable_sort(items.begin(), items.end(), by([](CInventoryItem* i) { return i->GetConditionToShow(); }, true));
        break;
    default: std::stable_sort(items.begin(), items.end(), fallback); break;
    }
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
    // the drop-down of orders beside the sort button; 0 leaves it out
    const float mode_width = xml.NavigateToNode("inventory_tools:sort_mode", 0) ?
        xml.ReadAttribFlt("inventory_tools:sort_mode", 0, "width", 96.0f) : 0.0f;

    const Fvector2 list_pos = m_list->GetWndPos();
    const Fvector2 list_size = m_list->GetWndSize();
    const float row = float(m_list->CellSize().y + m_list->CellsSpacing().y);
    if (row < 1.0f || size < 1.0f)
        return false;

    // The action buttons of this pane: panes="actor,loot,trader" (the player's alone by default).
    // Read under the row's node: the index of a path counts its first node, not its last.
    constexpr pcstr pane_names[] = { "actor", "loot", "trader" };
    struct SActionNode
    {
        shared_str action, glyph, hint;
    };
    xr_vector<SActionNode> actions;
    if (const XML_NODE tools = xml.NavigateToNode(root, 0))
    {
        for (size_t i = 0, n = xml.GetNodesNum(tools, "action"); i < n; ++i)
        {
            string256 panes;
            xr_strcpy(panes, xml.ReadAttrib(tools, "action", i, "panes", "actor"));
            string64 name;
            for (int k = 0, count = _GetItemCount(panes); k < count; ++k)
            {
                if (0 == xr_stricmp(_Trim(_GetItem(panes, k, name)), pane_names[u8(m_pane)]))
                {
                    actions.push_back({ xml.ReadAttrib(tools, "action", i, "action", ""),
                        xml.ReadAttrib(tools, "action", i, "glyph", ""), xml.ReadAttrib(tools, "action", i, "hint", "") });
                    break;
                }
            }
        }
    }

    // Square on screen: a widescreen layout is squeezed horizontally.
    const float kx = UI().get_current_kx();
    constexpr u32 filters = u32(EItemCategory::Count);
    const float mode_part = mode_width > 0.0f ? mode_width + spacing : 0.0f;
    const float action_part = actions.empty() ? 0.0f : gap + actions.size() * (size + spacing);
    const float width = (size + mode_part + gap + filters * (size + spacing) - spacing + action_part) * kx;
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

    m_color_normal = UIShape::ParseColor(xml.ReadAttrib("inventory_tools:glyph_color", 0, "value", nullptr),
        CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color", 0, color_rgba(240, 217, 182, 255)));
    m_color_hover = UIShape::ParseColor(xml.ReadAttrib("inventory_tools:glyph_color_hover", 0, "value", nullptr),
        CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color_hover", 0, color_rgba(0, 0, 0, 255)));
    m_color_active = UIShape::ParseColor(xml.ReadAttrib("inventory_tools:glyph_color_active", 0, "value", nullptr),
        CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color_active", 0, color_rgba(238, 155, 23, 255)));
    m_color_disabled = UIShape::ParseColor(xml.ReadAttrib("inventory_tools:glyph_color_disabled", 0, "value", nullptr),
        CUIXmlInit::GetColor(xml, "inventory_tools:glyph_color_disabled", 0, color_rgba(110, 102, 90, 255)));

    m_step = (size + spacing) * kx * fit;
    m_sort_width = button.x + spacing * kx * fit;
    m_mode_width = mode_width > 0.0f ? (mode_width + gap) * kx * fit : gap * kx * fit;

    constexpr pcstr sort_hints[] = { "hint_actor", "hint_loot", "hint_trader" };
    m_sort = AddButton(xml, "sort", sort_hints[u8(m_pane)], {}, button);
    if (mode_width > 0.0f)
        InitSortModes(xml, { mode_width * kx * fit, button.y }, fit);

    constexpr pcstr filter_nodes[filters] = { "filter_all", "filter_weapons", "filter_armour", "filter_food",
        "filter_medicine", "filter_artefacts", "filter_misc" };
    for (u32 i = 0; i < filters; ++i)
    {
        m_filters[i] = AddButton(xml, filter_nodes[i], "hint", {}, button);
        m_filters[i]->SetButtonAsSwitch(true);
    }

    // Flush with the grid's right edge, the last node rightmost.
    for (size_t i = 0; i < actions.size(); ++i)
    {
        const float x = list_size.x - float(actions.size() - i) * m_step + spacing * kx * fit;
        SAction action;
        action.button = AddButtonWith(xml, actions[i].glyph.c_str(), actions[i].hint.c_str(), { x, 0.0f }, button);
        action.action = actions[i].action;
        action.hint = actions[i].hint;
        m_actions.push_back(action);
    }

    m_sort_shown = false;
    ShowSort(true);
    SetActiveFilter(EItemCategory::All);
    return true;
}

void CUIInventoryToolbar::InitSortModes(CUIXml& xml, const Fvector2& size, float fit)
{
    constexpr pcstr node = "inventory_tools:sort_mode";

    m_mode = xr_new<CUI3tButton>();
    m_mode->SetAutoDelete(true);
    AttachChild(m_mode);
    m_mode->InitButton({}, size);
    if (auto skin = UIShape::ReadSkin(xml, node, 0))
        m_mode->SetShapeSkin(std::move(skin));
    else if (auto button = UIShape::ReadSkin(xml, "inventory_tools:button", 0))
        m_mode->SetShapeSkin(std::move(button));
    else
        m_mode->InitTexture(xml.Read("inventory_tools:button_texture", 0, "ui_inGame2_inventory_button"), false);
    string256 path;
    CUIXmlInit::InitText(xml, strconcat(path, node, ":text"), 0, m_mode->TextItemControl());
    m_mode->m_hint_text = xml.ReadAttrib(node, 0, "hint", "");

    // The caret at the right end, in the caption's colour.
    const float caret = size.y * 0.42f;
    m_mode_chevron = xr_new<CUIStatic>("caret");
    m_mode_chevron->SetAutoDelete(true);
    m_mode->AttachChild(m_mode_chevron);
    m_mode_chevron->SetWndPos({ size.x - (caret + size.y * 0.3f) * UI().get_current_kx(), (size.y - caret) * 0.5f });
    m_mode_chevron->SetWndSize({ caret * UI().get_current_kx(), caret });
    auto chevron = std::make_unique<SUIShapeSkin>();
    chevron->looks[SUIShapeSkin::Normal].kind = SUIShapeStyle::Kind::Chevron;
    chevron->looks[SUIShapeSkin::Normal].fill = 0xFFFFFFFF;
    chevron->looks[SUIShapeSkin::Normal].border = 1.5f;
    m_mode_chevron->SetShapeSkin(std::move(chevron));
    m_mode_chevron->Enable(false);

    // The list: a window of the menu's own (CUIActorMenu::InitInventoryTools attaches it last).
    const float row_height = xml.ReadAttribFlt(node, 0, "row_height", 20.0f) * fit;
    const float padding = xml.ReadAttribFlt(node, 0, "padding", 3.0f) * fit;
    m_mode_list = xr_new<CUIWindow>("sort_modes");
    m_mode_list->SetAutoDelete(true);
    m_mode_list->SetWndSize({ size.x, row_height * u32(EInventorySort::Count) + padding * 2.0f });
    m_mode_list->SetShapeSkin(UIShape::ReadSkin(xml, strconcat(path, node, ":list"), 0));
    for (u8 i = 0; i < u8(EInventorySort::Count); ++i)
    {
        auto* row = xr_new<CUI3tButton>();
        row->SetAutoDelete(true);
        m_mode_list->AttachChild(row);
        row->InitButton({ padding * UI().get_current_kx(), padding + row_height * i },
            { size.x - padding * 2.0f * UI().get_current_kx(), row_height });
        row->SetShapeSkin(UIShape::ReadSkin(xml, strconcat(path, node, ":row"), 0));
        CUIXmlInit::InitText(xml, strconcat(path, node, ":row:text"), 0, row->TextItemControl());
        row->TextItemControl()->SetText(StringTable().translate(inventory_tools::sort_mode_caption(EInventorySort(i))).c_str());
        row->SetButtonAsSwitch(true);
        m_mode_rows[i] = row;
    }
    m_mode_list->Show(false);
    SyncSortMode();
}

void CUIInventoryToolbar::ShowSort(bool show)
{
    if (show == m_sort_shown)
        return;
    m_sort_shown = show;
    m_sort->Show(show);
    m_sort->Enable(show);
    if (m_mode)
    {
        m_mode->Show(show);
        m_mode->Enable(show);
        if (!show)
            ShowSortModes(false);
    }

    // Flush with the grid's first column, whether the sort button is there or not.
    float x = 0.0f;
    const float y = 0.0f;

    m_sort->SetWndPos({ x, y });
    if (show)
    {
        x += m_sort_width;
        if (m_mode)
            m_mode->SetWndPos({ x, y });
        x += m_mode_width;
    }
    for (CUI3tButton* filter : m_filters)
    {
        filter->SetWndPos({ x, y });
        x += m_step;
    }
}

void CUIInventoryToolbar::ShowSortModes(bool show)
{
    if (!m_mode_list)
        return;
    if (show)
    {
        // under the drop-down's button, in the menu's coordinates
        const Fvector2 below{ GetWndPos().x + m_mode->GetWndPos().x, GetWndPos().y + m_mode->GetWndPos().y + m_mode->GetHeight() + 1.0f };
        m_mode_list->SetWndPos(below);
        SyncSortMode();
    }
    m_mode_list->Show(show);
    m_mode->SetButtonState(show ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
}

void CUIInventoryToolbar::SyncSortMode()
{
    if (!m_mode)
        return;
    const EInventorySort mode = inventory_tools::sort_mode();
    m_synced_mode = mode;
    m_mode->TextItemControl()->SetText(StringTable().translate(inventory_tools::sort_mode_caption(mode)).c_str());
    for (u8 i = 0; i < u8(EInventorySort::Count); ++i)
    {
        if (m_mode_rows[i] && !m_mode_rows[i]->CursorOverWindow())
            m_mode_rows[i]->SetButtonState(i == u8(mode) ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
    }
}

void CUIInventoryToolbar::OnMouseDown(const Fvector2& cursor)
{
    if (!SortModesShown())
        return;
    Frect list, button;
    m_mode_list->GetAbsoluteRect(list);
    m_mode->GetAbsoluteRect(button);
    Fvector2 at = cursor;
    if (!list.in(at) && !button.in(at))
        ShowSortModes(false);
}

const shared_str* CUIInventoryToolbar::ActionOf(const CUIWindow* button) const
{
    for (const SAction& action : m_actions)
    {
        if (button && action.button == button)
            return &action.action;
    }
    return nullptr;
}

void CUIInventoryToolbar::EnableAction(pcstr name, bool enable)
{
    for (const SAction& action : m_actions)
    {
        if (action.action == name && action.button->IsEnabled() != enable)
        {
            action.button->Enable(enable);
            action.button->SetButtonState(CUIButton::BUTTON_NORMAL);
        }
    }
}

CUI3tButton* CUIInventoryToolbar::AddButton(
    CUIXml& xml, pcstr node, pcstr hint_attr, const Fvector2& pos, const Fvector2& size)
{
    string256 path;
    xr_sprintf(path, "inventory_tools:%s", node);
    return AddButtonWith(xml, xml.ReadAttrib(path, 0, "glyph", nullptr), xml.ReadAttrib(path, 0, hint_attr, ""), pos, size);
}

CUI3tButton* CUIInventoryToolbar::AddButtonWith(CUIXml& xml, pcstr glyph, pcstr hint, const Fvector2& pos, const Fvector2& size)
{
    auto* button = xr_new<CUI3tButton>();
    button->SetAutoDelete(true);
    AttachChild(button);
    button->InitButton(pos, size);
    // the look of every button: a shape (inventory_tools:button), or the old 3t texture
    if (auto skin = UIShape::ReadSkin(xml, "inventory_tools:button", 0))
        button->SetShapeSkin(std::move(skin));
    else
        button->InitTexture(xml.Read("inventory_tools:button_texture", 0, "ui_inGame2_inventory_button"), false);
    button->m_hint_text = hint ? hint : "";

    if (glyph && glyph[0])
    {
        const float inset = xml.ReadAttribFlt("inventory_tools", 0, "glyph_inset", 0.14f);
        const Fvector2 margin{ size.x * inset, size.y * inset };
        if (CUIStatic* icon = AddGlyph(xml, button, glyph, margin, { size.x - 2.0f * margin.x, size.y - 2.0f * margin.y }))
            m_glyphs.emplace_back(button, icon);
    }
    return button;
}

CUIStatic* CUIInventoryToolbar::AddGlyph(CUIXml& xml, CUIWindow* owner, pcstr glyph, const Fvector2& pos, const Fvector2& size)
{
    auto* icon = xr_new<CUIStatic>("glyph");
    icon->SetAutoDelete(true);
    owner->AttachChild(icon);
    icon->SetWndPos(pos);
    icon->SetWndSize(size);

    // A pictogram the shader draws (UIShapeStyle.h), white so the state colour tints it; any other
    // name is a white mask texture, the way mods made their glyphs before.
    SUIShapeStyle::Kind kind;
    if (UIShape::FindKind(glyph, kind))
    {
        auto skin = UIShape::ReadSkin(xml, "inventory_tools:glyph", 0);
        if (!skin)
        {
            skin = std::make_unique<SUIShapeSkin>();
            skin->looks[SUIShapeSkin::Normal].fill = 0xFFFFFFFF;
        }
        skin->looks[SUIShapeSkin::Normal].kind = kind;
        icon->SetShapeSkin(std::move(skin));
    }
    else
    {
        icon->InitTexture(glyph, false);
        icon->SetStretchTexture(true);
    }
    // Clicks belong to the button.
    icon->Enable(false);
    return icon;
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
    if (m_mode && !m_mode->CursorOverWindow())
        m_mode->SetButtonState(SortModesShown() ? CUIButton::BUTTON_PUSHED : CUIButton::BUTTON_NORMAL);
    // an order set from the console shows too
    if (SortModesShown() || m_synced_mode != inventory_tools::sort_mode())
        SyncSortMode();

    for (auto& [button, glyph] : m_glyphs)
    {
        u32 color = m_color_normal;
        if (!button->IsEnabled())
            color = m_color_disabled;
        else if (button->GetButtonState() == CUIButton::BUTTON_PUSHED)
            color = m_color_active;
        else if (button->CursorOverWindow())
            color = m_color_hover;
        glyph->SetTextureColor(color);
    }
    if (m_mode_chevron)
        m_mode_chevron->SetTextureColor(m_mode->TextItemControl()->GetTextColor());
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
        RegisterCallback(toolbar->SortModeButton(), BUTTON_CLICKED, on_click);
        for (u8 i = 0; i < u8(EInventorySort::Count); ++i)
            RegisterCallback(toolbar->SortModeRow(EInventorySort(i)), BUTTON_CLICKED, on_click);
        for (const CUIInventoryToolbar::SAction& action : toolbar->Actions())
            RegisterCallback(action.button, BUTTON_CLICKED, on_click);
        m_inventory_tools.push_back(toolbar);
    }

    // The drop-down lists last: over every grid, and the first to take a click.
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        if (CUIWindow* list = toolbar->SortModeList())
            AttachChild(list);
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
        if (!toolbar->IsShown())
            toolbar->ShowSortModes(false);
    }
}

bool CUIActorMenu::CloseSortModes()
{
    bool closed = false;
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        closed |= toolbar->SortModesShown();
        toolbar->ShowSortModes(false);
    }
    return closed;
}

void CUIActorMenu::SetSortMode(EInventorySort mode)
{
    inventory_tools::set_sort_mode(mode);
    CloseSortModes();
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
        toolbar->SyncSortMode();

    // Every grid on screen takes the new order at once; the upgrade window keeps its target cell.
    if (m_currMenuMode == mmUpgrade || m_pending_sort.active || CUIDragDropListEx::m_drag_item)
        return;
    for (const EInventoryPane pane : { EInventoryPane::Actor, EInventoryPane::Loot, EInventoryPane::Trader })
    {
        if (GetInventoryPaneList(pane))
            RefillInventoryPane(pane, {});
    }
    PlaySnd(eItemToRuck);
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
        if (const shared_str* action = toolbar->ActionOf(w))
        {
            CloseSortModes();
            RunWindowAction(*action, nullptr);
            return;
        }
        if (w == toolbar->SortButton())
        {
            CloseSortModes();
            SortInventoryPane(toolbar->Pane());
            return;
        }
        if (w && w == toolbar->SortModeButton())
        {
            const bool show = !toolbar->SortModesShown();
            CloseSortModes();
            toolbar->ShowSortModes(show);
            return;
        }
        for (u8 i = 0; i < u8(EInventorySort::Count); ++i)
        {
            if (w && w == toolbar->SortModeRow(EInventorySort(i)))
            {
                SetSortMode(EInventorySort(i));
                return;
            }
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
        m_pending_sort.panes = u8(1 << u8(pane));
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
    for (const EInventoryPane pane : { EInventoryPane::Actor, EInventoryPane::Loot, EInventoryPane::Trader })
    {
        if (pending.panes & (1 << u8(pane)))
            RefillInventoryPane(pane, pending.released);
    }
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
    inventory_tools::sort_items(items);

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
