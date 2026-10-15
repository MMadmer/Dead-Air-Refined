// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The automatic choice of the GPU offloads (docs/dead-air/dx12-features/gpudriven.md 3.13): the
// GPU-driven trees (r__gpu_driven) and mesh grass (r__grass_mesh) move CPU work to the GPU, so at -1
// (auto, the default) they run only while the CPU is the frame's bottleneck. DirectX 12 only: on R4
// auto resolves to 0 and nothing is measured.
#pragma once

#include "xrEngine/Render.h"

namespace xray::render::RENDER_NAMESPACE
{
// The switches' auto value.
constexpr int kOffloadAuto = -1;

// A switch's value for this frame: a forced value as it is, auto as the last decision (1 GPU path, 0 CPU).
int da_offload_resolve(int setting);
// Auto can pick the GPU paths this session (the GPU frame timers exist).
bool da_offload_auto_possible();
// A level was loaded: its measurements start over.
void da_offload_level_load();

// The main thread's frame timing: the engine's markers, the CPU/GPU sync point's wait (QPC ticks), and
// the end of the frame's work (OwnFrameLimit, before the engine's limiter), where the decision is made.
void da_offload_frame_marker(IRender::FrameMarker marker);
void da_offload_sync_wait(u64 ticks);
void da_offload_frame_done();
} // namespace xray::render::RENDER_NAMESPACE
