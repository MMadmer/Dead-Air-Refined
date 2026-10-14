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
#include "ui/UIUpdateWnd.h"
#include "ui/UIMajorUpdateWnd.h"
#include "ui/UIContentWnd.h"
#include "ui/UIMessageBoxEx.h"
#include "ui/UILoadingScreen.h"
#include "xrEngine/xr_level_controller.h"
#include "xrEngine/da_media.h"
#include "MainMenu.h"
#include "ui/UIDialogWnd.h"
#include "xrUICore/Cursor/UICursor.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/stalker/ai_stalker.h"

namespace
{
// the window on top: the main menu's while it is open (over a level too), else the game's
CUIDialogWnd* top_window()
{
    CDialogHolder* holder = MainMenu() && MainMenu()->IsActive() ? static_cast<CDialogHolder*>(MainMenu()) : CurrentGameUI();
    return holder ? holder->TopInputReceiver() : nullptr;
}

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
        if (CUIDialogWnd* top = top_window())
            top->OnMouseAction(at.x, at.y, WINDOW_MOUSE_MOVE);
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
        CUIDialogWnd* top = top_window();
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

// The loading screen over whatever is on screen, its bar walking from 0 to 100 % every 8 seconds or
// standing at a given per cent, with a tip of the game's: for its layout and its shaders. Esc closes it.
class CUILoadingPreview final : public CUIDialogWnd
{
    UILoadingScreen* m_screen;
    float m_fixed;
    u32 m_start;

public:
    explicit CUILoadingPreview(float fixed) : CUIDialogWnd("CUILoadingPreview"), m_fixed(fixed), m_start(da_media_time())
    {
        SetWndRect(Frect().set(0.f, 0.f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
        m_screen = xr_new<UILoadingScreen>();
        m_screen->SetAutoDelete(true);
        AttachChild(m_screen);
        m_screen->Show(true);
        // the tip whole or none, as IGame_Persistent::LoadTitle shows it
        shared_str header, number, tip;
        if (StringTable().translate("ls_header", header) && StringTable().translate("ls_tip_number", number) &&
            StringTable().translate("ls_tip_7", tip))
        {
            string256 caption;
            xr_sprintf(caption, "%s%d:", number.c_str(), 7);
            m_screen->SetStageTip(header.c_str(), caption, tip.c_str());
        }
        m_screen->SetStageTitle(StringTable().translate("st_loading_details").c_str());
    }

    void Update() override
    {
        CUIDialogWnd::Update();
        const float progress = m_fixed >= 0.f ? m_fixed : float((da_media_time() - m_start) % 8000) / 8000.f;
        m_screen->Update(iFloor(progress * 1000.f), 1000);
    }

    bool OnKeyboardAction(int dik, EUIMessages action) override
    {
        if (action == WINDOW_KEY_PRESSED && IsBinded(kQUIT, dik))
        {
            HideDialog();
            return true;
        }
        return CUIDialogWnd::OnKeyboardAction(dik, action);
    }
};

// dar_ui_dialog update [available|downloading|ready|failed] | update_major | bug_report | content [repairing]
// | crash_prompt | loading [<per cent>]: one of the game's dialogs over whatever is on screen, the update
// windows over a made-up release - for their layouts.
class CCC_DarUiDialog final : public IConsole_Command
{
public:
    CCC_DarUiDialog(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        string64 what{}, state{};
        sscanf(args, "%63s %63s", what, state);

        UpdateService::Snapshot snapshot;
        snapshot.version = "1.6.1";
        snapshot.message = "";
        snapshot.themeEn = snapshot.themeRu = snapshot.majorThemeEn = snapshot.majorThemeRu = "%c[ui_clr_1]Seasons";
        xr_string changes;
        for (int i = 1; i <= 24; ++i)
        {
            string256 line;
            xr_sprintf(line, "- Change %d of the release, a line long enough to wrap in the list of the window.\\n", i);
            changes += line;
        }
        snapshot.changesEn = snapshot.changesRu = snapshot.majorChangesEn = snapshot.majorChangesRu = changes;
        snapshot.totalBytes = 734003200;
        snapshot.downloadedBytes = 293601280;
        snapshot.majorVersion = "2.0.0";

        if (0 == xr_stricmp(what, "update"))
        {
            snapshot.state = UpdateService::State::Available;
            if (0 == xr_stricmp(state, "downloading"))
                snapshot.state = UpdateService::State::Downloading;
            else if (0 == xr_stricmp(state, "ready"))
                snapshot.state = UpdateService::State::Ready;
            else if (0 == xr_stricmp(state, "failed"))
                snapshot.state = UpdateService::State::DownloadFailed;
            static std::unique_ptr<CUIUpdateWnd> window;
            if (!window)
            {
                window = std::make_unique<CUIUpdateWnd>();
                if (!window->Init())
                {
                    window.reset();
                    return;
                }
            }
            if (!window->IsShown())
                window->ShowDialog(true);
            window->Preview(snapshot);
        }
        else if (0 == xr_stricmp(what, "update_major"))
        {
            static std::unique_ptr<CUIMajorUpdateWnd> window;
            if (!window)
            {
                window = std::make_unique<CUIMajorUpdateWnd>();
                if (!window->Init())
                {
                    window.reset();
                    return;
                }
            }
            if (!window->IsShown())
                window->ShowDialog(true);
            window->Preview(snapshot);
        }
        else if (0 == xr_stricmp(what, "bug_report"))
            MainMenu()->ShowBugReportDialog();
        else if (0 == xr_stricmp(what, "content"))
        {
            ContentService::Snapshot content;
            content.state = 0 == xr_stricmp(state, "repairing") ? ContentService::State::Repairing : ContentService::State::Incomplete;
            content.problems = { "textures_03: missing", "sounds_00: damaged (sha mismatch)", "meshes_00: missing" };
            content.repairTotal = 734003200;
            content.repairDone = 293601280;
            static std::unique_ptr<CUIContentWnd> window;
            if (!window)
            {
                window = std::make_unique<CUIContentWnd>();
                if (!window->Init())
                {
                    window.reset();
                    return;
                }
            }
            if (!window->IsShown())
                window->ShowDialog(true);
            window->Preview(content);
        }
        else if (0 == xr_stricmp(what, "crash_prompt"))
        {
            // the bug report's question after a crash, in its own box
            static std::unique_ptr<CUIMessageBoxEx> box;
            if (!box)
                box = std::make_unique<CUIMessageBoxEx>();
            if (box->InitMessageBox("message_box_sys_yes_no") || box->InitMessageBox("message_box_yes_no"))
            {
                box->SetText(StringTable().translate("st_crash_report_detected_prompt").c_str());
                box->ShowDialog(true);
            }
        }
        else if (0 == xr_stricmp(what, "loading"))
        {
            static std::unique_ptr<CUILoadingPreview> window;
            if (window && window->IsShown())
                window->HideDialog();
            window = std::make_unique<CUILoadingPreview>(state[0] ? std::clamp(float(atof(state)) * 0.01f, 0.f, 1.f) : -1.f);
            window->ShowDialog(true);
        }
        else
            Msg("! dar_ui_dialog update [available|downloading|ready|failed] | update_major | bug_report | content [repairing] | crash_prompt | loading [<per cent>]");
    }

    void Save(IWriter*) override {}
};

// dar_ui_media <texture>: which file a texture name stands for (da_media.h) - a video, a GIF, a picture
class CCC_DarUiMedia final : public IConsole_Command
{
public:
    CCC_DarUiMedia(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        static constexpr pcstr kinds[] = { "nothing of these", "Theora video", "AVI", "GIF", "video", "sequence", "picture" };
        string_path fn{};
        const DaMediaKind kind = da_media_resolve(args, fn);
        Msg("* [media] %s: %s %s", args, kinds[size_t(kind)], fn);
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
        CUIDialogWnd* top = top_window();
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
    CMD1(CCC_DarUiDialog, "dar_ui_dialog");
    CMD1(CCC_DarUiMedia, "dar_ui_media");
    CMD1(CCC_DarUiCursorItem, "dar_ui_cursor_item");
    CMD1(CCC_DarUiKey, "dar_ui_key");
    CMD4(CCC_Integer, "ui_inv_hotkeys", (int*)&g_inventory_hotkeys_panel, 0, 1);
}
