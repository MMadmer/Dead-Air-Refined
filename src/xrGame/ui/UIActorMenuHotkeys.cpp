// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"
#include "UIActorMenuHotkeys.h"

#include "UIActorMenu.h"
#include "UICellItem.h"
#include "UIDragDropListEx.h"
#include "UIItemCompare.h"
#include "UIItemInfo.h"
#include "Actor.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "Inventory.h"
#include "InventoryBox.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "trade.h"
#include "WeaponAmmo.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrUICore/PropertiesBox/UIPropertiesBox.h"

#include "UIXmlInit.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/xr_level_controller.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "xrUICore/ui_base.h"

namespace
{
constexpr pcstr hotkeys_xml = "inventory_hotkeys.xml";
constexpr pcstr panel_xml = "actor_menu_hotkeys.xml";
constexpr pcstr panel_node = "hotkeys_panel";

xr_vector<SInventoryHotkey> g_hotkeys;
bool g_loaded{};
u8 g_fake_modifiers{};
int g_fake_key{};

void parse_modifiers(pcstr text, SInventoryHotkey& hotkey)
{
    hotkey.modifiers = 0;
    hotkey.hold_key = 0;
    string128 part;
    for (int i = 0, n = _GetItemCount(text, '+'); i < n; ++i)
    {
        _GetItem(text, i, part, '+');
        _Trim(part);
        if (0 == xr_stricmp(part, "alt"))
            hotkey.modifiers |= SInventoryHotkey::Alt;
        else if (0 == xr_stricmp(part, "ctrl"))
            hotkey.modifiers |= SInventoryHotkey::Ctrl;
        else if (0 == xr_stricmp(part, "shift"))
            hotkey.modifiers |= SInventoryHotkey::Shift;
        else if (const int key = inventory_hotkeys::key_of(part); key > 0)
            hotkey.hold_key = key;
        else if (part[0])
            Msg("! [ui] inventory hotkey [%s]: unknown key [%s]", hotkey.id.c_str(), part);
    }
}

void parse_trigger(pcstr text, SInventoryHotkey& hotkey)
{
    constexpr std::pair<pcstr, EHotkeyTrigger> names[] = { { "hover", EHotkeyTrigger::Hover },
        { "lmb", EHotkeyTrigger::LeftClick }, { "rmb", EHotkeyTrigger::RightClick },
        { "mmb", EHotkeyTrigger::MiddleClick }, { "wheel_up", EHotkeyTrigger::WheelUp },
        { "wheel_down", EHotkeyTrigger::WheelDown }, { "hold", EHotkeyTrigger::Hold }, { "none", EHotkeyTrigger::None } };
    for (const auto& [name, trigger] : names)
    {
        if (0 == xr_stricmp(text, name))
        {
            hotkey.trigger = trigger;
            return;
        }
    }
    // key:<the key's bind name or letter>
    if (0 == strncmp(text, "key:", 4))
    {
        if (const int dik = inventory_hotkeys::key_of(text + 4); dik > 0)
        {
            hotkey.trigger = EHotkeyTrigger::Key;
            hotkey.key = dik;
            return;
        }
    }
    Msg("! [ui] inventory hotkey [%s]: unknown trigger [%s]", hotkey.id.c_str(), text);
    hotkey.trigger = EHotkeyTrigger::None;
}

u8 parse_windows(pcstr text)
{
    u8 result{};
    string128 part;
    for (int i = 0, n = _GetItemCount(text); i < n; ++i)
    {
        _GetItem(text, i, part);
        _Trim(part);
        if (0 == xr_stricmp(part, "inventory"))
            result |= SInventoryHotkey::Inventory;
        else if (0 == xr_stricmp(part, "trade"))
            result |= SInventoryHotkey::Trade;
        else if (0 == xr_stricmp(part, "loot"))
            result |= SInventoryHotkey::Loot;
        else if (0 == xr_stricmp(part, "upgrade"))
            result |= SInventoryHotkey::Upgrade;
        else if (0 == xr_stricmp(part, "all"))
            result |= SInventoryHotkey::Every;
    }
    return result;
}

void read_file(CUIXml& xml)
{
    for (size_t i = 0, count = xml.GetNodesNum(xml.GetRoot(), "hotkey"); i < count; ++i)
    {
        const shared_str id = xml.ReadAttrib("hotkey", i, "id", "");
        if (!id.size())
            continue;
        const auto it = std::find_if(g_hotkeys.begin(), g_hotkeys.end(), [&id](const SInventoryHotkey& h) { return h.id == id; });
        if (xml.ReadAttribInt("hotkey", i, "remove", 0))
        {
            if (it != g_hotkeys.end())
                g_hotkeys.erase(it);
            continue;
        }

        // An id read before is changed by what this node sets; a new one starts after the last.
        SInventoryHotkey hotkey;
        if (it != g_hotkeys.end())
            hotkey = *it;
        else
            hotkey.order = g_hotkeys.empty() ? 10.0f : g_hotkeys.back().order + 10.0f;
        hotkey.id = id;
        if (cpcstr keys = xml.ReadAttrib("hotkey", i, "keys", nullptr))
            parse_modifiers(keys, hotkey);
        if (cpcstr trigger = xml.ReadAttrib("hotkey", i, "trigger", nullptr))
            parse_trigger(trigger, hotkey);
        if (cpcstr action = xml.ReadAttrib("hotkey", i, "action", nullptr))
            hotkey.action = action;
        if (cpcstr caption = xml.ReadAttrib("hotkey", i, "caption", nullptr))
            hotkey.caption = caption;
        constexpr pcstr window_captions[] = { "caption_inventory", "caption_trade", "caption_loot", "caption_upgrade" };
        for (u8 w = 0; w < std::size(window_captions); ++w)
        {
            if (cpcstr caption = xml.ReadAttrib("hotkey", i, window_captions[w], nullptr))
                hotkey.window_caption[w] = caption;
        }
        if (cpcstr windows = xml.ReadAttrib("hotkey", i, "windows", nullptr))
            hotkey.windows = parse_windows(windows);
        if (cpcstr label = xml.ReadAttrib("hotkey", i, "label", nullptr))
        {
            hotkey.label.clear();
            string256 part;
            for (int k = 0, n = _GetItemCount(label, '|'); k < n; ++k)
                hotkey.label.emplace_back(_Trim(_GetItem(label, k, part, '|')));
        }
        hotkey.order = xml.ReadAttribFlt("hotkey", i, "order", hotkey.order);

        if (it != g_hotkeys.end())
            *it = hotkey;
        else
            g_hotkeys.push_back(hotkey);
    }
}

void load()
{
    g_loaded = true;
    g_hotkeys.clear();

    CUIXml xml;
    if (xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, hotkeys_xml, false))
        read_file(xml);

