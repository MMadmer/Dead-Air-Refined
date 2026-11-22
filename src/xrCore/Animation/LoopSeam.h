// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "SkeletonMotions.hpp"

// Seamless loops for every cycled motion, as it is read (motions_value::load), from any omf: the
// game's, a mod's, a module's. A cycle runs over count-1 frames and shows the last key at the very
// instant it turns back to the first (CBlend::update_time), so the last key has to be the first
// one. A clip whose end does not run into its start (a step or more short of it, a plateau, an
// overshoot) jumps or kinks there each turn: its seam is rebuilt. Each channel's own motion is
// learnt from the clip (Burg linear prediction, a local parabola when that predicts worse),
// carried forward from the end and backward from the start, and the two are joined with continuous
// speed and acceleration over a window around the seam; a light smoothing then brings the window's
// curvature to the clip's. The motion's frame count and length never change: only key values in
// the window do, so marks, synced parts and anything timed by the clip stay where they were.
// Only what plays as a loop (no stop-at-end, no fx, no root mover, a module's description of the
// model included), only a clip whose end comes back near its start, only channels that come back
// themselves, move on both sides of the seam (a one-shot marked as a cycle rests at one end) and
// kink there harder than the clip itself does. -anim_raw_loops turns it off, -anim_loop_log names
// every motion changed.
namespace anim_loop
{
// A bone track of one motion as the file stores it: the keys stay in the reader's buffer.
struct RawTrack
{
    u8 flags{};
    const CKeyQR* r{}; // 1 key when flRKeyAbsent, else count
    const void* t{}; // CKeyQT16 or CKeyQT8 when flTKeyPresent, count of them
    Fvector size_t_{}, init_t{};
};

// What to create instead: count keys of a channel that was rebuilt, or nothing when the raw ones
// stand.
struct BuiltTrack
{
    bool rot{}, pos{};
    xr_vector<CKeyQR> r;
    xr_vector<CKeyQT16> t;
    Fvector size_t_{}, init_t{};
};

struct Result
{
    u32 rebuilt{}; // channels whose seam was rebuilt
    u32 resting{}; // kinked channels left alone: at rest on one side of the seam
    float worst_before{}, worst_after{}; // the worst seam kink, in multiples of the clip's own
};

XRCORE_API bool Enabled();
// tracks: the motion's bone tracks in data order; built gets one entry per track. def_flags are
// the flags the motion plays with (the file's, with a module's description applied).
XRCORE_API Result Close(const xr_vector<RawTrack>& tracks, u32 count, u16 def_flags, xr_vector<BuiltTrack>& built);
} // namespace anim_loop
