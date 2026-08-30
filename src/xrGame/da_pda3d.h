#pragma once

// 3D PDA feature core (docs/dead-air/pda-3d-port-plan.md). Holds the session state (is the
// hand presenter up, is the UI focused), reads the data side (dead_air_x64_pda3d.ltx) and
// publishes the screen-shader constants each frame. Mechanism here, bindings in data: no
// item sections, level names or foreign-mod script names live in C++.

namespace da_pda3d
{
// Lifecycle, driven by CPdaAnimatorItem.
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

// Per-frame service: computes g_pda_screen_affects (interference / power / boot) and
// g_pda_screen_rect (the PDA face sub-rect in canvas UV). Cheap; call once per frame.
void update();

// The show animation started - arms the boot-screen window.
void on_shown();

// UI -> item requests. ESC in the focused stage asks the item to lower from the face;
// the item consumes the flag in its UpdateCL.
void request_unzoom();
bool consume_unzoom_request();

// Activation bridge. The item lives and dies through the Lua compat script (the proven
// alife-create + activate_slot recipe of dinamic_hud); C++ only asks. Both return false
// when the script side is absent - the caller then falls back to the 2D dialog.
bool request_activate();
bool request_deactivate();

// THE one PDA-key behaviour, used by every entry point: not up -> raise; up and at the
// face -> just lower it from the face; up in hands -> put it away. Returns false only
// when a raise was needed and the script side is unavailable (2D fallback).
bool toggle();

// Hands swap for the PDA episode: the Gunslinger animation set is authored against its own
// hands rig, so player_hud switches to the configured hands model while the device is up
// and back on holster. Data-driven; an empty/missing hands_section keeps the current hands.
void swap_hands_in();
void swap_hands_out();
}

// Console: 1 = rasterize the PDA into the RT and suppress the 2D pass even with no
// presenter (pipeline debugging), 2 = also draw the RT content as a fullscreen overlay.
extern int g_pda3d_dbg;