    // A mod's inventory_hotkeys_<name>.xml, in name order.
    FS_FileSet files;
    string_path mask;
    FS.file_list(files, "$game_config$", FS_ListFiles, strconcat(sizeof(mask), mask, UI_PATH, DELIMITER "inventory_hotkeys_*.xml"));
    for (const auto& file : files)
    {
        string_path name, ext;
        _splitpath(file.name.c_str(), nullptr, nullptr, name, ext);
        // a widescreen twin is loaded in place of its file, never on its own
        const size_t length = xr_strlen(name);
        if (length > 3 && 0 == xr_stricmp(name + length - 3, "_16"))
            continue;
        xr_strcat(name, ext);
        CUIXml mod;
        if (mod.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, name, false))
            read_file(mod);
    }

    std::stable_sort(g_hotkeys.begin(), g_hotkeys.end(),
        [](const SInventoryHotkey& a, const SInventoryHotkey& b) { return a.order < b.order; });
}

bool held(SDL_Scancode left, SDL_Scancode right) { return pInput->iGetAsyncKeyState(left) || pInput->iGetAsyncKeyState(right); }

bool key_held(int key) { return key > 0 && (key == g_fake_key || pInput->iGetAsyncKeyState(key)); }

// the modifiers exactly, and the held key when the hotkey has one
bool modifiers_held(const SInventoryHotkey& hotkey, u8 modifiers)
{
    return hotkey.modifiers == modifiers && (!hotkey.hold_key || key_held(hotkey.hold_key));
}
} // namespace

const xr_vector<SInventoryHotkey>& inventory_hotkeys::all()
{
    if (!g_loaded)
        load();
    return g_hotkeys;
}

void inventory_hotkeys::reset() { g_loaded = false; }

u8 inventory_hotkeys::held_modifiers()
{
    u8 result = g_fake_modifiers;
    if (held(SDL_SCANCODE_LALT, SDL_SCANCODE_RALT))
        result |= SInventoryHotkey::Alt;
    if (held(SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL))
        result |= SInventoryHotkey::Ctrl;
    if (held(SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT))
        result |= SInventoryHotkey::Shift;
    return result;
}

const SInventoryHotkey* inventory_hotkeys::find(EHotkeyTrigger trigger, u8 window, int key)
{
    const u8 modifiers = held_modifiers();
    const SInventoryHotkey* plain = nullptr;
    for (const SInventoryHotkey& hotkey : all())
    {
        if (hotkey.trigger != trigger || !(hotkey.windows & window) || !hotkey.action.size() ||
            (trigger == EHotkeyTrigger::Key && hotkey.key != key) || !modifiers_held(hotkey, modifiers))
        {
            continue;
        }
        if (hotkey.hold_key)
            return &hotkey;
        if (!plain)
            plain = &hotkey;
    }
    return plain;
}

bool inventory_hotkeys::holding(pcstr action, u8 window)
{
    const SInventoryHotkey* hotkey = find(EHotkeyTrigger::Hold, window);
    return hotkey && hotkey->action == action;
}

int inventory_hotkeys::key_of(pcstr name)
{
    if (!name || !name[0])
        return 0;
    if (const int dik = KeynameToDik(name, true); dik > 0)
        return dik;
    // a letter or a word of a key: C, F1, RETURN
    string64 bind;
    xr_sprintf(bind, "k%s", name);
    for (char* c = bind + 1; *c; ++c)
        *c = char(toupper(u8(*c)));
    const int dik = KeynameToDik(bind, true);
    return dik > 0 ? dik : 0;
}

void inventory_hotkeys::set_fake_held(u8 modifiers, int key)
{
    g_fake_modifiers = modifiers;
    g_fake_key = key;
}

// =================================================================================================

bool CUIInventoryHotkeysPanel::Init(CUIXml& actor_menu_xml)
{
    // The actor_menu layout places the panel; one without it (an older layout, a UI mod's) gets ours.
    if (actor_menu_xml.NavigateToNode(panel_node, 0))
        return InitFrom(actor_menu_xml);
    CUIXml own;
    return own.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, panel_xml, false) && own.NavigateToNode(panel_node, 0) &&
        InitFrom(own);
}

