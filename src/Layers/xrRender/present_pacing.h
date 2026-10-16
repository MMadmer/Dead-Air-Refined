// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Vsync pacing shared by every swap chain: DXGI flip chains (DX11, DX12, FSR frame generation) through the frame
// latency waitable object (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, released once for every present
// that leaves the present queue), Vulkan through VK_KHR_present_wait (vk_swapchain.cpp).
//
// Why a pacer: DXGI bounds a vsync'd flip swap chain only by its buffers - N buffers accept N - 1 queued presents.
// Behind the NVIDIA Optimus copy to the integrated GPU (hybrid laptops: xrEngine.exe exports NvOptimusEnablement)
// a buffer is free again as soon as it has been copied, so N - 1 presents leave the queue every refresh: three
// buffers ran the game at twice the refresh rate with vsync on, half of the frames never shown. A present queued
// only after the previous one left the queue is one present per refresh on every path.
//
// Why not always that strict: the wait holds the CPU to the display. While frames take longer than a refresh,
// the frame recorded after the wait reached the GPU late and the GPU sat idle for up to a refresh (Vulkan lost
// a fifth of its frame rate at 60-70 fps). Frames that slow never complete two within a refresh, so one present
// may stay queued behind the shown one without any being skipped; as soon as frames keep up with the refresh the
// strict wait caps them again. DOOM Eternal's "triple buffering" is the same queue of one, without the cap.
#pragma once

#include <windows.h>

#include <cstring>

namespace xrpacing
{
// -vsync_strict: the previous present always reaches the display first (the pacing before the queue of one).
inline bool StrictVsync()
{
    static const bool strict = strstr(GetCommandLineA(), "-vsync_strict") != nullptr;
    return strict;
}

// The refresh period of the monitor that holds the window; 0 when it cannot be read.
inline double RefreshPeriod(HWND window)
{
    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // 0 and 1 stand for "the hardware default".
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
        return 0;
    return 1.0 / double(mode.dmDisplayFrequency);
}

// How fast presents come compared with the refresh. One thread at a time.
class PresentClock
{
public:
    // Once per present, before it is queued.
    void Tick(HWND window)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!m_frequency)
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            m_frequency = frequency.QuadPart;
        }
        // Re-read once a second: the window may move to another monitor, the mode may change.
        if (!m_refreshRead || now.QuadPart - m_refreshRead > m_frequency || window != m_window)
        {
            m_refreshRead = now.QuadPart;
            m_window = window;
            m_period = RefreshPeriod(window);
        }
        if (m_last)
        {
            const double interval = double(now.QuadPart - m_last) / double(m_frequency);
            // A load or a pause is no frame rate.
            if (interval < kLongGap)
                m_average = m_average > 0 ? m_average + (interval - m_average) * kSmoothing : interval;
        }
        m_last = now.QuadPart;
        // Hysteresis: a frame rate right at the refresh must not flip the decision every frame.
        if (m_period > 0 && m_average > 0)
            m_slower = m_average > m_period * (m_slower ? kStaySlower : kTurnSlower);
        else
            m_slower = false;
    }

    // Presents come slower than the display refreshes.
    bool Slower() const { return m_slower; }

    // Forgets the frame rate (presents this clock did not see, a new swap chain).
    void Reset()
    {
        m_last = 0;
        m_average = 0;
        m_slower = false;
    }

private:
    static constexpr double kSmoothing = 0.2;
    static constexpr double kLongGap = 0.25;
    static constexpr double kTurnSlower = 1.08;
    static constexpr double kStaySlower = 1.02;

    LONGLONG m_frequency = 0;
    LONGLONG m_last = 0;
    LONGLONG m_refreshRead = 0;
    HWND m_window = nullptr;
    double m_period = 0;
    double m_average = 0;
    bool m_slower = false;
};

// The waitable-object pacer of a DXGI flip chain. One thread at a time; a caller that moves between threads
// orders the calls itself.
class PresentPacer
{
public:
    PresentPacer() = default;
    PresentPacer(const PresentPacer&) = delete;
    PresentPacer& operator=(const PresentPacer&) = delete;
    ~PresentPacer() { Detach(); }

    // Takes over the handle GetFrameLatencyWaitableObject returned; null paces nothing. The window tells the
    // refresh rate.
    void Attach(HANDLE waitable, HWND window)
    {
        Detach();
        m_waitable = waitable;
        m_window = window;
    }

    void Detach()
    {
        if (m_waitable)
            CloseHandle(m_waitable);
        m_waitable = nullptr;
        m_presented = m_retired = 0;
        m_clock.Reset();
    }

    bool Attached() const { return !!m_waitable; }

    // Right before every Present that queues a frame. With vsync it returns once no more earlier presents wait
    // in the queue than the frame rate allows (see the top of the file). Without, tearing stays uncapped: only
    // the CPU is kept within DXGI's default frame latency of the display (the waitable chain no longer blocks
    // Present for it), and the count stays exact for switching vsync on later.
    void BeforePresent(bool vsync)
    {
        if (!m_waitable)
            return;
        m_clock.Tick(m_window);
        while (WaitForSingleObject(m_waitable, 0) == WAIT_OBJECT_0)
            ++m_retired;
        // The initial count of the object, and presents made before counting started.
        if (m_retired > m_presented)
            m_retired = m_presented;
        const UINT64 queued = !vsync ? kUnsyncedQueue : m_clock.Slower() && !StrictVsync() ? 1 : 0;
        while (m_presented - m_retired > queued)
        {
            // A late present (a GPU hitch) is still counted and waited for by the next frame.
            if (WaitForSingleObject(m_waitable, kWaitMs) != WAIT_OBJECT_0)
                break;
            ++m_retired;
        }
        ++m_presented;
    }

    // Starts counting afresh after presents this pacer did not see: returns once the object has stayed quiet
    // for `quietMs`. Nothing may present meanwhile.
    void Resync(DWORD quietMs)
    {
        if (!m_waitable)
            return;
        while (WaitForSingleObject(m_waitable, quietMs) == WAIT_OBJECT_0)
        {
        }
        m_presented = m_retired = 0;
        m_clock.Reset();
    }

private:
    static constexpr DWORD kWaitMs = 100;
    static constexpr UINT64 kUnsyncedQueue = 3;

    HANDLE m_waitable = nullptr;
    HWND m_window = nullptr;
    UINT64 m_presented = 0;
    UINT64 m_retired = 0;
    PresentClock m_clock;
};
} // namespace xrpacing
