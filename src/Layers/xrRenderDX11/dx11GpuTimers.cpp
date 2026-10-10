// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "dx11GpuTimers.h"

namespace xray::render::RENDER_NAMESPACE
{
dx11GpuTimers GpuTimers;

namespace
{
// Plain arrays: a feature may register from a static initialiser.
struct gpu_log_field
{
    pcstr key;
    dx11GpuTimers::FieldValue value;
};
constexpr size_t gpu_log_max_fields = 16;
gpu_log_field g_gpu_log_fields[gpu_log_max_fields];
size_t g_gpu_log_field_count = 0;
} // namespace

void dx11GpuTimers::RegisterField(pcstr key, FieldValue value)
{
    for (size_t i = 0; i < g_gpu_log_field_count; ++i)
    {
        if (!xr_strcmp(g_gpu_log_fields[i].key, key))
        {
            g_gpu_log_fields[i].value = value;
            return;
        }
    }
    R_ASSERT2(g_gpu_log_field_count < gpu_log_max_fields, "too many r__gpu_log fields");
    g_gpu_log_fields[g_gpu_log_field_count++] = { key, value };
}

void dx11GpuTimers::LogLine() const
{
    float mean[Count];
    for (u32 i = 0; i < Count; ++i)
        mean[i] = m_sum_frames ? float(m_sum[i] / m_sum_frames) : m_ms[i];
    const u32 frames = m_sum_frames;
    std::fill(std::begin(m_sum), std::end(m_sum), 0.0);
    m_sum_frames = 0;
    const auto ms = [&mean](Slot slot) { return mean[slot]; };

    // snprintf: a field that does not fit is cut, never a crash in the log line.
    string1024 extra;
    int length = std::snprintf(extra, sizeof(extra),
        " ripple=%.3f join=%.3f vrs=%.3f gd=%.3f grass=%.3f grass_gen=%.3f snow_prep=%.3f snow_grid=%.3f "
        "snow_layer=%.3f shrubs=%.3f shrubs_bend=%.3f trees=%.3f trees_bend=%.3f trees_bb=%.3f trees_bake=%.3f rt_scene=%.3f rt_sun=%.3f "
        "rt_ao=%.3f rt_refl=%.3f fire=%.3f fire_sim=%.3f",
        ms(Ripple), ms(JoinWait), ms(VrsRate) + ms(VrsLuma), ms(GdCull), ms(Grass), ms(GrassGen), ms(SnowPrep),
        ms(SnowGrid), ms(SnowLayer), ms(Shrubs), ms(ShrubsBend), ms(Trees), ms(TreesBend), ms(TreesBb), ms(TreesBake), ms(RtScene), ms(RtSun),
        ms(RtAO), ms(RtRefl), ms(Fire), ms(FireSim));
    for (size_t i = 0; i < g_gpu_log_field_count && length > 0 && size_t(length) < sizeof(extra); ++i)
    {
        const int added = std::snprintf(extra + length, sizeof(extra) - length, " %s=%.3f",
            g_gpu_log_fields[i].key, g_gpu_log_fields[i].value());
        if (added <= 0)
            break;
        length += added;
    }
    Msg("* [gpu] frame=%.3f scene=%.3f sun=%.3f lights=%.3f clouds=%.3f march=%.3f combine=%.3f ms "
        "(ao=%.3f forward=%.3f bloom=%.3f aa=%.3f post=%.3f)%s frames=%u",
        ms(Frame), ms(Scene), ms(Sun), ms(Lights), ms(Clouds), ms(CloudsMarch), ms(Combine), ms(CombineAO),
        ms(CombineForward), ms(CombineBloom), ms(CombineAA), ms(CombinePost), extra, frames);
}

pcstr dx11GpuTimers::name(Slot slot)
{
    switch (slot)
    {
    case Frame: return "frame";
    case Scene: return "scene";
    case Shadows: return "shadows";
    case Sun: return "sun";
    case Lights: return "lights";
    case Clouds: return "clouds";
    case CloudsMarch: return "clouds_march";
    case Combine: return "combine";
    case CombineAO: return "ao";
    case CombineForward: return "forward";
    case CombineBloom: return "bloom";
    case CombineAA: return "aa";
    case CombinePost: return "post";
    case Ripple: return "ripple";
    case JoinWait: return "join";
    case VrsRate: return "vrs_rate";
    case VrsLuma: return "vrs_luma";
    case GdCull: return "gd";
    case Grass: return "grass";
    case GrassGen: return "grass_gen";
    case SnowPrep: return "snow_prep";
    case SnowGrid: return "snow_grid";
    case SnowLayer: return "snow_layer";
    case Shrubs: return "shrubs";
    case ShrubsBend: return "shrubs_bend";
    case Trees: return "trees";
    case TreesBend: return "trees_bend";
    case TreesBb: return "trees_bb";
    case TreesBake: return "trees_bake";
    case RtScene: return "rt_scene";
    case RtSun: return "rt_sun";
    case RtAO: return "rt_ao";
    case RtRefl: return "rt_refl";
    case Fire: return "fire";
    case FireSim: return "fire_sim";
    default: return "?";
    }
}

void dx11GpuTimers::OnDeviceCreate()
{
    OnDeviceDestroy();
    if (!HW.pDevice)
        return;

    D3D11_QUERY_DESC disjoint{};
    disjoint.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC stamp{};
    stamp.Query = D3D11_QUERY_TIMESTAMP;

    bool ok = true;
    for (FrameQueries& f : m_ring)
    {
        ok = ok && SUCCEEDED(HW.pDevice->CreateQuery(&disjoint, &f.disjoint));
        for (u32 i = 0; i < Count && ok; ++i)
            for (u32 k = 0; k < Pairs && ok; ++k)
            {
                ok = ok && SUCCEEDED(HW.pDevice->CreateQuery(&stamp, &f.begin[i][k]));
                ok = ok && SUCCEEDED(HW.pDevice->CreateQuery(&stamp, &f.end[i][k]));
            }
        f.pending = false;
        std::fill(std::begin(f.issued), std::end(f.issued), u8(0));
    }
    if (!ok)
    {
        Msg("! [gpu-timers] timestamp queries unavailable - GPU pass timing disabled");
        OnDeviceDestroy();
        return;
    }
    m_enabled = true;
    m_valid = false;
    m_write = 0;
}

void dx11GpuTimers::OnDeviceDestroy()
{
    for (FrameQueries& f : m_ring)
    {
        _RELEASE(f.disjoint);
        for (u32 i = 0; i < Count; ++i)
            for (u32 k = 0; k < Pairs; ++k)
            {
                _RELEASE(f.begin[i][k]);
                _RELEASE(f.end[i][k]);
            }
        f.pending = false;
    }
    m_enabled = false;
    m_valid = false;
    m_in_frame = false;
}

void dx11GpuTimers::FrameBegin()
{
    if (!m_enabled || m_in_frame)
        return;
    // The slot about to be overwritten is the oldest one; harvest it first. Three frames of
    // latency is enough for the GPU to have finished it on every driver tried, and a frame
    // that is still in flight is simply skipped - the ring never blocks.
    FrameQueries& f = m_ring[m_write];
    if (f.pending)
        Resolve(f);

    auto* ctx = HW.get_context(HW.IMM_CTX_ID);
    ctx->Begin(f.disjoint);
    std::fill(std::begin(f.issued), std::end(f.issued), u8(0));
    m_in_frame = true;
    Begin(Frame);
}

void dx11GpuTimers::FrameEnd()
{
    if (!m_enabled || !m_in_frame)
        return;
    End(Frame);
    FrameQueries& f = m_ring[m_write];
    auto* ctx = HW.get_context(HW.IMM_CTX_ID);
    ctx->End(f.disjoint);
    f.pending = true;
    m_in_frame = false;
    m_write = (m_write + 1) % RingSize;
}

void dx11GpuTimers::Begin(Slot slot)
{
    if (!m_enabled || !m_in_frame)
        return;
    FrameQueries& f = m_ring[m_write];
    if (f.issued[slot] < Pairs)
        HW.get_context(HW.IMM_CTX_ID)->End(f.begin[slot][f.issued[slot]]);
}

void dx11GpuTimers::End(Slot slot)
{
    if (!m_enabled || !m_in_frame)
        return;
    FrameQueries& f = m_ring[m_write];
    if (f.issued[slot] < Pairs)
        HW.get_context(HW.IMM_CTX_ID)->End(f.end[slot][f.issued[slot]++]);
}

void dx11GpuTimers::Resolve(FrameQueries& f)
{
    auto* ctx = HW.get_context(HW.IMM_CTX_ID);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    // DONOTFLUSH: if the GPU has not reached this frame yet, keep the old numbers rather
    // than stall for new ones.
    if (ctx->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;
    f.pending = false;
    if (dj.Disjoint || dj.Frequency == 0)
    {
        m_valid = false;
        return;
    }
    const double to_ms = 1000.0 / double(dj.Frequency);
    for (u32 i = 0; i < Count; ++i)
    {
        double ms = 0.0;
        for (u32 k = 0; k < f.issued[i]; ++k)
        {
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(f.begin[i][k], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx->GetData(f.end[i][k], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && t1 >= t0)
                ms += double(t1 - t0) * to_ms;
        }
        m_ms[i] = float(ms);
    }
    for (u32 i = 0; i < Count; ++i)
        m_sum[i] += m_ms[i];
    ++m_sum_frames;
    m_valid = true;
}
} // namespace xray::render::RENDER_NAMESPACE
