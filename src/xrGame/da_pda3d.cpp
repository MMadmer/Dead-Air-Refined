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
#include "PdaAnimatorItem.h"
#include "xrScriptEngine/script_engine.hpp"

extern ENGINE_API shared_str current_player_hud_sect;

extern ENGINE_API Fvector4 g_pda_screen_affects;
extern ENGINE_API Fvector4 g_pda_screen_rect;
extern ENGINE_API Fvector4 g_pda_taa_bbox;
extern ENGINE_API float psHUD_FOV;

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
    // Two marks along the show motion, as fractions of its length. The screen lights up and
    // the loading sequence starts at the first; the UI goes live at the second. The boot
    // therefore plays WHILE the device is being raised and is done as it settles - the
    // Gunslinger mark_anm_show 0.85 is that second one ("display is active"), not the
    // power-on. Lighting up at 0.85 instead put the whole loader after the draw.
    float screen_on_mark{0.06f};
    float boot_done_mark{0.85f};
    // Interference approach speed, normalized units per second (the original ramps its
    // electronics counter at a fixed rate - a step change reads as a toggle, not a wave).
    float interference_ramp{0.15f};
    // Joystick quantizer: refresh period (ms) and cursor deadzone (canvas px per period).
    u32 joystick_period{100};
    float joystick_deadzone{2.f};
    // level name -> base interference, straight from the ltx; unknown levels contribute 0
    xr_vector<std::pair<shared_str, float>> level_interference;
};
SCfg cfg;

bool presenter{};
bool focused{};
float lua_poll_at{};
float lua_power{1.f};
float lua_interference{};
float x_smooth{};
float phase_at{};
float phase{};

// Screen power-on gate: dark until screen_on_at, boot sequence until boot_until.
float screen_on_at{-1.f};
float boot_until{-1.f};

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
    cfg.screen_on_mark =
        ini.line_exist("pda3d", "screen_on_mark") ? ini.r_float("pda3d", "screen_on_mark") : 0.06f;
    cfg.boot_done_mark =
        ini.line_exist("pda3d", "boot_done_mark") ? ini.r_float("pda3d", "boot_done_mark") : 0.85f;
    cfg.interference_ramp =
        ini.line_exist("pda3d", "interference_ramp") ? ini.r_float("pda3d", "interference_ramp") : 0.15f;
    cfg.joystick_period =
        ini.line_exist("pda3d", "joystick_update_period") ? ini.r_u32("pda3d", "joystick_update_period") : 100;
    cfg.joystick_deadzone =
        ini.line_exist("pda3d", "joystick_deadzone") ? ini.r_float("pda3d", "joystick_deadzone") : 2.f;
    cfg.screen_on_mark = clampr(cfg.screen_on_mark, 0.f, 1.f);
    cfg.boot_done_mark = clampr(cfg.boot_done_mark, cfg.screen_on_mark, 1.f);
    if (cfg.interference_ramp <= 0.f)
        cfg.interference_ramp = 0.15f;
    if (cfg.joystick_period < 16)
        cfg.joystick_period = 16;
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
float activate_at{};
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

// The presenter in the animation slot, if the item there is ours. Dead Air parks its own
// short-lived animation items in the same slot (the quick knife strike, the mask cleaning):
// treating any occupant as the presenter holstered and released them mid-play.
static CPdaAnimatorItem* presenter_item(CActor* actor)
{
    return actor ? smart_cast<CPdaAnimatorItem*>(actor->inventory().ItemFromSlot(ANIMATION_SLOT)) : nullptr;
}

// The Lua bridge stays the transport (alife create / activate_slot / release live there),
// but ONLY update() drives it now - key handlers never call these directly.
bool request_activate()
{
    load_cfg();
    if (!cfg.available)
        return false;
    const float now = Device.fTimeGlobal;
    if (now < activate_at)
        return true;
    activate_at = now + 0.3f;
    return call_toggle("_G.da_pda3d_activate");
}

bool request_deactivate()
{
    const float now = Device.fTimeGlobal;
    if (now < deactivate_at)
        return true;
    deactivate_at = now + 0.3f;
    return call_toggle("_G.da_pda3d_deactivate");
}

CUIPdaWnd* pda_wnd()
{
    CUIGameCustom* ui = CurrentGameUI();
    return ui ? ui->GetPdaMenuPtr() : nullptr;
}

