// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Vsync pacing of flip-model presents through the frame latency waitable object of a swap chain
// (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT): the object is released exactly once for
// every present that leaves the present queue.
//
// Why: DXGI bounds a vsync'd flip swap chain only by its buffers - N buffers accept N - 1 queued
// presents. Where the display shows every present for a refresh that is the refresh rate. Behind
// the NVIDIA Optimus copy to the integrated GPU (hybrid laptops: xrEngine.exe exports
// NvOptimusEnablement and the driver profile of the name enables it too) a buffer is free again as
// soon as it has been copied, so N - 1 presents leave the queue every refresh: three buffers ran
// the game at twice the refresh rate with vsync on, half of the frames never shown. Queueing a
// present only after the previous one has left the queue is one present per refresh on every path;
// the frame's GPU work is submitted before the wait and still overlaps it.
#pragma once

#include <windows.h>

namespace xrd3d12
{
// One thread at a time; a caller that moves between threads orders the calls itself.
class PresentPacer
{
public:
    PresentPacer() = default;
    PresentPacer(const PresentPacer&) = delete;
    PresentPacer& operator=(const PresentPacer&) = delete;
    ~PresentPacer() { Detach(); }

    // Takes over the handle GetFrameLatencyWaitableObject returned; null paces nothing.
    void Attach(HANDLE waitable)
    {
        Detach();
        m_waitable = waitable;
    }

    void Detach()
    {
        if (m_waitable)
            CloseHandle(m_waitable);
        m_waitable = nullptr;
        m_presented = m_retired = 0;
    }

    bool Attached() const { return !!m_waitable; }

    // Right before every Present that queues a frame. With vsync it returns once all earlier
    // presents have left the queue; without, it only keeps count, so tearing stays uncapped and
    // switching vsync on later starts from exact numbers.
    void BeforePresent(bool vsync)
    {
        if (!m_waitable)
            return;
        while (WaitForSingleObject(m_waitable, 0) == WAIT_OBJECT_0)
            ++m_retired;
        // The initial count of the object, and presents made before counting started.
        if (m_retired > m_presented)
            m_retired = m_presented;
        while (vsync && m_retired < m_presented)
        {
            // A late present (a GPU hitch) is still counted and waited for by the next frame.
            if (WaitForSingleObject(m_waitable, kWaitMs) != WAIT_OBJECT_0)
                break;
            ++m_retired;
        }
        ++m_presented;
    }

    // Starts counting afresh after presents this pacer did not see: returns once the object has
    // stayed quiet for `quietMs`. Nothing may present meanwhile.
    void Resync(DWORD quietMs)
    {
        if (!m_waitable)
            return;
        while (WaitForSingleObject(m_waitable, quietMs) == WAIT_OBJECT_0)
        {
        }
        m_presented = m_retired = 0;
    }

private:
    static constexpr DWORD kWaitMs = 100;

    HANDLE m_waitable = nullptr;
    UINT64 m_presented = 0;
    UINT64 m_retired = 0;
};
} // namespace xrd3d12
