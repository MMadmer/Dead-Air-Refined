// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Async compute on DirectX 12 (docs/dead-air/dx12-features/async.md): GTAO and the ripple solver as
// compute jobs that overlap the shadows and the G-buffer; the cloud march stays the pixel pass with its
// early-out (PLAN.md C14, measured in the final test phase). The frame calls these at the points of
// PLAN.md 2.5; the R4 module always takes the pixel path.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class CRenderTarget;

// The render-size scene depth under a fixed name, for the cloud march's early-out on the DirectX 12
// pixel path (da_clouds_march.s binds it).
inline constexpr pcstr da_cloud_depth_name = "$user$cloud_depth";

// The lane's render-target state behind CRenderTarget::m_da_async (PLAN.md 0.2).
struct DaAsyncTargets
{
    // da_cloud_depth_name pointed at the scene depth; empty under MSAA and on DirectX 11. Only with
    // it does the march skip hidden texels and write sentinels (da_clouds_hist.h).
    ref_texture cloud_depth;
};
// The outputs r__async_verify compares, in the order of the compare shader's elements.
inline constexpr pcstr da_async_verify_outputs[] = { "gtao_0", "ssao_temp", "ripple" };
// One frame's ripple solve as the CPU prepared it, for either path
// (r4_rendertarget_phase_water_ripple.cpp, CRenderTarget::phase_water_ripple_step).
struct DaRippleStep
{
    int steps{}; // fixed 30 Hz steps this frame, 1-3
    u32 texels{}; // across the window
    float win{}; // metres across the window
    float texel_m{};
    float shift_u{}, shift_v{}; // the window's move since the last step, in uv (first step only)
    float rain_p{}, rain_amp{}; // a texel's chance of a drop per step, and the crater's depth
    float depth{}; // the water body's mean depth
    float speed{}, edge{}; // the ring clock's speed and the absorbing rim, from the environment
};
// End of the CRenderTarget constructor / start of its destructor (r2_rendertarget.cpp).
void da_async_targets_create(CRenderTarget& target);
void da_async_targets_destroy(CRenderTarget& target);
// The ripple takes a step this frame: the job path records no job without it.
bool da_water_ripple_due(const CRenderTarget& target);
// A job that could not be made after its pass had advanced the CPU side: the ripple publishes nothing
// and keeps the window its field holds.
void da_water_ripple_lost(CRenderTarget& target);
// The compute shaders ship with the content: an install whose shaders are older than the binaries
// has none, and compiling a missing file is fatal. Without them the pixel passes stay.
bool da_async_shader_exists(pcstr name);

// Top of CRender::Render, after r_main.sync(): this frame's path.
void da_async_frame_begin();
// Fork A (after the RT scene build): the cloud map on the direct queue and the job "water_ripple".
// true = the frame skips its own ripple, cloud map and march (the march, and a pass without its
// compute version, run as pixel passes at fork B).
bool da_async_fork_early(CBackend& cmd_list);
// Fork B, after r_rain.sync(): the pixel march with its early-out, then the job "gtao" once the
// G-buffer is final.
void da_async_fork_gtao(CBackend& cmd_list);
// Head of phase_combine: joins this frame's jobs and publishes the ripple step.
void da_async_join(CBackend& cmd_list);
// The AO slot of phase_combine: true = GTAO already ran as a job this frame.
bool da_async_ao_done();
// 0 pixel, 1 compute inline, 2 compute async (the r__gpu_log "async" field).
int da_async_path_code();
// r__async_compute st_async_auto on this device (async.md 3.2), decided when the targets are made.
bool da_async_auto_recommended();
// r__async_verify: the next frame that has all three passes compares pixel and compute results.
void da_async_verify_request();
// r__async_verify's stages, called by the passes themselves (they own the state a second run has
// to put back). Port: the pixel run's output is kept under a name, the compute run's output is
// compared with it on the GPU and the result is logged at the next frame; restore copies a kept
// texture back. Schedule: the compute run on the immediate context is kept, and expect() has the
// join compare it with the job's result. Outside a verify frame the passes never call these.
bool da_async_verify_schedule();
// The name the ripple's job step is kept under in the schedule stage.
inline constexpr pcstr da_async_verify_ripple_job = "ripple_job";
void da_async_verify_keep(CBackend& cmd_list, pcstr name, ID3DTexture2D* source);
// The copy keep() made under that name (null without one).
ID3DTexture2D* da_async_verify_kept(pcstr name);
void da_async_verify_expect(pcstr name, ID3DTexture2D* live);
void da_async_verify_restore(CBackend& cmd_list, pcstr name, ID3DTexture2D* target);
void da_async_verify_compare(CBackend& cmd_list, pcstr name, ID3DTexture2D* live);
} // namespace xray::render::RENDER_NAMESPACE
