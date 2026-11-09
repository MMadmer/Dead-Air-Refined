// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrCore/XMS/xms_core.h"

// Which screen and which music one showing of the loading screen or the menu gets. The modules declare both in their
// manifests ([screens], [screen.<id>], [track.<id>]); the rules of who gives way to whom are here and nowhere else.
// Contract: docs/dead-air/MODDING.md, "Screens and music".
namespace XmsScreens
{
struct Track
{
    xr_string name; // relative to sounds\, no extension
    float weight{1.f};
    float volume{1.f};
};

struct Choice
{
    const XMS::Module* module{};     // null: the game's own screen
    const XMS::ScreenDecl* screen{};
    xr_vector<Track> tracks;
    bool game_music{true};           // the game's own music is among them
    bool next_track{false};          // a track that ends gives way to another
    bool video_sound{false};         // the screen's videos keep their sound
};

// A module's screen waits for the level to be known: some loading screen is limited to levels or game modes.
bool WaitsForLevel();

// `level` may be null or empty when it is not known yet.
Choice Choose(XMS::ScreenSurface surface, pcstr level);

// The menu's choice, kept while it is open: everything in it that asks - the music, the windows - reads this.
Choice& CurrentMenu();

// What the console says about it: xms_screens.
void Describe();

// QA: the screen the next showings of a surface get. "game", "<module id>.<screen id>", or "off" to pick again.
bool Force(pcstr surface, pcstr what);
} // namespace XmsScreens
