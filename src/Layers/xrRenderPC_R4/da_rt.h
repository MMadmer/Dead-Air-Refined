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

class IRenderable;

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class CRenderTarget;
class light;

enum class RtEffect : u32
{
    Reflections,
    Shadows,
    AO,
    GI, // diffuse global illumination
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
// The `da_rt_gi` shader constant: x the traced global illumination this frame (0/1), y the share of the stock ambient's
// shortfall of sky it takes away, z the least share it leaves, w the gain of its bounced light. Zeros on DX11.
Fvector4 da_rt_gi_constants();

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
void da_rt_frame_collect();                 // Calculate, once the main view's walk has posed the models: a worker's job
void da_rt_frame_scene(CBackend& cmd_list); // before async fork A: acceleration structures
void da_rt_sun_pass(CBackend& cmd_list);    // before the sun accumulation
bool da_rt_ao_pass(CBackend& cmd_list);     // the AO slot: true = RTAO ran instead of GTAO/SSAO
void da_rt_gloss_pass(CBackend& cmd_list);  // after the AO slot, before combine_1: glossy reflections
void da_rt_gi_pass(CBackend& cmd_list);     // after the AO slot, before combine_1: diffuse global illumination
// The glossy reflections' frame copy for combine_1's third target (the previous frame's linear colour their rays reuse);
// null when not wanted.
const ref_rt* da_rt_gloss_scene_target();
void da_rt_reflections_pass(CBackend& cmd_list); // after the scene grab, before the puddles

// Lamps (dxr.md 3.12, 3.15). The nearest lamps of the frame, shadowed and unshadowed, to the tier's count take their
// shadow from rays: the shadowed faces chosen leave `shadowed` for `traced` (a point lamp with all its faces), the
// unshadowed lamps chosen stay in `unshadowed` and light through the masks; a shadowed lamp whose map something else
// reads keeps it (a volumetric beam's, the procedural shrubs' that cast into it, a reach past the characters the scene
// holds). Every lamp lit this frame goes to the lights the rays' hits take. Then the visibility is traced into the
// masks the lamps' volumes read (r2_R_lights.cpp, before the lamps light).
void da_rt_lamps_select(xr_vector<light*>& shadowed, const xr_vector<light*>& unshadowed, xr_vector<light*>& traced, bool volumetric);
void da_rt_lamps_pass(CBackend& cmd_list);
// A lamp face's slot in this frame's masks (its owner's, for a point lamp's face), -1 for one with its map.
int da_rt_lamp_slot(const light* L);
// This frame's masks, one array (null where none or not traced).
ID3D11ShaderResourceView* da_rt_lamp_view();

// What the rest of the frame has to do for it.
bool da_rt_wants_scene_grab();
bool da_rt_skip_static_casters(); // the near and middle cascades leave static casters to the rays
// The moving objects (and the player's first-person shadow model) the sun's rays trace this frame: the near and middle
// cascades leave them out (chosen in da_rt_frame_collect, before the cascades are drawn).
bool da_rt_moving_wanted();
bool da_rt_traces_moving(IRenderable* root);
// A visual the last top level traced for its object (its own model or what it carries; da_rt_traces_moving's objects).
bool da_rt_traced_visual(IRenderVisual* visual);
bool da_rt_traces_actor();
bool da_rt_wants_motion();
bool da_rt_needs_ao_targets();

// With the render targets (after the vrs targets) and before they go.
void da_rt_targets_create(CRenderTarget& target);
void da_rt_targets_destroy(CRenderTarget& target);
} // namespace xray::render::RENDER_NAMESPACE
