// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "da_watchdog.h"

#include "xrCore/Debug/CrashReport.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

int g_dar_watchdog = 1;

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
constexpr ULONGLONG kHangMs = 15000;

std::atomic<ULONGLONG> g_last_beat{0};
std::thread g_thread;
std::mutex g_lock;
std::condition_variable g_wake;
bool g_stop = false;
DWORD g_main_thread = 0;

// The main thread's registers while it is held, its stack walked after it runs again: walking it suspended would wait
// on any lock it holds (the heap's, dbghelp's), and a frozen thread's stack stays where it froze anyway.
bool capture_main(CONTEXT& context)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_thread);
    if (!thread)
        return false;
    bool ok = false;
    if (SuspendThread(thread) != DWORD(-1))
    {
        context = {};
        context.ContextFlags = CONTEXT_FULL;
        ok = !!GetThreadContext(thread, &context);
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return ok;
}

void on_hang(ULONGLONG silent_ms)
{
    Msg("! [watchdog] no frame for %llu s: the game has stopped (main thread's stack below, a hang report is written)",
        silent_ms / 1000);
    CONTEXT context;
    if (capture_main(context))
    {
        // The instruction first: code without unwind data (the script JIT's) ends the walk before its first frame.
        Msg("! [watchdog] main thread at 0x%016llx", u64(context.Rip));
        xrDebug::LogStackTrace("! [watchdog] main thread:", &context);
        FlushLog();
        CrashReporter::WriteHang(context);
    }
    else
    {
        Msg("! [watchdog] the main thread could not be read (error %lu)", GetLastError());
        FlushLog();
    }
}

void run()
{
    bool reported = false;
    std::unique_lock lock(g_lock);
    while (!g_stop)
    {
        g_wake.wait_for(lock, std::chrono::milliseconds(500));
        if (g_stop)
            break;
        const ULONGLONG last = g_last_beat.load(std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        if (!last || now - last < kHangMs)
        {
            reported = false;
            continue;
        }
        // Nothing to report: someone is in the debugger, an error is being reported, or the switch is off.
        if (reported || !g_dar_watchdog || IsDebuggerPresent() || xrDebug::ProcessingFailure())
            continue;
        reported = true;
        lock.unlock();
        on_hang(now - last);
        lock.lock();
    }
}
} // namespace

void da_watchdog_start()
{
    if (g_thread.joinable())
        return;
    g_main_thread = GetCurrentThreadId();
    g_last_beat.store(GetTickCount64(), std::memory_order_relaxed);
    g_stop = false;
    g_thread = std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"X-RAY Watchdog");
        run();
    });
}

void da_watchdog_stop()
{
    if (!g_thread.joinable())
        return;
    {
        std::lock_guard lock(g_lock);
        g_stop = true;
    }
    g_wake.notify_all();
    g_thread.join();
}

void da_watchdog_beat() { g_last_beat.store(GetTickCount64(), std::memory_order_relaxed); }
#else
void da_watchdog_start() {}
void da_watchdog_stop() {}
void da_watchdog_beat() {}
#endif
