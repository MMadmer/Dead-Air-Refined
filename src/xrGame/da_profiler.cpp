// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "pch_script.h"

#include "da_profiler.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <algorithm>
#include <unordered_map>

#if defined(XR_PLATFORM_WINDOWS)
#include <dbghelp.h>

namespace
{
constexpr u32 MAX_FRAMES = 20;
constexpr u32 MAX_SAMPLES = 120000;

struct sample_t
{
    u16 depth;
    DWORD64 frames[MAX_FRAMES];
};

// Unwinding a suspended thread touches its stack directly, and a stack caught mid-prologue can
// hand back a frame pointer that leads anywhere. SEH keeps a bad walk from taking the game with
// it; the function holds no C++ objects so the compiler accepts the __try.
u16 walk_suspended(CONTEXT& ctx, DWORD64* out, u32 maxFrames)
{
    u16 count = 0;
    __try
    {
        while (count < maxFrames && ctx.Rip)
        {
            out[count++] = ctx.Rip;

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (!entry)
            {
                // A leaf with no unwind data: the return address is where RSP points.
                const DWORD64 ret = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                if (!ret)
                    break;
                ctx.Rip = ret;
                ctx.Rsp += sizeof(DWORD64);
                continue;
            }

            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisherFrame, nullptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Keep whatever frames were already good.
    }
    return count;
}

class sampler
{
    HANDLE m_target{};
    HANDLE m_thread{};
    volatile LONG m_stop{};
    u32 m_interval_us{ 1000 };

    // Raw, fixed, and outside the engine heap on purpose. Growing a container while the
    // target thread is suspended can take a lock that only the suspended thread can
    // release, which hangs the game against its own profiler.
    sample_t* m_storage{};
    u32 m_count{};
    u64 m_suspend_failures{};

public:
    bool running() const { return m_thread != nullptr; }
    u32 sample_count() const { return m_count; }
    u64 suspend_failures() const { return m_suspend_failures; }
    const sample_t* samples() const { return m_storage; }

    bool start(u32 hz)
    {
        if (m_thread)
            return false;

        // Called from the console, which runs on the game thread - so this duplicates exactly
        // the thread whose time the report is about.
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_target,
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            return false;

        m_interval_us = hz ? (1000000u / hz) : 1000u;
        if (!m_storage)
        {
            m_storage = static_cast<sample_t*>(
                VirtualAlloc(nullptr, sizeof(sample_t) * MAX_SAMPLES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!m_storage)
            {
                CloseHandle(m_target);
                m_target = nullptr;
                return false;
            }
        }
        m_count = 0;
        m_suspend_failures = 0;
        m_stop = 0;

        DWORD id = 0;
        m_thread = CreateThread(nullptr, 0, &sampler::thunk, this, 0, &id);
        if (!m_thread)
        {
            CloseHandle(m_target);
            m_target = nullptr;
            return false;
        }
        SetThreadPriority(m_thread, THREAD_PRIORITY_TIME_CRITICAL);
        return true;
    }

    void stop()
    {
        if (!m_thread)
            return;
        InterlockedExchange(&m_stop, 1);
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
        CloseHandle(m_target);
        m_target = nullptr;
    }

private:
    static DWORD WINAPI thunk(LPVOID self)
    {
        static_cast<sampler*>(self)->run();
        return 0;
    }

    void run()
    {
        // Sleep(0)-grade pacing: a waitable timer keeps the cadence honest without a busy loop.
        HANDLE timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(m_interval_us) * 10;
        if (timer)
            SetWaitableTimer(timer, &due, m_interval_us / 1000 ? m_interval_us / 1000 : 1, nullptr, nullptr, FALSE);

        while (!InterlockedCompareExchange(&m_stop, 0, 0))
        {
            if (timer)
                WaitForSingleObject(timer, 50);
            else
                Sleep(1);

            if (m_count >= MAX_SAMPLES)
                break;

            if (SuspendThread(m_target) == DWORD(-1))
            {
                m_suspend_failures++;
                continue;
            }

            // Written straight into the reserved slot - nothing is allocated here.
            sample_t& s = m_storage[m_count];
            s.depth = 0;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(m_target, &ctx))
                s.depth = walk_suspended(ctx, s.frames, MAX_FRAMES);

            ResumeThread(m_target);

            if (s.depth)
                ++m_count;
        }

        if (timer)
            CloseHandle(timer);
    }
};

sampler g_sampler;

// ---------------------------------------------------------------- symbolization
struct dbg
{
    HMODULE lib{};
    decltype(&SymInitialize) init{};
    decltype(&SymSetOptions) setOptions{};
    decltype(&SymGetOptions) getOptions{};
    decltype(&SymFromAddr) fromAddr{};
    decltype(&SymGetModuleInfo64) moduleInfo{};
    decltype(&SymRefreshModuleList) refresh{};

    bool load()
    {
        if (fromAddr)
            return true;
        lib = GetModuleHandleA("dbghelp.dll");
        if (!lib)
            lib = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib)
            return false;

        init = reinterpret_cast<decltype(init)>(GetProcAddress(lib, "SymInitialize"));
        setOptions = reinterpret_cast<decltype(setOptions)>(GetProcAddress(lib, "SymSetOptions"));
        getOptions = reinterpret_cast<decltype(getOptions)>(GetProcAddress(lib, "SymGetOptions"));
        fromAddr = reinterpret_cast<decltype(fromAddr)>(GetProcAddress(lib, "SymFromAddr"));
        moduleInfo = reinterpret_cast<decltype(moduleInfo)>(GetProcAddress(lib, "SymGetModuleInfo64"));
        refresh = reinterpret_cast<decltype(refresh)>(GetProcAddress(lib, "SymRefreshModuleList"));
        return fromAddr && init && setOptions && getOptions;
    }

    void begin()
    {
        if (!load())
            return;
        setOptions(getOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        // The crash reporter may already own the session; an "already initialized" answer is fine.
        init(GetCurrentProcess(), nullptr, TRUE);
        if (refresh)
            refresh(GetCurrentProcess());
    }
};

dbg g_dbg;

struct resolved
{
    xr_string name;
    xr_string module;
};

const resolved& resolve(DWORD64 address, std::unordered_map<DWORD64, resolved>& cache)
{
    auto it = cache.find(address);
    if (it != cache.end())
        return it->second;

    resolved r;
    if (g_dbg.fromAddr)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (g_dbg.fromAddr(GetCurrentProcess(), address, &displacement, info))
            r.name = info->Name;

        if (g_dbg.moduleInfo)
        {
            IMAGEHLP_MODULE64 mi{};
            mi.SizeOfStruct = sizeof(mi);
            if (g_dbg.moduleInfo(GetCurrentProcess(), address, &mi))
                r.module = mi.ModuleName;
        }
    }
    if (r.name.empty())
    {
        string64 tmp;
        xr_sprintf(tmp, "0x%llx", static_cast<unsigned long long>(address));
        r.name = tmp;
    }
    if (r.module.empty())
        r.module = "?";

    return cache.emplace(address, std::move(r)).first->second;
}

struct entry
{
    xr_string key;
    u64 self{};
    u64 inclusive{};
};

void report(u32 topSelf, u32 topInclusive)
{
    const sample_t* samples = g_sampler.samples();
    const u32 sampleCount = g_sampler.sample_count();
    if (!samples || !sampleCount)
    {
        Msg("! [profiler] no samples - was the game thread running?");
        return;
    }

    g_dbg.begin();

    std::unordered_map<DWORD64, resolved> cache;
    std::unordered_map<xr_string, entry> table;

    xr_vector<xr_string> seen;
    for (u32 i = 0; i < sampleCount; ++i)
    {
        const sample_t& s = samples[i];
        seen.clear();
        for (u16 f = 0; f < s.depth; ++f)
        {
            const resolved& r = resolve(s.frames[f], cache);
            xr_string key = r.module + "!" + r.name;

            if (f == 0)
            {
                auto& e = table[key];
                e.key = key;
                e.self++;
            }
            // A recursive function must not count its own frames twice towards inclusive time.
            if (std::find(seen.begin(), seen.end(), key) == seen.end())
            {
                auto& e = table[key];
                e.key = key;
                e.inclusive++;
                seen.emplace_back(std::move(key));
            }
        }
    }

    xr_vector<entry> rows;
    rows.reserve(table.size());
    for (auto& kv : table)
        rows.emplace_back(kv.second);

    const double total = double(sampleCount);

    Msg("~ [profiler] %u sample(s), %u distinct symbol(s), %llu suspend failure(s)", sampleCount,
        u32(rows.size()), g_sampler.suspend_failures());

    std::sort(rows.begin(), rows.end(), [](const entry& a, const entry& b) { return a.self > b.self; });
    Msg("~ [profiler] --- self time (where the game thread actually was) ---");
    for (u32 i = 0; i < topSelf && i < rows.size(); ++i)
    {
        if (!rows[i].self)
            break;
        Msg("~ [profiler] self %6.2f%%  %6llu  %s", 100.0 * double(rows[i].self) / total, rows[i].self,
            rows[i].key.c_str());
    }

    std::sort(rows.begin(), rows.end(), [](const entry& a, const entry& b) { return a.inclusive > b.inclusive; });
    Msg("~ [profiler] --- inclusive time (function or anything it called) ---");
    for (u32 i = 0; i < topInclusive && i < rows.size(); ++i)
    {
        if (!rows[i].inclusive)
            break;
        Msg("~ [profiler] incl %6.2f%%  %6llu  %s", 100.0 * double(rows[i].inclusive) / total, rows[i].inclusive,
            rows[i].key.c_str());
    }
    Msg("~ [profiler] done");
}

// "Who calls this?" - the question that turns a guess about a hot symbol into a measurement.
// The samples already hold whole stacks, so this only has to read them again.
void report_callers(pcstr needle, u32 top)
{
    const sample_t* samples = g_sampler.samples();
    const u32 sampleCount = g_sampler.sample_count();
    if (!samples || !sampleCount)
    {
        Msg("! [profiler] no samples to search - run a profile first");
        return;
    }

    g_dbg.begin();
    std::unordered_map<DWORD64, resolved> cache;
    std::unordered_map<xr_string, u64> callers;
    u64 matched = 0;

    for (u32 i = 0; i < sampleCount; ++i)
    {
        const sample_t& s = samples[i];
        for (u16 f = 0; f < s.depth; ++f)
        {
            const resolved& r = resolve(s.frames[f], cache);
            if (!strstr(r.name.c_str(), needle))
                continue;

            matched++;
            // Three frames of context, not one. A single caller is often just the
            // constructor or the inline wrapper of the thing you asked about, and the
            // question you actually had is who called THAT.
            xr_string chain;
            for (u16 up = 1; up <= 3 && f + up < s.depth; ++up)
            {
                const resolved& r_up = resolve(s.frames[f + up], cache);
                if (!chain.empty())
                    chain += " <- ";
                chain += r_up.name;
            }
            callers[chain.empty() ? xr_string("<stack bottom>") : chain]++;
            break;
        }
    }

    if (!matched)
    {
        Msg("~ [profiler] no sample contains a symbol matching \"%s\"", needle);
        return;
    }

    xr_vector<std::pair<xr_string, u64>> rows(callers.begin(), callers.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    Msg("~ [profiler] \"%s\" appears in %llu of %u sample(s) - %.2f%% of the thread", needle, matched, sampleCount,
        100.0 * double(matched) / double(sampleCount));
    for (u32 i = 0; i < top && i < rows.size(); ++i)
        Msg("~ [profiler] from %6.2f%%  %6llu  %s", 100.0 * double(rows[i].second) / double(matched),
            rows[i].second, rows[i].first.c_str());
    Msg("~ [profiler] callers done");
}

class CCC_ProfileCallers : public IConsole_Command
{
public:
    CCC_ProfileCallers(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = false; }

    void Execute(pcstr args) override
    {
        if (!args || !xr_strlen(args))
        {
            Msg("! [profiler] usage: dar_profile_callers <part of a symbol name>");
            return;
        }
        string256 needle;
        xr_strcpy(needle, args);
        report_callers(needle, 14);
    }
};

class CCC_ProfileStart : public IConsole_Command
{
public:
    CCC_ProfileStart(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        u32 hz = 1000;
        if (args && xr_strlen(args))
            sscanf(args, "%u", &hz);
        if (hz < 50)
            hz = 50;
        if (hz > 4000)
            hz = 4000;

        if (g_sampler.running())
        {
            Msg("! [profiler] already running");
            return;
        }
        if (!g_sampler.start(hz))
        {
            Msg("! [profiler] could not start");
            return;
        }
        Msg("~ [profiler] sampling the game thread at %u Hz", hz);
    }
};

class CCC_ProfileStop : public IConsole_Command
{
public:
    CCC_ProfileStop(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (!g_sampler.running())
        {
            Msg("! [profiler] not running");
            return;
        }
        u32 top = 45;
        if (args && xr_strlen(args))
            sscanf(args, "%u", &top);
        g_sampler.stop();
        report(top, top / 2);
        Msg("~ [profiler] samples kept - dar_profile_callers <symbol> to ask who called what");
    }
};
} // namespace

void da_profiler_register()
{
    CMD1(CCC_ProfileStart, "dar_profile_start");
    CMD1(CCC_ProfileStop, "dar_profile_stop");
    CMD1(CCC_ProfileCallers, "dar_profile_callers");
}

#else // !XR_PLATFORM_WINDOWS

void da_profiler_register() {}

#endif
