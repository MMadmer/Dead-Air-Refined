// Copyright (c) 2026 Grizzly
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
        Frame, // CRender::Render from the CPU/GPU sync point on
        Scene, // G-buffer fill: geometry, trees, grass
        Shadows, // sun shadow cascades (smap rendering)
        Sun, // sun accumulation, cloud shadows included
        Lights, // local lights
        Clouds, // the cloud cache retrace and the sky/cloud composite
        CloudsMarch, // the volumetric march at half resolution
        Combine, // combine + post-processing + AA
        // Parts of Combine
        CombineAO, // ambient occlusion (GTAO/HDAO/SSAO)
        CombineForward, // forward geometry: water, particles, sorted, volumetric light
        CombineBloom,
        CombineAA, // SMAA/FXAA/TAA
        CombinePost, // sun shafts, upscale, the final combine, flares, pp
        // DirectX 12 features (docs/dead-air/dx12-features/PLAN.md 2.6): each is bracketed where
        // the feature's work is recorded, the ones of today's passes by the frame itself.
        Ripple, // the water ripple steps on the direct queue (pixel path)
        JoinWait, // the direct queue waiting for the compute jobs at the join
        VrsRate, // the VRS rate image
        VrsLuma, // the VRS luminance contrast
        GdCull, // GPU-driven trees: main-view cull, args and scatter
        Grass, // the G-buffer grass, parallel lists included
        GrassGen, // blade grass compute: generation, args, ground window, push field
        SnowPrep, // the ground window and the snow depth (stamps, refill, composite)
        SnowGrid, // the near snow grid in the G-buffer
        SnowLayer, // the full-screen snow layer over the G-buffer
        Shrubs, // the procedural shrubs in the G-buffer
        ShrubsBend, // the procedural shrubs' bends of the frame (compute)
        Trees, // the upgraded trees in the G-buffer
        TreesBend, // the upgraded trees' bends of the frame (compute)
        TreesBb, // the upgraded trees' far billboards in the G-buffer
        TreesBake, // the billboards' runtime bake (the trees' atlas, the stock path's level_lods)
        RtScene, // ray tracing: acceleration structure builds
        RtSun, // ray traced sun shadows
        RtAO, // ray traced ambient occlusion
        RtRefl, // ray traced reflections
        RtGloss, // ray traced reflections of glossy surfaces (trace and temporal)
        Fire, // the fluid fires' volumes drawn, every one of the frame
        FireSim, // the fluid fires' simulation steps, every grid of the frame
        Count
    };

    void OnDeviceCreate();
    void OnDeviceDestroy();

    // Call once per frame around the whole render, on the immediate context.
    void FrameBegin();
    void FrameEnd();

    // A slot bracketed several times in a frame (a population's view and shadow bends, the lights' after them) sums
    // its brackets, Pairs of them at the most.
    void Begin(Slot slot);
    void End(Slot slot);

    // Last resolved frame. valid() is false until the first readback lands and whenever the
    // clock was disjoint (power state change, overclock).
    bool valid() const { return m_valid; }
    float ms(Slot slot) const { return m_ms[slot]; }
    static pcstr name(Slot slot);

    bool enabled() const { return m_enabled; }

    // A field of the r__gpu_log line that is not a direct-queue slot (compute-queue job times, the
    // async path): registered once by its feature, printed as key=%.3f after the slot fields.
    using FieldValue = float (*)();
    static void RegisterField(pcstr key, FieldValue value);

    // The "[gpu]" line of r__gpu_log; nothing else writes it. Slot values are the mean of every frame
    // resolved since the previous line (one frame alone varies by 5-10 %).
    void LogLine() const;

private:
    static constexpr u32 RingSize = 4, Pairs = 8;

    struct FrameQueries
    {
        ID3D11Query* disjoint{};
        ID3D11Query* begin[Count][Pairs]{};
        ID3D11Query* end[Count][Pairs]{};
        u8 issued[Count]{};
        bool pending{};
    };

    // Resolve the oldest frame if its results are ready; returns without waiting otherwise.
    void Resolve(FrameQueries& f);

    FrameQueries m_ring[RingSize];
    u32 m_write{};
    float m_ms[Count]{};
    // Sums since the last log line; the log line consumes them.
    mutable double m_sum[Count]{};
    mutable u32 m_sum_frames{};
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
