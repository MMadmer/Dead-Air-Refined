#include "stdafx.h"

#include <SDL.h>

#if defined(XR_PLATFORM_WINDOWS)
#include <Psapi.h>
#elif defined(XR_PLATFORM_LINUX)
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#elif defined(XR_PLATFORM_BSD)
#include <sys/time.h>
#include <sys/resource.h>
#elif defined(XR_PLATFORM_HAIKU)
#include <OS.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#if defined(USE_MIMALLOC)
    #include "mimalloc.h"

    static_assert(xrMemory::SMALL_SIZE_MAX <= MI_SMALL_SIZE_MAX, "Please, adjust SMALL_SIZE_ALLOC_MAX");

    #define xr_internal_malloc(size) mi_malloc(size)
    #define xr_internal_malloc_aligned(size, alignment) mi_malloc_aligned(size, alignment)
    #define xr_internal_malloc_nothrow(size) mi_malloc(size)
    #define xr_internal_malloc_nothrow_aligned(size, alignment) mi_malloc_aligned(size, alignment)
    #define xr_internal_small_alloc(size) mi_malloc_small(size)
    #define xr_internal_small_free(ptr) mi_free(ptr)

    #define xr_internal_realloc(ptr, size) mi_realloc(ptr, size)
    #define xr_internal_realloc_aligned(ptr, size, alignment) mi_realloc_aligned(ptr, size, alignment)

    #define xr_internal_free(ptr) mi_free(ptr)
    #define xr_internal_free_size(ptr, size) mi_free_size(ptr, size)
    #define xr_internal_free_aligned(ptr, alignment) mi_free_aligned(ptr, alignment)
    #define xr_internal_free_size_aligned(ptr, size, alignment) mi_free_size_aligned(ptr, size, alignment)
#elif defined(USE_XR_ALIGNED_MALLOC)
    #include "Memory/xrMemory_align.h"

    #define xr_internal_malloc(size) malloc(size)
    #define xr_internal_malloc_aligned(size, alignment) xr_aligned_malloc(size, alignment)
    #define xr_internal_malloc_nothrow(size) xr_malloc(size)
    #define xr_internal_malloc_nothrow_aligned(size, alignment) mi_malloc_aligned(size, alignment)
    #define xr_internal_small_alloc(size) xr_aligned_malloc(size)
    #define xr_internal_small_free(ptr) xr_aligned_free(ptr)

    #define xr_internal_realloc(ptr, size) xr_aligned_realloc(ptr, size)
    #define xr_internal_realloc_aligned(ptr, size, alignment) xr_aligned_realloc(ptr, size, alignment)

    #define xr_internal_free(ptr) xr_aligned_free(ptr)
    #define xr_internal_free_size(ptr, size) xr_aligned_free(ptr)
    #define xr_internal_free_aligned(ptr, alignment) xr_aligned_free(ptr)
    #define xr_internal_free_size_aligned(ptr, size, alignment) xr_aligned_free(ptr)
#elif defined(USE_PURE_ALLOC)
    // Additional bytes of memory to hide memory problems on Release
    // But for Debug we don't need this if we want to find these problems
    #ifdef NDEBUG
        constexpr size_t xr_reserved_tail = 8;
    #else
        constexpr size_t xr_reserved_tail = 0;
    #endif

    #define xr_internal_malloc(size) malloc(size + xr_reserved_tail)
    #define xr_internal_malloc_aligned(size, alignment) malloc(size + xr_reserved_tail)
    #define xr_internal_malloc_nothrow(size) malloc(size + xr_reserved_tail)
    #define xr_internal_malloc_nothrow_aligned(size, alignment) malloc(size + xr_reserved_tail)
    #define xr_internal_small_alloc(size) malloc(size + xr_reserved_tail)
    #define xr_internal_small_free(ptr) free(ptr)

    #define xr_internal_realloc(ptr, size) realloc(ptr, size + xr_reserved_tail)
    #define xr_internal_realloc_aligned(ptr, size, alignment) realloc(ptr, size + xr_reserved_tail)

    #define xr_internal_free(ptr) free(ptr)
    #define xr_internal_free_size(ptr, size) free(ptr)
    #define xr_internal_free_aligned(ptr, alignment) free(ptr)
    #define xr_internal_free_size_aligned(ptr, size, alignment) free(ptr)
#else
    #error Please, define explicitly which allocator you want to use
#endif

xrMemory Memory;
// Also used in src\xrCore\xrDebug.cpp to prevent use of g_pStringContainer before it initialized
bool shared_str_initialized = false;

extern int out_of_memory_handler(size_t size);

