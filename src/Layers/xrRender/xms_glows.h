// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The glows the modules add to a level (`overlay.things`, `[glow:...]`): a picture that always
// faces the viewer, at a place of the level, fading in while the place is in sight and out when
// something of the level stands between. The level's own glows and the ones lamps ask the
// renderer for are no part of this: the game draws neither, and that stays as it is.
namespace xray::render::RENDER_NAMESPACE
{
// Reads the level's glows; a level without any costs nothing afterwards.
void xms_glows_level_load();
void xms_glows_level_unload();
// Draws them into the frame's final picture, where the lens flares are drawn.
void xms_glows_render();
} // namespace xray::render::RENDER_NAMESPACE
