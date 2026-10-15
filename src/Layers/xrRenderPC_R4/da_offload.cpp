// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The offloads' auto choice (da_offload.h, gpudriven.md 3.13).
//
// CPU side: the main thread's work of a frame, from the engine's SimulationStart marker to OwnFrameLimit
// (the parallel sequence joined, the engine's limiter not yet waited), minus its waits on the GPU - the
// CPU/GPU sync point and Present (the swap chain and the layer's frame fence). GPU side: the Frame slot
// of the GPU timers, bounded by that frame (a CPU-bound frame's timestamps may span a submission gap).
// Both are smoothed over r__offload_smooth seconds; the first r__offload_warmup seconds of a level (its
// streaming and first updates keep the main thread busy for a while) are not measured.
//
// When one side exceeds the other by r__offload_margin for r__offload_agree seconds, and the last switch
// is r__offload_dwell seconds old, the frame switches: CPU-bound -> the GPU paths, GPU-bound -> the CPU
// paths. A switch is a trial: r__offload_dwell seconds later the frame must be shorter than before it,
// or the path it left comes back ("no gain" - mesh grass, for one, draws on the render thread what the
// vertex path draws on a worker). A path left less than r__offload_recall seconds ago is taken back by
// the rule only if its frame was shorter by the margin, so a switch that only moved the bottleneck is
// not undone; once, r__offload_recall seconds after a kept switch, the path left is tried again
// ("recheck"), since the frame the switch was judged against may have been a passing load.
// The decision is made at the end of a frame and read by the next one (the grass latch in
// OnCameraUpdated, the trees' switch at the top of Calculate).

#include "stdafx.h"

#include "da_offload.h"

#ifdef XR_D3D12
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "xrEngine/IGame_Persistent.h"

#include <cmath>
#endif

namespace xray::render::RENDER_NAMESPACE
{
#ifdef XR_D3D12
namespace
{
enum Path : u32
{
    PathCpu,
    PathGpu,
};

// Main thread only.
struct Controller
{
    // The frame being timed: its start, an open Present, its waits on the GPU so far (QPC ticks).
    u64 start{}, present{}, waited{};
    bool rendered{};
    // The previous frame was measured: the time since it counts.
    u64 last_sample{};
    // Seconds of measured frames still to skip.
    float warmup{};
    // Smoothed, ms: the main thread's work, the GPU's frame, the frame the main thread saw.
    float cpu{}, gpu{}, frame{};
    bool primed{};