void xrMemory::_initialize()
{
    ZoneScoped;
#if defined(XR_PLATFORM_WINDOWS)
    m_emergencyReserve = VirtualAlloc(nullptr, EMERGENCY_RESERVE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    m_emergencyReserve = xr_internal_malloc_nothrow(EMERGENCY_RESERVE_SIZE);
#endif
    if (!m_emergencyReserve)
        Msg("! Unable to reserve %zu MiB of emergency memory", EMERGENCY_RESERVE_SIZE / (1024 * 1024));

    g_pStringContainer = xr_new<str_container>();
    shared_str_initialized = true;
    g_pSharedMemoryContainer = xr_new<smem_container>();
}

void xrMemory::_destroy()
{
    ZoneScoped;
    xr_delete(g_pSharedMemoryContainer);
    xr_delete(g_pStringContainer);
    release_emergency_reserve();
}

xrMemoryStatistics xrMemory::statistics() const
{
    xrMemoryStatistics result;
#if defined(XR_PLATFORM_WINDOWS)
    MEMORY_BASIC_INFORMATION memoryInfo{};
    while (VirtualQuery(memoryInfo.BaseAddress, &memoryInfo, sizeof(memoryInfo)))
    {
        switch (memoryInfo.State)
        {
        case MEM_FREE:
            result.virtualFree += memoryInfo.RegionSize;
            result.largestFreeBlock = std::max(result.largestFreeBlock, memoryInfo.RegionSize);
            break;
        case MEM_RESERVE:
            result.virtualReserved += memoryInfo.RegionSize;
            break;
        case MEM_COMMIT:
            result.virtualCommitted += memoryInfo.RegionSize;
            if (memoryInfo.Type == MEM_MAPPED)
                result.mappedBytes += memoryInfo.RegionSize;
            break;
        }

        const auto nextAddress = static_cast<const std::byte*>(memoryInfo.BaseAddress) + memoryInfo.RegionSize;
        if (nextAddress <= memoryInfo.BaseAddress)
            break;
        memoryInfo.BaseAddress = const_cast<std::byte*>(nextAddress);
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    {
        result.privateBytes = counters.PrivateUsage;
        result.workingSet = counters.WorkingSetSize;
    }
#elif defined(XR_PLATFORM_LINUX)
    struct sysinfo si;
    sysinfo(&si);
    result.virtualFree = si.freeram * si.mem_unit;
    result.virtualReserved = si.bufferram * si.mem_unit;
    result.virtualCommitted = (si.totalram - si.freeram + si.totalswap - si.freeswap) * si.mem_unit;
#elif defined(XR_PLATFORM_HAIKU)
    system_info info;
    if (get_system_info(&info) == B_OK)
    {
        result.virtualFree = B_PAGE_SIZE * (uint64)(info.max_pages - info.used_pages);
        result.virtualReserved = B_PAGE_SIZE * (uint64)info.cached_pages;
        result.virtualCommitted = B_PAGE_SIZE * (uint64)info.used_pages;
    }
#endif
    return result;
}

XRCORE_API void vminfo(size_t* free, size_t* reserved, size_t* committed)
{
    const auto stats = Memory.statistics();
    *free = stats.virtualFree;
    *reserved = stats.virtualReserved;
    *committed = stats.virtualCommitted;
}

XRCORE_API void log_vminfo()
{
    const auto stats = Memory.statistics();
    Msg("* [%s] VA: free[%zu MiB], largest[%zu MiB], reserved[%zu MiB], committed[%zu MiB]",
        SDL_GetPlatform(), stats.virtualFree / 1048576, stats.largestFreeBlock / 1048576,
        stats.virtualReserved / 1048576, stats.virtualCommitted / 1048576);
    Msg("* [%s] process: private[%zu MiB], working set[%zu MiB], mapped[%zu MiB]",
        SDL_GetPlatform(), stats.privateBytes / 1048576, stats.workingSet / 1048576, stats.mappedBytes / 1048576);
}

size_t xrMemory::mem_usage()
{
#if defined(XR_PLATFORM_WINDOWS)
    return statistics().privateBytes;
#elif defined(XR_PLATFORM_LINUX) || defined(XR_PLATFORM_BSD) || defined(XR_PLATFORM_APPLE)
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss;
#elif defined(XR_PLATFORM_HAIKU)
    system_info info;
    get_system_info(&info);
    return B_PAGE_SIZE * (uint64)info.used_pages;
#else
    return 0;
#endif
}

void xrMemory::mem_compact()
{
    if (g_pStringContainer)
        g_pStringContainer->clean();
    if (g_pSharedMemoryContainer)
        g_pSharedMemoryContainer->clean();

#if defined(USE_MIMALLOC)
    mi_collect(true);
#endif

#if defined(XR_PLATFORM_WINDOWS)
    if (strstr(Core.Params, "-swap_on_compact"))
        SetProcessWorkingSetSize(GetCurrentProcess(), size_t(-1), size_t(-1));
#endif
}

bool xrMemory::release_emergency_reserve() noexcept
{
    void* reserve = std::exchange(m_emergencyReserve, nullptr);
    if (!reserve)
        return false;

#if defined(XR_PLATFORM_WINDOWS)
    VirtualFree(reserve, 0, MEM_RELEASE);
#else
    xr_internal_free(reserve);
#endif
    return true;
}

bool xrMemory::recover_allocation_failure() noexcept
{
    if (m_recoveryInProgress.test_and_set())
        return false;

    const bool reserveReleased = release_emergency_reserve();
    mem_compact();
    m_recoveryInProgress.clear();
    return reserveReleased;
}

void* xrMemory::mem_alloc(size_t size)
{
    auto result = xr_internal_malloc(size);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_malloc(size);
        if (!result)
            out_of_memory_handler(size);
    }
    //TracyAlloc(result, size);
    return result;
}

void* xrMemory::mem_alloc(size_t size, size_t alignment)
{
    auto result = xr_internal_malloc_aligned(size, alignment);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_malloc_aligned(size, alignment);
        if (!result)
            out_of_memory_handler(size);
    }
    //TracyAlloc(result, size);
    return result;
}