bool CUIInventoryHotkeysPanel::InitFrom(CUIXml& xml)
{
    CUIXmlInit::InitWindow(xml, panel_node, 0, this);
    // separators and plates of the layout's own
    CUIXmlInit::InitAutoStaticGroup(xml, panel_node, 0, this);
    m_row_height = xml.ReadAttribFlt(panel_node, 0, "row_height", m_row_height);
    m_keys_width = xml.ReadAttribFlt(panel_node, 0, "keys_width", m_keys_width);
    m_chip_gap = xml.ReadAttribFlt(panel_node, 0, "chip_gap", m_chip_gap);
    m_chip_padding = xml.ReadAttribFlt(panel_node, 0, "chip_padding", m_chip_padding);

    string256 path;
    const auto node = [&path](pcstr child) { return strconcat(path, panel_node, ":", child); };

    if (xml.NavigateToNode(node("title"), 0))
    {
        auto* title = xr_new<CUIStatic>("title");
        title->SetAutoDelete(true);
        AttachChild(title);
        CUIXmlInit::InitStatic(xml, path, 0, title);
    }

    m_list = xr_new<CUIScrollView>();
    m_list->SetAutoDelete(true);
    AttachChild(m_list);
    CUIXmlInit::InitScrollView(xml, node("list"), 0, m_list);

    if (xml.NavigateToNode(node("close"), 0))
    {
        m_close = xr_new<CUI3tButton>();
        m_close->SetAutoDelete(true);
        AttachChild(m_close);
        CUIXmlInit::Init3tButton(xml, path, 0, m_close);
        // the cross in the button, in its caption's colour
        auto* cross = xr_new<CUIStatic>("cross");
        cross->SetAutoDelete(true);
        m_close->AttachChild(cross);
        const Fvector2 size = m_close->GetWndSize();
        const float side = size.y * 0.5f;
        const float kx = UI().get_current_kx();
        cross->SetWndPos({ (size.x - side * kx) * 0.5f, (size.y - side) * 0.5f });
        cross->SetWndSize({ side * kx, side });
        auto skin = std::make_unique<SUIShapeSkin>();
        skin->looks[SUIShapeSkin::Normal].kind = SUIShapeStyle::Kind::Cross;
        skin->looks[SUIShapeSkin::Normal].fill = UIShape::ParseColor(xml.ReadAttrib(path, 0, "cross", "$text_dim"), 0xFFFFFFFF);
        skin->looks[SUIShapeSkin::Normal].border = 1.5f;
        cross->SetShapeSkin(std::move(skin));
        cross->Enable(false);
    }

    m_chip_skin = UIShape::ReadSkin(xml, node("chip"), 0);
    CUIXmlInit::InitFont(xml, strconcat(path, panel_node, ":chip:text"), 0, m_chip_color, m_chip_font);
    CUIXmlInit::InitFont(xml, strconcat(path, panel_node, ":plus:text"), 0, m_plus_color, m_plus_font);
    CUIXmlInit::InitFont(xml, strconcat(path, panel_node, ":caption:text"), 0, m_caption_color, m_caption_font);
    m_rule_skin = UIShape::ReadSkin(xml, node("rule"), 0);

    Show(false);
    return true;
}

bool CUIInventoryHotkeysPanel::IsUnderCursor() const
{
    if (!IsShown())
        return false;
    Frect rect;
    const_cast<CUIInventoryHotkeysPanel*>(this)->GetAbsoluteRect(rect);
    Fvector2 cursor = GetUICursor().GetCursorPosition();
    return rect.in(cursor);
}

bool CUIInventoryHotkeysPanel::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    inherited::OnMouseAction(x, y, mouse_action);
    // nothing under the panel takes a click or the wheel while the cursor is over it
    return true;
}

CUIStatic* CUIInventoryHotkeysPanel::MakeChip(CUIWindow* row, pcstr text, float x, float height)
{
    auto* chip = xr_new<CUIStatic>("chip");
    chip->SetAutoDelete(true);
    row->AttachChild(chip);
    if (m_chip_font)
        chip->TextItemControl()->SetFont(m_chip_font);
    chip->TextItemControl()->SetTextColor(m_chip_color);
    chip->TextItemControl()->SetTextAlignment(CGameFont::alCenter);
    chip->TextItemControl()->SetVTextAlignment(valCenter);
    chip->SetText(text);
    chip->SetHeight(height);
    chip->AdjustWidthToText();
    chip->SetWidth(chip->GetWidth() + 2.0f * m_chip_padding * UI().get_current_kx());
    chip->SetWndPos({ x, (m_row_height - height) * 0.5f });
    if (m_chip_skin)
        chip->SetShapeSkin(std::make_unique<SUIShapeSkin>(*m_chip_skin));
    return chip;
}

