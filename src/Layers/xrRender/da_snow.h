// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Volumetric seasonal snow (docs/dead-air/SNOW.md): the level fields on the GPU, the trail maps stamped by
// every creature, the near snow grid in the G-buffer and the full-screen snow layer over every opaque
// surface. Reads xrEngine's da_snow_field.h; shaders sample the snow through r3\da_snow_sample.h.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

void da_snow_level_load();   // after da_ground_level_load()
void da_snow_level_unload(); // before da_ground_level_unload()
// Any snow this frame: the level-mean depth of da_snow_frame() is above 0 (0 skips every snow pass; the
// world reports a small positive mean on the onset and melt days a class cover key is above 0).
bool da_snow_active();
// Main thread, once per frame before Calculate() (CRender::OnCameraUpdated): the field upload, the passes
// of the frame, the trail windows' places and every constant the binders hand to the passes (the sun
// cascades, recorded on worker threads, read them).
void da_snow_frame_publish();
// Immediate context, after da_ground_frame_prepare() and before da_grass_frame_prepare(): the windows'
// scroll, stamps, refill, composite of the current snow depth.
void da_snow_frame_prepare(CBackend& cmd_list);
// G-buffer phase, after the details/grass and before phase_scene_end(): the near snow grid.
void da_snow_render_grid(CBackend& cmd_list);
// After the rain apply (the r_rain.sync() slot): the snow layer rewrites the packed G-buffer.
void da_snow_apply_layer(CBackend& cmd_list);
// xrRender_sync_preset_derived: the snow columns of preset 0..4 (minimum..extreme).
void da_snow_apply_preset(u32 preset);
// The constants of r3\da_snow_sample.h (bound by the standard binders): the fine window and the level-wide
// texture of the current snow depth. The textures are $user$da_snow_fine and $user$da_snow_level.
Fvector4 da_snow_fine_xf();
Fvector4 da_snow_level_xf();
float da_snow_cap();

// ---- additions of the snow stage (da_snow.cpp) --------------------------------------------------------
// da_snow_misc of r3\da_snow_sample.h as a whole: x cap (m), y fine window half-size (m), z 1 while the depth
// textures hold snow, w width of the fine-to-level blend (m). The binder sets this vector.
Fvector4 da_snow_misc();
// True while the depth textures hold snow (level-mean depth > 0): da_snow_misc().z.
bool da_snow_depth_active();
// Renderer shutdown (CRender::destroy, before the render target goes): every GPU object and named texture
// of the snow and of da_ground.cpp.
void da_snow_destroy();
// QA diagnostic (r__snow_stats): the state of the snow passes in the log.
void da_snow_stats();
// The level's terrain texture ($user$da_snow_ground, R16_UNORM: y = lvl.x + v * lvl.y, 0 = none) and its texel
// transform (texel = xz * xf.xy + xf.zw, centres at +0.5): da_ground's cover test. Returns the upload's version,
// 0 while there is none.
u32 da_snow_level_terrain(Fvector4& xf, Fvector4& lvl);
// The snow grid in the sun cascades (render_phase_sun.cpp, worker threads): the two near ones, so the walls of
// a trail and a drift's lee cast shadows; `vp` the cascade's world to clip transform (its tiles culled by it).
bool da_snow_casts(u32 cascade);
void da_snow_render_cascade(CBackend& cmd_list, u32 cascade, const Fmatrix& vp);
} // namespace xray::render::RENDER_NAMESPACE
