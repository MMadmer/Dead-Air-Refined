// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// A small sampling profiler for the benchmark (-profile): a background thread suspends the
// measured thread every 100 microseconds, unwinds its stack and counts functions (exclusive = the
// sampled instruction, inclusive = anywhere on the stack). Nothing is allocated while the thread
// is suspended (it may hold the heap lock).
#include "harness.h"

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace layertest
{
namespace
{
constexpr u32 kMaxDepth = 48;

struct Sample
{
    u32 depth;
    DWORD64 frames[kMaxDepth];
};

std::atomic<bool> g_running{false};
std::thread g_sampler;
std::vector<Sample> g_samples;

u32 Unwind(CONTEXT& context, DWORD64* frames)
{
    u32 depth = 0;
    while (depth < kMaxDepth && context.Rip)
    {
        frames[depth++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (!function)
        {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
    }
    return depth;
}

void SamplerLoop(HANDLE thread)
{
    Sample sample{};
    while (g_running.load(std::memory_order_acquire))
    {
        if (SuspendThread(thread) != DWORD(-1))
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            sample.depth = GetThreadContext(thread, &context) ? Unwind(context, sample.frames) : 0;
            ResumeThread(thread);
            if (sample.depth)
                g_samples.push_back(sample);
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
        while (std::chrono::steady_clock::now() < until)
            YieldProcessor();
    }
    CloseHandle(thread);
}

std::string FunctionOf(DWORD64 address, std::unordered_map<DWORD64, std::string>& cache)
{
    if (const auto it = cache.find(address); it != cache.end())
        return it->second;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    std::string name;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
        name = symbol->Name;
    else
    {
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        const char* file = std::strrchr(path, '\\');
        name = std::string("[") + (file ? file + 1 : path) + "]";
    }
    cache.emplace(address, name);
    return name;
}
} // namespace

void ProfilerStart()
{
    g_samples.clear();
    g_samples.reserve(1 << 20);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_running.store(true, std::memory_order_release);
    g_sampler = std::thread(SamplerLoop, self);
}

void ProfilerStop(const char* title, u32 top)
{
    g_running.store(false, std::memory_order_release);
    if (g_sampler.joinable())
        g_sampler.join();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::unordered_map<DWORD64, std::string> names;
    std::map<std::string, u32> exclusive, inclusive;
    for (const Sample& s : g_samples)
    {
        ++exclusive[FunctionOf(s.frames[0], names)];
        std::vector<std::string> seen;
        for (u32 i = 0; i < s.depth; ++i)
        {
            std::string name = FunctionOf(s.frames[i], names);
            if (std::find(seen.begin(), seen.end(), name) == seen.end())
                seen.push_back(std::move(name));
        }
        for (const std::string& name : seen)
            ++inclusive[name];
    }
    // Heap and kernel allocations: which layer function they come from.
    std::map<std::string, u32> allocators;
    for (const Sample& s : g_samples)
    {
        const std::string leaf = FunctionOf(s.frames[0], names);
        if (leaf != "RtlAllocateHeap" && leaf != "NtGdiDdDDICreateAllocation" && leaf != "RtlFreeHeap")
            continue;
        for (u32 i = 1; i < s.depth; ++i)
        {
            const std::string name = FunctionOf(s.frames[i], names);
            if (name.rfind("xrd3d12::", 0) == 0)
            {
                ++allocators[leaf + " <- " + FunctionOf(s.frames[i - 1], names) + " <- " + name];
                break;
            }
        }
    }
    SymCleanup(GetCurrentProcess());
    for (const auto& [name, count] : allocators)
        std::printf("  ALLOC %5u  %s\n", count, name.c_str());
    const auto print = [&](const char* kind, const std::map<std::string, u32>& counts) {
        std::vector<std::pair<u32, std::string>> sorted;
        for (const auto& [name, count] : counts)
            sorted.emplace_back(count, name);
        std::sort(sorted.rbegin(), sorted.rend());
        std::printf("PROFILE %s (%s, %zu samples)\n", title, kind, g_samples.size());
        for (u32 i = 0; i < top && i < sorted.size(); ++i)
            std::printf("  %5.1f%%  %s\n", 100.0 * sorted[i].first / double(g_samples.size()), sorted[i].second.c_str());
    };
    print("exclusive", exclusive);
    print("inclusive", inclusive);
}
} // namespace layertest