void CUIInventoryHotkeysPanel::Fill(u8 window)
{
    if (!m_list)
        return;
    m_window = window;
    m_list->Clear();

    const float kx = UI().get_current_kx();
    const float width = m_list->GetDesiredChildWidth();
    const float chip_height = std::max(m_row_height - 6.0f, 10.0f);
    const auto key_name = [](int dik) -> xr_string
    {
        if (const keyboard_key* key = DikToPtr(dik, true))
            return key->key_local_name.size() ? key->key_local_name : xr_string(key->key_name);
        return "?";
    };

    for (const SInventoryHotkey& hotkey : inventory_hotkeys::all())
    {
        if (!(hotkey.windows & window))
            continue;

        // The keys column: the label when the hotkey has one, else its modifiers and its trigger.
        xr_vector<xr_string> chips;
        if (!hotkey.label.empty())
        {
            for (const shared_str& part : hotkey.label)
            {
                if (0 == strncmp(part.c_str(), "action:", 7))
                {
                    const EGameActions action = ActionNameToId(part.c_str() + 7, true);
                    cpcstr bound = action != kNOTBINDED ? GetActionBinding(action) : nullptr;
                    chips.emplace_back(bound && bound[0] ? bound : "-");
                }
                else
                    chips.emplace_back(StringTable().translate(part).c_str());
            }
        }
        else
        {
            if (hotkey.modifiers & SInventoryHotkey::Ctrl)
                chips.emplace_back(StringTable().translate("st_da_hk_ctrl").c_str());
            if (hotkey.modifiers & SInventoryHotkey::Alt)
                chips.emplace_back(StringTable().translate("st_da_hk_alt").c_str());
            if (hotkey.modifiers & SInventoryHotkey::Shift)
                chips.emplace_back(StringTable().translate("st_da_hk_shift").c_str());
            if (hotkey.hold_key)
                chips.emplace_back(key_name(hotkey.hold_key));
            constexpr pcstr triggers[] = { "", "st_da_hk_hover", "st_da_hk_lmb", "st_da_hk_rmb", "st_da_hk_mmb",
                "st_da_hk_wheel_up", "st_da_hk_wheel_down", "", "st_da_hk_hold" };
            static_assert(std::size(triggers) == size_t(EHotkeyTrigger::Hold) + 1);
            if (hotkey.trigger == EHotkeyTrigger::Key)
                chips.emplace_back(key_name(hotkey.key));
            else if (triggers[u8(hotkey.trigger)][0])
                chips.emplace_back(StringTable().translate(triggers[u8(hotkey.trigger)]).c_str());
        }

        auto* row = xr_new<CUIWindow>("hotkey");
        row->SetWndSize({ width, m_row_height });
        float x = 0.0f;
        for (size_t i = 0; i < chips.size(); ++i)
        {
            if (i)
            {
                auto* plus = xr_new<CUIStatic>("plus");
                plus->SetAutoDelete(true);
                row->AttachChild(plus);
                if (m_plus_font)
                    plus->TextItemControl()->SetFont(m_plus_font);
                plus->TextItemControl()->SetTextColor(m_plus_color);
                plus->TextItemControl()->SetTextAlignment(CGameFont::alCenter);
                plus->TextItemControl()->SetVTextAlignment(valCenter);
                plus->SetText("+");
                plus->SetWndPos({ x, 0.0f });
                plus->SetWndSize({ 8.0f * kx, m_row_height });
                x += 8.0f * kx;
            }
            x += MakeChip(row, chips[i].c_str(), x, chip_height)->GetWidth() + m_chip_gap * kx;
        }

        // What it does, in the rest of the row, as many lines as it takes.
        const float keys = std::max(m_keys_width * kx, x + m_chip_gap * kx);
        auto* caption = xr_new<CUIStatic>("caption");
        caption->SetAutoDelete(true);
        row->AttachChild(caption);
        if (m_caption_font)
            caption->TextItemControl()->SetFont(m_caption_font);
        caption->TextItemControl()->SetTextColor(m_caption_color);
        caption->TextItemControl()->SetVTextAlignment(valCenter);
        caption->SetTextComplexMode(true);
        caption->SetWndPos({ keys, 0.0f });
        caption->SetWndSize({ std::max(width - keys - 8.0f * kx, 20.0f), m_row_height });
        caption->SetText(StringTable().translate(hotkey.Caption(window)).c_str());
        caption->AdjustHeightToText();
        const float height = std::max(m_row_height, caption->GetHeight() + 4.0f);
        caption->SetHeight(height);
        row->SetHeight(height);

        // a hair between rows
        if (m_rule_skin)
        {
            auto* rule = xr_new<CUIStatic>("rule");
            rule->SetAutoDelete(true);
            row->AttachChild(rule);
            rule->SetWndPos({ 0.0f, height - 1.0f });
            rule->SetWndSize({ width, 1.0f });
            rule->SetShapeSkin(std::make_unique<SUIShapeSkin>(*m_rule_skin));
        }
        m_list->AddWindow(row, true);
    }
}

// =================================================================================================
// The actor menu's side: what a hotkey does in the window it is pressed in, the picks of a
// comparison, the hint of an item as new

bool RemoveItemFromList(CUIDragDropListEx* lst, PIItem pItem); // UIActorMenuInventory.cpp

// ui_inv_hotkeys: the panel is up whenever the menu opens (console_commands_ui.cpp)
u32 g_inventory_hotkeys_panel = 0;

namespace
{
pcstr window_name(u8 window)
{
    switch (window)
    {
    case SInventoryHotkey::Trade: return "trade";
    case SInventoryHotkey::Loot: return "loot";
    case SInventoryHotkey::Upgrade: return "upgrade";
    default: return "inventory";
    }
}

bool shown_on_screen(CUIWindow* w)
{
    for (; w; w = w->GetParent())
    {
        if (!w->IsShown())
            return false;
    }
    return true;
}
} // namespace

void CUIActorMenu::InitHotkeys(CUIXml& uiXml)
{
    // a mod's hotkeys and weights are read again with the UI
    inventory_hotkeys::reset();
    item_compare::reset();

    // The picks of a comparison, over the icons of every grid.
    for (CUIDragDropListEx* list : m_pLists)
    {
        if (list)
            list->SetCellState(CUIDragDropListEx::CELL_STATE(this, &CUIActorMenu::CellPickState));
    }

    m_hotkeys_panel = xr_new<CUIInventoryHotkeysPanel>();
    if (m_hotkeys_panel->Init(uiXml))
    {
        m_hotkeys_panel->SetAutoDelete(true);
        AttachChild(m_hotkeys_panel);
        RegisterCallback(m_hotkeys_panel->CloseButton(), BUTTON_CLICKED,
            CUIWndCallback::void_function(this, &CUIActorMenu::OnHotkeysPanelClose));
    }
    else
        xr_delete(m_hotkeys_panel);

    // over everything, the comparison last
    m_compare_wnd = xr_new<CUIItemCompareWnd>();
    if (m_compare_wnd->Init(uiXml))
    {
        m_compare_wnd->SetAutoDelete(true);
        AttachChild(m_compare_wnd);
    }
    else
        xr_delete(m_compare_wnd);
}

