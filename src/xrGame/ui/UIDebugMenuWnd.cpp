// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "StdAfx.h"
#include "xrEngine/XR_IOConsole.h"
#include "UIDebugMenuWnd.h"

#include "ModSearch.h"
#include "UIHelper.h"
#include "UIInventoryUtilities.h"
#include "UIXmlInit.h"
#include "Actor.h"
#include "Level.h"
#include "UIGameSP.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/xr_level_controller.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrUICore/EditBox/UIEditBox.h"
#include "xrUICore/Static/UIShape.h"
#include "xrUICore/Static/UIStatic.h"

using luabind::object;

// Options -> Game -> Other (dead_air_x64_gameplay_options.script): the key opens the window only when it is on
// (g_dar_debug_menu, the engine's: the console's and the editor's keys follow it too).

namespace
{
constexpr pcstr kScript = "dead_air_x64_debug_menu";

bool lua_is(const object& o, int type) { return o.is_valid() && luabind::type(o) == type; }

xr_string lua_str(const object& t, pcstr key)
{
    const object v = t[key];
    if (lua_is(v, LUA_TSTRING))
        return luabind::object_cast<pcstr>(v);
    if (lua_is(v, LUA_TNUMBER))
    {
        string64 text{};
        xr_sprintf(text, "%g", luabind::object_cast<float>(v));
        return text;
    }
    return {};
}

int lua_int(const object& t, pcstr key, int fallback)
{
    const object v = t[key];
    return lua_is(v, LUA_TNUMBER) ? iFloor(luabind::object_cast<float>(v) + 0.5f) : fallback;
}

// A caption may be a string id (translated) or a text already in the UI code page.
xr_string dbg_text(const xr_string& text)
{
    if (text.empty())
        return {};
    const bool idLike = text.size() <= 128 && text.find_first_of(" \t") == xr_string::npos;
    if (idLike && StringTable().has_translation(text.c_str()))
        return StringTable().translate(text.c_str()).c_str();
    return text;
}

xr_string dbg_format(pcstr id, auto... args)
{
    string1024 text{};
    xr_sprintf(text, sizeof(text), StringTable().translate(id).c_str(), args...);
    return text;
}

// ---- the search ----------------------------------------------------------------------------------------------------
// Every word of the query has to match the game name (the start of a word of it, inside it, or one typo away -
// ModSearch's rule) or the internal name (its start, the start of a part after an underscore, inside it, or its
// letters in order), in the layout it was typed in or in the other one (QWERTY <-> JCUKEN: "fr47" finds "ak47").

// the same key in the two layouts, lower case, Windows-1251
constexpr std::string_view kEnKeys = "qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
constexpr u8 kRuKeys[] = {0xE9, 0xF6, 0xF3, 0xEA, 0xE5, 0xED, 0xE3, 0xF8, 0xF9, 0xE7, 0xF5, 0xFA, 0xF4, 0xFB, 0xE2,
    0xE0, 0xEF, 0xF0, 0xEE, 0xEB, 0xE4, 0xE6, 0xFD, 0xFF, 0xF7, 0xF1, 0xEC, 0xE8, 0xF2, 0xFC, 0xE1, 0xFE, 0xB8};
static_assert(std::size(kRuKeys) == kEnKeys.size());

xr_string swap_layout(std::string_view word)
{
    xr_string out(word);
    for (char& c : out)
    {
        const size_t en = kEnKeys.find(c);
        if (en != std::string_view::npos)
        {
            c = static_cast<char>(kRuKeys[en]);
            continue;
        }
        for (size_t i = 0; i != std::size(kRuKeys); ++i)
            if (static_cast<u8>(c) == kRuKeys[i])
            {
                c = kEnKeys[i];
                break;
            }
    }
    return out;
}

// Cyrillic typed for a Latin internal name, letter by letter ("medkit" spelt in Cyrillic finds it); Windows-1251,
// lower case.
xr_string transliterate(std::string_view word)
{
    static constexpr pcstr kLatin[32] = {"a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "h", "c", "ch", "sh", "sch", "", "y", "", "e", "yu", "ya"};
    xr_string out;
    for (const char c : word)
    {
        const u8 b = static_cast<u8>(c);
        if (b >= 0xE0)
            out += kLatin[b - 0xE0];
        else if (b == 0xB8)
            out += "e";
        else
            out += c;
    }
    return out;
}

xr_string lower_ascii(std::string_view text)
{
    xr_string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool subsequence(std::string_view text, std::string_view word)
{
    size_t at = 0;
    for (const char c : word)
    {
        at = text.find(c, at);
        if (at == std::string_view::npos)
            return false;
        ++at;
    }
    return true;
}

float word_score(std::string_view name, std::string_view sec, std::string_view word)
{
    float best = 0.f;
    if (const size_t at = name.find(word); at != std::string_view::npos)
        best = std::max(best, at == 0 ? 10.f : name[at - 1] == ' ' ? 8.f : 5.f);
    if (const size_t at = sec.find(word); at != std::string_view::npos)
        best = std::max(best, at == 0 ? 9.f : sec[at - 1] == '_' ? 7.f : 4.f);
    if (best > 0.f)
        return best;
    // one typo in a long word of the name
    if (word.size() >= 5)
    {
        const xr_vector<xr_string> one{xr_string(word)};
        ModSearch::Fields fields;
        fields.name = xr_string(name);
        if (ModSearch::Score(one, fields) > 0.f)
            return 3.f;
    }
    if (word.size() >= 3 && subsequence(sec, word))
        return 1.f;
    return 0.f;
}

// 0: no match
float entry_score(std::string_view name, std::string_view sec, const xr_vector<xr_string>& words, std::string_view whole)
{
    float total = 0.f;
    for (const xr_string& word : words)
    {
        float s = word_score(name, sec, word);
        if (s <= 0.f)
            s = 0.9f * word_score(name, sec, swap_layout(word));
        if (s <= 0.f)
            s = 0.8f * word_score(name, sec, transliterate(word));
        if (s <= 0.f)
            return 0.f;
        total += s;
    }
    if (whole == sec || whole == name)
        total += 100.f;
    return total;
}

CUIShape* dbg_shape(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* shape = xr_new<CUIShape>();
    CUIXmlInit::InitWindow(xml, path, 0, shape);
    UIShape::Read(xml, path, 0, shape->style);
    shape->SetAutoDelete(true);
    parent->AttachChild(shape);
    return shape;
}

CUIShapeButton* dbg_button(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* button = xr_new<CUIShapeButton>();
    CUIXmlInit::InitButton(xml, path, 0, button);
    button->InitLooks(xml, path, 0);
    button->SetAutoDelete(true);
    parent->AttachChild(button);
    return button;
}

CUIWindow* dbg_group(CUIXml& xml, pcstr path, CUIWindow* parent)
{
    auto* group = xr_new<CUIWindow>(path);
    CUIXmlInit::InitWindow(xml, path, 0, group);
    group->SetAutoDelete(true);
    parent->AttachChild(group);
    return group;
}

u32 dbg_color(CUIXml& xml, pcstr attribute, u32 fallback)
{
    return UIShape::ParseColor(xml.ReadAttrib("main:colors", 0, attribute, nullptr), fallback);
}

// QA's commands for the open window (dar_debug_menu_do), run on its next frame
xr_vector<xr_string>& dbg_queue()
{
    static xr_vector<xr_string> queue;
    return queue;
}
} // namespace

// ---- the list ------------------------------------------------------------------------------------------------------
// As many rows as the list's height holds, filled from the entries under the scroll position: thousands of items
// cost the rows on screen and no more.
class CUIDebugRow final : public CUIWindow
{
public:
    CUIDebugRow() : CUIWindow("CUIDebugRow") {}

    CUIDebugMenuWnd* owner{};
    size_t slot{};
    size_t shown{size_t(-1)};
    bool selected{};
    SUIShapeStyle looks[UIShape::LookCount];
    CUIShape* back{};
    CUIStatic* name{};
    CUIStatic* sec{};
    CUIStatic* badge{};
    CUIStatic* note{};

    void Draw() override
    {
        back->style = looks[selected ? CUIShapeButton::lookChecked :
                CursorOverWindow()   ? CUIShapeButton::lookHighlight :
                                       CUIShapeButton::lookNormal];
        CUIWindow::Draw();
    }

    bool OnMouseDown(int button) override
    {
        if (button != MOUSE_1 || shown == size_t(-1))
            return CUIWindow::OnMouseDown(button);
        owner->OnRowClicked(shown, false);
        return true;
    }

    bool OnDbClick() override
    {
        if (shown != size_t(-1))
            owner->OnRowClicked(shown, true);
        return true;
    }
};

class CUIDebugList final : public CUIWindow
{
public:
    CUIDebugList() : CUIWindow("CUIDebugList") {}

    CUIDebugMenuWnd* owner{};
    xr_vector<CUIDebugRow*> rows;
    CUIShape* track{};
    CUIShape* thumb{};
    float rowHeight{42.f};
    float barLeft{};
    size_t total{};
    size_t top{};
    bool dragging{};

    size_t Visible() const { return rows.size(); }
    size_t MaxTop() const { return total > Visible() ? total - Visible() : 0; }

    void SetTop(size_t value)
    {
        top = std::min(value, MaxTop());
        LayoutBar();
    }

    void LayoutBar()
    {
        const bool needed = total > Visible();
        track->Show(needed);
        thumb->Show(needed);
        if (!needed)
            return;
        const float h = track->GetHeight();
        const float thumbH = std::max(h * float(Visible()) / float(total), 20.f);
        const float y = (h - thumbH) * float(top) / float(std::max<size_t>(MaxTop(), 1));
        thumb->SetHeight(thumbH);
        thumb->SetWndPos(Fvector2().set(thumb->GetWndPos().x, track->GetWndPos().y + y));
    }

    void OnMouseScroll(float direction) override
    {
        const size_t step = 3;
        if (direction == WINDOW_MOUSE_WHEEL_UP)
            SetTop(top > step ? top - step : 0);
        else if (direction == WINDOW_MOUSE_WHEEL_DOWN)
            SetTop(top + step);
        owner->OnListScrolled();
    }

    void DragTo(float y)
    {
        const float h = track->GetHeight();
        const float k = std::clamp((y - track->GetWndPos().y) / std::max(h, 1.f), 0.f, 1.f);
        SetTop(size_t(k * float(MaxTop()) + 0.5f));
        owner->OnListScrolled();
    }

    bool OnMouseAction(float x, float y, EUIMessages action) override
    {
        if (dragging)
        {
            if (action == WINDOW_MOUSE_MOVE)
                DragTo(y);
            if (action == WINDOW_LBUTTON_UP || !pInput->iGetAsyncKeyState(MOUSE_1))
            {
                dragging = false;
                GetParent()->SetCapture(this, false);
            }
            return true;
        }
        if ((action == WINDOW_LBUTTON_DOWN || action == WINDOW_LBUTTON_DB_CLICK) && track->IsShown() && x >= barLeft)
        {
            dragging = true;
            GetParent()->SetCapture(this, true);
            DragTo(y);
            return true;
        }
        return CUIWindow::OnMouseAction(x, y, action);
    }
};

// ---- the window ----------------------------------------------------------------------------------------------------
CUIDebugMenuWnd::CUIDebugMenuWnd() : CUIDialogWnd(CUIDebugMenuWnd::GetDebugType()) {}

CUIDebugMenuWnd::~CUIDebugMenuWnd() { xr_delete(m_xml); }

bool CUIDebugMenuWnd::Enabled() { return g_dar_debug_menu != 0; }

bool CUIDebugMenuWnd::Init()
{
    m_xml = xr_new<CUIXml>();
    if (!m_xml->Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "ui_debug_menu.xml", false))
        return false;
    CUIXml& xml = *m_xml;

    CUIXmlInit::InitWindow(xml, "main", 0, this);
    m_textColor = dbg_color(xml, "text", 0xffece9e2);
    m_mutedColor = dbg_color(xml, "muted", 0xff9a968c);
    m_dimColor = dbg_color(xml, "dim", 0xff7c786f);
    m_accentColor = dbg_color(xml, "accent", 0xffd2be73);
    m_goodColor = dbg_color(xml, "good", 0xff7fb069);
    m_badColor = dbg_color(xml, "bad", 0xffdc645a);

    dbg_shape(xml, "main:shade", this);
    dbg_shape(xml, "main:shadow", this);
    dbg_shape(xml, "main:frame", this);
    UIHelper::CreateStatic(xml, "main:caption", this);
    m_hotkeyHint = UIHelper::CreateStatic(xml, "main:hotkey_hint", this);
    m_close = dbg_button(xml, "main:close", this);
    m_tabArea = dbg_group(xml, "main:tabs", this);
    dbg_shape(xml, "main:separator", this);

    dbg_shape(xml, "main:cat_back", this);
    UIHelper::CreateStatic(xml, "main:cat_caption", this);
    m_cats = xr_new<CUIShapeScrollView>();
    CUIXmlInit::InitScrollView(xml, "main:cats", 0, m_cats);
    m_cats->InitLooks(xml, "main:cats", 0);
    m_cats->SetAutoDelete(true);
    AttachChild(m_cats);

    dbg_shape(xml, "main:search_back", this);
    dbg_shape(xml, "main:search_icon", this);
    m_searchHint = UIHelper::CreateStatic(xml, "main:search_hint", this);
    m_search = UIHelper::CreateEditBox(xml, "main:search", this);
    m_count = UIHelper::CreateStatic(xml, "main:count", this);
    dbg_shape(xml, "main:list_back", this);

    // the list and its rows
    m_list = xr_new<CUIDebugList>();
    CUIXmlInit::InitWindow(xml, "main:list", 0, m_list);
    m_list->SetAutoDelete(true);
    m_list->owner = this;
    AttachChild(m_list);
    m_list->rowHeight = xml.ReadAttribFlt("main:list", 0, "row_height", 42.f);
    m_list->track = dbg_shape(xml, "main:list:track", m_list);
    m_list->thumb = dbg_shape(xml, "main:list:thumb", m_list);
    m_list->barLeft = m_list->track->GetWndPos().x - 6.f;
    const size_t rows = std::max<size_t>(size_t(m_list->GetHeight() / m_list->rowHeight), 1);
    for (size_t slot = 0; slot != rows; ++slot)
    {
        auto* row = xr_new<CUIDebugRow>();
        CUIXmlInit::InitWindow(xml, "main:list:row", 0, row);
        row->SetWndPos(Fvector2().set(0.f, float(slot) * m_list->rowHeight));
        row->owner = this;
        row->slot = slot;
        u32 unusedText[UIShape::LookCount]{};
        UIShape::ReadLooks(xml, "main:list:row:back", 0, row->looks, unusedText, 0);
        row->back = dbg_shape(xml, "main:list:row:back", row);
        row->name = UIHelper::CreateStatic(xml, "main:list:row:name", row);
        row->name->SetEllipsis(true);
        row->sec = UIHelper::CreateStatic(xml, "main:list:row:sec", row);
        row->sec->SetEllipsis(true);
        row->badge = UIHelper::CreateStatic(xml, "main:list:row:badge", row);
        row->note = UIHelper::CreateStatic(xml, "main:list:row:note", row);
        row->SetAutoDelete(true);
        m_list->AttachChild(row);
        m_list->rows.push_back(row);
    }
    m_listStatus = UIHelper::CreateStatic(xml, "main:list_status", this);

    // the card
    m_card = dbg_group(xml, "main:card", this);
    dbg_shape(xml, "main:card:back", m_card);
    m_cardHint = UIHelper::CreateStatic(xml, "main:card:hint", m_card);
    m_icon = UIHelper::CreateStatic(xml, "main:card:icon", m_card);
    m_iconBox[0] = m_icon->GetWndPos().x;
    m_iconBox[1] = m_icon->GetWndPos().y;
    m_iconBox[2] = m_icon->GetWidth();
    m_iconBox[3] = m_icon->GetHeight();
    m_title = UIHelper::CreateStatic(xml, "main:card:title", m_card);
    m_sub = UIHelper::CreateStatic(xml, "main:card:sub", m_card);
    m_sub->SetEllipsis(true);
    m_rule = dbg_shape(xml, "main:card:rule", m_card);
    m_body = xr_new<CUIShapeScrollView>();
    CUIXmlInit::InitScrollView(xml, "main:card:body", 0, m_body);
    m_body->InitLooks(xml, "main:card:body", 0);
    m_body->SetAutoDelete(true);
    m_card->AttachChild(m_body);
    m_status = UIHelper::CreateStatic(xml, "main:card:status", m_card);
    m_choice[0] = dbg_button(xml, "main:card:choice_a", m_card);
    m_choice[1] = dbg_button(xml, "main:card:choice_b", m_card);
    m_countLabel = UIHelper::CreateStatic(xml, "main:card:count_label", m_card);
    m_minus = dbg_button(xml, "main:card:count_minus", m_card);
    m_countEdit = UIHelper::CreateEditBox(xml, "main:card:count", m_card);
    m_plus = dbg_button(xml, "main:card:count_plus", m_card);
    m_buttons[1] = dbg_button(xml, "main:card:second", m_card);
    m_buttons[2] = dbg_button(xml, "main:card:third", m_card);
    m_buttons[0] = dbg_button(xml, "main:card:primary", m_card);

    const std::pair<CUIShapeButton*, void (CUIDebugMenuWnd::*)(CUIWindow*, void*)> handlers[] = {
        {m_close, &CUIDebugMenuWnd::OnClose}, {m_choice[0], &CUIDebugMenuWnd::OnChoiceA},
        {m_choice[1], &CUIDebugMenuWnd::OnChoiceB}, {m_minus, &CUIDebugMenuWnd::OnMinus}, {m_plus, &CUIDebugMenuWnd::OnPlus},
        {m_buttons[0], &CUIDebugMenuWnd::OnPrimary}, {m_buttons[1], &CUIDebugMenuWnd::OnSecond},
        {m_buttons[2], &CUIDebugMenuWnd::OnThird}};
    for (const auto& [button, handler] : handlers)
    {
        Register(button);
        AddCallback(button, BUTTON_CLICKED, CUIWndCallback::void_function(this, handler));
    }

    BuildTabs();
    if (m_tabs.empty())
        return false;
    SetTab(0);
    return true;
}

// The tabs the script offers: { { id = "items", caption = "st_dbg_tab_items" }, ... }.
void CUIDebugMenuWnd::BuildTabs()
{
    luabind::functor<object> tabs;
    string128 name{};
    xr_sprintf(name, "%s.tabs", kScript);
    if (!GEnv.ScriptEngine->functor(name, tabs))
    {
        Msg("! [debug menu] %s is missing", name);
        return;
    }
    object result;
    try
    {
        result = tabs();
    }
    catch (...)
    {
        Msg("! [debug menu] %s failed", name);
        return;
    }
    for (int i = 1; lua_is(result, LUA_TTABLE); ++i)
    {
        const object t = result[i];
        if (!lua_is(t, LUA_TTABLE))
            break;
        Tab& tab = m_tabs.emplace_back();
        tab.id = lua_str(t, "id");
        tab.caption = dbg_text(lua_str(t, "caption"));
    }

    CUIXml& xml = *m_xml;
    const float step = xml.ReadAttribFlt("main:tabs", 0, "step", 90.f);
    for (size_t i = 0; i != m_tabs.size(); ++i)
    {
        CUIShapeButton* button = dbg_button(xml, "main:tabs:tab", m_tabArea);
        button->SetWndPos(Fvector2().set(step * float(i), button->GetWndPos().y));
        button->SetText(m_tabs[i].caption.c_str());
        button->SetMessageTarget(this);
        m_tabButtons.push_back(button);
    }
}

// The tab's categories and entries from the script: { all = true, categories = { "st_..", .. }, entries = { { key,
// name, sec, badge, note, cat }, .. } }.
void CUIDebugMenuWnd::BuildTab(Tab& tab)
{
    tab.built = true;
    tab.categories.clear();
    tab.entries.clear();
    luabind::functor<object> build;
    string128 name{};
    xr_sprintf(name, "%s.build", kScript);
    if (!GEnv.ScriptEngine->functor(name, build))
        return;
    object result;
    try
    {
        result = build(tab.id.c_str());
    }
    catch (...)
    {
        Msg("! [debug menu] %s(%s) failed", name, tab.id.c_str());
        return;
    }
    if (!lua_is(result, LUA_TTABLE))
        return;
    const object all = result["all"];
    tab.all = !lua_is(all, LUA_TBOOLEAN) || luabind::object_cast<bool>(all);
    const object cats = result["categories"];
    for (int i = 1; lua_is(cats, LUA_TTABLE); ++i)
    {
        const object c = cats[i];
        if (!lua_is(c, LUA_TSTRING))
            break;
        tab.categories.push_back(dbg_text(luabind::object_cast<pcstr>(c)));
    }
    const object entries = result["entries"];
    if (lua_is(entries, LUA_TTABLE))
    {
        for (int i = 1;; ++i)
        {
            const object e = entries[i];
            if (!lua_is(e, LUA_TTABLE))
                break;
            Entry& entry = tab.entries.emplace_back();
            entry.key = lua_str(e, "key");
            entry.name = dbg_text(lua_str(e, "name"));
            entry.sec = lua_str(e, "sec");
            entry.badge = dbg_text(lua_str(e, "badge"));
            entry.note = dbg_text(lua_str(e, "note"));
            entry.category = u16(std::clamp(lua_int(e, "cat", 0), 0, int(tab.categories.size())));
            entry.foldName = ModSearch::Fold(entry.name);
            entry.lowSec = lower_ascii(entry.sec);
            if (entry.key.empty())
                entry.key = entry.sec;
        }
    }
    if (!tab.all && tab.category == 0 && !tab.categories.empty())
        tab.category = 1;
    tab.category = std::min(tab.category, tab.categories.size());
    // the entry picked before a rebuild stays picked
    tab.selected = size_t(-1);
    for (size_t i = 0; i != tab.entries.size(); ++i)
        if (tab.entries[i].key == tab.selectedKey)
            tab.selected = i;
}

void CUIDebugMenuWnd::SetTab(size_t index)
{
    if (index >= m_tabs.size())
        return;
    // the query and the count belong to their tab
    if (Tab* old = Current())
    {
        old->query = m_search->GetText();
        old->count = atoi(m_countEdit->GetText());
    }
    m_tab = index;
    Tab& tab = m_tabs[index];
    if (!tab.built)
        BuildTab(tab);
    for (size_t i = 0; i != m_tabButtons.size(); ++i)
        m_tabButtons[i]->checked = i == index;
    m_search->SetText(tab.query.c_str());
    LayoutCategories();
    Refilter(true);
    tab.carded = false;
    Describe();
}

// A button per category, "All" first where the tab has it.
void CUIDebugMenuWnd::LayoutCategories()
{
    m_cats->Clear();
    m_catButtons.clear();
    Tab& tab = *Current();
    CUIXml& xml = *m_xml;
    const auto add = [&](const xr_string& caption) {
        auto* button = xr_new<CUIShapeButton>();
        CUIXmlInit::InitButton(xml, "main:cats:cat", 0, button);
        button->InitLooks(xml, "main:cats:cat", 0);
        button->SetText(caption.c_str());
        button->SetMessageTarget(this);
        m_cats->AddWindow(button, true);
        m_catButtons.push_back(button);
    };
    if (tab.all)
        add(StringTable().translate("st_dbg_all").c_str());
    for (const xr_string& caption : tab.categories)
        add(caption);
    for (size_t i = 0; i != m_catButtons.size(); ++i)
        m_catButtons[i]->checked = (tab.all ? i : i + 1) == tab.category;
    m_cats->ScrollToBegin();
}

void CUIDebugMenuWnd::SetCategory(size_t index)
{
    Tab& tab = *Current();
    tab.category = index;
    for (size_t i = 0; i != m_catButtons.size(); ++i)
        m_catButtons[i]->checked = (tab.all ? i : i + 1) == tab.category;
    Refilter(false);
}

// The entries of the category that match the query, best first; the order of the script without a query.
void CUIDebugMenuWnd::Refilter(bool keepTop)
{
    Tab& tab = *Current();
    tab.query = m_search->GetText();
    tab.shown.clear();
    const xr_string folded = ModSearch::Fold(tab.query);
    xr_vector<xr_string> words;
    for (const std::string_view w : ModSearch::Words(folded))
        words.emplace_back(w);
    // the internal names keep their underscores: "wpn_ak" is one word of them
    xr_vector<xr_string> raw;
    {
        const xr_string low = lower_ascii(tab.query);
        size_t start = low.find_first_not_of(" \t");
        while (start != xr_string::npos)
        {
            const size_t end = low.find_first_of(" \t", start);
            raw.emplace_back(low.substr(start, end == xr_string::npos ? xr_string::npos : end - start));
            start = end == xr_string::npos ? end : low.find_first_not_of(" \t", end);
        }
    }
    const xr_string whole = lower_ascii(tab.query);
    xr_vector<std::pair<float, u32>> scored;
    for (u32 i = 0; i != tab.entries.size(); ++i)
    {
        const Entry& e = tab.entries[i];
        if (tab.category != 0 && e.category != tab.category)
            continue;
        if (raw.empty())
        {
            tab.shown.push_back(i);
            continue;
        }
        // the folded words for the name, the raw ones for the internal name: either set may carry the match
        float s = entry_score(e.foldName, e.lowSec, raw, whole);
        if (s <= 0.f && !words.empty())
            s = entry_score(e.foldName, e.lowSec, words, whole);
        if (s > 0.f)
            scored.emplace_back(s, i);
    }
    if (!raw.empty())
    {
        // a tie goes to the shorter internal name: the item itself before its scene's or its upgrade's copy
        std::stable_sort(scored.begin(), scored.end(), [&tab](const auto& a, const auto& b) {
            if (a.first != b.first)
                return a.first > b.first;
            return tab.entries[a.second].lowSec.size() < tab.entries[b.second].lowSec.size();
        });
        for (const auto& [score, index] : scored)
            tab.shown.push_back(index);
    }
    m_list->total = tab.shown.size();
    m_list->SetTop(keepTop ? tab.top : 0);
    tab.top = m_list->top;
    m_count->SetText(dbg_format("st_dbg_count_fmt", u32(tab.shown.size()), u32(tab.entries.size())).c_str());
    m_listStatus->SetText(tab.entries.empty() ? StringTable().translate("st_dbg_empty").c_str() :
            tab.shown.empty()                 ? StringTable().translate("st_dbg_nothing_found").c_str() :
                                                "");
    RefreshRows();
}

void CUIDebugMenuWnd::RefreshRows()
{
    Tab& tab = *Current();
    for (size_t slot = 0; slot != m_list->rows.size(); ++slot)
    {
        CUIDebugRow* row = m_list->rows[slot];
        const size_t shown = m_list->top + slot;
        if (shown >= tab.shown.size())
        {
            row->shown = size_t(-1);
            row->Show(false);
            continue;
        }
        const Entry& e = tab.entries[tab.shown[shown]];
        row->shown = shown;
        row->Show(true);
        row->selected = tab.shown[shown] == tab.selected;
        row->name->SetText(e.name.c_str());
        row->sec->SetText(e.sec.c_str());
        row->badge->SetText(e.badge.c_str());
        row->note->SetText(e.note.c_str());
    }
}

void CUIDebugMenuWnd::OnListScrolled()
{
    if (Tab* tab = Current())
        tab->top = m_list->top;
    RefreshRows();
}

void CUIDebugMenuWnd::OnRowClicked(size_t shown, bool twice)
{
    Tab& tab = *Current();
    if (shown >= tab.shown.size())
        return;
    Select(tab.shown[shown], false);
    if (twice)
        Act(0);
}

void CUIDebugMenuWnd::Select(size_t entry, bool scrollTo)
{
    Tab& tab = *Current();
    if (entry >= tab.entries.size())
        return;
    tab.selected = entry;
    tab.selectedKey = tab.entries[entry].key;
    if (scrollTo)
    {
        const auto it = std::ranges::find(tab.shown, u32(entry));
        if (it != tab.shown.end())
        {
            const size_t at = size_t(it - tab.shown.begin());
            if (at < m_list->top)
                m_list->SetTop(at);
            else if (at >= m_list->top + m_list->Visible())
                m_list->SetTop(at + 1 - m_list->Visible());
            tab.top = m_list->top;
        }
    }
    RefreshRows();
    tab.carded = false;
    Describe();
}

void CUIDebugMenuWnd::MoveSelection(int step)
{
    Tab& tab = *Current();
    if (tab.shown.empty())
        return;
    const auto it = std::ranges::find(tab.shown, u32(tab.selected));
    int at = it == tab.shown.end() ? (step > 0 ? -1 : int(tab.shown.size())) : int(it - tab.shown.begin());
    at = std::clamp(at + step, 0, int(tab.shown.size()) - 1);
    Select(tab.shown[at], true);
}

// The card of the picked entry from the script: { title, sub, body, icon = <item section>, choice = { a, b },
// choice_default = 1, count = { min, max, default }, primary, second, third }.
void CUIDebugMenuWnd::Describe()
{
    Tab& tab = *Current();
    tab.card = {};
    if (tab.selected < tab.entries.size())
    {
        luabind::functor<object> describe;
        string128 name{};
        xr_sprintf(name, "%s.describe", kScript);
        object result;
        if (GEnv.ScriptEngine->functor(name, describe))
        {
            try
            {
                result = describe(tab.id.c_str(), tab.entries[tab.selected].key.c_str());
            }
            catch (...)
            {
                Msg("! [debug menu] %s(%s, %s) failed", name, tab.id.c_str(), tab.entries[tab.selected].key.c_str());
            }
        }
        if (lua_is(result, LUA_TTABLE))
        {
            Card& card = tab.card;
            card.title = dbg_text(lua_str(result, "title"));
            card.sub = lua_str(result, "sub");
            card.body = dbg_text(lua_str(result, "body"));
            card.icon = lua_str(result, "icon");
            const object choice = result["choice"];
            if (lua_is(choice, LUA_TTABLE))
            {
                for (int i = 0; i != 2; ++i)
                {
                    const object c = choice[i + 1];
                    card.choice[i] = lua_is(c, LUA_TSTRING) ? dbg_text(luabind::object_cast<pcstr>(c)) : xr_string();
                }
            }
            card.choiceDefault = std::clamp(lua_int(result, "choice_default", 1) - 1, 0, 1);
            const object count = result["count"];
            if (lua_is(count, LUA_TTABLE))
            {
                card.count = true;
                card.countMin = lua_int(count, "min", 1);
                card.countMax = std::max(lua_int(count, "max", 100), card.countMin);
                card.countDefault = std::clamp(lua_int(count, "default", 1), card.countMin, card.countMax);
                // "reset = 1": a new pick starts from the default (a set's count of the item), not from the last one
                card.countReset = lua_int(count, "reset", 0) != 0;
            }
            card.buttons[0] = dbg_text(lua_str(result, "primary"));
            card.buttons[1] = dbg_text(lua_str(result, "second"));
            card.buttons[2] = dbg_text(lua_str(result, "third"));
        }
        if (tab.card.title.empty())
            tab.card.title = tab.entries[tab.selected].name;
    }
    FillCard();
}

void CUIDebugMenuWnd::FillCard()
{
    Tab& tab = *Current();
    const bool picked = tab.selected < tab.entries.size();
    const Card& card = tab.card;
    m_cardHint->Show(!picked);
    for (CUIWindow* w : std::initializer_list<CUIWindow*>{m_title, m_sub, m_rule, m_body, m_status})
        w->Show(picked);
    m_title->SetText(card.title.c_str());
    m_sub->SetText(card.sub.c_str());
    m_body->Clear();
    if (picked && !card.body.empty())
    {
        CUIStatic* text = xr_new<CUIStatic>("debug menu body");
        CUIXmlInit::InitStatic(*m_xml, "main:card:body_text", 0, text);
        text->SetWidth(m_body->GetDesiredChildWidth());
        text->SetText(card.body.c_str());
        text->AdjustHeightToText();
        m_body->AddWindow(text, true);
    }
    m_body->ScrollToBegin();

    // an item's inventory icon, fitted into its place
    m_icon->Show(false);
    if (picked && !card.icon.empty() && pSettings->section_exist(card.icon.c_str()) &&
        pSettings->line_exist(card.icon.c_str(), "inv_grid_width"))
    {
        const pcstr s = card.icon.c_str();
        const float gx = pSettings->r_float(s, "inv_grid_x"), gy = pSettings->r_float(s, "inv_grid_y");
        const float gw = pSettings->r_float(s, "inv_grid_width"), gh = pSettings->r_float(s, "inv_grid_height");
        if (gw > 0.f && gh > 0.f)
        {
            m_icon->SetShader(InventoryUtilities::GetItemIconsShader(card.icon.c_str()));
            Frect rect;
            rect.lt.set(gx * INV_GRID_WIDTH, gy * INV_GRID_HEIGHT);
            rect.rb.set(gw * INV_GRID_WIDTH, gh * INV_GRID_HEIGHT);
            rect.rb.add(rect.lt);
            m_icon->GetUIStaticItem().SetTextureRect(rect);
            m_icon->TextureOn();
            m_icon->SetStretchTexture(true);
            // the cells keep their proportion (x squeezed on a wide screen)
            const float kx = UI().get_current_kx();
            float w = gw * INV_GRID_WIDTH * kx, h = gh * INV_GRID_HEIGHT;
            const float fit = std::min({m_iconBox[2] / w, m_iconBox[3] / h, 1.f});
            w *= fit;
            h *= fit;
            m_icon->SetWndPos(Fvector2().set(m_iconBox[0] + (m_iconBox[2] - w) * 0.5f, m_iconBox[1] + (m_iconBox[3] - h) * 0.5f));
            m_icon->SetWndSize(Fvector2().set(w, h));
            m_icon->GetUIStaticItem().SetSize(Fvector2().set(w, h));
            m_icon->Show(true);
        }
    }

    for (int i = 0; i != 2; ++i)
    {
        m_choice[i]->Show(picked && !card.choice[0].empty());
        m_choice[i]->SetText(card.choice[i].c_str());
    }
    if (!tab.carded)
    {
        tab.choice = card.choiceDefault;
        if (card.count)
            tab.count = card.countReset ? card.countDefault : std::clamp(tab.count, card.countMin, card.countMax);
        tab.carded = true;
    }
    SetChoice(tab.choice);
    for (CUIWindow* w : std::initializer_list<CUIWindow*>{m_countLabel, m_minus, m_countEdit, m_plus})
        w->Show(picked && card.count);
    if (card.count)
        SetCount(tab.count);
    for (int i = 0; i != 3; ++i)
    {
        m_buttons[i]->Show(picked && !card.buttons[i].empty());
        m_buttons[i]->SetText(card.buttons[i].c_str());
    }
    m_status->SetText(Device.dwTimeGlobal < tab.statusUntil ? tab.status.c_str() : "");
}

void CUIDebugMenuWnd::SetChoice(int choice)
{
    Tab& tab = *Current();
    tab.choice = std::clamp(choice, 0, 1);
    m_choice[0]->checked = tab.choice == 0;
    m_choice[1]->checked = tab.choice == 1;
}

void CUIDebugMenuWnd::SetCount(int count)
{
    Tab& tab = *Current();
    tab.count = std::clamp(count, tab.card.countMin, tab.card.countMax);
    string32 text{};
    xr_sprintf(text, "%d", tab.count);
    m_countText = text;
    m_countEdit->SetText(text);
}

void CUIDebugMenuWnd::SetStatus(const xr_string& text)
{
    Tab& tab = *Current();
    tab.status = text;
    tab.statusUntil = Device.dwTimeGlobal + 4000;
    m_status->SetText(text.c_str());
}

// The script does what the button says: act(tab, key, button, count, choice) -> { msg = "...", rebuild = true }
// (or the message alone). A rebuild reads the tab again (what was near, what the player now holds).
void CUIDebugMenuWnd::Act(size_t button)
{
    Tab& tab = *Current();
    if (tab.selected >= tab.entries.size() || button >= 3 || tab.card.buttons[button].empty())
        return;
    if (tab.card.count)
        SetCount(atoi(m_countEdit->GetText()));
    luabind::functor<object> act;
    string128 name{};
    xr_sprintf(name, "%s.act", kScript);
    if (!GEnv.ScriptEngine->functor(name, act))
        return;
    static constexpr pcstr kButtons[] = {"primary", "second", "third"};
    object result;
    try
    {
        result = act(tab.id.c_str(), tab.entries[tab.selected].key.c_str(), kButtons[button], tab.count, tab.choice + 1);
    }
    catch (...)
    {
        Msg("! [debug menu] %s(%s, %s, %s) failed", name, tab.id.c_str(), tab.entries[tab.selected].key.c_str(), kButtons[button]);
        SetStatus(StringTable().translate("st_dbg_failed").c_str());
        return;
    }
    bool rebuild = false;
    if (lua_is(result, LUA_TSTRING))
        SetStatus(dbg_text(luabind::object_cast<pcstr>(result)));
    else if (lua_is(result, LUA_TTABLE))
    {
        SetStatus(dbg_text(lua_str(result, "msg")));
        const object r = result["rebuild"];
        rebuild = lua_is(r, LUA_TBOOLEAN) && luabind::object_cast<bool>(r);
        const object c = result["close"];
        if (lua_is(c, LUA_TBOOLEAN) && luabind::object_cast<bool>(c))
        {
            HideDialog();
            return;
        }
    }
    if (rebuild)
    {
        const size_t top = tab.top;
        BuildTab(tab);
        LayoutCategories();
        tab.top = top;
        Refilter(true);
    }
    // the card may say something new (a state that changed)
    const int count = tab.count, choice = tab.choice;
    Describe();
    tab.count = count;
    SetCount(count);
    SetChoice(choice);
}

void CUIDebugMenuWnd::Show(bool status)
{
    inherited::Show(status);
    if (!status)
        return;
    // what is near and what the player holds change while the window is closed
    for (Tab& tab : m_tabs)
        tab.built = false;
    if (Tab* tab = Current())
    {
        BuildTab(*tab);
        LayoutCategories();
        Refilter(true);
        tab->carded = false;
        Describe();
    }
    const int dik = GetActionDik(kDEBUG_MENU, 0);
    // the key's own name ("F7"), not its id ("kF7")
    const keyboard_key* key = dik ? DikToPtr(dik, true) : nullptr;
    const xr_string keyName = !key ? xr_string() : !key->key_local_name.empty() ? key->key_local_name :
        xr_string(key->key_name[0] == 'k' ? key->key_name + 1 : key->key_name);
    m_hotkeyHint->SetText(keyName.empty() ? "" : dbg_format("st_dbg_hotkey_fmt", keyName.c_str()).c_str());
    m_search->CaptureFocus(true);
}

void CUIDebugMenuWnd::Update()
{
    inherited::Update();

    xr_vector<xr_string>& queue = dbg_queue();
    while (!queue.empty())
    {
        const xr_string command = queue.front();
        queue.erase(queue.begin());
        Command(command.c_str());
    }

    Tab* tab = Current();
    if (!tab)
        return;
    if (0 != xr_strcmp(m_search->GetText(), tab->query.c_str()))
        Refilter(false);
    m_searchHint->Show(!m_search->GetText()[0]);
    if (tab->card.count && m_countText != m_countEdit->GetText() && !m_countEdit->IsInputFocused())
        SetCount(atoi(m_countEdit->GetText()));
    if (!tab->status.empty() && Device.dwTimeGlobal >= tab->statusUntil)
    {
        tab->status.clear();
        m_status->SetText("");
    }
}

void CUIDebugMenuWnd::SendMessage(CUIWindow* window, s16 message, void* data)
{
    if (message == BUTTON_CLICKED)
    {
        for (size_t i = 0; i != m_tabButtons.size(); ++i)
            if (window == m_tabButtons[i])
                SetTab(i);
        for (size_t i = 0; i != m_catButtons.size(); ++i)
            if (window == m_catButtons[i])
                SetCategory(Current()->all ? i : i + 1);
    }
    CUIWndCallback::OnEvent(window, message, data);
    inherited::SendMessage(window, message, data);
}

bool CUIDebugMenuWnd::OnKeyboardAction(int dik, EUIMessages keyboardAction)
{
    if (keyboardAction == WINDOW_KEY_PRESSED)
    {
        if (IsBinded(kQUIT, dik) || IsBinded(kDEBUG_MENU, dik))
        {
            HideDialog();
            return true;
        }
        switch (dik)
        {
        case SDL_SCANCODE_UP: MoveSelection(-1); return true;
        case SDL_SCANCODE_DOWN: MoveSelection(1); return true;
        case SDL_SCANCODE_PAGEUP: MoveSelection(-int(m_list->Visible())); return true;
        case SDL_SCANCODE_PAGEDOWN: MoveSelection(int(m_list->Visible())); return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: Act(0); return true;
        default: break;
        }
        // Ctrl+Tab / Ctrl+Shift+Tab walk the tabs
        if (dik == SDL_SCANCODE_TAB && pInput->iGetAsyncKeyState(SDL_SCANCODE_LCTRL))
        {
            const bool back = pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT);
            SetTab((m_tab + (back ? m_tabs.size() - 1 : 1)) % m_tabs.size());
            return true;
        }
    }
    return inherited::OnKeyboardAction(dik, keyboardAction);
}

