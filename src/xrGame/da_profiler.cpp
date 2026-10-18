// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "pch_script.h"

#include "da_profiler.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <algorithm>
#include <unordered_map>

#if defined(XR_PLATFORM_WINDOWS)
#include <dbghelp.h>
#include <tlhelp32.h>

namespace
{
constexpr u32 MAX_FRAMES = 20;
constexpr u32 MAX_SAMPLES = 120000;
// "all": every thread of the process but the sampler's own, the rounds capped at this rate.
constexpr u32 MAX_THREADS = 256;
constexpr u32 MAX_ALL_HZ = 250;

struct sample_t
{
    u16 depth;
    u16 thread; // slot in the sampler's thread table
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

    // All-threads mode: the table of the process's threads, refreshed between rounds (nothing is
    // suspended then), with their names for the report; a thread that exited keeps its slot.
    bool m_all{};
    DWORD m_self_id{};
    u32 m_thread_count{};
    DWORD m_ids[MAX_THREADS]{};
    HANDLE m_handles[MAX_THREADS]{};
    char m_names[MAX_THREADS][48]{};
    u32 m_rounds{};

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
    bool all() const { return m_all; }
    u32 rounds() const { return m_rounds; }
    u32 thread_count() const { return m_thread_count; }
    const char* thread_name(u32 slot) const { return m_names[slot]; }
    DWORD thread_id(u32 slot) const { return m_ids[slot]; }

    bool start(u32 hz, bool all)
    {
        if (m_thread)
            return false;

        // Called from the console, which runs on the game thread - so this duplicates exactly
        // the thread whose time the report is about.
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_target,
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            return false;

        m_all = all;
        close_threads();
        m_rounds = 0;
        if (all && hz > MAX_ALL_HZ)
            hz = MAX_ALL_HZ;
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
        // The ids and names stay for the report; the handles go.
        for (u32 i = 0; i < m_thread_count; ++i)
        {
            if (m_handles[i])
                CloseHandle(m_handles[i]);
            m_handles[i] = nullptr;
        }
    }

private:
    static DWORD WINAPI thunk(LPVOID self)
    {
        static_cast<sampler*>(self)->run();
        return 0;
    }

    void close_threads()
    {
        for (u32 i = 0; i < m_thread_count; ++i)
        {
            if (m_handles[i])
                CloseHandle(m_handles[i]);
            m_handles[i] = nullptr;
        }
        m_thread_count = 0;
    }