// Joystick state: the accumulated cursor motion of the current period, quantized into
// 8 compass directions + click + idle - becomes an animation-name suffix, exactly the
// original's thumb-on-the-stick mechanic.
enum class EDir : u8
{
    Idle,
    Up,
    UpRight,
    Right,
    DownRight,
    Down,
    DownLeft,
    Left,
    UpLeft,
    Click
};
struct SJoystick
{
    Fvector2 accum{};
    EDir dir{EDir::Idle};
    u32 next_step{};
    bool click_pending{};
} joy;

EDir dir_by_angle(float a)
{
    // atan2 result mapped onto 8 sectors of 45 degrees (PI/4), sector centers on the axes.
    // Screen y grows downward, so +y is Down.
    constexpr float S = PI / 8.f; // half-sector
    if (a >= -S && a < S)
        return EDir::Right;
    if (a >= S && a < 3 * S)
        return EDir::DownRight;
    if (a >= 3 * S && a < 5 * S)
        return EDir::Down;
    if (a >= 5 * S && a < 7 * S)
        return EDir::DownLeft;
    if (a >= -3 * S && a < -S)
        return EDir::UpRight;
    if (a >= -5 * S && a < -3 * S)
        return EDir::Up;
    if (a >= -7 * S && a < -5 * S)
        return EDir::UpLeft;
    return EDir::Left;
}
} // namespace

void request_unzoom() { unzoom_request = true; }
bool consume_unzoom_request()
{
    const bool r = unzoom_request;
    unzoom_request = false;
    return r;
}

bool show_window()
{
    load_cfg();
    if (!cfg.available)
        return false;
    CUIGameCustom* ui = CurrentGameUI();
    CUIPdaWnd* pda = ui ? ui->GetPdaMenuPtr() : nullptr;
    if (!pda)
        return false;
    ui->AddDialogToRender(pda); // fires Show(true) itself - info portions once
    return true;
}

void hide_window()
{
    CUIGameCustom* ui = CurrentGameUI();
    CUIPdaWnd* pda = ui ? ui->GetPdaMenuPtr() : nullptr;
    if (!pda)
        return;
    if (focused)
    {
        ui->UnfocusHeldDialog(pda);
        TimeDilator()->SetCurrentMode(UITimeDilator::None);
        set_ui_focused(false);
    }
    ui->RemoveDialogToRender(pda); // fires Show(false) itself
}

bool window_shown()
{
    CUIPdaWnd* pda = pda_wnd();
    return pda && pda->IsShown();
}

bool toggle()
{
    if (window_shown())
    {
        if (focused)
            request_unzoom(); // at the face: first step back is lowering it
        else
            hide_window();
        return true;
    }
    return show_window();
}

void joystick_accum(float dx, float dy)
{
    joy.accum.x += dx;
    joy.accum.y += dy;
}

void joystick_click() { joy.click_pending = true; }

bool joystick_step()
{
    load_cfg();
    const u32 now = Device.dwTimeGlobal;
    if (now < joy.next_step)
        return false;
    joy.next_step = now + cfg.joystick_period;

    EDir dir = EDir::Idle;
    if (joy.click_pending)
        dir = EDir::Click;
    else if (_abs(joy.accum.x) >= cfg.joystick_deadzone || _abs(joy.accum.y) >= cfg.joystick_deadzone)
        dir = dir_by_angle(atan2f(joy.accum.y, joy.accum.x));

    joy.click_pending = false;
    joy.accum.set(0.f, 0.f);

    if (dir == joy.dir)
        return false;
    joy.dir = dir;
    return true;
}

const char* joystick_suffix()
{
    switch (joy.dir)
    {
    case EDir::Up: return "_up";
    case EDir::UpRight: return "_up_right";
    case EDir::Right: return "_right";
    case EDir::DownRight: return "_down_right";
    case EDir::Down: return "_down";
    case EDir::DownLeft: return "_down_left";
    case EDir::Left: return "_left";
    case EDir::UpLeft: return "_up_left";
    case EDir::Click: return "_click";
    default: return "";
    }
}

