// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The billboards' runtime bake (docs/dead-air/TREES.md "The billboards", SEASONS.md): no billboard atlas ships, the
// engine draws them for the season's day. The trees' atlas pair (da_trees.h: a template's eight views) from the trees'
// own far tiers, and a runtime copy of the level's level_lods whose stock-path vegetation cells (reeds, sedge, duckweed,
// the stock fallen trees and debris, a mod's vegetation) are drawn again from their parts with the latched cards.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

// The bake's shaders ship and compile: the trees' atlas is made with mips and the bake fills it (da_trees.cpp
// build_billboards; else the stock cells' copy stays).
bool da_trees_bake_available();
// The bake fills the trees' atlas this level: the stock cells' copy stays off (da_trees.cpp pre_render).
bool da_trees_bake_owns_atlas();
// level_Load after da_veg_level_load (the census: the stock path's vegetation); level_Unload before
// da_veg_level_unload; renderer destruction.
void da_trees_bake_level_load();
void da_trees_bake_level_unload();
void da_trees_bake_destroy();
// CRender::Render after da_veg_bend, on the immediate context before the G-buffer: the level's first bakes, the cells
// whose look the season's day, the snow, the sun or the presets moved (a few views a frame), the stock path's atlas
// when the texture latch moved.
void da_trees_bake_frame(CBackend& cmd);
// r__trees_stats's lines; r__trees_bb_dump (both atlases and the stock path's copy as DDS and PNG in
// $screenshots$, next frame); r__trees_bb_rebake (everything again, next frame).
void da_trees_bake_stats();
void da_trees_bake_dump();
void da_trees_bake_rebake();
} // namespace xray::render::RENDER_NAMESPACE