void* xrMemory::mem_alloc(size_t size, const std::nothrow_t&) noexcept
{
    auto result = xr_internal_malloc_nothrow(size);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_malloc_nothrow(size);
    }
    //TracyAlloc(result, size);
    return result;
}

void* xrMemory::mem_alloc(size_t size, size_t alignment, const std::nothrow_t&) noexcept
{
    auto result = xr_internal_malloc_nothrow_aligned(size, alignment);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_malloc_nothrow_aligned(size, alignment);
    }
    //TracyAlloc(result, size);
    return result;
}

void* xrMemory::small_alloc(size_t size) noexcept
{
    const auto result = xr_internal_small_alloc(size);
    //TracyAllocN(result, size, "small alloc");
    return result;
}

void xrMemory::small_free(void* ptr) noexcept
{
    //TracyFree(ptr);
    xr_internal_small_free(ptr);
}

void* xrMemory::mem_realloc(void* ptr, size_t size)
{
    //TracyFree(ptr);
    auto result = xr_internal_realloc(ptr, size);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_realloc(ptr, size);
        if (!result)
            out_of_memory_handler(size);
    }
    //TracyAllocN(result, size, "realloc");
    return result;
}

void* xrMemory::mem_realloc(void* ptr, size_t size, size_t alignment)
{
    //TracyFree(ptr);
    auto result = xr_internal_realloc_aligned(ptr, size, alignment);
    if (!result && size)
    {
        recover_allocation_failure();
        result = xr_internal_realloc_aligned(ptr, size, alignment);
        if (!result)
            out_of_memory_handler(size);
    }
    //TracyAllocN(result, size, "realloc");
    return result;
}

void xrMemory::mem_free(void* ptr)
{
    //TracyFree(ptr);
    xr_internal_free(ptr);
}

void xrMemory::mem_free(void* ptr, size_t alignment)
{
    //TracyFree(ptr);
    xr_internal_free_aligned(ptr, alignment);
}

// xr_strdup
XRCORE_API pstr xr_strdup(pcstr string)
{
    VERIFY(string);
    const size_t len = xr_strlen(string) + 1;
    auto memory = static_cast<char*>(xr_malloc(len));
    CopyMemory(memory, string, len);
    return memory;
}

[[nodiscard]] void* operator new(size_t size)
{
    return Memory.mem_alloc(size);
}

[[nodiscard]] void* operator new[](size_t size)
{
    return Memory.mem_alloc(size);
}

[[nodiscard]] void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    return Memory.mem_alloc(size, std::nothrow);
}

[[nodiscard]] void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    return Memory.mem_alloc(size, std::nothrow);
}

[[nodiscard]] void* operator new(size_t size, std::align_val_t alignment)
{
    return Memory.mem_alloc(size, static_cast<size_t>(alignment));
}

[[nodiscard]] void* operator new[](size_t size, std::align_val_t alignment)
{
    return Memory.mem_alloc(size, static_cast<size_t>(alignment));
}

[[nodiscard]] void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return Memory.mem_alloc(size, static_cast<size_t>(alignment), std::nothrow);
}

[[nodiscard]] void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return Memory.mem_alloc(size, static_cast<size_t>(alignment), std::nothrow);
}

void operator delete(void* ptr) noexcept
{
    Memory.mem_free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    Memory.mem_free(ptr);
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept
{
    Memory.mem_free(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void* ptr, std::align_val_t alignment) noexcept
{
    Memory.mem_free(ptr, static_cast<size_t>(alignment));
}

void operator delete(void* ptr, size_t) noexcept
{
    Memory.mem_free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    Memory.mem_free(ptr);
}

void operator delete(void* ptr, size_t, std::align_val_t alignment) noexcept
{
    Memory.mem_free(ptr, static_cast<size_t>(alignment));
}

void operator delete[](void* ptr, size_t, std::align_val_t alignment) noexcept
{
    Memory.mem_free(ptr, static_cast<size_t>(alignment));
}

XRCORE_API void* xr_malloc(size_t size)
{
    return Memory.mem_alloc(size);
}

XRCORE_API void* xr_realloc(void* ptr, size_t size)
{
    return Memory.mem_realloc(ptr, size);
}