void joystick_reset()
{
    joy.accum.set(0.f, 0.f);
    joy.dir = EDir::Idle;
    joy.click_pending = false;
    joy.next_step = 0;
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
void set_ui_focused(bool f)
{
    focused = f;
    if (f)
        joystick_reset();
}
bool ui_focused() { return focused; }

bool want_rt() { return window_shown() || presenter || g_pda3d_dbg > 0; }

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

void on_shown(u32 motion_start_ms, u32 motion_end_ms)
{
    load_cfg();
    const float now = Device.fTimeGlobal;
    // Both boot marks ride the show motion, so the sequence scales with whatever draw
    // animation the data provides: dark while the device leaves the pocket, the loader
    // running as it comes up, and the live UI the moment it settles in front of the eyes.
    if (motion_end_ms > motion_start_ms)
    {
        const float dur = float(motion_end_ms - motion_start_ms) / 1000.f;
        // The motion has just been started by the caller, so elapsed is ~0 - measure it
        // anyway, a late call must not push the whole sequence past the animation.
        const float elapsed = float(s32(Device.dwTimeGlobal - motion_start_ms)) / 1000.f;
        screen_on_at = now + (cfg.screen_on_mark * dur - elapsed);
        boot_until = now + (cfg.boot_done_mark * dur - elapsed);
    }
    else
    {
        // No timings (savegame restore, missing motion): fall back to a fixed window.
        screen_on_at = now;
        boot_until = now + cfg.boot_time;
    }
    if (boot_until <= screen_on_at)
        boot_until = screen_on_at + 0.05f;

    if (g_pda3d_dbg > 0)
        Msg("* [pda3d] boot: draw %.2fs, screen on at +%.2fs, ui live at +%.2fs",
            motion_end_ms > motion_start_ms ? float(motion_end_ms - motion_start_ms) / 1000.f : 0.f,
            screen_on_at - now, boot_until - now);
}

void reset()
{
    presenter = false;
    focused = false;
    unzoom_request = false;
    hands_swapped = false;
    prev_hands = "";
    x_smooth = 0.f;
    lua_interference = 0.f;
    lua_power = 1.f;
    lua_poll_at = 0.f;
    screen_on_at = -1.f;
    boot_until = -1.f;
    activate_at = 0.f;
    deactivate_at = 0.f;
    joystick_reset();
}

void update()
{
    load_cfg();
    poll_lua();

    const float now = Device.fTimeGlobal;

    // ---- Window-led ownership: derive the presenter from the window, every frame -------
    // The window shown but no presenter item -> raise one. The window hidden but the item
    // still up -> put it away. State never leaks because nothing here is remembered.
    if (cfg.available && g_pGameLevel && Level().bReady)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (actor && !actor->g_Alive())
            actor = nullptr;
        const bool win = window_shown();
        PIItem in_slot = presenter_item(actor);
        if (actor)
        {
            // window up, device not raised (missing, spawned-but-idle, or mid-holster) ->
            // ask the script to raise; window down but anything still up -> put it away.
            if (win && !presenter)
            {
                if (!request_activate())
                {
                    // The script bridge refused with a live actor: the Lua side is absent
                    // or broken. Kill the 3D capability for this session and reopen the
                    // plain 2D dialog - a shown window that can never grow a device is the
                    // one state the player cannot escape.
                    Msg("! [pda3d] activation bridge dead - falling back to the 2D dialog");
                    cfg.available = false;
                    hide_window();
                    if (CUIGameCustom* ui = CurrentGameUI())
                        ui->ShowPdaMenu();
                }
            }
            else if (!win && (presenter || in_slot))
                request_deactivate();
        }
    }

    // Hands swap back only AFTER the presenter item is fully gone (released from the slot):
    // reloading the hands while any hud item is attached dangles its resolved MotionIDs.
    if (hands_swapped && !presenter)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (!presenter_item(actor))
            swap_hands_out();
    }

    // Self-heal: the presenter flag with no item behind it for over a second means some
    // path we did not foresee killed the item without its state machine (net_Destroy covers
    // the known ones). A leaked flag suppresses the 2D dialog and swallows every toggle -
    // force-clear it.
    static float orphan_since = -1.f;
    if (presenter)
    {
        CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
        if (presenter_item(actor))
            orphan_since = -1.f;
        else if (orphan_since < 0.f)
            orphan_since = now;
        else if (now - orphan_since > 1.f)
        {
            Msg("! [pda3d] presenter flag with no item in the slot - force reset");
            orphan_since = -1.f;
            hide_window();
            set_presenter_active(false);
        }
    }
    else
        orphan_since = -1.f;

    // ---- Screen state ------------------------------------------------------------------
    // Interference: the strongest of the per-level base and the Lua-supplied value. A dead
    // battery pushes the SAME channel past the shader's blackout threshold - one contract,
    // three states (see model_pda_screen.ps).
    float x_target = std::max(level_base_interference(), lua_interference);
    if (lua_power <= cfg.power_low)
        x_target = std::max(x_target, cfg.blackout_level);

    // Rate-limited approach (the original's electronics counter): the screen degrades and
    // recovers as a wave, never as a switch flip. Only the ORGANIC sources ride this ramp.
    const float max_step = cfg.interference_ramp * Device.fTimeDelta;
    const float delta = x_target - x_smooth;
    if (_abs(delta) <= max_step)
        x_smooth = x_target;
    else
        x_smooth += (delta > 0.f ? max_step : -max_step);

    // Boot window: the shader shows the loading sequence only while `a` is set AND x is at
    // or past its 0.08 gate, so the floor is applied to the OUTPUT, after the ramp - never
    // through it. Ramping into the gate made the boot screen arrive ~0.5 s late (0.08 at
    // 0.15/s), so the live UI flashed first, then the loader, then the UI again.
    const bool booting = boot_until > 0.f && now >= screen_on_at && now < boot_until;
    const float x_out = booting ? std::max(x_smooth, 0.10f) : x_smooth;

    // The y channel is the original's "phase/random driver" - it feeds tear amplitude and
    // the high-interference image shift. A held value stepped a few times a second reads as
    // analogue glitching; per-frame randomness reads as white noise.
    if (now >= phase_at)
    {
        phase_at = now + 0.125f;
        phase = ::Random.randF(0.f, 1.f);
    }

    // Brightness gates on the power-on mark: the device comes out of the pocket dark and
    // lights up when the thumb hits the button during the draw motion.
    const float bright = (screen_on_at > 0.f && now < screen_on_at) ? 0.f : cfg.brightness;

    g_pda_screen_affects.set(x_out, phase, bright, booting ? 1.f : 0.f);

    // The PDA face sub-rect: taken from the live layout every frame, so aspect swaps
    // (pda.xml vs pda_16.xml), UI resets and modded layouts all stay correct.
    if (CUIPdaWnd* pda = pda_wnd())
    {
        Fvector4 uv;
        if (pda->GetScreenRectUV(uv))
            g_pda_screen_rect = uv;
    }

    // TAA exclusion bbox: project the held device's bounds with the SAME hud projection
    // the renderer uses this frame (psHUD_FOV already carries the zoom factor). The device
    // screen is a forward pass with no G-buffer depth - reprojection under it follows the
    // background and smears the display, so da_taa.ps passes this box through untouched.
    g_pda_taa_bbox.set(1.f, 1.f, 0.f, 0.f);
    if (presenter && g_player_hud)
        if (attachable_hud_item* hi = g_player_hud->attached_item(0))
            if (hi->m_model)
                if (IRenderVisual* v = hi->m_model->dcast_RenderVisual())
                {
                    Fmatrix proj;
                    proj.build_projection(
                        deg2rad(psHUD_FOV * Device.fFOV), Device.fASPECT, 0.05f, 100.f);
                    Fmatrix view_proj, to_clip;
                    view_proj.mul(proj, Device.mView);
                    to_clip.mul(view_proj, hi->m_item_transform); // full 4x4: keeps the w row
                    const Fbox& box = v->getVisData().box;
                    Fvector2 mn{2.f, 2.f}, mx{-2.f, -2.f};
                    bool ok = true;
                    for (int i = 0; i < 8 && ok; ++i)
                    {
                        Fvector c;
                        box.getpoint(i, c);
                        Fvector4 clip;
                        to_clip.transform(clip, c);
                        if (clip.w <= 0.01f)
                            ok = false;
                        else
                        {
                            const float x = clip.x / clip.w * 0.5f + 0.5f;
                            const float y = clip.y / clip.w * -0.5f + 0.5f;
                            mn.x = std::min(mn.x, x);
                            mn.y = std::min(mn.y, y);
                            mx.x = std::max(mx.x, x);
                            mx.y = std::max(mx.y, y);
                        }
                    }
                    if (ok)
                    {
                        constexpr float m = 0.012f; // one clamp-neighbourhood of margin
                        g_pda_taa_bbox.set(clampr(mn.x - m, 0.f, 1.f), clampr(mn.y - m, 0.f, 1.f),
                            clampr(mx.x + m, 0.f, 1.f), clampr(mx.y + m, 0.f, 1.f));
                    }
                }
}
} // namespace da_pda3d
