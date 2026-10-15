// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The vegetation core (docs/dead-air/TREES.md, SHRUBS.md): one module for the procedural shrubs (da_shrubs.cpp) and
// the upgraded level trees (da_trees.cpp). The stock models they replace leave the sector hierarchies at level load;
// every frame the core walks the populations' instances against the view, the sun cascades, the rain map and the
// shadowed local lights, emits their chunks and draws them - the hooks below are the renderer's only contact with it.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class dxRender_Visual;

// The shaders ship with the content: the shrubs can run.
bool da_veg_available();
// A population stands on this level (its data names stock models, its switch is on).
bool da_veg_active();
// xrRender_sync_preset_derived: the columns of preset 0..4 (minimum..extreme), from the next frame.
void da_veg_apply_preset(u32 preset);
// level_Load after LoadOverlayVisuals and before the GPU-driven registry: the census of the stock models, both
// populations' placements and templates, the replaced models out of their hierarchies. level_Unload before the
// details; renderer destruction.
void da_veg_level_load();
void da_veg_level_unload();
void da_veg_destroy();
// Top of CRender::Calculate (main thread, before any view walks the graph): a changed r__shrubs or r__trees takes the
// stock models out of their hierarchies or puts them back (the GPU-driven registry and the ray tracing scene rebuilt).
void da_veg_apply_switch();
// CRender::Calculate, main thread, immediate context, after the occlusion task is joined and before r_sun.run() (its
// task draws the cascades on a worker at once): the season tables, the frame's instances in the view, the sun cascades
// and the rain map, their records and chunks uploaded.
void da_veg_frame_prepare(CBackend& cmd);
// CRender::Render, immediate context, before the shadow maps' lists run and before the G-buffer: the populations' work
// before the frame's draws (the trees' billboard atlas filled), then the frame's bends (r3\da_shrub_bend.cs) into the
// buffer da_veg_frame_prepare sized.
void da_veg_bend(CBackend& cmd);
// The G-buffer, after the blade grass (r2_R_render.cpp): the shrubs, the trees, the trees' far billboards.
void da_veg_render_gbuffer(CBackend& cmd);
// Sun cascade `cascade`: the vegetation casts into it this frame (a cascade with no other caster still opens its map
// for it); its draws, at the details site of render_phase_sun.cpp.
bool da_veg_casts(u32 cascade);
void da_veg_render_cascade(CBackend& cmd, u32 cascade);
// The rain's occlusion map (r3_R_rain.cpp flush, the map bound with the rain's transform): the crowns shelter the
// ground under them as the stock models did.
bool da_veg_casts_rain();
void da_veg_render_rain(CBackend& cmd);
// The shadowed local lights of the frame (r2_R_lights.cpp, their transforms computed, before any is drawn): the
// instances in their reach build their chunks; then each light's shadow map draws them.
struct DaVegLight
{
    const void* key{};
    Fvector position{}, direction{};
    float range{}, cone{};
    u32 smap_size{};
    Fmatrix combine{};
};
void da_veg_lights_prepare(CBackend& cmd, const DaVegLight* lights, u32 count);
bool da_veg_light_casts(const void* light);
void da_veg_render_light(CBackend& cmd, const void* light);
// The stock models standing replaced (their FLODs, out of the hierarchies): the ray tracing scene's stand-ins for the
// vegetation, which lives in the frame's buffers and in no acceleration structure (da_rt_scene.cpp).
void da_veg_replaced_visuals(xr_vector<dxRender_Visual*>& out);
// What a stand-in of the ray tracing scene (a replaced FLOD or one of its parts) stands for: its population (0 the
// shrubs, 1 the trees), its species' row in the vegetation tables (r3\da_shrub_common.h shrub_prof, shrub_seas), its
// template, and whether its stock cards keep their look all year (a dead plant, a Red Forest pine with its red
// needles) - the season-aware stand-ins of da_rt_scene.cpp (their cards thinned by the day's leaf share, coloured by
// the table).
struct DaVegStandIn
{
    u32 population{}, species{}, tmpl{};
    bool dead{};
};
bool da_veg_stand_in(const dxRender_Visual* visual, DaVegStandIn& out);
// The vegetation tables for those stand-ins (DirectX 12): the buffer of the species' profile and season rows
// (rewritten when the day moves), a species' profile rows, the season block's first float4 and a species' season
// rows, the snow on the branches now; false while no population stands.
bool da_veg_rt_tables(ID3D11Buffer*& buffer, u32& prof_f4, u32& seas_first, u32& seas_f4, float& snow);
} // namespace xray::render::RENDER_NAMESPACE
