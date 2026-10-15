// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include <atomic>
#include <functional>

struct SDL_Window;

// The engine's splash: the Grizzly emblem (tools/branding/grizzly_emblem.py) in neon lines on black, drawn by a pixel
// shader at the window's own resolution, so it is sharp on any screen. The neon lights up stroke by stroke, catches
// with a flicker and then breathes. It opens before the game's renderer and has a Direct3D 11 device of its own
// whichever renderer the player chose (hardware, else WARP); only without Direct3D at all (not Windows) is the
// finished frame drawn once on the CPU by the same formula.

// The splash window's size for the desktop it opens on: a share of the display's height, 16:10.
void grizzly_splash_size(int& width, int& height);

// Draws into the window on the calling thread until should_exit is set; idle runs between frames.
void grizzly_splash_run(SDL_Window* window, const std::atomic_bool& should_exit, const std::function<void()>& idle);
