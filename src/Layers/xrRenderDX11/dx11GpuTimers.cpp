#include "stdafx.h"
#include "dx11GpuTimers.h"

namespace xray::render::RENDER_NAMESPACE
{
dx11GpuTimers GpuTimers;

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
    case Combine: return "combine";
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
        {
            ok = ok && SUCCEEDED(HW.pDevice->CreateQuery(&stamp, &f.begin[i]));
            ok = ok && SUCCEEDED(HW.pDevice->CreateQuery(&stamp, &f.end[i]));
        }
        f.pending = false;
        std::fill(std::begin(f.issued), std::end(f.issued), false);
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
        {
            _RELEASE(f.begin[i]);
            _RELEASE(f.end[i]);
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
    std::fill(std::begin(f.issued), std::end(f.issued), false);
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
    HW.get_context(HW.IMM_CTX_ID)->End(f.begin[slot]);
}

void dx11GpuTimers::End(Slot slot)
{
    if (!m_enabled || !m_in_frame)
        return;
    FrameQueries& f = m_ring[m_write];
    HW.get_context(HW.IMM_CTX_ID)->End(f.end[slot]);
    f.issued[slot] = true;
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
        if (!f.issued[i])
        {
            m_ms[i] = 0.f;
            continue;
        }
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(f.begin[i], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(f.end[i], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            m_ms[i] = 0.f;
            continue;
        }
        m_ms[i] = t1 >= t0 ? float(double(t1 - t0) * to_ms) : 0.f;
    }
    m_valid = true;
}
} // namespace xray::render::RENDER_NAMESPACE