u8 CUIActorMenu::HotkeyWindow() const
{
    switch (m_currMenuMode)
    {
    case mmTrade: return SInventoryHotkey::Trade;
    case mmDeadBodySearch: return SInventoryHotkey::Loot;
    case mmUpgrade: return SInventoryHotkey::Upgrade;
    case mmInventory: return SInventoryHotkey::Inventory;
    default: return 0;
    }
}

void CUIActorMenu::SyncHotkeys()
{
    if (m_compare_wnd && m_compare_wnd->IsShown() && m_currMenuMode == mmUndefined)
        m_compare_wnd->Close();
    if (!m_hotkeys_panel)
        return;
    const bool show = g_inventory_hotkeys_panel && m_currMenuMode != mmUndefined;
    if (show)
        m_hotkeys_panel->Fill(HotkeyWindow());
    m_hotkeys_panel->Show(show);
}

void CUIActorMenu::ShowHotkeysPanel(bool show)
{
    g_inventory_hotkeys_panel = show;
    SyncHotkeys();
}

bool CUIActorMenu::HotkeysPanelShown() const { return m_hotkeys_panel && m_hotkeys_panel->IsShown(); }

void CUIActorMenu::OnHotkeysPanelClose(CUIWindow*, void*) { ShowHotkeysPanel(false); }

bool CUIActorMenu::OverHotkeysPanel() const { return m_hotkeys_panel && m_hotkeys_panel->IsUnderCursor(); }

CUICellItem* CUIActorMenu::CellUnderCursor()
{
    Fvector2 cursor = GetUICursor().GetCursorPosition();
    for (CUIDragDropListEx* list : m_pLists)
    {
        if (!list || !shown_on_screen(list))
            continue;
        Frect area;
        list->GetAbsoluteRect(area);
        if (!area.in(cursor))
            continue;
        for (CUIWindow* w : list->GetContainer()->GetChildWndList())
        {
            if (!w->IsShown())
                continue;
            Frect rect;
            w->GetAbsoluteRect(rect);
            if (rect.in(cursor))
                return static_cast<CUICellItem*>(w);
        }
    }
    return nullptr;
}

bool CUIActorMenu::FindCellCenter(pcstr section, Fvector2& at)
{
    for (CUIDragDropListEx* list : m_pLists)
    {
        if (!list || !shown_on_screen(list))
            continue;
        for (CUIWindow* w : list->GetContainer()->GetChildWndList())
        {
            CUICellItem* cell = static_cast<CUICellItem*>(w);
            const PIItem item = static_cast<PIItem>(cell->m_pData);
            if (!cell->IsShown() || !item || item->object().cNameSect() != section)
                continue;
            Frect rect;
            cell->GetAbsoluteRect(rect);
            rect.getcenter(at);
            return true;
        }
    }
    return false;
}

bool CUIActorMenu::GridsCovered() const
{
    if (m_compare_wnd && m_compare_wnd->IsShown())
        return true;
    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        if (toolbar->SortModesShown())
            return true;
    }
    return false;
}

bool CUIActorMenu::TryHotkey(EHotkeyTrigger trigger, int key)
{
    const u8 window = HotkeyWindow();
    if (!window || CUIDragDropListEx::m_drag_item || m_UIPropertiesBox->IsShown())
        return false;
    // a key works wherever the cursor is; a click only on the grids
    const bool mouse = trigger != EHotkeyTrigger::Key;
    if (mouse && (OverHotkeysPanel() || GridsCovered()))
        return false;
    const SInventoryHotkey* hotkey = inventory_hotkeys::find(trigger, window, key);
    return hotkey && RunWindowAction(hotkey->action, CellUnderCursor());
}

CUICellItem* CUIActorMenu::CellOfItem(u16 id)
{
    const auto holds = [id](CUICellItem* cell)
    {
        const PIItem item = static_cast<PIItem>(cell->m_pData);
        return item && item->object_id() == id;
    };
    for (CUIDragDropListEx* list : m_pLists)
    {
        if (!list)
            continue;
        for (CUIWindow* w : list->GetContainer()->GetChildWndList())
        {
            CUICellItem* cell = static_cast<CUICellItem*>(w);
            if (holds(cell))
                return cell;
            for (u32 i = 0; i < cell->ChildsCount(); ++i)
            {
                if (holds(cell->Child(i)))
                    return cell;
            }
        }
    }
    return nullptr;
}

void CUIActorMenu::RunPendingHover()
{
    if (m_pending_hover.item == u16(-1))
        return;
    const SPendingHover hover = m_pending_hover;
    m_pending_hover = {};
    CUICellItem* cell = CellOfItem(hover.item);
    if (cell && !RunWindowAction(hover.action, cell))
        m_hover_refused = hover.item;
}

