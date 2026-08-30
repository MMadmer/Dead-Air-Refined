#include "StdAfx.h"
#include "da_pda3d.h"

#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "UIGameCustom.h"
#include "UIGameSP.h"
#include "ui/UIPdaWnd.h"
#include "UITimeDilator.h"
#include "player_hud.h"
#include "xrScriptEngine/script_engine.hpp"

extern ENGINE_API shared_str current_player_hud_sect;

extern ENGINE_API Fvector4 g_pda_screen_affects;
extern ENGINE_API Fvector4 g_pda_screen_rect;

int g_pda3d_dbg = 0;

namespace da_pda3d
{
namespace
{
struct SCfg
{
    bool loaded{};
    bool available{};
    shared_str animator_section;
    shared_str hands_section;
    float boot_time{1.2f};
    float blackout_level{0.42f};
    float power_low{0.05f};
    float brightness{1.f};
    // level name -> base interference, straight from the ltx; unknown levels contribute 0
    xr_vector<std::pair<shared_str, float>> level_interference;
};
SCfg cfg;

bool presenter{};
bool focused{};
float boot_until{-1.f};
float lua_poll_at{};
float lua_power{1.f};
float lua_interference{};
float x_smooth{};
float phase_at{};
float phase{};

void load_cfg()
{
    if (cfg.loaded)
        return;
    cfg.loaded = true;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_pda3d.ltx");
    if (!FS.exist(path))
    {
        Msg("! [pda3d] dead_air_x64_pda3d.ltx not found - 3D PDA disabled, 2D fallback");
        return;
    }
    CInifile ini(path, TRUE);
    if (!ini.section_exist("pda3d"))
        return;
    cfg.animator_section = ini.line_exist("pda3d", "animator_section") ?
        ini.r_string("pda3d", "animator_section") : "";
    cfg.hands_section = ini.line_exist("pda3d", "hands_section") ?
        ini.r_string("pda3d", "hands_section") : "";
    cfg.boot_time = ini.line_exist("pda3d", "boot_time") ? ini.r_float("pda3d", "boot_time") : 1.2f;
    cfg.blackout_level =
        ini.line_exist("pda3d", "blackout_level") ? ini.r_float("pda3d", "blackout_level") : 0.42f;
    cfg.power_low =
        ini.line_exist("pda3d", "power_low_threshold") ? ini.r_float("pda3d", "power_low_threshold") : 0.05f;
    cfg.brightness = ini.line_exist("pda3d", "brightness") ? ini.r_float("pda3d", "brightness") : 1.f;
    if (ini.section_exist("pda3d_interference_levels"))
        for (const auto& [name, value] : ini.r_section("pda3d_interference_levels").Data)
            if (name.size() && value.size())
                cfg.level_interference.emplace_back(name, float(atof(value.c_str())));

    // Capability check: the whole feature exists only while the data side is complete -
    // animator section present, it names a hud, the hud names a model. Anything missing
    // means the 2D dialog keeps working exactly as before.
    if (!cfg.animator_section.size() || !pSettings->section_exist(cfg.animator_section))
    {
        Msg("! [pda3d] animator section missing - 2D fallback");
        return;
    }
    if (!pSettings->line_exist(cfg.animator_section, "hud"))
        return;
    const shared_str hud = pSettings->r_string(cfg.animator_section, "hud");
    if (!pSettings->section_exist(hud) || !pSettings->line_exist(hud, "item_visual"))
        return;
    cfg.available = true;
    Msg("* [pda3d] enabled: item [%s]", cfg.animator_section.c_str());
}

float level_base_interference()
{
    if (!g_pGameLevel)
        return 0.f;
    const shared_str& name = Level().name();
    for (const auto& [lvl, k] : cfg.level_interference)
        if (lvl == name)
            return k;
    return 0.f;
}

// Optional Lua sources with safe defaults: pda.get_power() -> 0..1 (1 when absent),
// pda.get_interference() -> 0..1 (0 when absent). Polled a few times a second - these
// cross the Lua boundary and must not run per frame.
void poll_lua()
{
    const float now = Device.fTimeGlobal;
    if (now < lua_poll_at)
        return;
    lua_poll_at = now + 0.25f;

    luabind::functor<float> fn;
    if (GEnv.ScriptEngine->functor<float>("dead_air_x64_pda3d.get_power", fn))
    {
        const float v = fn();
        lua_power = clampr(v, 0.f, 1.f);
    }
    else
        lua_power = 1.f;
    if (GEnv.ScriptEngine->functor<float>("dead_air_x64_pda3d.get_interference", fn))
    {
        const float v = fn();
        lua_interference = clampr(v, 0.f, 1.f);
    }
    else
        lua_interference = 0.f;
}
} // namespace

namespace
{
bool unzoom_request{};
float deactivate_at{};
bool hands_swapped{};
shared_str prev_hands;

bool call_toggle(pcstr fn_name)
{
    luabind::functor<bool> fn;
    if (!GEnv.ScriptEngine->functor<bool>(fn_name, fn))
        return false;
    return fn();
}
} // namespace

void request_unzoom() { unzoom_request = true; }
bool consume_unzoom_request()
{
    const bool r = unzoom_request;
    unzoom_request = false;
    return r;
}

bool request_activate()
{
    load_cfg();
    if (!cfg.available)
        return false;
    return call_toggle("_G.da_pda3d_activate");
}

bool toggle()
{
    if (presenter)
    {
        if (focused)
            request_unzoom(); // at the face: first step back is lowering it
        else
            request_deactivate();
        return true;
    }
    return request_activate();
}

bool request_deactivate()
{
    // Throttled: the force-hidden watchdog in UpdateCL fires per frame until the item
    // actually starts hiding.
    const float now = Device.fTimeGlobal;
    if (now < deactivate_at)
        return true;
    deactivate_at = now + 0.5f;
    return call_toggle("_G.da_pda3d_deactivate");
}

void swap_hands_in()
{
    load_cfg();
    if (!cfg.hands_section.size() || !pSettings->section_exist(cfg.hands_section))
        return;
    if (!g_player_hud || hands_swapped)
        return;
    // The REAL hands section lives on player_hud; current_player_hud_sect is a diagnostic
    // global that tracks the last loaded ITEM hud section - the wrong thing to remember.
    prev_hands = g_player_hud->section_name();
    if (prev_hands == cfg.hands_section)
        return;
    hands_swapped = true;
    Msg("* [pda3d] hands swap in: [%s] -> [%s]", prev_hands.c_str(), cfg.hands_section.c_str());
    g_player_hud->load(cfg.hands_section);
}

void swap_hands_out()
{
    if (!hands_swapped)
        return;
    hands_swapped = false;
    Msg("* [pda3d] hands swap out -> [%s]", prev_hands.c_str());
    // Outfit changes only re-load hands on their own events, and opening the inventory
    // lowers the device first - so the remembered section is always current here.
    if (g_player_hud && prev_hands.size())
        g_player_hud->load(prev_hands);
}

void set_presenter_active(bool active)
{
    presenter = active;
    if (!active)
        focused = false;
}
bool presenter_active() { return presenter; }
void set_ui_focused(bool f) { focused = f; }
bool ui_focused() { return focused; }

bool want_rt() { return presenter || g_pda3d_dbg > 0; }

bool available()
{
    load_cfg();
    return cfg.available;
}

const shared_str& animator_section()
{
    load_cfg();
    return cfg.animator_section;
}

void on_shown()
{
    load_cfg();
    boot_until = Device.fTimeGlobal + cfg.boot_time;
}

void update()
{
    load_cfg();
    poll_lua();

    const float now = Device.fTimeGlobal;

    // Hands swap back only AFTER the presenter item is fully gone (released from the slot):
    // reloading the hands while any hud item is attached dangles its resolved MotionIDs.
    if (hands_swapped && !presenter)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (!actor || !actor->inventory().ItemFromSlot(13))
            swap_hands_out();
    }

