// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Reed beds (docs/dead-air/GRASS.md, reed beds). The level's stock reed and sedge models are placements: where one
// stands, the blade grass grows the reed bed's own species (common reed, cattail, reed canary grass, sedges) through
// the year, rooted on the bed under the water (da_grass_set_reed_beds). Past the blades' radius the stock model's
// billboard stands for the bed, its cells drawn for the day by the bake (da_trees_bake.cpp).
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
struct R_dsgraph_structure;
class dxRender_Visual;

// level_Load after da_veg_level_load (its census of the stock models) and before the bake and the GPU-driven
// registry: the stock reed models leave their hierarchies and the blades grow their beds. level_Unload before the
// vegetation core's: they come back.
void da_reeds_level_load();
void da_reeds_level_unload();
// Top of CRender::Calculate, before any view walks the graph: a changed r__reeds (QA) takes the stock models out of
// their hierarchies or puts them back (the GPU-driven registry and the ray tracing scene rebuilt).
void da_reeds_apply_switch();
// CRender::Calculate, main thread, right after the main view's graph is built: the billboards of the replaced models
// past the blades' radius into its LOD list, fading in over the last part of it as the blades fade out.
void da_reeds_frame(R_dsgraph_structure& dsgraph);
// The replaced models (the ray tracing scene's stand-ins, da_veg_replaced_visuals).
void da_reeds_replaced_visuals(xr_vector<dxRender_Visual*>& out);
} // namespace xray::render::RENDER_NAMESPACE