bool CUIActorMenu::TryHoverHotkey(CUICellItem* itm)
{
    const u8 window = HotkeyWindow();
    const SInventoryHotkey* hotkey = window ? inventory_hotkeys::find(EHotkeyTrigger::Hover, window) : nullptr;
    if (!hotkey)
    {
        // the modifiers let go: the next press acts at once, wherever the cursor is
        m_hover_cursor = { -1.0f, -1.0f };
        m_hover_refused = u16(-1);
        return false;
    }
    if (!itm || !itm->m_pData || CUIDragDropListEx::m_drag_item || m_UIPropertiesBox->IsShown() || OverHotkeysPanel() ||
        GridsCovered())
    {
        return false;
    }
    // What is worn is never taken by a passing cursor: the way from one grid to the other crosses
    // the slots, the belt and the quick slots.
    const EDDListType list = GetListType(itm->OwnerList());
    if (list == iActorSlot || list == iActorBelt || list == iQuickSlot)
        return false;
    // an item the window refused stays where it is until the cursor is over another
    const u16 id = static_cast<PIItem>(itm->m_pData)->object_id();
    if (id == m_hover_refused)
        return false;
    m_hover_refused = u16(-1);
    const Fvector2 cursor = GetUICursor().GetCursorPosition();
    if (cursor.similar(m_hover_cursor, 0.5f))
        return false;
    m_hover_cursor = cursor;
    m_pending_hover = { hotkey->action, id };
    return true;
}

bool CUIActorMenu::RunHotkeyAction(pcstr action)
{
    return action && RunWindowAction(action, CellUnderCursor());
}

bool CUIActorMenu::RunWindowAction(const shared_str& action, CUICellItem* cell)
{
    // the window's own
    if (action == "toggle_panel")
    {
        ShowHotkeysPanel(!HotkeysPanelShown());
        return true;
    }
    if (action == "compare")
    {
        OpenCompare();
        return true;
    }
    if (action == "clear_picks")
    {
        const bool had = !m_picks.empty();
        ClearPicks();
        return had;
    }
    if (0 == strncmp(action.c_str(), "script:", 7))
    {
        luabind::functor<void> function;
        if (!GEnv.ScriptEngine->functor(action.c_str() + 7, function))
        {
            Msg("! [ui] inventory hotkey: no function [%s]", action.c_str() + 7);
            return false;
        }
        const PIItem item = cell ? static_cast<PIItem>(cell->m_pData) : nullptr;
        function(item ? item->object().lua_game_object() : nullptr, window_name(HotkeyWindow()));
        return true;
    }

    // an item's
    if (!cell || !cell->m_pData)
        return false;
    if (action == "move")
        return MoveCell(cell, true);
    if (action == "move_one")
        return MoveCell(cell, false);
    if (action == "split")
        return SplitCell(cell);
    if (action == "drop")
        return DropCell(cell, false);
    if (action == "drop_stack")
        return DropCell(cell, true);
    if (action == "pick")
    {
        TogglePick(cell);
        return true;
    }
    if (action == "use")
    {
        const PIItem item = static_cast<PIItem>(cell->m_pData);
        if (item->parent_id() != m_pActorInvOwner->object_id() || m_currMenuMode == mmUpgrade)
            return false;
        SetCurrentItem(cell);
        InfoCurItem(nullptr);
        const bool used = TryUseItem(cell);
        UpdateItemsPlace();
        return used;
    }
    Msg("! [ui] inventory hotkey: unknown action [%s]", action.c_str());
    return false;
}

bool CUIActorMenu::MoveToOtherSide(CUICellItem* itm, bool equip)
{
    switch (GetListType(itm->OwnerList()))
    {
    case iActorSlot:
    case iActorBelt:
        return m_currMenuMode == mmDeadBodySearch ? ToDeadBodyBag(itm, false) : ToBag(itm, false);
    case iActorBag:
        switch (m_currMenuMode)
        {
        case mmTrade: return ToActorTrade(itm, false);
        case mmDeadBodySearch: return ToDeadBodyBag(itm, false);
        case mmInventory:
        {
            // what a double click does, short of eating it
            if (!equip)
                return false;
            if (TryActiveSlot(itm))
                return true;
            const PIItem item = static_cast<PIItem>(itm->m_pData);
            CInventory& inventory = m_pActorInvOwner->inventory();
            if (!inventory.SlotIsPersistent(item->BaseSlot()) && inventory.ItemFromSlot(item->BaseSlot()) == item)
                return ToBag(itm, false);
            return ToSlot(itm, false, item->BaseSlot()) || ToBelt(itm, false) || ToSlot(itm, true, item->BaseSlot());
        }
        default: return false;
        }
    case iActorTrade: return ToBag(itm, false);
    case iPartnerTradeBag: return ToPartnerTrade(itm, false);
    case iPartnerTrade: return ToPartnerTradeBag(itm, false);
    case iDeadBodyBag: return ToBag(itm, false);
    default: return false;
    }
}

bool CUIActorMenu::MoveCell(CUICellItem* cell, bool whole_stack)
{
    // A pile changes hands item by item; putting on is one item whatever the pile.
    const bool equip = m_currMenuMode == mmInventory;
    const u32 count = whole_stack && !equip ? cell->ChildsCount() + 1 : 1;
    SetCurrentItem(nullptr);
    InfoCurItem(nullptr);

    bool moved = false;
    for (u32 i = 0; i < count; ++i)
    {
        // the cell itself goes with its last item
        const bool last = !cell->ChildsCount();
        if (!MoveToOtherSide(cell, equip))
            break;
        moved = true;
        if (last)
            break;
    }
    if (moved)
    {
        UpdateItemsPlace();
        if (m_currMenuMode == mmTrade)
            UpdatePrices();
        PlaySnd(eItemToRuck);
    }
    return moved;
}

bool CUIActorMenu::PaneOfList(CUIDragDropListEx* list, EInventoryPane& pane)
{
    for (const EInventoryPane p : { EInventoryPane::Actor, EInventoryPane::Loot, EInventoryPane::Trader })
    {
        if (list && GetInventoryPaneList(p) == list)
        {
            pane = p;
            return true;
        }
    }
    return false;
}