    // Self-heal: the presenter flag with no item behind it for over a second means some
    // path we did not foresee killed the item without its state machine (net_Destroy covers
    // the known ones). A leaked flag is the worst failure mode - it suppresses the 2D
    // dialog and swallows every toggle - so it gets force-cleared here.
    static float orphan_since = -1.f;
    if (presenter)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (actor && actor->inventory().ItemFromSlot(13))
            orphan_since = -1.f;
        else if (orphan_since < 0.f)
            orphan_since = now;
        else if (now - orphan_since > 1.f)
        {
            Msg("! [pda3d] presenter flag with no item in the slot - force reset");
            orphan_since = -1.f;
            if (CUIGameCustom* ui = CurrentGameUI())
                if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
                {
                    if (focused)
                    {
                        ui->UnfocusHeldDialog(pda);
                        TimeDilator()->SetCurrentMode(UITimeDilator::None);
                    }
                    ui->RemoveDialogToRender(pda);
                }
            set_presenter_active(false);
        }
    }
    else
        orphan_since = -1.f;

    // Interference: the strongest of the per-level base and the Lua-supplied value. A dead
    // battery pushes the SAME channel past the shader's blackout threshold - one contract,
    // three states (see model_pda_screen.ps).
    float x_target = std::max(level_base_interference(), lua_interference);
    if (lua_power <= cfg.power_low)
        x_target = std::max(x_target, cfg.blackout_level);

    // Boot window: while it lasts the shader's `a` branch shows the loading sequence, and x
    // is floored to the 0.08 gate that branch requires.
    const bool booting = boot_until > 0.f && now < boot_until;
    if (booting)
        x_target = std::max(x_target, 0.10f);

    // Ease the level instead of stepping it: the screen dying/waking reads as electronics,
    // not as a toggle.
    const float k = 1.f - expf(-Device.fTimeDelta / 0.15f);
    x_smooth += (x_target - x_smooth) * k;

    // The y channel is the original's "phase/random driver" - it feeds tear amplitude and
    // the high-interference image shift. A held value stepped a few times a second reads as
    // analogue glitching; per-frame randomness reads as white noise.
    if (now >= phase_at)
    {
        phase_at = now + 0.125f;
        phase = ::Random.randF(0.f, 1.f);
    }

    g_pda_screen_affects.set(x_smooth, phase, cfg.brightness, booting ? 1.f : 0.f);

    // The PDA face sub-rect: taken from the live layout every frame, so aspect swaps
    // (pda.xml vs pda_16.xml), UI resets and modded layouts all stay correct.
    if (CUIGameCustom* ui = CurrentGameUI())
        if (CUIPdaWnd* pda = ui->GetPdaMenuPtr())
        {
            Fvector4 uv;
            if (pda->GetScreenRectUV(uv))
                g_pda_screen_rect = uv;
        }
}
} // namespace da_pda3d