void CUIDebugMenuWnd::OnTab(CUIWindow*, void*) {}
void CUIDebugMenuWnd::OnCategory(CUIWindow*, void*) {}
void CUIDebugMenuWnd::OnClose(CUIWindow*, void*) { HideDialog(); }
void CUIDebugMenuWnd::OnChoiceA(CUIWindow*, void*) { SetChoice(0); }
void CUIDebugMenuWnd::OnChoiceB(CUIWindow*, void*) { SetChoice(1); }
void CUIDebugMenuWnd::OnMinus(CUIWindow*, void*)
{
    Tab& tab = *Current();
    // a tenth at a time over ten, one below
    SetCount(atoi(m_countEdit->GetText()) - (tab.count > 10 ? tab.count / 10 : 1));
}
void CUIDebugMenuWnd::OnPlus(CUIWindow*, void*)
{
    Tab& tab = *Current();
    SetCount(atoi(m_countEdit->GetText()) + (tab.count >= 10 ? tab.count / 10 : 1));
}
void CUIDebugMenuWnd::OnPrimary(CUIWindow*, void*) { Act(0); }
void CUIDebugMenuWnd::OnSecond(CUIWindow*, void*) { Act(1); }
void CUIDebugMenuWnd::OnThird(CUIWindow*, void*) { Act(2); }

