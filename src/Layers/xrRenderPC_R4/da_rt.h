// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Ray tracing, the "RTX" category (docs/dead-air/dx12-features/dxr.md 3.2): the public surface the
// frame calls at the points of PLAN.md 2.5. Never part of a quality preset. The R4 module compiles
// every function as "off"; R5 (DirectX 12) and R6 (Vulkan) trace.
#pragma once

// The modules whose device traces rays (the RT lane): DirectX 12 (xrD3D12) and Vulkan (xrVulkan).
#if defined(XR_D3D12) || defined(XR_VULKAN)
#define DA_RT_LANE 1
#endif

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class CRenderTarget;

enum class RtEffect : u32
{
    Reflections,
    Shadows,
    AO,
    Count
};

// RT usable this session: tier >= 1.1, DXC up, RT device init ok, no MSAA, no -dx12_no_rt. Independent
// of the effect settings, so the menu never locks a player out of turning one on.
bool da_rt_available();

// Renderer lifetime (r2.cpp).
void da_rt_device_create();
void da_rt_device_destroy();
void da_rt_reset_begin();
void da_rt_reset_end();
// The settings of this frame, latched before da_temporal_frame_begin (which asks da_rt_wants_motion):
// cascades, dispatches, shader constants and the motion vectors read the latch, so a toggle never
// leaves a frame with shadows from both systems or neither.
void da_rt_frame_begin();
// An effect runs this frame (latched).
bool da_rt_live(RtEffect effect);
// This frame's AO slot runs RTAO (latched in da_rt_frame_begin, before async fork A): the async
// path records no GTAO job then (PLAN.md C18).
bool da_rt_ao_live();
// r__rt_quality of this frame: 0 low, 1 medium, 2 high.
u32 da_rt_quality();
// The `da_rt` shader constant: x reflections, y sun shadows, z AO live this frame (0/1), w quality.
// Zeros on DX11.
Fvector4 da_rt_constants();

// Level lifetime (r2_loader.cpp): from the end of level_Load the visuals may be walked - there when an
// effect is on, else on the first frame that wants one (a level played without RT is never walked);
// freed first on unload.
void da_rt_level_load();
void da_rt_level_unload();

// Textures were reloaded with new views (CResourceManager::SyncTextureSeason, main thread between
// frames): the scene's bindless tables move to the new views now, while the replaced ones are still
// alive, so no slot holds a released view.
void da_rt_textures_replaced();

// The frame (r2_R_render.cpp, phase_combine).
void da_rt_frame_scene(CBackend& cmd_list); // before async fork A: acceleration structures
void da_rt_sun_pass(CBackend& cmd_list);    // before the sun accumulation
bool da_rt_ao_pass(CBackend& cmd_list);     // the AO slot: true = RTAO ran instead of GTAO/SSAO
void da_rt_reflections_pass(CBackend& cmd_list); // after the scene grab, before the puddles

// What the rest of the frame has to do for it.
bool da_rt_wants_scene_grab();
bool da_rt_skip_static_casters(); // the near and middle cascades leave static casters to the rays
bool da_rt_wants_motion();
bool da_rt_needs_ao_targets();

// With the render targets (after the vrs targets) and before they go.
void da_rt_targets_create(CRenderTarget& target);
void da_rt_targets_destroy(CRenderTarget& target);
} // namespace xray::render::RENDER_NAMESPACE
