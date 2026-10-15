// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// Spent cartridge cases: a model per calibre thrown from the ejection port, bouncing off the
// level and the objects in it and lying where they land, kicked aside by feet. The stock shell
// was a sprite particle in hud space that fell through the world.
//
// Not physics shells: a case is a point with a radius under gravity, raycast against the
// collision geometry, so hundreds cost a few rays a frame and nothing a frame once they rest.
// How many may exist at once is the quality preset's (ps_shells_max). Calibres, models,
// masses, ejection and sounds live in dead_air_x64_shells.ltx.

class CGameObject;

namespace da_shells
{
struct Ejection
{
    CGameObject* shooter{};    // never stops its own cases while they fly clear of it
    shared_str weapon;         // weapon section (ejection overrides)
    shared_str particles;      // the weapon's shell_particles (calibre when the ammo names none)
    shared_str ammo;           // ammo section of the shot
    Fvector position{};        // shell point; hud space when hud is set
    Fvector forward{};         // fire direction
    Fvector carrier_velocity{}; // the shooter's own motion
    bool hud{};                // the first-person weapon, drawn with its own field of view
};

// True when a case was thrown (or the shot is too far to bother): the sprite must not play.
bool eject(const Ejection& shot);
// Main thread, once a frame.
void update();
void level_unload();
// qa_shells: how many cases there are, flying, sliding and lying, and where the nearest lie.
void report();
// qa_shells look <height>: the script camera over the cases, looking down at them.
void look(float height);
// QA: the player put a distance before the middle of his own cases, facing them.
void approach(float distance);
} // namespace da_shells