    // New threads of the process into the table. Runs between rounds: no thread is suspended, so the
    // snapshot's allocations cannot meet a lock a suspended thread holds.
    void refresh_threads()
    {
        using describe_fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto describe =
            reinterpret_cast<describe_fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == m_self_id)
                continue;
            if (std::find(m_ids, m_ids + m_thread_count, te.th32ThreadID) != m_ids + m_thread_count)
                continue;
            if (m_thread_count >= MAX_THREADS)
                break;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                te.th32ThreadID);
            if (!h)
                continue;
            const u32 slot = m_thread_count++;
            m_ids[slot] = te.th32ThreadID;
            m_handles[slot] = h;
            m_names[slot][0] = 0;
            PWSTR name = nullptr;
            if (describe && SUCCEEDED(describe(h, &name)) && name)
            {
                WideCharToMultiByte(CP_UTF8, 0, name, -1, m_names[slot], sizeof(m_names[slot]), nullptr, nullptr);
                LocalFree(name);
            }
        }
        CloseHandle(snap);
    }

    // One suspended thread into the next slot of the storage.
    void take(HANDLE target, u16 slot)
    {
        if (m_count >= MAX_SAMPLES)
            return;
        if (SuspendThread(target) == DWORD(-1))
        {
            m_suspend_failures++;
            return;
        }
        sample_t& s = m_storage[m_count];
        s.depth = 0;
        s.thread = slot;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(target, &ctx))
            s.depth = walk_suspended(ctx, s.frames, MAX_FRAMES);
        ResumeThread(target);
        if (s.depth)
            ++m_count;
    }

    void run()
    {
        m_self_id = GetCurrentThreadId();
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

            if (!m_all)
            {
                // Written straight into the reserved slot - nothing is allocated here.
                take(m_target, 0);
                continue;
            }
            if (m_rounds % 64 == 0)
                refresh_threads();
            ++m_rounds;
            for (u32 i = 0; i < m_thread_count; ++i)
            {
                if (m_handles[i])
                    take(m_handles[i], u16(i));
            }
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
    decltype(&SymGetLineFromAddr64) lineFromAddr{};

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
        lineFromAddr = reinterpret_cast<decltype(lineFromAddr)>(GetProcAddress(lib, "SymGetLineFromAddr64"));
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

// A thread blocked in the kernel (a wait, a sleep, an idle worker's park): in the all-threads
// report such samples are idle time, not work.
bool waiting(const sample_t& s, std::unordered_map<DWORD64, resolved>& cache)
{
    if (!s.depth)
        return true;
    const resolved& leaf = resolve(s.frames[0], cache);
    if (leaf.module != "ntdll" && leaf.module != "win32u")
        return false;
    const char* n = leaf.name.c_str();
    return strstr(n, "Wait") || strstr(n, "Delay") || strstr(n, "RemoveIoCompletion") || strstr(n, "GetMessage") ||
        strstr(n, "UserMsgWait");
}

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

    const bool all = g_sampler.all();
    u32 busy = 0;
    if (all)
    {
        // Per thread: the share of the rounds it was running (100 % = a whole core).
        xr_vector<std::pair<u32, u32>> perThread(g_sampler.thread_count(), {0u, 0u});
        for (u32 i = 0; i < sampleCount; ++i)
        {
            if (samples[i].thread < perThread.size() && !waiting(samples[i], cache))
                perThread[samples[i].thread].first++;
        }
        for (u32 t = 0; t < perThread.size(); ++t)
            perThread[t].second = t;
        std::sort(perThread.begin(), perThread.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const double rounds = double(std::max(g_sampler.rounds(), 1u));
        double cores = 0.0;
        for (const auto& [n, t] : perThread)
            cores += double(n) / rounds;
        Msg("~ [profiler] all threads: %u round(s), %u thread(s), %.2f core(s) busy on average", g_sampler.rounds(),
            g_sampler.thread_count(), cores);
        for (u32 k = 0; k < perThread.size() && k < 24; ++k)
        {
            if (!perThread[k].first)
                break;
            const u32 t = perThread[k].second;
            Msg("~ [profiler] thread %6lu %-40s busy %6.2f%%", g_sampler.thread_id(t), g_sampler.thread_name(t)[0] ? g_sampler.thread_name(t) : "-",
                100.0 * double(perThread[k].first) / rounds);
        }
    }

    xr_vector<xr_string> seen;
    for (u32 i = 0; i < sampleCount; ++i)
    {
        const sample_t& s = samples[i];
        if (all && waiting(s, cache))
            continue;
        ++busy;
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

    // All threads: the shares are of the busy samples, so a row reads "this much of the work".
    const double total = double(std::max(all ? busy : sampleCount, 1u));

    Msg("~ [profiler] %u sample(s), %u busy, %u distinct symbol(s), %llu suspend failure(s)", sampleCount, busy,
        u32(rows.size()), g_sampler.suspend_failures());

    std::sort(rows.begin(), rows.end(), [](const entry& a, const entry& b) { return a.self > b.self; });
    Msg(all ? "~ [profiler] --- self time (the work of every thread) ---" :
              "~ [profiler] --- self time (where the game thread actually was) ---");
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
        if (g_sampler.all() && waiting(s, cache))
            continue;
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

// "Where inside this function?" - the samples whose stack passes through the symbol, by the source line of
// that frame (for a caller frame the line of the call it made), so a hot callee inlined or called through
// a pointer - a driver entry point - shows where it is called from.
void report_lines(pcstr needle, u32 top)
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
    std::unordered_map<DWORD64, xr_string> lines;
    std::unordered_map<xr_string, u64> hits;
    u64 matched = 0;

    for (u32 i = 0; i < sampleCount; ++i)
    {
        const sample_t& s = samples[i];
        if (g_sampler.all() && waiting(s, cache))
            continue;
        for (u16 f = 0; f < s.depth; ++f)
        {
            const resolved& r = resolve(s.frames[f], cache);
            if (!strstr(r.name.c_str(), needle))
                continue;
            matched++;
            // A return address points past its call: one byte back is the call's line.
            const DWORD64 address = f ? s.frames[f] - 1 : s.frames[f];
            auto it = lines.find(address);
            if (it == lines.end())
            {
                xr_string text = "?";
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD displacement = 0;
                if (g_dbg.lineFromAddr && g_dbg.lineFromAddr(GetCurrentProcess(), address, &displacement, &line))
                {
                    pcstr file = strrchr(line.FileName, '\\');
                    string256 tmp;
                    xr_sprintf(tmp, "%s:%lu%s", file ? file + 1 : line.FileName, line.LineNumber, f ? "" : " (self)");
                    text = tmp;
                }
                it = lines.emplace(address, std::move(text)).first;
            }
            hits[it->second]++;
            break;
        }
    }

    if (!matched)
    {
        Msg("~ [profiler] no sample contains a symbol matching \"%s\"", needle);
        return;
    }
    xr_vector<std::pair<xr_string, u64>> rows(hits.begin(), hits.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    Msg("~ [profiler] \"%s\" by line: %llu of %u sample(s)", needle, matched, sampleCount);
    for (u32 i = 0; i < top && i < rows.size(); ++i)
        Msg("~ [profiler] line %6.2f%%  %6llu  %s", 100.0 * double(rows[i].second) / double(matched), rows[i].second,
            rows[i].first.c_str());
    Msg("~ [profiler] lines done");
}

class CCC_ProfileLines : public IConsole_Command
{
public:
    CCC_ProfileLines(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = false; }

    void Execute(pcstr args) override
    {
        if (!args || !xr_strlen(args))
        {
            Msg("! [profiler] usage: dar_profile_lines <part of a symbol name>");
            return;
        }
        string256 needle;
        xr_strcpy(needle, args);
        report_lines(needle, 30);
    }
};

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
        // "<Hz> [all]": all samples every thread of the process (rounds up to 250 Hz).
        u32 hz = 1000;
        char mode[16]{};
        if (args && xr_strlen(args))
            sscanf(args, "%u %15s", &hz, mode);
        const bool all = !xr_stricmp(mode, "all");
        if (hz < 50)
            hz = 50;
        if (hz > 4000)
            hz = 4000;

        if (g_sampler.running())
        {
            Msg("! [profiler] already running");
            return;
        }
        if (!g_sampler.start(hz, all))
        {
            Msg("! [profiler] could not start");
            return;
        }
        Msg(all ? "~ [profiler] sampling every thread at up to %u Hz" : "~ [profiler] sampling the game thread at %u Hz", hz);
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
    CMD1(CCC_ProfileLines, "dar_profile_lines");
}

#else // !XR_PLATFORM_WINDOWS

void da_profiler_register() {}

#endif