CGameObject* CUIActorMenu::PaneOwner(EInventoryPane pane)
{
    switch (pane)
    {
    case EInventoryPane::Actor: return smart_cast<CGameObject*>(m_pActorInvOwner);
    case EInventoryPane::Loot:
        if (m_pPartnerInvOwner)
            return m_pPartnerInvOwner->deadbody_can_take_status() ? smart_cast<CGameObject*>(m_pPartnerInvOwner) : nullptr;
        return m_pInvBox && m_pInvBox->can_take() ? m_pInvBox : nullptr;
    default: return nullptr; // a trader keeps his goods as they are
    }
}

bool CUIActorMenu::SplitCell(CUICellItem* cell)
{
    EInventoryPane pane;
    const PIItem item = static_cast<PIItem>(cell->m_pData);
    if (!PaneOfList(cell->OwnerList(), pane) || item->IsQuestItem() || m_pending_sort.active || m_currMenuMode == mmUpgrade)
        return false;

    // A pile of items: half of it to the other side of a trade or a search.
    if (cell->ChildsCount() && m_currMenuMode != mmInventory)
    {
        const u32 half = (cell->ChildsCount() + 1) / 2;
        SetCurrentItem(nullptr);
        InfoCurItem(nullptr);
        u32 moved = 0;
        while (moved < half && cell->ChildsCount() && MoveToOtherSide(cell, false))
            ++moved;
        if (moved)
        {
            UpdateItemsPlace();
            if (m_currMenuMode == mmTrade)
                UpdatePrices();
            PlaySnd(eItemToRuck);
        }
        return moved != 0;
    }

    CGameObject* owner = PaneOwner(pane);
    if (!owner)
        return false;
    const auto spawn = [](CGameObject* to, pcstr section, u16 rounds)
    {
        u32 vertex = to->ai_location().level_vertex_id();
        if ((!ai().get_level_graph() || !ai().level_graph().valid_vertex_id(vertex)) && Actor())
            vertex = Actor()->ai_location().level_vertex_id();
        CSE_Abstract* abstract = Level().spawn_item(section, to->Position(), vertex, to->ID(), true);
        if (CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(abstract); ammo && rounds)
            ammo->a_elapsed = rounds;
        NET_Packet packet;
        abstract->Spawn_Write(packet, TRUE);
        Level().Send(packet, net_flags(TRUE));
        F_entity_Destroy(abstract);
    };

    SetCurrentItem(nullptr);
    InfoCurItem(nullptr);
    m_pending_sort = {};
    m_pending_sort.panes = u8(1 << u8(pane));

    if (CWeaponAmmo* ammo = smart_cast<CWeaponAmmo*>(item))
    {
        // One box in two. In a search the other half goes across at once: the way to take a few
        // rounds and leave the rest.
        if (ammo->m_boxCurr < 2)
            return false;
        const u16 half = ammo->m_boxCurr / 2;
        CGameObject* to = owner;
        if (m_currMenuMode == mmDeadBodySearch)
        {
            const EInventoryPane other = pane == EInventoryPane::Actor ? EInventoryPane::Loot : EInventoryPane::Actor;
            if (CGameObject* across = PaneOwner(other))
            {
                to = across;
                m_pending_sort.panes |= u8(1 << u8(other));
            }
        }
        ammo->m_boxCurr -= half;
        if (ammo->m_pInventory)
            ammo->m_pInventory->InvalidateState();
        spawn(to, ammo->object().cNameSect().c_str(), half);
    }
    else
    {
        // a numbered-section stack: vodka_3 is vodka_2 and vodka
        shared_str first, second;
        if (!inventory_tools::split_family(item->object().cNameSect(), first, second))
            return false;
        for (CUIDragDropListEx* l : m_pLists)
        {
            while (l && RemoveItemFromList(l, item)) {}
        }
        m_pending_sort.released.push_back(item->object_id());
        item->object().DestroyObject();
        spawn(owner, first.c_str(), 0);
        spawn(owner, second.c_str(), 0);
    }

    // laid out now, and again once the new items have arrived
    for (const EInventoryPane p : { EInventoryPane::Actor, EInventoryPane::Loot })
    {
        if (m_pending_sort.panes & (1 << u8(p)))
            RefillInventoryPane(p, m_pending_sort.released);
    }
    m_pending_sort.started = Device.dwTimeContinual;
    m_pending_sort.active = true;
    PlaySnd(eItemToRuck);
    return true;
}

