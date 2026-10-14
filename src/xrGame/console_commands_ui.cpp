// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "StdAfx.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "xrEngine/xr_object.h"
#include "xrScriptEngine/script_engine.hpp"
#include "Level.h"
#include "Actor.h"
#include "InventoryBox.h"
#include "UIGameSP.h"
#include "ui/UIActorMenu.h"
#include "ui/UIActorMenuHotkeys.h"
#include "ui/UIDialogWnd.h"
#include "xrUICore/Cursor/UICursor.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/stalker/ai_stalker.h"

namespace
{
enum class Wanted : u8
{
    Trader,
    Body,
    Stash,
    Mutant,
};

bool fits(IGameObject* object, Wanted wanted)
{
    switch (wanted)
    {
    case Wanted::Trader:
    {
        auto* stalker = smart_cast<CAI_Stalker*>(object);
        return stalker && stalker->g_Alive() && stalker->IsTradeEnabled();
    }
    case Wanted::Body:
    {
        auto* stalker = smart_cast<CAI_Stalker*>(object);
        return stalker && !stalker->g_Alive();
    }
    case Wanted::Stash: return smart_cast<CInventoryBox*>(object) != nullptr;
    case Wanted::Mutant:
    {
        auto* monster = smart_cast<CBaseMonster*>(object);
        return monster && !monster->g_Alive();
    }
    }
    return false;
}

// the nearest object of the level that fits, however far away it is
IGameObject* nearest(Wanted wanted)
{
    const Fvector& from = Actor()->Position();
    IGameObject* best{};
    float bestDistance = flt_max;
    for (u32 i = 0, count = Level().Objects.o_count(); i < count; ++i)
    {
        IGameObject* object = Level().Objects.o_get_by_iterator(i);
        if (!object || object->getDestroy() || !fits(object, wanted))
            continue;
        const float distance = from.distance_to_sqr(object->Position());
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = object;
        }
    }
    return best;
}

// dar_ui_open inventory|trade|upgrade|loot|stash|mutant: one of the actor's windows on the nearest fitting
// object of the level, wherever it stands - the trader does not have to be within reach. A shortcut
// for working on the layouts of these windows.
class CCC_DarUiOpen final : public IConsole_Command
{
public:
    CCC_DarUiOpen(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        auto* gameUi = smart_cast<CUIGameSP*>(CurrentGameUI());
        CActor* actor = Actor();
        if (!g_pGameLevel || !gameUi || !actor || !actor->g_Alive())
        {
            Msg("! dar_ui_open: no live actor on a level");
            return;
        }

        string64 what{};
        sscanf(args, "%63s", what);
        if (!what[0] || 0 == xr_stricmp(what, "inventory"))
        {
            if (!gameUi->GetActorMenu().IsShown())
                gameUi->ShowActorMenu();
            return;
        }

        const auto open = [&](Wanted wanted, pcstr kind) -> IGameObject*
        {
            IGameObject* object = nearest(wanted);
            if (!object)
                Msg("! dar_ui_open %s: nothing of the kind on this level", kind);
            else
                Msg("* dar_ui_open %s: [%s] %.1f m away", kind, object->cName().c_str(),
                    actor->Position().distance_to(object->Position()));
            return object;
        };

        if (0 == xr_stricmp(what, "trade"))
        {
            if (IGameObject* object = open(Wanted::Trader, what))
            {
                gameUi->GetActorMenu().SetDistanceExempt(true);
                gameUi->StartTrade(actor, smart_cast<CInventoryOwner*>(object));
            }
        }
        else if (0 == xr_stricmp(what, "upgrade"))
        {
            if (IGameObject* object = open(Wanted::Trader, what))
            {
                gameUi->GetActorMenu().SetDistanceExempt(true);
                gameUi->StartUpgrade(actor, smart_cast<CInventoryOwner*>(object));
            }
        }
        else if (0 == xr_stricmp(what, "loot"))
        {
            if (IGameObject* object = open(Wanted::Body, what))
            {
                gameUi->GetActorMenu().SetDistanceExempt(true);
                gameUi->StartCarBody(actor, smart_cast<CInventoryOwner*>(object));
            }
        }
        else if (0 == xr_stricmp(what, "stash"))
        {
            if (IGameObject* object = open(Wanted::Stash, what))
            {
                gameUi->GetActorMenu().SetDistanceExempt(true);
                gameUi->StartCarBody(actor, smart_cast<CInventoryBox*>(object));
            }
        }
        else if (0 == xr_stricmp(what, "mutant"))
        {
            IGameObject* object = open(Wanted::Mutant, what);
            luabind::functor<void> openLoot;
            if (object && GEnv.ScriptEngine->functor("ui_mutant_loot.open_by_id", openLoot))
                openLoot(object->ID());
        }
        else
            Msg("! dar_ui_open: unknown window [%s]", what);
    }

    void GetStatus(TStatus& status) override { xr_strcpy(status, "inventory|trade|upgrade|loot|stash|mutant"); }
    void Save(IWriter*) override {}

    void fill_tips(vecTips& tips, u32) override
    {
        for (pcstr window : {"inventory", "trade", "upgrade", "loot", "stash", "mutant"})
            tips.emplace_back(window);
    }
};
// dar_ui_cursor <x> <y>: the cursor to a point of the 1024 x 768 layout space, as if the mouse had
// moved there; what lies under it shows its hint.
class CCC_DarUiCursor final : public IConsole_Command
{
public:
    CCC_DarUiCursor(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        Fvector2 at{};
        if (sscanf(args, "%f %f", &at.x, &at.y) != 2)
        {
            Msg("! dar_ui_cursor <x> <y>, in the 1024 x 768 layout space");
            return;
        }
        GetUICursor().SetUICursorPosition(at);
        if (CDialogHolder* holder = CurrentGameUI())
        {
            if (CUIDialogWnd* top = holder->TopInputReceiver())
                top->OnMouseAction(at.x, at.y, WINDOW_MOUSE_MOVE);
        }
    }

