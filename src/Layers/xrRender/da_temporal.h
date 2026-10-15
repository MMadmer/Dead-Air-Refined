// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Temporal state shared by the renderer: whether TAA or an upscaler runs this frame, the
// projection jitter, history resets. See docs/dead-air/DX12_RENDERER.md, section 3.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// TAA or an upscaler runs this frame: motion vectors are rendered and the projection is jittered.
bool da_temporal_active();
// An upscaler (FSR or DLSS) runs this frame; the render size may differ from the display size.
bool da_upscaler_active();
// The motion vectors are rendered this frame: something temporal runs, or frame generation reads
// them (then the projection stays unjittered). The velocity pass keys on this; TAA and the
// upscalers on da_temporal_active().
bool da_motion_vectors_active();
// This frame's jitter in render pixels, each axis in [-0.5, 0.5] (0 when nothing temporal runs).
Fvector2 da_jitter_px();
// The same offset in clip-space units, as added to the projection (x: +2*px/W, y: -2*py/H).
Fvector2 da_jitter_clip();
// Temporal history (TAA, upscalers, motion vectors of the previous frame) must be discarded.
bool da_history_reset();
// Requests a history reset for the next frame (level load, teleport, camera cut, size change).
void da_history_request_reset(pcstr why);
// A frame that took this frame's reset and then rendered nothing (the first one after a renderer
// reset) hands it on to the next frame.
void da_history_carry_reset();
// Motion vectors (shaders/r3/da_velocity.h): the main view's view-projection without the jitter,
// this frame's (Device.mFullTransform) and the previous frame's - the same matrix after a history
// reset. Every context gets the pair in SetCacheXform (R_xforms::set_VP_motion).
const Fmatrix& da_vp_nojit();
const Fmatrix& da_vp_prev();
// The pair for the HUD, from its unjittered projection: the previous one is what the HUD was
// drawn with on the previous frame (this frame's after a gap or a reset). hud_transform_helper.
void da_hud_vp(const Fmatrix& hud_project, Fmatrix& vp_nojit, Fmatrix& vp_prev);

// ---- the frame's side of it (da_temporal.cpp) ----------------------------------------------------

// Once per frame, before the view is handed to the contexts (CRender::OnCameraUpdated): the
// jitter phase, camera cuts, the menu -> game transition, pending history resets.
void da_temporal_frame_begin();
// At the end of the frame's rendering (CRender::Render): this frame's view-projections become
// the next frame's previous ones.
void da_temporal_frame_end();
// P with this frame's jitter added to its clip-space offset (_31/_32). P itself, bit for bit,
// when nothing temporal runs. Main view, HUD and light volumes only: shadow, rain and HUD-shadow
// projections and Device.mProject stay unjittered.
Fmatrix da_jitter_project(const Fmatrix& P);
// The deferred position reconstruction constants of a projection with a vertical field of view
// of fov_deg degrees at render size, (HorzTan, VertTan, 2*HorzTan/W, 2*VertTan/H), with this
// frame's jitter folded into xy: pos2d * zw - xy then lands on the unjittered view ray.
Fvector4 da_pos_decompress(float fov_deg);

// The size of the frame the current stage draws: the render size from the start of the 3D scene
// to the upscale, the display size everywhere else. screen_res follows it.
void da_stage_scene(bool scene);
u32 da_stage_width();
u32 da_stage_height();
} // namespace xray::render::RENDER_NAMESPACE