// ---- opening it ----------------------------------------------------------------------------------------------------
void CUIDebugMenuWnd::Toggle()
{
    CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
    if (!ui || !g_pGameLevel || !Actor())
        return;
    if (!ui->DebugMenu)
    {
        ui->DebugMenu = xr_new<CUIDebugMenuWnd>();
        if (!ui->DebugMenu->Init())
        {
            Msg("! [debug menu] ui_debug_menu.xml or %s is missing: the menu stays closed", kScript);
            xr_delete(ui->DebugMenu);
            return;
        }
    }
    if (ui->DebugMenu->IsShown())
        ui->DebugMenu->HideDialog();
    else
        ui->DebugMenu->ShowDialog(false);
}

// QA: "tab <id>", "category <n>" (0 = all), "search <text>", "pick <n>" (the n-th row shown, 0-based) or
// "pick_key <key>", "count <n>", "choice <1|2>", "press <primary|second|third>", "state" (the window's state in the
// log). Queued while the window is opening (dar_debug_menu_do runs them on its next frame).
void CUIDebugMenuWnd::Command(pcstr command)
{
    CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
    CUIDebugMenuWnd* wnd = ui ? ui->DebugMenu : nullptr;
    if (!wnd || !wnd->IsShown())
    {
        dbg_queue().emplace_back(command);
        return;
    }
    string256 verb{}, arg{};
    sscanf(command, "%255s", verb);
    const char* rest = command + xr_strlen(verb);
    while (*rest == ' ')
        ++rest;
    xr_strcpy(arg, rest);
    Tab& tab = *wnd->Current();
    if (!xr_strcmp(verb, "tab"))
    {
        for (size_t i = 0; i != wnd->m_tabs.size(); ++i)
            if (wnd->m_tabs[i].id == arg)
                wnd->SetTab(i);
    }
    else if (!xr_strcmp(verb, "category"))
        wnd->SetCategory(std::min<size_t>(atoi(arg), tab.categories.size()));
    else if (!xr_strcmp(verb, "search"))
    {
        wnd->m_search->SetText(arg);
        wnd->Refilter(false);
    }
    else if (!xr_strcmp(verb, "pick"))
    {
        const size_t n = size_t(atoi(arg));
        if (n < wnd->Current()->shown.size())
            wnd->Select(wnd->Current()->shown[n], true);
    }
    else if (!xr_strcmp(verb, "pick_key"))
    {
        for (size_t i = 0; i != tab.entries.size(); ++i)
            if (tab.entries[i].key == arg)
                wnd->Select(i, true);
    }
    else if (!xr_strcmp(verb, "count"))
        wnd->SetCount(atoi(arg));
    else if (!xr_strcmp(verb, "choice"))
        wnd->SetChoice(atoi(arg) - 1);
    else if (!xr_strcmp(verb, "press"))
        wnd->Act(!xr_strcmp(arg, "second") ? 1 : !xr_strcmp(arg, "third") ? 2 : 0);
    if (!xr_strcmp(verb, "state") || !xr_strcmp(verb, "press") || !xr_strcmp(verb, "search"))
    {
        Tab& t = *wnd->Current();
        const Entry* e = t.selected < t.entries.size() ? &t.entries[t.selected] : nullptr;
        Msg("* [debug menu] tab %s, category %u, query '%s', %u of %u shown, first '%s', picked '%s', count %d, choice %d, status '%s'",
            t.id.c_str(), u32(t.category), t.query.c_str(), u32(t.shown.size()), u32(t.entries.size()),
            t.shown.empty() ? "" : t.entries[t.shown[0]].sec.c_str(), e ? e->sec.c_str() : "", t.count, t.choice + 1,
            t.status.c_str());
    }
}
