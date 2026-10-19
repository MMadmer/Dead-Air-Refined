// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Procedural blade grass after Ghost of Tsushima (docs/dead-air/GRASS.md): blades generated and culled
// in compute from the level's detail slots, drawn indirectly as procedural strips, with seasons, snow,
// wind, motors and the push of every creature. The stock detail path keeps every detail class that is
// not grass.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

void da_grass_level_load();   // after Details->Load() and da_snow_level_load()
void da_grass_level_unload(); // before Details->Unload()
// Blades are drawn this frame (level has blade classes, compute shaders present, details on).
bool da_grass_active();
// Immediate context, after da_snow_frame_prepare(): the generation compute passes of this frame.
void da_grass_frame_prepare(CBackend& cmd_list);
// G-buffer phase, right after the stock Details->Render() (immediate context).
void da_grass_render_gbuffer(CBackend& cmd_list);
// Sun cascade `cascade` (0 near, 1 mid), at the details site of render_phase_sun.cpp.
void da_grass_render_cascade(CBackend& cmd_list, u32 cascade);
// xrRender_sync_preset_derived: the grass columns of preset 0..4 (minimum..extreme).
void da_grass_apply_preset(u32 preset);
// r__grass_density: the density tier 0..4, the quality preset's tiers.
void da_grass_apply_density(u32 density);
// How far the blades reach (m): past the detail radius, their cover thinning from d2 on.
float da_grass_reach();

// ---- Additions of the GRASS stage -------------------------------------------------------------------
// Blades draw into sun cascade `cascade` this frame (render_phase_sun.cpp: a cascade with no other caster
// still has to open its map for them).
bool da_grass_casts(u32 cascade);
// The compute shaders ship with the content and this level did not fail to set the blades up: the detail
// manager leaves the blade classes to the blades (CDetailManager::Load).
bool da_grass_blades_available();
// Main thread, once per frame (CDetailManager::UpdateRenderState, before the calc task and the passes):
// the season keys of the species at D and D_prev, the camera, the settings, the bin windows.
void da_grass_frame_latch();
// CRender::Calculate, main thread, before r_sun.run(): the latch of a frame the detail manager did not update
// (a save's screenshot under the menu), taken before the sun task's cascades read it.
void da_grass_frame_ensure_latch();
// The detail calc task (CDetailManager::DispatchMTCalc): the tiles and compute groups of this frame.
void da_grass_calc(const Fvector& eye);
// Share 0..1 of blade grass around the camera this frame (wind_veg_green, the vegetation audio).
float da_grass_green();
// Does a boot at `feet` brush through blade grass (the press motor's rustle probe, calc task)?
bool da_grass_brushes(const Fvector& feet, float radius, float min_height, float below_feet, float above_feet);
// The ground tint of the terrain (binders of deffer_impl_flat.ps): world xz -> $user$grass_cover uv,
// strength and fade, where the blades' basal pairs fade out (tint2.xy), the canopy colour of the 16
// communities at the day (w = grass_visible) and at the reference day of the latched terrain folder. False
// while no level has a cover map (tint off).
bool da_grass_ground_tint(Fvector4& cover_xf, Fvector4& tint, Fvector4& tint2, Fvector4 (&canopy)[16], Fvector4 (&canopy_ref)[16]);
// The tufts' window of the terrain ($user$grass_tufts, deffer_impl_flat.ps da_gtuft): world xz -> its wrapped uv, 1
// while it is valid, how far it reaches around the camera (m).
Fvector4 da_grass_tuft_window();
// r__grass_blades_stats (QA): lanes, groups, records per bin against the bin windows, memory.
void da_grass_stats();
// r__grass_species_pin <name|off> (QA): every lane of the blade grass grows that species (species.json ids).
void da_grass_pin_species(pcstr name);
// Renderer shutdown (CRender::destroy, before the Resources go): every GPU object and $user$grass_cover.
void da_grass_destroy();

// A reed bed the stock reed models stood for (da_reeds.cpp): its centre and radius, the bed under it, the water over
// that bed and the community that grows there.
struct DaReedBed
{
    float x{}, z{}, r{};
    float y_bed{}, depth{};
    s32 community{-1};
};
// The reed beds of the level (empty: none): the blades grow their communities there, rooted on the bed and standing
// out of the water by their own height. reload: a loaded grass is set up again with them at once (not while the level
// goes).
void da_grass_set_reed_beds(xr_vector<DaReedBed>&& beds, bool reload, float emerge);
} // namespace xray::render::RENDER_NAMESPACE