    void Save(IWriter*) override {}
};

// dar_ui_click left|right [<x> <y>]: a click of the top window where the cursor is (or at the point
// given, which the cursor moves to first).
class CCC_DarUiClick final : public IConsole_Command
{
public:
    CCC_DarUiClick(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        string32 button{};
        Fvector2 at = GetUICursor().GetCursorPosition();
        const int read = sscanf(args, "%31s %f %f", button, &at.x, &at.y);
        const bool right = 0 == xr_stricmp(button, "right");
        const bool middle = 0 == xr_stricmp(button, "middle");
        if (read < 1 || (!right && !middle && 0 != xr_stricmp(button, "left")))
        {
            Msg("! dar_ui_click left|right|middle [<x> <y>]");
            return;
        }
        CDialogHolder* holder = CurrentGameUI();
        CUIDialogWnd* top = holder ? holder->TopInputReceiver() : nullptr;
        if (!top)
            return;
        GetUICursor().SetUICursorPosition(at);
        top->OnMouseAction(at.x, at.y, WINDOW_MOUSE_MOVE);
        // a button takes the release only when it knows the cursor is over it: an update in between
        top->Update();
        top->OnMouseAction(at.x, at.y, right ? WINDOW_RBUTTON_DOWN : (middle ? WINDOW_CBUTTON_DOWN : WINDOW_LBUTTON_DOWN));
        top->OnMouseAction(at.x, at.y, right ? WINDOW_RBUTTON_UP : (middle ? WINDOW_CBUTTON_UP : WINDOW_LBUTTON_UP));
    }

    void Save(IWriter*) override {}
};

// dar_ui_cursor_item <section>: the cursor onto the first cell of the actor menu holding such an item
class CCC_DarUiCursorItem final : public IConsole_Command
{
public:
    CCC_DarUiCursorItem(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        CUIGameSP* game = smart_cast<CUIGameSP*>(CurrentGameUI());
        Fvector2 at;
        if (!game || !game->GetActorMenu().IsShown() || !game->GetActorMenu().FindCellCenter(args, at))
        {
            Msg("! dar_ui_cursor_item: no [%s] on screen", args);
            return;
        }
        GetUICursor().SetUICursorPosition(at);
        game->GetActorMenu().OnMouseAction(at.x, at.y, WINDOW_MOUSE_MOVE);
    }

    void Save(IWriter*) override {}
};

// dar_ui_hold [alt] [ctrl] [shift] [<key>] | none: the keys the inventory hotkeys read as held
// down until the next call, for a scenario that has no hands.
class CCC_DarUiHold final : public IConsole_Command
{
public:
    CCC_DarUiHold(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        u8 modifiers{};
        int key{};
        string64 part;
        for (int i = 0, n = _GetItemCount(args, ' '); i < n; ++i)
        {
            _GetItem(args, i, part, ' ');
            if (0 == xr_stricmp(part, "alt"))
                modifiers |= SInventoryHotkey::Alt;
            else if (0 == xr_stricmp(part, "ctrl"))
                modifiers |= SInventoryHotkey::Ctrl;
            else if (0 == xr_stricmp(part, "shift"))
                modifiers |= SInventoryHotkey::Shift;
            else if (0 != xr_stricmp(part, "none") && part[0])
                key = inventory_hotkeys::key_of(part);
        }
        inventory_hotkeys::set_fake_held(modifiers, key);
    }

    void Save(IWriter*) override {}
};

// dar_ui_key <key>: a key pressed and let go in the top window
class CCC_DarUiKey final : public IConsole_Command
{
public:
    CCC_DarUiKey(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        const int key = inventory_hotkeys::key_of(args);
        CDialogHolder* holder = CurrentGameUI();
        CUIDialogWnd* top = holder ? holder->TopInputReceiver() : nullptr;
        if (!key || !top)
        {
            Msg("! dar_ui_key <key>: no such key or no window");
            return;
        }
        top->OnKeyboardAction(key, WINDOW_KEY_PRESSED);
        top->OnKeyboardAction(key, WINDOW_KEY_RELEASED);
    }

    void Save(IWriter*) override {}
};
} // namespace

// the order of the actor menu's item grids (UIActorMenuTools.h, EInventorySort)
extern u32 g_inventory_sort;
// the panel of hotkeys is up when the menu opens (UIActorMenuHotkeys.cpp)
extern u32 g_inventory_hotkeys_panel;
const xr_token inventory_sort_tokens[] = { { "default", 0 }, { "type", 1 }, { "name", 2 }, { "weight", 3 },
    { "price", 4 }, { "condition", 5 }, { nullptr, 0 } };

void register_ui_console_commands()
{
    CMD1(CCC_DarUiOpen, "dar_ui_open");
    CMD1(CCC_DarUiCursor, "dar_ui_cursor");
    CMD1(CCC_DarUiClick, "dar_ui_click");
    CMD3(CCC_Token, "ui_inv_sort", &g_inventory_sort, inventory_sort_tokens);
    CMD1(CCC_DarUiHold, "dar_ui_hold");
    CMD1(CCC_DarUiCursorItem, "dar_ui_cursor_item");
    CMD1(CCC_DarUiKey, "dar_ui_key");
    CMD4(CCC_Integer, "ui_inv_hotkeys", (int*)&g_inventory_hotkeys_panel, 0, 1);
}
