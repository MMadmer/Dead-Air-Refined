#pragma once

// 3D PDA feature core (docs/dead-air/pda-3d-port-plan.md, docs/dead-air/pda-1to1-plan.md).
// Holds the session state (is the hand presenter up, is the UI focused), reads the data side
// (dead_air_x64_pda3d.ltx) and publishes the screen-shader constants each frame.
// Mechanism here, bindings in data: no item sections, level names or foreign-mod script
// names live in C++.
//
// Ownership model is WINDOW-LED (the Gunslinger original's scheme): the PDA dialog window
// being shown is the single source of truth. Key handlers only show/hide the window;
// update() derives everything else each frame - spawns the presenter item when the window
// is up, puts it away when the window goes down. State can not leak because it is not
// stored, it is recomputed.

namespace da_pda3d
{
// Lifecycle facts, reported by CPdaAnimatorItem (the item IS in hands / at the face).
void set_presenter_active(bool active);
bool presenter_active();
// Second input stage: the PDA is raised to the face and the UI owns the cursor.
void set_ui_focused(bool focused);
bool ui_focused();

// Does anything want the PDA rasterized into $user$ui this frame?
bool want_rt();

// Feature capability: the config exists, the animator section exists and carries a hud.
// Missing anything = the whole feature silently stays 2D.
bool available();
// The animator item section name from the config (empty when unavailable).
const shared_str& animator_section();

// Per-frame service: the window-led ownership watch (spawn/put away the presenter to match
// the window), g_pda_screen_affects (interference / power / boot / brightness) and
// g_pda_screen_rect (the PDA face sub-rect in canvas UV). Call once per frame.
void update();

// Full session reset - actor respawn / savegame load. Drops every derived flag, the
// interference ramp, the boot window and the remembered hands so nothing survives into
// the next life.
void reset();

// The show animation started; motion timings mark when the screen physically turns on
// (screen_on_mark of the show motion) - before that moment the screen stays dark, after
// it the boot sequence plays. Times in ms of Device.dwTimeGlobal; pass 0/0 when unknown.
void on_shown(u32 motion_start_ms, u32 motion_end_ms);

// UI -> item requests. ESC/P/M in the focused stage ask the item to lower from the face;
// the item consumes the flag in its UpdateCL.
void request_unzoom();
bool consume_unzoom_request();

// Window control - the ONLY thing input paths do. show_window puts the dialog on the
// render list (render-only, no input stack); hide_window removes it. update() notices and
// walks the presenter item after them.
bool show_window();
void hide_window();
bool window_shown();

// THE one PDA-key behaviour, used by every entry point: window hidden -> show it; shown
// and at the face -> lower from the face; shown in hands -> hide it. Returns false only
// when showing was needed and the feature is unavailable (2D fallback).
bool toggle();

// Joystick: the focused-stage cursor drives the thumb on the device. The dialog reports
// raw cursor motion and clicks; the item asks for the current animation suffix
// ("_up".."_up_left", "_click" or "") once per joystick period and replays idle on change.
void joystick_accum(float dx, float dy);
void joystick_click();
// Steps the quantizer if the period elapsed; returns true when the suffix changed.
bool joystick_step();
const char* joystick_suffix();
void joystick_reset();

// Hands swap for the PDA episode: the Gunslinger animation set is authored against its own
// hands rig, so player_hud switches to the configured hands model while the device is up
// and back on holster. Data-driven; an empty/missing hands_section keeps the current hands.
void swap_hands_in();
void swap_hands_out();
}

// Console: 1 = rasterize the PDA into the RT and suppress the 2D pass even with no
// presenter (pipeline debugging), 2 = also draw the RT content as a fullscreen overlay.
extern int g_pda3d_dbg;
