// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Static/UIShapeStyle.h"

class CGameFont;

class CUIXml;
class CUIStatic;
class CUIScrollView;
class CUI3tButton;

// The item hotkeys of the actor menu: data, never code. inventory_hotkeys.xml lists them, every
// inventory_hotkeys_<name>.xml a mod ships comes after it in name order, and a hotkey with the id of
// one read before replaces it (remove="1" takes it out). What each one does is an action of the
// engine or a function of a script; the panel that lists them shows what the window it is in allows.
enum class EHotkeyTrigger : u8
{
    None, // a row that only tells: the stock double click, the drag, a bound game action
    Hover, // the cursor comes over an item while the modifiers are held
    LeftClick,
    RightClick,
    MiddleClick,
    WheelUp,
    WheelDown,
    Key, // a key pressed
    Hold, // while the modifiers (and the held key) are down: a view, not a deed
};

struct SInventoryHotkey
{
    enum Modifier : u8
    {
        Alt = 1 << 0,
        Ctrl = 1 << 1,
        Shift = 1 << 2,
    };
    enum Window : u8
    {
        Inventory = 1 << 0,
        Trade = 1 << 1,
        Loot = 1 << 2,
        Upgrade = 1 << 3,
        Every = Inventory | Trade | Loot | Upgrade,
    };

    shared_str id;
    // An item's: move, move_one, split, use, to_slot, drop, drop_stack, pick (for a comparison).
    // The window's: toggle_panel, compare, clear_picks; a Hold trigger's: pristine.
    // script:<module>.<function> is called with the item's game object (nil for a key) and the
    // window's name. Empty for a row that only tells.
    shared_str action;
    shared_str caption; // a string id: what it does
    shared_str window_caption[4]; // what it does in one window (caption_inventory, _trade, _loot, _upgrade)
    xr_vector<shared_str> label; // the keys column when the trigger does not say it: string ids, action:<name>
    EHotkeyTrigger trigger{ EHotkeyTrigger::None };
    int key{}; // the scancode of a Key trigger
    int hold_key{}; // a key held with the modifiers: keys="C" makes C + a click
    u8 modifiers{};
    u8 windows{ Every };
    float order{};

    // the caption for one window (a Window bit), the common one where it has none of its own
    const shared_str& Caption(u8 window) const
    {
        for (u8 w = 0; w < 4; ++w)
        {
            if ((window & (1 << w)) && window_caption[w].size())
                return window_caption[w];
        }
        return caption;
    }
};

namespace inventory_hotkeys
{
const xr_vector<SInventoryHotkey>& all();
// read again on the next use: the UI is being rebuilt
void reset();
// the modifiers held now, as SInventoryHotkey::Modifier bits
u8 held_modifiers();
// The hotkey of `trigger` in a window whose modifiers are held exactly, its held key too: one
// asking for a held key wins over one without. Null when none.
const SInventoryHotkey* find(EHotkeyTrigger trigger, u8 window, int key = 0);
// a Hold hotkey of this action is held down now
bool holding(pcstr action, u8 window);
// a key by its bind name (kC, mouse3) or its letter (C)
int key_of(pcstr name);
// the console holds modifiers and a key down for the QA scenarios (dar_ui_hold)
void set_fake_held(u8 modifiers, int key);
} // namespace inventory_hotkeys

// The panel listing the hotkeys of the window it is in: over the menu, scrolled when they do not fit.
// Its place, size and looks are the hotkeys_panel node of the actor_menu layout, or of
// actor_menu_hotkeys.xml when the layout has none.
class CUIInventoryHotkeysPanel final : public CUIWindow
{
    using inherited = CUIWindow;

public:
    CUIInventoryHotkeysPanel() : CUIWindow("hotkeys_panel") {}

    bool Init(CUIXml& actor_menu_xml);
    // the rows of one window (SInventoryHotkey::Window)
    void Fill(u8 window);
    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    CUI3tButton* CloseButton() const { return m_close; }
    bool IsUnderCursor() const;

    pcstr GetDebugType() override { return "CUIInventoryHotkeysPanel"; }

private:
    bool InitFrom(CUIXml& xml);
    CUIStatic* MakeChip(CUIWindow* row, pcstr text, float x, float height);

    CUIScrollView* m_list{};
    CUI3tButton* m_close{};
    // the looks the rows are built from
    std::unique_ptr<SUIShapeSkin> m_chip_skin;
    std::unique_ptr<SUIShapeSkin> m_rule_skin;
    CGameFont* m_chip_font{};
    CGameFont* m_plus_font{};
    CGameFont* m_caption_font{};
    u32 m_chip_color{ 0xFFFFFFFF };
    u32 m_plus_color{ 0xFFFFFFFF };
    u32 m_caption_color{ 0xFFFFFFFF };
    float m_row_height{ 20.0f };
    float m_keys_width{ 120.0f };
    float m_chip_gap{ 3.0f };
    float m_chip_padding{ 5.0f };
    u8 m_window{};
};