    Path path{PathCpu};
    bool announced{}, in_use{};
    // The last switch still has to show a frame shorter than `before`; a kept rule switch is rechecked.
    bool trial{}, recheck{};
    float before{};
    // Seconds since the last switch, and how long the other path has been wanted.
    float since_switch{}, agree{};
    // Each path's smoothed frame when it was last left, and the seconds since.
    float left_frame[2]{}, left_age[2]{};
    bool left_valid[2]{};
};
Controller g_auto;

bool auto_in_use() { return ps_r__gpu_driven == kOffloadAuto || ps_r__grass_mesh == kOffloadAuto; }

void announce(pcstr reason, bool timings, float before = 0.f)
{
    const pcstr path = g_auto.path == PathGpu ? "gpu" : "cpu";
    const bool trees = ps_r__gpu_driven == kOffloadAuto;
    const bool grass = ps_r__grass_mesh == kOffloadAuto;
    string128 what;
    xr_sprintf(what, "%s%s%s%s%s", trees ? "trees " : "", trees ? path : "", trees && grass ? ", " : "",
        grass ? "grass " : "", grass ? path : "");
    if (!timings)
        Msg("* [offload] auto: %s -> %s", reason, what);
    else if (before > 0.f)
        Msg("* [offload] auto: %s -> %s (cpu %.2f ms, gpu %.2f ms, frame %.2f ms against %.2f ms before)", reason,
            what, g_auto.cpu, g_auto.gpu, g_auto.frame, before);
    else
        Msg("* [offload] auto: %s -> %s (cpu %.2f ms, gpu %.2f ms, frame %.2f ms)", reason, what, g_auto.cpu,
            g_auto.gpu, g_auto.frame);
}

// Measuring starts over: a new level, or a switch set back to auto. Whatever was on trial is settled.
void restart_measuring()
{
    Controller& c = g_auto;
    c.warmup = ps_r__offload_warmup;
    c.primed = false;
    c.agree = 0.f;
    c.trial = c.recheck = false;
}

void switch_to(Path path, bool trial, bool recheck)
{
    Controller& c = g_auto;
    c.left_frame[c.path] = c.frame;
    c.left_age[c.path] = 0.f;
    c.left_valid[c.path] = true;
    c.before = c.frame;
    c.path = path;
    c.trial = trial;
    c.recheck = recheck;
    c.since_switch = 0.f;
    c.agree = 0.f;
}

void sample(u64 start, u64 waited, u64 now)
{
    Controller& c = g_auto;
    const double to_ms = 1000.0 / double(CPU::qpc_freq);
    const float dt = c.last_sample ? float(double(now - c.last_sample) * to_ms * 0.001) : 0.f;
    c.last_sample = now;
    if (c.warmup > 0.f)
    {
        c.warmup -= dt;
        return;
    }

    const u64 span = now - start;
    const float frame = float(double(span) * to_ms);
    const float cpu = float(double(span - std::min(waited, span)) * to_ms);
    const float gpu = std::min(GpuTimers.ms(dx11GpuTimers::Frame), frame);
    // Time-weighted smoothing; a long frame weighs at most one smoothing period.
    const float tau = ps_r__offload_smooth;
    const float k = c.primed ? 1.f - std::exp(-std::min(dt, tau) / tau) : 1.f;
    c.cpu += (cpu - c.cpu) * k;
    c.gpu += (gpu - c.gpu) * k;
    c.frame += (frame - c.frame) * k;
    c.primed = true;
    c.since_switch += dt;
    c.left_age[PathCpu] += dt;
    c.left_age[PathGpu] += dt;

    const Path other = c.path == PathGpu ? PathCpu : PathGpu;
    if (c.trial && c.since_switch >= ps_r__offload_dwell)
    {
        c.trial = false;
        if (c.frame >= c.before)
        {
            const float before = c.before;
            switch_to(other, false, false);
            announce("no gain", true, before);
            return;
        }
    }
    if (c.recheck && !c.trial && c.since_switch >= ps_r__offload_recall)
    {
        switch_to(other, true, false);
        announce("recheck", true);
        return;
    }

    const float margin = 1.f + ps_r__offload_margin;
    // CPU-bound: the GPU paths take work off the CPU; GPU-bound: the CPU paths take it off the GPU.
    const Path want = c.cpu > c.gpu * margin ? PathGpu : c.gpu > c.cpu * margin ? PathCpu : c.path;
    if (want == c.path)
    {
        c.agree = 0.f;
        return;
    }
    c.agree += dt;
    if (c.agree < ps_r__offload_agree || c.since_switch < ps_r__offload_dwell)
        return;
    // A path left recently comes back only if its frame was shorter by the margin: the switch that left
    // it moved the bottleneck to the other side, and undoing it would only move it back.
    if (c.left_valid[want] && c.left_age[want] < ps_r__offload_recall && c.left_frame[want] * margin > c.frame)
        return;
    switch_to(want, true, true);
    announce(want == PathGpu ? "cpu-bound" : "gpu-bound", true);
}
} // namespace
#endif

int da_offload_resolve(int setting)
{
    if (setting != kOffloadAuto)
        return setting;
#ifdef XR_D3D12
    return g_auto.path == PathGpu ? 1 : 0;
#else
    return 0;
#endif
}

bool da_offload_auto_possible()
{
#ifdef XR_D3D12
    return GpuTimers.enabled();
#else
    return false;
#endif
}

void da_offload_level_load()
{
#ifdef XR_D3D12
    // The path stays (the bottleneck is mostly the machine and the settings); what was measured goes.
    Controller& c = g_auto;
    c.last_sample = 0;
    c.since_switch = ps_r__offload_dwell;
    c.left_valid[PathCpu] = c.left_valid[PathGpu] = false;
    restart_measuring();
#endif
}

void da_offload_frame_marker(IRender::FrameMarker marker)
{
#ifdef XR_D3D12
    Controller& c = g_auto;
    switch (marker)
    {
    case IRender::FrameMarker::SimulationStart:
        c.start = CPU::QPC();
        c.present = 0;
        c.waited = 0;
        c.rendered = false;
        break;
    case IRender::FrameMarker::PresentStart:
        c.present = CPU::QPC();
        break;
    case IRender::FrameMarker::PresentEnd:
        if (c.present)
            c.waited += CPU::QPC() - c.present;
        c.present = 0;
        c.rendered = true;
        break;
    default:
        break;
    }
#else
    UNUSED(marker);
#endif
}

void da_offload_sync_wait(u64 ticks)
{
#ifdef XR_D3D12
    g_auto.waited += ticks;
#else
    UNUSED(ticks);
#endif
}

void da_offload_frame_done()
{
#ifdef XR_D3D12
    Controller& c = g_auto;
    const u64 now = CPU::QPC();
    const u64 start = std::exchange(c.start, 0);
    const u64 waited = std::exchange(c.waited, 0);
    const bool rendered = std::exchange(c.rendered, false);
    const auto skip = [&c] { c.last_sample = 0; };
    // A switch set back to -1 takes the last decision at once and measures from scratch.
    const bool in_use = auto_in_use();
    const bool resumed = in_use && !c.in_use && c.announced;
    c.in_use = in_use;
    if (!in_use)
        return skip();
    if (resumed)
    {
        restart_measuring();
        announce("resumed", false);
    }
    if (!GpuTimers.enabled())
    {
        // Nothing to compare with: the CPU paths.
        if (!c.announced || c.path != PathCpu)
        {
            c.path = PathCpu;
            c.announced = true;
            announce("no GPU timers", false);
        }
        return skip();
    }
    // Level frames the renderer drew past the loading screen's precache, with a resolved GPU frame.
    if (!start || start > now || !rendered || Device.dwPrecacheFrame || !GpuTimers.valid() || Device.Paused() ||
        !g_pGamePersistent || g_pGamePersistent->MainMenuActiveOrLevelNotExist())
        return skip();
    if (!c.announced)
    {
        c.announced = true;
        announce("start", false);
    }
    sample(start, waited, now);
    // r__gpu_log: the smoothed times the decisions see, on the [gpu] line's cadence.
    if (ps_r__gpu_log > 0 && Device.dwFrame % u32(ps_r__gpu_log) == 0)
    {
        Msg("* [offload] path=%s cpu=%.3f gpu=%.3f frame=%.3f ms warmup=%.1f s", c.path == PathGpu ? "gpu" : "cpu",
            c.cpu, c.gpu, c.frame, std::max(c.warmup, 0.f));
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
