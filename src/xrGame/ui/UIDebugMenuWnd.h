// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "UIDialogWnd.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUIDebugList;
class CUIEditBox;
class CUIShape;
class CUIShapeButton;
class CUIShapeScrollView;
class CUIStatic;
class CUIXml;

// The debug menu (docs/dead-air/DEBUG_MENU.md): a window over the running game, in the look of the main menu's
// own windows, with a tab per kind of thing - items, characters, teleport, the player, the world, the story, what
// is near. Every tab has the same three panes: its categories, the list of its entries with a search over their
// game and internal names, and the card of the picked entry with its count, a two-way choice and up to three
// buttons. What a tab lists and what its buttons do is the script's (dead_air_x64_debug_menu.script: tabs, build,
// describe, act), so a mod adds a tab or a button there. Off by default (dar_debug_menu, Options -> Game); the key
// is the action debug_menu (Options -> Controls, F7 when it was free).
class CUIDebugMenuWnd final : public CUIDialogWnd, public CUIWndCallback
{
    using inherited = CUIDialogWnd;

public:
    CUIDebugMenuWnd();
    ~CUIDebugMenuWnd() override;

    bool Init();
    void Show(bool status) override;
    void Update() override;
    void SendMessage(CUIWindow* window, s16 message, void* data) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboardAction) override;
    pcstr GetDebugType() override { return "CUIDebugMenuWnd"; }

    // The key, the console (dar_debug_menu_open) and QA: shows the window over the game or hides it.
    static void Toggle();
    // QA: the tab, the category, the query, the entry and the buttons by name, on the open window (dar_debug_menu_do).
    static void Command(pcstr command);
    static bool Enabled();

    // the list's rows report their clicks here (CUIDebugList)
    void OnRowClicked(size_t shown, bool twice);
    void OnListScrolled();

private:
    struct Entry
    {
        xr_string key;              // the script's own id of the entry, given back to describe and act
        xr_string name, sec;        // UI code page: the shown name, the internal one
        xr_string badge, note;      // the row's right column: a state (accent) and a small note
        xr_string foldName, lowSec; // for the search
        u16 category{};             // 1-based into the tab's categories; 0 only under "All"
    };
    struct Card
    {
        xr_string title, sub, body, icon;
        xr_string choice[2];
        int choiceDefault{};
        bool count{};
        int countMin{1}, countMax{1}, countDefault{1};
        bool countReset{};
        xr_string buttons[3]; // primary, second, third
    };
    struct Tab
    {
        xr_string id, caption;
        bool built{};
        bool all{true};
        xr_vector<xr_string> categories;
        xr_vector<Entry> entries;
        size_t category{};          // 0 = all (when the tab has it)
        xr_string query;            // UI code page
        xr_vector<u32> shown;       // entries in the list, best first
        size_t top{};               // the first shown row
        xr_string selectedKey;
        size_t selected{size_t(-1)}; // into entries
        Card card;
        bool carded{};
        int count{1};
        int choice{};
        xr_string status;
        u32 statusUntil{};
    };

    void BuildTabs();
    void BuildTab(Tab& tab);
    void SetTab(size_t index);
    void LayoutCategories();
    void SetCategory(size_t index);
    void Refilter(bool keepTop);
    void RefreshRows();
    void Select(size_t entry, bool scrollTo);
    void MoveSelection(int step);
    void Describe();
    void FillCard();
    void Act(size_t button);
    void SetCount(int count);
    void SetChoice(int choice);
    void SetStatus(const xr_string& text);

    void OnTab(CUIWindow*, void*);
    void OnCategory(CUIWindow*, void*);
    void OnClose(CUIWindow*, void*);
    void OnChoiceA(CUIWindow*, void*);
    void OnChoiceB(CUIWindow*, void*);
    void OnMinus(CUIWindow*, void*);
    void OnPlus(CUIWindow*, void*);
    void OnPrimary(CUIWindow*, void*);
    void OnSecond(CUIWindow*, void*);
    void OnThird(CUIWindow*, void*);

    Tab* Current() { return m_tab < m_tabs.size() ? &m_tabs[m_tab] : nullptr; }

    CUIXml* m_xml{};
    CUIStatic* m_hotkeyHint{};
    CUIShapeButton* m_close{};
    CUIWindow* m_tabArea{};
    xr_vector<CUIShapeButton*> m_tabButtons;
    CUIShapeScrollView* m_cats{};
    xr_vector<CUIShapeButton*> m_catButtons;
    CUIEditBox* m_search{};
    CUIStatic* m_searchHint{};
    CUIStatic* m_count{};
    CUIDebugList* m_list{};
    CUIStatic* m_listStatus{};
    // the card
    CUIWindow* m_card{};
    CUIStatic* m_cardHint{};
    CUIStatic* m_icon{};
    CUIStatic* m_title{};
    CUIStatic* m_sub{};
    CUIShape* m_rule{};
    CUIShapeScrollView* m_body{};
    CUIStatic* m_status{};
    CUIShapeButton* m_choice[2]{};
    CUIStatic* m_countLabel{};
    CUIShapeButton* m_minus{};
    CUIShapeButton* m_plus{};
    CUIEditBox* m_countEdit{};
    CUIShapeButton* m_buttons[3]{};
    float m_iconBox[4]{}; // x, y, w, h of the icon's place

    u32 m_textColor{}, m_mutedColor{}, m_dimColor{}, m_accentColor{}, m_goodColor{}, m_badColor{};
    xr_vector<Tab> m_tabs;
    size_t m_tab{};
    xr_string m_countText;
};
