// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The level's collision mesh on the GPU and the top-down products rasterised from it (docs/dead-air/SNOW.md):
// the near window of the topmost non-passable surface (Htop) that the snow sits on. The grass rasterises
// its own ground (the stock slot-box rule) from the same upload.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

// The collision triangles of the level as raw SRVs: verts = float3 per vertex (12 B), tris = uint4 per
// triangle (i0, i1, i2, flags) with flags bits 0-15 the GMLib material index, bit 16 passable, bit 17 water
// (liquid and passable), bit 18 road (asphalt / gravel / concrete, rails excluded).
struct DaGroundCdb
{
    ID3D11ShaderResourceView* verts = nullptr;
    ID3D11ShaderResourceView* tris = nullptr;
    u32 tri_count = 0;
    Fbox bounds{};
};

// r2_loader: after the level's geometry and CDB exist / before they go.
void da_ground_level_load();
void da_ground_level_unload();
// Immediate context, once per frame before the snow and grass passes: re-rasterises the window strips the
// camera moved into; nothing while the snow has nothing to draw (da_snow_depth_active), the whole window on
// the frame it comes back.
void da_ground_frame_prepare(CBackend& cmd_list);
// False while there is no level upload.
bool da_ground_cdb(DaGroundCdb& out);
// The near Htop window (world y of the topmost non-passable surface, R32F, toroidal), never null while a
// level is loaded; uv = frac(world.xz * xf.xy + xf.zw).
ID3D11ShaderResourceView* da_ground_top_srv();
Fvector4 da_ground_top_xf();

// ---- additions of the snow stage (da_ground.cpp) ------------------------------------------------------
// The same window's snow base, $user$da_ground_base (RGBA32F, toroidal): x = the base, Htop opened with a 3 x 3
// square (posts, rails and thin props removed; world y, -10000 where there is no surface; the water surface
// where one lies over it), or the floor under a cover (a roof, a deck, a vehicle: more than 0.35 m over the
// texel's lowest floor and 0.6 m over the level's terrain); y = cliff taper 0..1 (0 on a cliff texel, 1 half a
// metre from it; -1 - taper under a water surface); z = the shelter of a cover; w = the opened Htop itself. Htop is $user$da_ground_top. The waterline's continuous quantities are $user$da_ground_shore
// (RGBA32F, toroidal): x the opened solid surface under the water too, y the highest water surface within 8 texels
// (-10000: none), z how far inside that reach the texel lies (1 up to half of it, 0 at its end).
ID3D11ShaderResourceView* da_ground_base_srv();
// QA: the window's texels along a line, read back on the next frame (r__snow_window_probe x z dx dz count).
void da_ground_window_probe(pcstr args);
// Constants of the window for the snow shaders: win = (centre x, centre z, half-size the window holds
// valid data for, blend band to the level base), win2 = (texels per side, texels per metre, 0, 0).
// False while there is no window.
bool da_ground_window(Fvector4& win, Fvector4& win2);
// The window size of preset 0..4 (minimum..extreme); applied at the next frame. da_snow_apply_preset
// forwards here.
void da_ground_apply_preset(u32 preset);
// Renderer shutdown (CRender::destroy): every GPU object and named texture goes. da_snow_destroy calls it.
void da_ground_destroy();
// The snow shells (the snow on roofs, decks, vehicles, crates): built from the collision's faces that look up
// once per bake (da_snow.cpp calls it with the level's fields uploaded), drawn by chunks in the view and the
// reach (the caller has set the shell pass and its constants). Returns the chunks drawn.
void da_ground_shells_sync();
u32 da_ground_shells_render(CBackend& cmd_list, float reach);
} // namespace xray::render::RENDER_NAMESPACE