bool CUIActorMenu::DropCell(CUICellItem* cell, bool whole_stack)
{
    const PIItem item = static_cast<PIItem>(cell->m_pData);
    if (item->IsQuestItem() || item->parent_id() != m_pActorInvOwner->object_id() || m_currMenuMode == mmUpgrade)
        return false;
    InfoCurItem(nullptr);
    SetCurrentItem(cell);
    if (whole_stack)
        DropAllCurrentItem();
    else
    {
        SendEvent_Item_Drop(item, m_pActorInvOwner->object_id());
        SetCurrentItem(nullptr);
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// The comparison

bool CUIActorMenu::IsPicked(CUICellItem* cell) const
{
    if (m_picks.empty())
        return false;
    const auto picked = [this](const CUICellItem* c)
    {
        const PIItem item = static_cast<PIItem>(c->m_pData);
        return item && std::find(m_picks.begin(), m_picks.end(), item->object_id()) != m_picks.end();
    };
    if (picked(cell))
        return true;
    for (u32 i = 0; i < cell->ChildsCount(); ++i)
    {
        if (picked(cell->Child(i)))
            return true;
    }
    return false;
}

u8 CUIActorMenu::CellPickState(CUICellItem* cell)
{
    if (m_picks.empty() || !cell)
        return 0;
    if (IsPicked(cell))
        return CUIDragDropListEx::cellPicked;
    const PIItem item = static_cast<PIItem>(cell->m_pData);
    return item && u8(item_compare::kind_of(*item)) != m_pick_kind ? CUIDragDropListEx::cellDimmed : 0;
}

void CUIActorMenu::TogglePick(CUICellItem* cell)
{
    const PIItem item = static_cast<PIItem>(cell->m_pData);
    if (IsPicked(cell))
    {
        // the cell's items leave the comparison, a pile's grouped ones too
        const auto drop = [this](CUICellItem* c)
        {
            if (const PIItem it = static_cast<PIItem>(c->m_pData))
                m_picks.erase(std::remove(m_picks.begin(), m_picks.end(), it->object_id()), m_picks.end());
        };
        drop(cell);
        for (u32 i = 0; i < cell->ChildsCount(); ++i)
            drop(cell->Child(i));
    }
    else
    {
        // only the kind of the first pick, and no more than a comparison takes
        const item_compare::EKind kind = item_compare::kind_of(*item);
        if ((!m_picks.empty() && u8(kind) != m_pick_kind) || m_picks.size() >= item_compare::max_items())
            return;
        m_picks.push_back(item->object_id());
        m_pick_kind = u8(kind);
    }
    PlaySnd(eProperties);
}

bool CUIActorMenu::PickItem(CInventoryItem* item)
{
    if (!item)
        return false;
    if (const auto it = std::find(m_picks.begin(), m_picks.end(), item->object_id()); it != m_picks.end())
    {
        m_picks.erase(it);
        return true;
    }
    const item_compare::EKind kind = item_compare::kind_of(*item);
    if ((!m_picks.empty() && u8(kind) != m_pick_kind) || m_picks.size() >= item_compare::max_items())
        return false;
    m_picks.push_back(item->object_id());
    m_pick_kind = u8(kind);
    return true;
}

void CUIActorMenu::ClearPicks()
{
    m_picks.clear();
    m_pick_kind = 0;
}

void CUIActorMenu::PrunePicks()
{
    m_picks.erase(std::remove_if(m_picks.begin(), m_picks.end(),
                      [](u16 id) { return !smart_cast<CInventoryItem*>(Level().Objects.net_Find(id)); }),
        m_picks.end());

    for (CUIInventoryToolbar* toolbar : m_inventory_tools)
    {
        toolbar->EnableAction("compare", m_picks.size() >= 2);
        toolbar->EnableAction("clear_picks", !m_picks.empty());
    }
}

u32 CUIActorMenu::ComparePrice(CInventoryItem& item)
{
    // a trader's price where there is one, the way the hint shows it
    if (m_currMenuMode == mmTrade && m_partner_trade && item.m_pInventory)
    {
        const CInventoryOwner* owner = smart_cast<CInventoryOwner*>(item.m_pInventory->GetOwner());
        return m_partner_trade->GetItemPrice(&item, owner == m_pActorInvOwner);
    }
    // elsewhere the base price the condition leaves, on the trade's own curve
    const float power = pSettings->read_if_exists<float>("trade", "buy_condition_koeff", 1.0f);
    const float factor = item.GetDrainCondition() ? 1.0f : clampr(powf(item.GetCondition() * 0.9f + 0.11f, power), 0.0f, 1.0f);
    return u32(float(item.Cost()) * factor);
}

void CUIActorMenu::OpenCompare()
{
    PrunePicks();
    if (!m_compare_wnd || m_picks.size() < 2)
        return;
    xr_vector<CInventoryItem*> items;
    for (const u16 id : m_picks)
        items.push_back(smart_cast<CInventoryItem*>(Level().Objects.net_Find(id)));
    CloseSortModes();
    InfoCurItem(nullptr);
    m_compare_wnd->Open(items, [this](CInventoryItem& item) { return ComparePrice(item); });
}

void CUIActorMenu::UpdatePristineInfo(CUICellItem* cell_item)
{
    m_pristine_shown = false;
    if (!m_ItemInfoPristine || !cell_item || !inventory_hotkeys::holding("pristine", HotkeyWindow()))
        return;
    const PIItem item = static_cast<PIItem>(cell_item->m_pData);
    if (!item || !item->IsUsingCondition())
        return;

    // The same hint over the item as new: full condition, no faults, no fouling.
    item->BeginPristineView();
    m_ItemInfoPristine->InitItem(cell_item, nullptr, m_currMenuMode == mmTrade ? ComparePrice(*item) : u32(-1));
    item->EndPristineView();
    if (m_ItemInfoPristine->UIName)
    {
        string512 name;
        xr_sprintf(name, "%s %s", item->NameItem(), StringTable().translate("st_da_pristine_suffix").c_str());
        m_ItemInfoPristine->UIName->SetText(name);
    }

    // Beside the item's own hint, on whichever side has the room; where neither has, the two move
    // together, the item's own first.
    const Frect main = m_ItemInfo->GetWndRect();
    const float width = m_ItemInfoPristine->GetWidth();
    const float gap = 4.0f;
    const float right = UI_BASE_WIDTH - GetWndRect().left;
    float x;
    if (main.right + gap + width <= right)
        x = main.right + gap;
    else if (main.left - gap - width >= 0.0f)
        x = main.left - gap - width;
    else
    {
        const float left = clampr(main.left, 0.0f, std::max(0.0f, right - main.width() - gap - width));
        m_ItemInfo->SetWndPos({ left, main.top });
        x = left + main.width() + gap;
    }
    const float y = std::max(0.0f, std::min(main.top, UI_BASE_HEIGHT - m_ItemInfoPristine->GetHeight()));
    m_ItemInfoPristine->SetWndPos({ x, y });
    m_pristine_shown = true;
}
