// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// GPU pass timing for the DX11 renderer.
//
// Until this existed the engine had no way to say what a render pass costs on the GPU: every
// CStatTimer measures the CPU side of issuing the commands, which for a GPU-bound pass is
// nothing. Wind lives almost entirely in vertex shaders and the clouds in a raymarch, so
// without this every cost claim for either is a guess.
//
// One disjoint query per frame brackets a set of timestamp pairs. Results are read back three
// frames later without flushing, so the queries never stall the pipeline; the numbers shown
// are therefore a few frames old, which does not matter for a statistic.
//
// Issued from the immediate context only. The engine records parts of the scene on deferred
// contexts, and a timestamp recorded there would resolve at an unpredictable point of the
// executed command list.

namespace xray::render::RENDER_NAMESPACE
{
class dx11GpuTimers
{
public:
    enum Slot : u32
    {
        Frame, // the whole CRender::Render
        Scene, // G-buffer fill: geometry, trees, grass
        Shadows, // sun shadow cascades (smap rendering)
        Sun, // sun accumulation, cloud shadows included
        Lights, // local lights
        Clouds, // the cloud cache retrace and the sky/cloud composite
        CloudsMarch, // the volumetric march at half resolution
        Combine, // combine + post-processing + AA
        Count
    };

    void OnDeviceCreate();
    void OnDeviceDestroy();

    // Call once per frame around the whole render, on the immediate context.
    void FrameBegin();
    void FrameEnd();

    void Begin(Slot slot);
    void End(Slot slot);

    // Last resolved frame. valid() is false until the first readback lands and whenever the
    // clock was disjoint (power state change, overclock).
    bool valid() const { return m_valid; }
    float ms(Slot slot) const { return m_ms[slot]; }
    static pcstr name(Slot slot);

    bool enabled() const { return m_enabled; }

private:
    static constexpr u32 RingSize = 4;

    struct FrameQueries
    {
        ID3D11Query* disjoint{};
        ID3D11Query* begin[Count]{};
        ID3D11Query* end[Count]{};
        bool issued[Count]{};
        bool pending{};
    };

    // Resolve the oldest frame if its results are ready; returns without waiting otherwise.
    void Resolve(FrameQueries& f);

    FrameQueries m_ring[RingSize];
    u32 m_write{};
    float m_ms[Count]{};
    bool m_valid{};
    bool m_enabled{};
    bool m_in_frame{};
};

extern dx11GpuTimers GpuTimers;

// Scoped bracket; a no-op when the timers could not be created.
class dx11GpuTimerScope
{
    dx11GpuTimers::Slot m_slot;

public:
    explicit dx11GpuTimerScope(dx11GpuTimers::Slot slot) : m_slot(slot) { GpuTimers.Begin(slot); }
    ~dx11GpuTimerScope() { GpuTimers.End(m_slot); }
};
} // namespace xray::render::RENDER_NAMESPACE
