// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Infrastructure of xrVulkan: logging, hashing, formats, the hazard state machine, the timeline, deferred
// release, the memory suballocator, upload pages, dynamic buffer heaps, creation-time uploads, descriptor
// set layouts and pools, null resources and query pools.
#include "vk_layer.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <numeric>
#include <unordered_set>

namespace xrvk
{
namespace
{
constexpr u64 kMB = 1ull << 20;
constexpr u64 kInitAutoFlushBytes = 256 * kMB;  // pending creation-time copies before a forced submit
constexpr size_t kMaxFreeUploadPages = 64;      // pooled frame pages kept after a spike (512 MB)
constexpr size_t kMaxFreeInitPages = 4;
constexpr u64 kDynamicGrowthWarning = 1024 * kMB;
constexpr VkDeviceSize kLargeGranularity = 64 * 1024;
constexpr size_t kMaxFreeLarge = 32;
constexpr u32 kQueriesPerPool = 4096;
constexpr u32 kSetsPerPool = 2048;
// Small device allocations get their own blocks (DOOM's small-alloc heaps): they never fragment the
// big blocks the render targets and textures live in.
constexpr VkDeviceSize kSmallAllocation = 64 * 1024;
constexpr VkDeviceSize kSmallBlockSize = 16 * kMB;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    if (alignment <= 1)
        return value;
    if (!(alignment & (alignment - 1)))
        return (value + alignment - 1) & ~(alignment - 1);
    return (value + alignment - 1) / alignment * alignment;
}

// ---- logging ----------------------------------------------------------------------------------

std::atomic<LogSink> g_logSink{nullptr};
std::mutex g_logLock;
std::mutex g_onceLock;

std::unordered_set<const void*>& OnceKeys()
{
    // Never destroyed: LogOnce may run while other statics are torn down.
    static auto* keys = new std::unordered_set<const void*>();
    return *keys;
}

void WriteLine(const char* fmt, va_list args)
{
    constexpr char kPrefix[] = "[vulkan] ";
    constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
    // The engine log colours a line by its first character: a leading "! ", "~ " or "* " marker of the
    // message goes in front of the prefix.
    constexpr size_t kMarkerLength = 2;
    char text[2048];
    const int written = std::vsnprintf(text, sizeof(text), fmt ? fmt : "", args);
    const size_t textLength = written < 0 ? 0 : (std::min)(size_t(written), sizeof(text) - 1);
    size_t marker = 0;
    if (textLength >= kMarkerLength && (text[0] == '!' || text[0] == '~' || text[0] == '*') && text[1] == ' ')
        marker = kMarkerLength;

    char line[sizeof(text) + kPrefixLength + 2];
    size_t length = 0;
    std::memcpy(line, text, marker);
    length += marker;
    std::memcpy(line + length, kPrefix, kPrefixLength);
    length += kPrefixLength;
    std::memcpy(line + length, text + marker, textLength - marker);
    length += textLength - marker;
    while (length > kPrefixLength + marker && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        --length;
    line[length] = 0;

    const LogSink sink = g_logSink.load(std::memory_order_acquire);
    try
    {
        std::lock_guard lock(g_logLock);
        if (sink)
            sink(line);
        line[length] = '\n';
        line[length + 1] = 0;
        OutputDebugStringA(line);
    }
    catch (...)
    {
    }
}

// ---- hashing (xxHash64, Yann Collet's algorithm) ----------------------------------------------

constexpr u64 kPrime1 = 0x9E3779B185EBCA87ull;
constexpr u64 kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr u64 kPrime3 = 0x165667B19E3779F9ull;
constexpr u64 kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr u64 kPrime5 = 0x27D4EB2F165667C5ull;

u64 Read64(const u8* p)
{
    u64 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

u32 Read32(const u8* p)
{
    u32 value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

u64 Round(u64 acc, u64 input)
{
    acc += input * kPrime2;
    acc = std::rotl(acc, 31);
    return acc * kPrime1;
}

u64 MergeRound(u64 acc, u64 value)
{
    acc ^= Round(0, value);
    return acc * kPrime1 + kPrime4;
}

// ---- formats ------------------------------------------------------------------------------------

struct FormatTable
{
    FormatInfo entries[192];
    FormatTable();
    void Set(DXGI_FORMAT dxgi, VkFormat vk, u32 bytes, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        u32 blockWidth = 1)
    {
        FormatInfo& f = entries[dxgi];
        f.format = vk;
        f.blockBytes = bytes;
        f.blockWidth = blockWidth;
        f.aspect = aspect;
        f.depth = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        f.stencil = (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        f.compressed = blockWidth > 1;
    }
};

// D24S8 is missing on AMD: the depth formats that need it switch to D32S8 at device creation.
std::atomic<bool> g_d24Supported{true};
FormatTable g_formats;

FormatTable::FormatTable()
{
    constexpr VkImageAspectFlags kDS = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    Set(DXGI_FORMAT_R32G32B32A32_TYPELESS, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    Set(DXGI_FORMAT_R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    Set(DXGI_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, 16);
    Set(DXGI_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT, 16);
    Set(DXGI_FORMAT_R32G32B32_TYPELESS, VK_FORMAT_R32G32B32_SFLOAT, 12);
    Set(DXGI_FORMAT_R32G32B32_FLOAT, VK_FORMAT_R32G32B32_SFLOAT, 12);
    Set(DXGI_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_UINT, 12);
    Set(DXGI_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SINT, 12);
    Set(DXGI_FORMAT_R16G16B16A16_TYPELESS, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
    Set(DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8);
    Set(DXGI_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 8);
    Set(DXGI_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, 8);
    Set(DXGI_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 8);
    Set(DXGI_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, 8);
    Set(DXGI_FORMAT_R32G32_TYPELESS, VK_FORMAT_R32G32_SFLOAT, 8);
    Set(DXGI_FORMAT_R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT, 8);
    Set(DXGI_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, 8);
    Set(DXGI_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SINT, 8);
    Set(DXGI_FORMAT_R32G8X24_TYPELESS, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, kDS);
    Set(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, kDS);
    Set(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, kDS);
    Set(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 8, kDS);
    Set(DXGI_FORMAT_R10G10B10A2_TYPELESS, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4);
    Set(DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4);
    Set(DXGI_FORMAT_R10G10B10A2_UINT, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4);
    Set(DXGI_FORMAT_R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4);
    Set(DXGI_FORMAT_R8G8B8A8_TYPELESS, VK_FORMAT_R8G8B8A8_UNORM, 4);
    Set(DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4);
    Set(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4);
    Set(DXGI_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 4);
    Set(DXGI_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 4);
    Set(DXGI_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 4);
    Set(DXGI_FORMAT_R16G16_TYPELESS, VK_FORMAT_R16G16_SFLOAT, 4);
    Set(DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4);
    Set(DXGI_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 4);
    Set(DXGI_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_UINT, 4);
    Set(DXGI_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 4);
    Set(DXGI_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SINT, 4);
    Set(DXGI_FORMAT_R32_TYPELESS, VK_FORMAT_R32_SFLOAT, 4);
    Set(DXGI_FORMAT_D32_FLOAT, VK_FORMAT_D32_SFLOAT, 4, VK_IMAGE_ASPECT_DEPTH_BIT);
    Set(DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4);
    Set(DXGI_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 4);
    Set(DXGI_FORMAT_R32_SINT, VK_FORMAT_R32_SINT, 4);
    Set(DXGI_FORMAT_R24G8_TYPELESS, VK_FORMAT_D24_UNORM_S8_UINT, 4, kDS);
    Set(DXGI_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 4, kDS);
    Set(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, VK_FORMAT_D24_UNORM_S8_UINT, 4, kDS);
    Set(DXGI_FORMAT_X24_TYPELESS_G8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 4, kDS);
    Set(DXGI_FORMAT_R8G8_TYPELESS, VK_FORMAT_R8G8_UNORM, 2);
    Set(DXGI_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 2);
    Set(DXGI_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UINT, 2);
    Set(DXGI_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, 2);
    Set(DXGI_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SINT, 2);
    Set(DXGI_FORMAT_R16_TYPELESS, VK_FORMAT_R16_SFLOAT, 2);
    Set(DXGI_FORMAT_R16_FLOAT, VK_FORMAT_R16_SFLOAT, 2);
    Set(DXGI_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, 2, VK_IMAGE_ASPECT_DEPTH_BIT);
    Set(DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 2);
    Set(DXGI_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, 2);
    Set(DXGI_FORMAT_R16_SNORM, VK_FORMAT_R16_SNORM, 2);
    Set(DXGI_FORMAT_R16_SINT, VK_FORMAT_R16_SINT, 2);
    Set(DXGI_FORMAT_R8_TYPELESS, VK_FORMAT_R8_UNORM, 1);
    Set(DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1);
    Set(DXGI_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 1);
    Set(DXGI_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, 1);
    Set(DXGI_FORMAT_R8_SINT, VK_FORMAT_R8_SINT, 1);
    Set(DXGI_FORMAT_A8_UNORM, VK_FORMAT_R8_UNORM, 1);
    Set(DXGI_FORMAT_R9G9B9E5_SHAREDEXP, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4);
    Set(DXGI_FORMAT_BC1_TYPELESS, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC1_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC1_UNORM_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC2_TYPELESS, VK_FORMAT_BC2_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC2_UNORM, VK_FORMAT_BC2_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC2_UNORM_SRGB, VK_FORMAT_BC2_SRGB_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC3_TYPELESS, VK_FORMAT_BC3_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC3_UNORM, VK_FORMAT_BC3_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC3_UNORM_SRGB, VK_FORMAT_BC3_SRGB_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC4_TYPELESS, VK_FORMAT_BC4_UNORM_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC4_UNORM, VK_FORMAT_BC4_UNORM_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC4_SNORM, VK_FORMAT_BC4_SNORM_BLOCK, 8, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC5_TYPELESS, VK_FORMAT_BC5_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC5_UNORM, VK_FORMAT_BC5_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC5_SNORM, VK_FORMAT_BC5_SNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_B5G6R5_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16, 2);
    Set(DXGI_FORMAT_B5G5R5A1_UNORM, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2);
    Set(DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4);
    Set(DXGI_FORMAT_B8G8R8X8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4);
    Set(DXGI_FORMAT_B8G8R8A8_TYPELESS, VK_FORMAT_B8G8R8A8_UNORM, 4);
    Set(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4);
    Set(DXGI_FORMAT_B8G8R8X8_TYPELESS, VK_FORMAT_B8G8R8A8_UNORM, 4);
    Set(DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4);
    Set(DXGI_FORMAT_BC6H_TYPELESS, VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC6H_UF16, VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC6H_SF16, VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC7_TYPELESS, VK_FORMAT_BC7_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC7_UNORM, VK_FORMAT_BC7_UNORM_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_BC7_UNORM_SRGB, VK_FORMAT_BC7_SRGB_BLOCK, 16, VK_IMAGE_ASPECT_COLOR_BIT, 4);
    Set(DXGI_FORMAT_B4G4R4A4_UNORM, VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2);
    for (u32 i = 0; i < 192; ++i)
    {
        FormatInfo& f = entries[i];
        const DXGI_FORMAT dxgi = DXGI_FORMAT(i);
        f.typeless = TypelessOf(dxgi) == dxgi && f.format != VK_FORMAT_UNDEFINED &&
            dxgi != DXGI_FORMAT_R11G11B10_FLOAT && dxgi != DXGI_FORMAT_R9G9B9E5_SHAREDEXP &&
            dxgi != DXGI_FORMAT_B5G6R5_UNORM && dxgi != DXGI_FORMAT_B5G5R5A1_UNORM &&
            dxgi != DXGI_FORMAT_B8G8R8A8_UNORM && dxgi != DXGI_FORMAT_B8G8R8X8_UNORM &&
            dxgi != DXGI_FORMAT_A8_UNORM && dxgi != DXGI_FORMAT_B4G4R4A4_UNORM;
        f.srgb = f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB ||
            f.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f.format == VK_FORMAT_BC2_SRGB_BLOCK ||
            f.format == VK_FORMAT_BC3_SRGB_BLOCK || f.format == VK_FORMAT_BC7_SRGB_BLOCK;
    }
}

FormatInfo g_d32s8For24 = [] {
    FormatInfo f;
    f.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    f.blockBytes = 8;
    f.depth = f.stencil = true;
    return f;
}();
FormatInfo g_unknownFormat;

constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT |
    VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT |
    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
} // namespace

// ============================================================================================
// Logging, hashing, formats
// ============================================================================================

void SetLogSink(LogSink sink)
{
    g_logSink.store(sink, std::memory_order_release);
}

void Log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

void LogOnce(const void* key, const char* fmt, ...)
{
    bool first = true;
    try
    {
        std::lock_guard lock(g_onceLock);
        first = OnceKeys().insert(key).second;
    }
    catch (...)
    {
    }
    if (!first)
        return;
    va_list args;
    va_start(args, fmt);
    WriteLine(fmt, args);
    va_end(args);
}

u64 Hash64(const void* data, size_t size, u64 seed)
{
    if (!data)
        size = 0;
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + size;
    u64 h;
    if (size >= 32)
    {
        const u8* const limit = end - 32;
        u64 v1 = seed + kPrime1 + kPrime2;
        u64 v2 = seed + kPrime2;
        u64 v3 = seed;
        u64 v4 = seed - kPrime1;
        do
        {
            v1 = Round(v1, Read64(p));
            v2 = Round(v2, Read64(p + 8));
            v3 = Round(v3, Read64(p + 16));
            v4 = Round(v4, Read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = MergeRound(h, v1);
        h = MergeRound(h, v2);
        h = MergeRound(h, v3);
        h = MergeRound(h, v4);
    }
    else
        h = seed + kPrime5;
    h += u64(size);
    while (end - p >= 8)
    {
        h ^= Round(0, Read64(p));
        h = std::rotl(h, 27) * kPrime1 + kPrime4;
        p += 8;
    }
    if (end - p >= 4)
    {
        h ^= u64(Read32(p)) * kPrime1;
        h = std::rotl(h, 23) * kPrime2 + kPrime3;
        p += 4;
    }
    while (p < end)
    {
        h ^= u64(*p) * kPrime5;
        h = std::rotl(h, 11) * kPrime1;
        ++p;
    }
    h ^= h >> 33;
    h *= kPrime2;
    h ^= h >> 29;
    h *= kPrime3;
    h ^= h >> 32;
    return h;
}

const FormatInfo& Format(DXGI_FORMAT format)
{
    const u32 index = u32(format);
    if (index >= 192)
        return g_unknownFormat;
    const FormatInfo& f = g_formats.entries[index];
    if (f.format == VK_FORMAT_D24_UNORM_S8_UINT && !g_d24Supported.load(std::memory_order_relaxed))
        return g_d32s8For24;
    return f;
}

VkFormat ViewFormat(DXGI_FORMAT format, bool readStencil)
{
    const FormatInfo& f = Format(format);
    if (!f.depth)
        return f.format;
    // Depth images are viewed with their own format; the aspect picks depth or stencil.
    (void)readStencil;
    return f.format;
}

bool IsDepthFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT TypelessOf(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return DXGI_FORMAT_R32G32B32_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
        return DXGI_FORMAT_R8G8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return DXGI_FORMAT_R8_TYPELESS;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return DXGI_FORMAT_BC1_TYPELESS;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return DXGI_FORMAT_BC2_TYPELESS;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return DXGI_FORMAT_BC3_TYPELESS;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return DXGI_FORMAT_BC4_TYPELESS;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
        return DXGI_FORMAT_BC5_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
        return DXGI_FORMAT_BC6H_TYPELESS;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return DXGI_FORMAT_BC7_TYPELESS;
    default:
        return format;
    }
}

void InitFormatSupport(VkPhysicalDevice physical, bool allowD24)
{
    VkFormatProperties props{};
    g_vk.vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &props);
    const bool d24 = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    g_d24Supported.store(d24 && allowD24, std::memory_order_relaxed);
    if (!d24)
        Log("* D24S8 depth is not supported by the device: D32S8 stands in");
    else if (!allowD24)
        Log("* D32S8 stands in for D24S8 depth");
}

// ============================================================================================
// Hazards
// ============================================================================================

bool IsWriteAccess(VkAccessFlags2 access)
{
    return (access & kWriteAccess) != 0;
}

bool Transition(ImageState& state, VkImageLayout layout, VkPipelineStageFlags2 stages, VkAccessFlags2 access,
    VkPipelineStageFlags2& srcStages, VkAccessFlags2& srcAccess, VkImageLayout& oldLayout)
{
    const VkAccessFlags2 writes = access & kWriteAccess;
    oldLayout = state.layout;
    // Buffers (no layout) pass UNDEFINED for both.
    if (layout != state.layout && layout != VK_IMAGE_LAYOUT_UNDEFINED)
    {
        srcStages = state.writeStages | state.readStages;
        srcAccess = state.writeAccess;
        state.layout = layout;
        // The transition is a write ordered before `stages`; later accesses elsewhere chain on it.
        state.writeStages = stages;
        state.writeAccess = writes;
        state.readStages = writes ? VK_PIPELINE_STAGE_2_NONE : stages;
        state.readAccess = writes ? VK_ACCESS_2_NONE : access;
        return true;
    }
    if (writes)
    {
        srcStages = state.writeStages | state.readStages;
        srcAccess = state.writeAccess;
        state.writeStages = stages;
        state.writeAccess = writes;
        state.readStages = VK_PIPELINE_STAGE_2_NONE;
        state.readAccess = VK_ACCESS_2_NONE;
        return srcStages != VK_PIPELINE_STAGE_2_NONE;
    }
    // A read: nothing to wait for without a write, or when the write is already visible to it.
    if (state.writeStages == VK_PIPELINE_STAGE_2_NONE ||
        (!(stages & ~state.readStages) && !(access & ~state.readAccess)))
    {
        state.readStages |= stages;
        state.readAccess |= access;
        return false;
    }
    srcStages = state.writeStages;
    srcAccess = state.writeAccess;
    state.readStages |= stages;
    state.readAccess |= access;
    return true;
}

// ============================================================================================
// Synchronisation
// ============================================================================================

bool Timeline::Init(VkDevice device)
{
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    const VkResult result = g_vk.vkCreateSemaphore(device, &info, nullptr, &m_semaphore);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateSemaphore (timeline) failed: %s", ResultText(result));
        return false;
    }
    m_next.store(1, std::memory_order_release);
    m_completed.store(0, std::memory_order_release);
    return true;
}

void Timeline::Shutdown(VkDevice device)
{
    if (m_semaphore)
        g_vk.vkDestroySemaphore(device, m_semaphore, nullptr);
    m_semaphore = VK_NULL_HANDLE;
}

u64 Timeline::Completed(VkDevice device)
{
    const u64 known = m_completed.load(std::memory_order_acquire);
    if (known + 1 >= m_next.load(std::memory_order_acquire))
        return known;
    u64 value = 0;
    if (g_vk.vkGetSemaphoreCounterValue(device, m_semaphore, &value) != VK_SUCCESS)
        return known;
    u64 previous = known;
    while (value > previous && !m_completed.compare_exchange_weak(previous, value, std::memory_order_acq_rel))
    {
    }
    return (std::max)(value, previous);
}

void Timeline::Wait(VkDevice device, u64 value)
{
    if (!value || value <= m_completed.load(std::memory_order_acquire))
        return;
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &m_semaphore;
    info.pValues = &value;
    // A lost device never signals: give up after 10 s instead of hanging the game.
    const VkResult result = g_vk.vkWaitSemaphores(device, &info, 10ull * 1000 * 1000 * 1000);
    if (result == VK_TIMEOUT)
    {
        static constexpr char kFormat[] = "! timeline wait for %llu timed out (GPU hang?)";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(value));
        return;
    }
    u64 previous = m_completed.load(std::memory_order_acquire);
    while (value > previous && !m_completed.compare_exchange_weak(previous, value, std::memory_order_acq_rel))
    {
    }
}

void DeferredReleaser::Defer(std::function<void()>&& action)
{
    const u64 value = m_timeline ? m_timeline->NextValue() : 0;
    try
    {
        std::lock_guard lock(m_lock);
        m_entries.push_back({value, std::move(action)});
    }
    catch (...)
    {
        // Out of memory: leaking is the safe failure (releasing now may free what the GPU reads).
        static constexpr char kFormat[] = "! deferred releaser out of memory, an object leaks";
        LogOnce(kFormat, kFormat);
    }
}

void DeferredReleaser::Collect(bool all)
{
    std::vector<Entry> ready;
    {
        std::lock_guard lock(m_lock);
        if (m_entries.empty())
            return;
        const u64 completed = all || !m_device ? ~0ull : m_timeline->Completed(m_device->vk);
        // Entries are pushed with non-decreasing values per thread; scan all of them (values from
        // different threads interleave).
        size_t keep = 0;
        for (size_t i = 0; i < m_entries.size(); ++i)
        {
            // Value v means "after the submission with value v": complete once completed >= v.
            if (all || m_entries[i].value <= completed)
                ready.push_back(std::move(m_entries[i]));
            else
                m_entries[keep++] = std::move(m_entries[i]);
        }
        m_entries.resize(keep);
    }
    for (Entry& entry : ready)
        entry.action();
}

// ============================================================================================
// Memory
// ============================================================================================

bool MemoryAllocator::Init(Device* device)
{
    m_device = device;
    g_vk.vkGetPhysicalDeviceMemoryProperties(device->physical, &m_props);
    m_bufferImageGranularity = (std::max)(VkDeviceSize(1), device->limits.bufferImageGranularity);

    auto find = [&](VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoid) {
        u32 best = ~0u;
        int bestScore = -1;
        for (u32 i = 0; i < m_props.memoryTypeCount; ++i)
        {
            const VkMemoryPropertyFlags flags = m_props.memoryTypes[i].propertyFlags;
            if ((flags & required) != required)
                continue;
            // Device-coherent / uncached AMD types are for debugging; protected memory is unusable.
            if (flags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_PROTECTED_BIT))
                continue;
            int score = 1;
            score += std::popcount(flags & preferred) * 4;
            score -= std::popcount(flags & avoid) * 3;
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    };

    m_classType[u32(MemoryClass::Device)] = find(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    m_classType[u32(MemoryClass::Staging)] = find(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    m_classType[u32(MemoryClass::Readback)] = find(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Resizable BAR: a DEVICE_LOCAL | HOST_VISIBLE heap close to the VRAM size (the plain 256 MB BAR is too
    // small for the frame data of 3 frames in flight and stays unused).
    u32 rebar = ~0u;
    VkDeviceSize vram = 0;
    for (u32 h = 0; h < m_props.memoryHeapCount; ++h)
    {
        if (m_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            vram = (std::max)(vram, m_props.memoryHeaps[h].size);
    }
    for (u32 i = 0; i < m_props.memoryTypeCount; ++i)
    {
        const VkMemoryPropertyFlags flags = m_props.memoryTypes[i].propertyFlags;
        constexpr VkMemoryPropertyFlags kWanted =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((flags & kWanted) != kWanted || (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD))
            continue;
        const VkDeviceSize heap = m_props.memoryHeaps[m_props.memoryTypes[i].heapIndex].size;
        if (heap >= 1024 * kMB && heap * 2 >= vram)
        {
            rebar = i;
            device->caps.rebarBytes = heap;
            break;
        }
    }
    const bool rebarAllowed = !(device->disabledFeatures & NativeFeatureRebar);
    if (rebar != ~0u && rebarAllowed)
    {
        m_classType[u32(MemoryClass::Upload)] = rebar;
        hostVisibleDevice = true;
    }
    else
        m_classType[u32(MemoryClass::Upload)] = m_classType[u32(MemoryClass::Staging)];
    device->caps.rebar = hostVisibleDevice;

    for (u32 c = 0; c < u32(MemoryClass::Count); ++c)
    {
        if (m_classType[c] == ~0u)
        {
            Log("! no memory type for class %u", c);
            return false;
        }
    }
    return true;
}

void MemoryAllocator::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (Block& block : m_blocks)
    {
        if (block.memory)
            g_vk.vkFreeMemory(m_device->vk, block.memory, nullptr);
    }
    m_blocks.clear();
}

u32 MemoryAllocator::TypeFor(MemoryClass cls, u32 typeBits) const
{
    const u32 preferred = m_classType[u32(cls)];
    if (preferred != ~0u && (typeBits & (1u << preferred)))
        return preferred;
    // The resource cannot live in the class's type: the closest type it allows.
    const VkMemoryPropertyFlags wanted = m_props.memoryTypes[preferred].propertyFlags;
    u32 best = ~0u;
    int bestScore = -1000;
    for (u32 i = 0; i < m_props.memoryTypeCount; ++i)
    {
        if (!(typeBits & (1u << i)))
            continue;
        const VkMemoryPropertyFlags flags = m_props.memoryTypes[i].propertyFlags;
        if (cls != MemoryClass::Device && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        const int score = std::popcount(flags & wanted) * 2 - std::popcount(flags & ~wanted);
        if (score > bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

bool MemoryAllocator::AllocateDedicated(const VkMemoryRequirements& req, u32 type, VkImage image, VkBuffer buffer,
    Allocation& out)
{
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = req.size;
    info.memoryTypeIndex = type;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    dedicated.buffer = buffer;
    if (image || buffer)
        info.pNext = &dedicated;
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    // Buffers carry device addresses with ray queries; any memory a buffer may be bound to needs the flag.
    if (m_device->caps.rayQuery)
    {
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        flagsInfo.pNext = info.pNext;
        info.pNext = &flagsInfo;
    }
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    if (m_device->memoryPriority)
    {
        // Render targets and other dedicated images stay resident first (DOOM's top priority bucket).
        priority.priority = image ? 1.0f : 0.5f;
        priority.pNext = info.pNext;
        info.pNext = &priority;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkAllocateMemory(m_device->vk, &info, nullptr, &memory);
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkAllocateMemory (%llu MB, type %u) failed: %s";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(req.size >> 20), type, ResultText(result));
        return false;
    }
    out = {};
    out.memory = memory;
    out.offset = 0;
    out.size = req.size;
    out.type = type;
    out.block = ~0u;
    if (m_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        void* cpu = nullptr;
        if (g_vk.vkMapMemory(m_device->vk, memory, 0, VK_WHOLE_SIZE, 0, &cpu) != VK_SUCCESS)
        {
            g_vk.vkFreeMemory(m_device->vk, memory, nullptr);
            return false;
        }
        out.cpu = static_cast<u8*>(cpu);
    }
    m_heapUse[m_props.memoryTypes[type].heapIndex].fetch_add(req.size, std::memory_order_relaxed);
    return true;
}

bool MemoryAllocator::Allocate(const VkMemoryRequirements& req, MemoryClass cls, bool dedicated, VkImage image,
    VkBuffer buffer, Allocation& out)
{
    const u32 type = TypeFor(cls, req.memoryTypeBits);
    if (type == ~0u)
    {
        static constexpr char kFormat[] = "! no memory type fits a resource (bits 0x%X, class %u)";
        LogOnce(kFormat, kFormat, req.memoryTypeBits, unsigned(cls));
        return false;
    }
    const bool big = req.size >= kDedicatedThreshold;
    if (dedicated || big)
        return AllocateDedicated(req, type, dedicated ? image : VK_NULL_HANDLE, dedicated ? buffer : VK_NULL_HANDLE, out);

    // Buffers and images share no block: bufferImageGranularity never matters (DOOM aligns to it instead).
    const bool linear = buffer != VK_NULL_HANDLE;
    const bool isSmall = req.size <= kSmallAllocation;
    const u32 kind = (linear ? 1u : 0u) | (isSmall ? 2u : 0u);
    const VkDeviceSize heapSize = m_props.memoryHeaps[m_props.memoryTypes[type].heapIndex].size;
    VkDeviceSize blockSize = isSmall ? kSmallBlockSize : (heapSize < 4096 * kMB ? kSmallHeapBlockSize : kBlockSize);
    blockSize = (std::max)(blockSize, AlignUp(req.size, 64 * 1024));
    const VkDeviceSize alignment = (std::max)(req.alignment, VkDeviceSize(16));

    std::lock_guard lock(m_lock);
    for (u32 b = 0; b < m_blocks.size(); ++b)
    {
        Block& block = m_blocks[b];
        if (!block.memory || block.type != type || block.kind != kind)
            continue;
        for (size_t i = 0; i < block.free.size(); ++i)
        {
            auto& [offset, size] = block.free[i];
            const VkDeviceSize start = AlignUp(offset, alignment);
            if (start + req.size > offset + size)
                continue;
            const VkDeviceSize end = start + req.size;
            const VkDeviceSize tail = offset + size - end;
            const VkDeviceSize head = start - offset;
            if (head && tail)
            {
                size = head;
                block.free.insert(block.free.begin() + i + 1, {end, tail});
            }
            else if (head)
                size = head;
            else if (tail)
            {
                offset = end;
                size = tail;
            }
            else
                block.free.erase(block.free.begin() + i);
            out = {};
            out.memory = block.memory;
            out.offset = start;
            out.size = req.size;
            out.cpu = block.cpu ? block.cpu + start : nullptr;
            out.type = type;
            out.block = b;
            block.used += req.size;
            return true;
        }
    }

    // A new block.
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = blockSize;
    info.memoryTypeIndex = type;
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if (m_device->caps.rayQuery)
    {
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        info.pNext = &flagsInfo;
    }
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    if (m_device->memoryPriority)
    {
        // Small allocations (constants, descriptors-sized data) always get the top bucket (DOOM).
        priority.priority = isSmall ? 1.0f : 0.5f;
        priority.pNext = info.pNext;
        info.pNext = &priority;
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkAllocateMemory(m_device->vk, &info, nullptr, &memory);
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkAllocateMemory (block %llu MB, type %u) failed: %s";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(blockSize >> 20), type, ResultText(result));
        return false;
    }
    Block block;
    block.memory = memory;
    block.size = blockSize;
    block.type = type;
    block.kind = kind;
    if (m_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        void* cpu = nullptr;
        if (g_vk.vkMapMemory(m_device->vk, memory, 0, VK_WHOLE_SIZE, 0, &cpu) != VK_SUCCESS)
        {
            g_vk.vkFreeMemory(m_device->vk, memory, nullptr);
            return false;
        }
        block.cpu = static_cast<u8*>(cpu);
    }
    const VkDeviceSize used = AlignUp(req.size, 16);
    if (used < blockSize)
        block.free.push_back({used, blockSize - used});
    block.used = req.size;
    m_heapUse[m_props.memoryTypes[type].heapIndex].fetch_add(blockSize, std::memory_order_relaxed);
    u32 index = ~0u;
    for (u32 b = 0; b < m_blocks.size(); ++b)
    {
        if (!m_blocks[b].memory)
        {
            index = b;
            break;
        }
    }
    if (index == ~0u)
    {
        index = u32(m_blocks.size());
        m_blocks.push_back(std::move(block));
    }
    else
        m_blocks[index] = std::move(block);
    out = {};
    out.memory = memory;
    out.offset = 0;
    out.size = req.size;
    out.cpu = m_blocks[index].cpu;
    out.type = type;
    out.block = index;
    return true;
}

void MemoryAllocator::Free(const Allocation& allocation)
{
    if (!allocation.memory)
        return;
    const u32 heap = m_props.memoryTypes[allocation.type].heapIndex;
    if (allocation.block == ~0u)
    {
        g_vk.vkFreeMemory(m_device->vk, allocation.memory, nullptr);
        m_heapUse[heap].fetch_sub(allocation.size, std::memory_order_relaxed);
        return;
    }
    std::lock_guard lock(m_lock);
    if (allocation.block >= m_blocks.size())
        return;
    Block& block = m_blocks[allocation.block];
    if (block.memory != allocation.memory)
        return;
    // Insert sorted and coalesce with the neighbours.
    auto& free = block.free;
    const VkDeviceSize start = allocation.offset;
    const VkDeviceSize size = AlignUp(allocation.size, 16);
    auto it = std::lower_bound(free.begin(), free.end(), start,
        [](const std::pair<VkDeviceSize, VkDeviceSize>& range, VkDeviceSize value) { return range.first < value; });
    it = free.insert(it, {start, size});
    if (it + 1 != free.end() && it->first + it->second == (it + 1)->first)
    {
        it->second += (it + 1)->second;
        free.erase(it + 1);
    }
    if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first)
    {
        (it - 1)->second += it->second;
        free.erase(it);
    }
    block.used -= (std::min)(block.used, allocation.size);
    // An empty block goes back to the driver unless it is the last of its kind (no churn at level loads).
    if (!block.used && free.size() == 1 && free[0].first == 0 && free[0].second == block.size)
    {
        u32 sameKind = 0;
        for (const Block& other : m_blocks)
            sameKind += other.memory && other.type == block.type && other.kind == block.kind;
        if (sameKind > 1)
        {
            g_vk.vkFreeMemory(m_device->vk, block.memory, nullptr);
            m_heapUse[heap].fetch_sub(block.size, std::memory_order_relaxed);
            block = Block{};
        }
    }
}

u64 MemoryAllocator::BytesInUse(u32 heap) const
{
    return heap < VK_MAX_MEMORY_HEAPS ? m_heapUse[heap].load(std::memory_order_relaxed) : 0;
}

// ============================================================================================
// Upload memory
// ============================================================================================

namespace
{
constexpr VkBufferUsageFlags kUploadUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

// A host-visible buffer of `size` bytes in `cls` memory, persistently mapped.
bool CreateHostBuffer(Device* device, MemoryClass cls, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
    Allocation& memory)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    if (device->caps.rayQuery)
        info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = g_vk.vkCreateBuffer(device->vk, &info, nullptr, &buffer);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateBuffer (host, %llu KB) failed: %s", static_cast<unsigned long long>(size >> 10), ResultText(result));
        return false;
    }
    VkMemoryRequirements req{};
    g_vk.vkGetBufferMemoryRequirements(device->vk, buffer, &req);
    // Pages are big: their own allocation (no fragmentation of the upload heap).
    if (!device->memory.Allocate(req, cls, true, VK_NULL_HANDLE, buffer, memory) || !memory.cpu)
    {
        g_vk.vkDestroyBuffer(device->vk, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        device->memory.Free(memory);
        memory = {};
        return false;
    }
    result = g_vk.vkBindBufferMemory(device->vk, buffer, memory.memory, memory.offset);
    if (result != VK_SUCCESS)
    {
        g_vk.vkDestroyBuffer(device->vk, buffer, nullptr);
        device->memory.Free(memory);
        buffer = VK_NULL_HANDLE;
        memory = {};
        return false;
    }
    return true;
}

UploadPage* CreatePage(Device* device, MemoryClass cls, VkDeviceSize size)
{
    auto* page = new (std::nothrow) UploadPage();
    if (!page)
        return nullptr;
    if (!CreateHostBuffer(device, cls, size, kUploadUsage, page->buffer, page->memory))
    {
        delete page;
        return nullptr;
    }
    page->size = size;
    return page;
}

void DestroyPage(Device* device, UploadPage* page)
{
    if (!page)
        return;
    if (page->buffer)
        g_vk.vkDestroyBuffer(device->vk, page->buffer, nullptr);
    device->memory.Free(page->memory);
    delete page;
}
} // namespace

bool UploadPagePool::Init(Device* device, MemoryClass cls)
{
    m_device = device;
    m_class = cls;
    return true;
}

void UploadPagePool::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (UploadPage* page : m_free)
        DestroyPage(m_device, page);
    for (UploadPage* page : m_used)
        DestroyPage(m_device, page);
    m_free.clear();
    m_used.clear();
    m_frameValues.clear();
}

UploadPage* UploadPagePool::Acquire(VkDeviceSize minSize, u64 frame)
{
    std::lock_guard lock(m_lock);
    const bool dedicated = minSize > kUploadPageSize;
    UploadPage* page = nullptr;
    if (!dedicated)
    {
        for (size_t i = 0; i < m_free.size(); ++i)
        {
            if (!m_free[i]->dedicated)
            {
                page = m_free[i];
                m_free.erase(m_free.begin() + i);
                break;
            }
        }
    }
    if (!page)
    {
        page = CreatePage(m_device, m_class, dedicated ? AlignUp(minSize, kLargeGranularity) : kUploadPageSize);
        if (!page)
            return nullptr;
        page->dedicated = dedicated;
    }
    page->frame = frame;
    m_bytesInUse.fetch_add(page->size, std::memory_order_relaxed);
    try
    {
        m_used.push_back(page);
    }
    catch (...)
    {
        DestroyPage(m_device, page);
        return nullptr;
    }
    return page;
}

void UploadPagePool::EndFrame(u64 frame, u64 timelineValue)
{
    std::lock_guard lock(m_lock);
    try
    {
        m_frameValues.push_back({frame, timelineValue});
    }
    catch (...)
    {
    }
}

void UploadPagePool::Recycle()
{
    std::lock_guard lock(m_lock);
    if (m_used.empty())
        return;
    const u64 completed = m_device->timeline.Completed(m_device->vk);
    // The newest frame whose last submission completed: its pages and every older one are free.
    u64 doneFrame = 0;
    size_t drop = 0;
    for (size_t i = 0; i < m_frameValues.size(); ++i)
    {
        if (m_frameValues[i].second > completed)
            break;
        doneFrame = m_frameValues[i].first;
        drop = i + 1;
    }
    m_frameValues.erase(m_frameValues.begin(), m_frameValues.begin() + drop);
    if (!doneFrame)
        return;
    size_t keep = 0;
    for (size_t i = 0; i < m_used.size(); ++i)
    {
        UploadPage* page = m_used[i];
        if (page->frame <= doneFrame)
        {
            m_bytesInUse.fetch_sub(page->size, std::memory_order_relaxed);
            if (page->dedicated || m_free.size() >= kMaxFreeUploadPages)
                DestroyPage(m_device, page);
            else
                m_free.push_back(page);
        }
        else
            m_used[keep++] = page;
    }
    m_used.resize(keep);
}

UploadAlloc UploadAllocator::Allocate(VkDeviceSize size, VkDeviceSize alignment, u64 frame)
{
    UploadAlloc out{};
    if (!m_pool || !size)
        return out;
    alignment = (std::max)(alignment, VkDeviceSize(16));
    if (m_frame != frame)
    {
        // Pages of a frame are tagged with it; a new frame starts a new page.
        m_page = nullptr;
        m_frame = frame;
    }
    VkDeviceSize start = m_page ? AlignUp(m_offset, alignment) : 0;
    if (!m_page || start + size > m_page->size)
    {
        UploadPage* page = m_pool->Acquire(size, frame);
        if (!page)
            return out;
        if (page->dedicated)
        {
            out.buffer = page->buffer;
            out.offset = 0;
            out.cpu = page->memory.cpu;
            out.size = size;
            return out;
        }
        m_page = page;
        m_offset = 0;
        start = 0;
    }
    out.buffer = m_page->buffer;
    out.offset = start;
    out.cpu = m_page->memory.cpu + start;
    out.size = size;
    m_offset = start + size;
    return out;
}

// ---- dynamic buffer heaps -----------------------------------------------------------------------

DynamicHeap::~DynamicHeap()
{
    Device* device = m_device;
    for (Block& block : m_blocks)
    {
        const Block copy = block;
        device->releaser.Defer([device, copy] {
            g_vk.vkDestroyBuffer(device->vk, copy.buffer, nullptr);
            device->memory.Free(copy.memory);
        });
    }
}

u32 DynamicHeap::ClassOf(VkDeviceSize size)
{
    u32 cls = 0;
    while (cls < kClassCount && (256ull << cls) < size)
        ++cls;
    return cls;
}

bool DynamicHeap::CreateBlock(VkDeviceSize size, Block& out)
{
    out = {};
    if (!CreateHostBuffer(m_device, MemoryClass::Upload, size, kUploadUsage, out.buffer, out.memory))
        return false;
    out.size = size;
    try
    {
        m_blocks.push_back(out);
    }
    catch (...)
    {
        g_vk.vkDestroyBuffer(m_device->vk, out.buffer, nullptr);
        m_device->memory.Free(out.memory);
        return false;
    }
    m_bytes += size;
    if (m_bytes > kDynamicGrowthWarning)
    {
        static constexpr char kFormat[] = "~ dynamic buffer versions of one context exceed %llu MB: frames are not ending";
        LogOnce(kFormat, kFormat, static_cast<unsigned long long>(m_bytes >> 20));
    }
    return true;
}

void DynamicHeap::DropBlock(VkBuffer buffer)
{
    for (size_t i = 0; i < m_blocks.size(); ++i)
    {
        if (m_blocks[i].buffer != buffer)
            continue;
        // Only reclaimed (GPU-idle) blocks get here.
        g_vk.vkDestroyBuffer(m_device->vk, m_blocks[i].buffer, nullptr);
        m_device->memory.Free(m_blocks[i].memory);
        m_bytes -= m_blocks[i].size;
        m_blocks.erase(m_blocks.begin() + i);
        return;
    }
}

void DynamicHeap::PushRetired(const UploadAlloc& alloc)
{
    try
    {
        if (m_retiredCount == m_retired.size())
        {
            std::vector<Retired> grown((std::max)(m_retired.size() * 2, size_t(1024)));
            for (size_t i = 0; i < m_retiredCount; ++i)
                grown[i] = m_retired[(m_retiredHead + i) % m_retired.size()];
            m_retired.swap(grown);
            m_retiredHead = 0;
        }
        m_retired[(m_retiredHead + m_retiredCount) % m_retired.size()] = {alloc, m_device->frame.load(std::memory_order_acquire)};
        ++m_retiredCount;
    }
    catch (...)
    {
        // The block is lost; nothing else is affected.
    }
}

void DynamicHeap::Reclaim(u64 frame)
{
    while (m_retiredCount)
    {
        const Retired& entry = m_retired[m_retiredHead];
        // EndFrame(F) waits for F - kFramesInFlight: a version retired in frame R is idle from R + N + 1.
        if (entry.frame + kFramesInFlight + 1 > frame)
            break;
        const u32 cls = ClassOf(entry.alloc.size);
        if (cls < kClassCount)
            m_free[cls].push_back(entry.alloc);
        else
        {
            if (m_freeLarge.size() >= kMaxFreeLarge)
            {
                DropBlock(m_freeLarge.front().buffer);
                m_freeLarge.erase(m_freeLarge.begin());
            }
            m_freeLarge.push_back(entry.alloc);
        }
        m_retiredHead = (m_retiredHead + 1) % m_retired.size();
        --m_retiredCount;
    }
}

bool DynamicHeap::Allocate(VkDeviceSize size, UploadAlloc& out, const UploadAlloc* retire)
{
    try
    {
        std::lock_guard lock(m_lock);
        if (m_orphaned)
            return false;
        Reclaim(m_device->frame.load(std::memory_order_acquire));
        // 256: minUniformBufferOffsetAlignment of every desktop GPU, and dynamic CB offsets need it.
        const VkDeviceSize aligned = AlignUp(size ? size : 1, 256);
        const u32 cls = ClassOf(aligned);
        UploadAlloc block{};
        if (cls < kClassCount)
        {
            std::vector<UploadAlloc>& list = m_free[cls];
            if (!list.empty())
            {
                block = list.back();
                list.pop_back();
            }
            else
            {
                const VkDeviceSize classSize = 256ull << cls;
                if (!m_page.buffer || m_pageUsed + classSize > kUploadPageSize)
                {
                    if (!CreateBlock(kUploadPageSize, m_page))
                        return false;
                    m_pageUsed = 0;
                }
                block.buffer = m_page.buffer;
                block.offset = m_pageUsed;
                block.cpu = m_page.memory.cpu + m_pageUsed;
                m_pageUsed += classSize;
            }
            block.size = 256ull << cls;
        }
        else
        {
            const VkDeviceSize capacity = AlignUp(aligned, kLargeGranularity);
            const auto it = std::find_if(m_freeLarge.begin(), m_freeLarge.end(),
                [capacity](const UploadAlloc& a) { return a.size == capacity; });
            if (it != m_freeLarge.end())
            {
                block = *it;
                m_freeLarge.erase(it);
            }
            else
            {
                Block created;
                if (!CreateBlock(capacity, created))
                    return false;
                block.buffer = created.buffer;
                block.offset = 0;
                block.cpu = created.memory.cpu;
            }
            block.size = capacity;
        }
        out = block;
        ++m_outstanding;
        if (retire)
        {
            --m_outstanding;
            PushRetired(*retire);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool DynamicHeap::Retire(const UploadAlloc& alloc)
{
    std::lock_guard lock(m_lock);
    if (m_outstanding)
        --m_outstanding;
    if (m_orphaned)
        return !m_outstanding;
    PushRetired(alloc);
    return false;
}

bool DynamicHeap::Orphan()
{
    std::lock_guard lock(m_lock);
    m_orphaned = true;
    // The memory goes after the GPU: only buffers of the dead context still name its blocks, and nothing
    // draws with them any more. The object itself stays until their versions are retired.
    Device* device = m_device;
    for (Block& block : m_blocks)
    {
        const Block copy = block;
        device->releaser.Defer([device, copy] {
            g_vk.vkDestroyBuffer(device->vk, copy.buffer, nullptr);
            device->memory.Free(copy.memory);
        });
    }
    m_blocks.clear();
    for (std::vector<UploadAlloc>& list : m_free)
        list.clear();
    m_freeLarge.clear();
    m_retired.clear();
    m_retiredHead = m_retiredCount = 0;
    m_page = {};
    m_pageUsed = 0;
    m_bytes = 0;
    return !m_outstanding;
}

void RetireDynamicVersion(Resource* resource)
{
    if (!resource)
        return;
    DynamicHeap* heap = resource->current.heap;
    if (!heap)
        return;
    resource->current.heap = nullptr;
    if (heap->Retire(resource->current.alloc))
        delete heap;
}

// ============================================================================================
// Creation-time uploads
// ============================================================================================

bool InitUploader::Init(Device* device, u32 family, VkQueue queue, std::mutex* queueLock, bool transferQueue)
{
    m_device = device;
    m_family = family;
    m_queue = queue;
    m_queueLock = queueLock;
    m_transfer = transferQueue;
    if (!m_timeline.Init(device->vk))
        return false;
    for (VkCommandPool& pool : m_pools)
    {
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = family;
        if (g_vk.vkCreateCommandPool(device->vk, &info, nullptr, &pool) != VK_SUCCESS)
            return false;
    }
    return true;
}

void InitUploader::Shutdown()
{
    if (!m_device)
        return;
    Flush();
    m_timeline.Wait(m_device->vk, m_lastSubmitted);
    for (VkCommandPool& pool : m_pools)
    {
        if (pool)
            g_vk.vkDestroyCommandPool(m_device->vk, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    for (UploadPage* page : m_pagesInFlight)
        DestroyPage(m_device, page);
    for (UploadPage* page : m_freePages)
        DestroyPage(m_device, page);
    if (m_page)
        DestroyPage(m_device, m_page);
    m_pagesInFlight.clear();
    m_freePages.clear();
    m_page = nullptr;
    m_timeline.Shutdown(m_device->vk);
    m_device = nullptr;
}

VkCommandBuffer InitUploader::Recording()
{
    if (m_cmd)
        return m_cmd;
    // The pool of this slot was last submitted kFramesInFlight + 1 flushes ago: wait for it, then reset.
    VkCommandPool pool = m_pools[m_poolIndex];
    m_timeline.Wait(m_device->vk, m_poolValue[m_poolIndex]);
    g_vk.vkResetCommandPool(m_device->vk, pool, 0);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (g_vk.vkAllocateCommandBuffers(m_device->vk, &alloc, &m_cmd) != VK_SUCCESS)
    {
        m_cmd = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vk.vkBeginCommandBuffer(m_cmd, &begin);
    return m_cmd;
}

UploadAlloc InitUploader::Staging(VkDeviceSize size, VkDeviceSize alignment)
{
    UploadAlloc out{};
    alignment = (std::max)(alignment, VkDeviceSize(16));
    if (size > kInitUploadPageSize)
    {
        // One page for the whole request; it lives until the submission that reads it completes.
        UploadPage* page = CreatePage(m_device, MemoryClass::Staging, AlignUp(size, kLargeGranularity));
        if (!page)
            return out;
        page->dedicated = true;
        page->frame = m_timeline.NextValue();
        m_pagesInFlight.push_back(page);
        out.buffer = page->buffer;
        out.cpu = page->memory.cpu;
        out.size = size;
        return out;
    }
    VkDeviceSize start = m_page ? AlignUp(m_pageOffset, alignment) : 0;
    if (!m_page || start + size > m_page->size)
    {
        if (m_page)
        {
            m_page->frame = m_timeline.NextValue();
            m_pagesInFlight.push_back(m_page);
        }
        m_page = nullptr;
        // A page whose submission completed is reused.
        const u64 done = m_timeline.Completed(m_device->vk);
        for (size_t i = 0; i < m_pagesInFlight.size(); ++i)
        {
            UploadPage* page = m_pagesInFlight[i];
            if (page->frame > done)
                continue;
            m_pagesInFlight.erase(m_pagesInFlight.begin() + i);
            --i;
            if (page->dedicated || m_freePages.size() >= kMaxFreeInitPages)
                DestroyPage(m_device, page);
            else
                m_freePages.push_back(page);
        }
        if (!m_freePages.empty())
        {
            m_page = m_freePages.back();
            m_freePages.pop_back();
        }
        else
            m_page = CreatePage(m_device, MemoryClass::Staging, kInitUploadPageSize);
        if (!m_page)
            return out;
        m_pageOffset = 0;
        start = 0;
    }
    out.buffer = m_page->buffer;
    out.offset = start;
    out.cpu = m_page->memory.cpu + start;
    out.size = size;
    m_pageOffset = start + size;
    return out;
}

void InitUploader::UploadBuffer(VkBuffer dst, VkDeviceSize dstOffset, const void* data, VkDeviceSize size)
{
    if (!dst || !data || !size)
        return;
    bool flush = false;
    {
        std::lock_guard lock(m_lock);
        const UploadAlloc staging = Staging(size, 16);
        VkCommandBuffer cmd = staging.cpu ? Recording() : VK_NULL_HANDLE;
        if (!cmd)
        {
            static constexpr char kFormat[] = "! creation-time buffer upload dropped (out of memory)";
            LogOnce(kFormat, kFormat);
            return;
        }
        std::memcpy(staging.cpu, data, size_t(size));
        VkBufferCopy region{staging.offset, dstOffset, size};
        g_vk.vkCmdCopyBuffer(cmd, staging.buffer, dst, 1, &region);
        m_pendingBytes += size;
        flush = m_pendingBytes >= kInitAutoFlushBytes;
    }
    if (flush)
        Flush();
}

void InitUploader::ZeroBuffer(VkBuffer dst)
{
    if (!dst)
        return;
    std::lock_guard lock(m_lock);
    if (VkCommandBuffer cmd = Recording())
        g_vk.vkCmdFillBuffer(cmd, dst, 0, VK_WHOLE_SIZE, 0);
}

void InitUploader::UploadTexture(Resource& texture, u32 firstSubresource, u32 count, const D3D11_SUBRESOURCE_DATA* data,
    VkImageLayout finalLayout)
{
    const FormatInfo& fmt = Format(texture.format);
    const u32 blockWidth = fmt.blockWidth;
    const u32 blockBytes = fmt.blockBytes ? fmt.blockBytes : 4;
    bool flush = false;
    {
        std::lock_guard lock(m_lock);
        VkCommandBuffer cmd = Recording();
        if (!cmd)
            return;
        // The whole image: UNDEFINED -> TRANSFER_DST (all subresources are written or cleared below).
        VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = texture.image;
        toDst.subresourceRange = {texture.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toDst;
        g_vk.vkCmdPipelineBarrier2(cmd, &dep);

        std::vector<VkBufferImageCopy> regions;
        regions.reserve(count);
        VkBuffer lastBuffer = VK_NULL_HANDLE;
        auto flushRegions = [&] {
            if (!regions.empty())
                g_vk.vkCmdCopyBufferToImage(cmd, lastBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    u32(regions.size()), regions.data());
            regions.clear();
        };
        for (u32 i = 0; i < count; ++i)
        {
            const D3D11_SUBRESOURCE_DATA& sub = data[i];
            if (!sub.pSysMem)
                continue;
            const u32 index = firstSubresource + i;
            const u32 mip = index % texture.mipLevels;
            const u32 layer = index / texture.mipLevels;
            const u32 w = (std::max)(1u, texture.width >> mip);
            const u32 h = (std::max)(1u, texture.height >> mip);
            const u32 d = (std::max)(1u, texture.depth >> mip);
            const u32 blocksX = (w + blockWidth - 1) / blockWidth;
            const u32 blocksY = (h + blockWidth - 1) / blockWidth;
            const VkDeviceSize rowBytes = VkDeviceSize(blocksX) * blockBytes;
            const VkDeviceSize sliceBytes = rowBytes * blocksY;
            const VkDeviceSize total = sliceBytes * d;
            // Offsets must be multiples of the texel block size and of 4.
            const VkDeviceSize align = std::lcm(VkDeviceSize(blockBytes), VkDeviceSize(16));
            const UploadAlloc staging = Staging(total, align);
            if (!staging.cpu)
            {
                static constexpr char kFormat[] = "! creation-time texture upload dropped (out of memory)";
                LogOnce(kFormat, kFormat);
                continue;
            }
            const u8* src = static_cast<const u8*>(sub.pSysMem);
            const VkDeviceSize srcRow = sub.SysMemPitch ? sub.SysMemPitch : rowBytes;
            const VkDeviceSize srcSlice = sub.SysMemSlicePitch ? sub.SysMemSlicePitch : srcRow * blocksY;
            for (u32 z = 0; z < d; ++z)
            {
                const u8* slice = src + srcSlice * z;
                u8* out = staging.cpu + sliceBytes * z;
                if (srcRow == rowBytes)
                    std::memcpy(out, slice, size_t(sliceBytes));
                else
                {
                    for (u32 y = 0; y < blocksY; ++y)
                        std::memcpy(out + rowBytes * y, slice + srcRow * y, size_t(rowBytes));
                }
            }
            if (staging.buffer != lastBuffer)
            {
                flushRegions();
                lastBuffer = staging.buffer;
            }
            VkBufferImageCopy& region = regions.emplace_back();
            region.bufferOffset = staging.offset;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            // Depth/stencil images take creation data for the depth aspect only (D3D11 allows no more).
            region.imageSubresource = {fmt.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : texture.aspect, mip,
                texture.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 0u : layer, 1};
            region.imageExtent = {w, h, d};
            m_pendingBytes += total;
        }
        flushRegions();

        VkImageMemoryBarrier2 toFinal = toDst;
        toFinal.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toFinal.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // The graphics queue waits for this submission (all commands); the transfer queue has no other stages.
        toFinal.dstStageMask = m_transfer ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toFinal.dstAccessMask = m_transfer ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT;
        toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toFinal.newLayout = finalLayout;
        dep.pImageMemoryBarriers = &toFinal;
        g_vk.vkCmdPipelineBarrier2(cmd, &dep);
        flush = m_pendingBytes >= kInitAutoFlushBytes;
    }
    if (flush)
        Flush();
}

void InitUploader::InitialLayout(Resource& texture, VkImageLayout finalLayout, bool clear)
{
    std::lock_guard lock(m_lock);
    VkCommandBuffer cmd = Recording();
    if (!cmd)
        return;
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {texture.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    if (clear)
    {
        // D3D11 resources start zeroed in practice and some code relies on it.
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        g_vk.vkCmdPipelineBarrier2(cmd, &dep);
        const VkImageSubresourceRange range = barrier.subresourceRange;
        if (texture.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        {
            const VkClearDepthStencilValue value{0.f, 0};
            g_vk.vkCmdClearDepthStencilImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        }
        else
        {
            const VkClearColorValue value{};
            g_vk.vkCmdClearColorImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    barrier.dstStageMask = m_transfer ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = m_transfer ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.newLayout = finalLayout;
    g_vk.vkCmdPipelineBarrier2(cmd, &dep);
}

u64 InitUploader::Flush()
{
    std::lock_guard lock(m_lock);
    if (!m_cmd)
        return 0;
    m_device->Count(Device::StatInitFlushes);
    m_device->Count(Device::StatInitKB, m_pendingBytes >> 10);
    g_vk.vkEndCommandBuffer(m_cmd);
    const u64 value = m_timeline.Take();
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = m_cmd;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = m_timeline.Handle();
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    VkResult result;
    {
        std::unique_lock queueLock(*m_queueLock);
        result = g_vk.vkQueueSubmit2(m_queue, 1, &submit, VK_NULL_HANDLE);
    }
    if (result != VK_SUCCESS)
    {
        m_device->ReportLost(result);
        // Nothing waits for a failed submission: signal it from the host so waits do not hang.
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = m_timeline.Handle();
        info.value = value;
        g_vk.vkSignalSemaphore(m_device->vk, &info);
    }
    m_poolValue[m_poolIndex] = value;
    m_poolIndex = (m_poolIndex + 1) % (kFramesInFlight + 1);
    m_cmd = VK_NULL_HANDLE;
    m_lastSubmitted = value;
    m_pendingBytes = 0;
    if (m_page)
    {
        // The current page stays; its contents up to here are read by `value`.
        m_page->frame = value;
    }
    return value;
}

// ============================================================================================
// Descriptors
// ============================================================================================

bool LayoutCache::Init(Device* device)
{
    m_device = device;
    m_empty = Set(Stage::VS, {});
    return m_empty != nullptr;
}

void LayoutCache::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (auto& [hash, layout] : m_layouts)
    {
        g_vk.vkDestroyPipelineLayout(m_device->vk, layout->layout, nullptr);
        delete layout;
    }
    for (auto& [hash, set] : m_sets)
    {
        if (set->update)
            g_vk.vkDestroyDescriptorUpdateTemplate(m_device->vk, set->update, nullptr);
        g_vk.vkDestroyDescriptorSetLayout(m_device->vk, set->layout, nullptr);
        delete set;
    }
    m_layouts.clear();
    m_sets.clear();
    m_empty = nullptr;
}

const SetLayout* LayoutCache::Set(Stage stage, const std::vector<SetBinding>& bindings)
{
    // The stage is part of the key: stageFlags are part of the layout.
    u64 hash = Hash64(bindings.data(), bindings.size() * sizeof(SetBinding), 0x5e7u + u64(stage));
    std::lock_guard lock(m_lock);
    if (const auto it = m_sets.find(hash); it != m_sets.end())
        return it->second;

    auto* set = new (std::nothrow) SetLayout();
    if (!set)
        return nullptr;
    set->bindings = bindings;
    set->hash = hash;
    const VkShaderStageFlags stageFlags = stage == Stage::CS ? VK_SHADER_STAGE_COMPUTE_BIT :
        stage == Stage::VS ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_MESH_BIT_EXT :
        stage == Stage::GS ? VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TASK_BIT_EXT :
        stage == Stage::HS ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT :
        stage == Stage::DS ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    // Mesh and task bits only where the device has mesh shaders (the layout must not name unknown stages).
    const VkShaderStageFlags allowed = m_device->caps.meshShaders ? ~0u :
        ~VkShaderStageFlags(VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT);

    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    vkBindings.reserve(bindings.size());
    entries.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i)
    {
        const SetBinding& b = bindings[i];
        VkDescriptorSetLayoutBinding& vb = vkBindings.emplace_back();
        vb.binding = b.binding;
        vb.descriptorType = VkTypeOf(b.type);
        vb.descriptorCount = 1;
        vb.stageFlags = stageFlags & allowed;
        VkDescriptorUpdateTemplateEntry& e = entries.emplace_back();
        e.dstBinding = b.binding;
        e.dstArrayElement = 0;
        e.descriptorCount = 1;
        e.descriptorType = vb.descriptorType;
        e.offset = i * sizeof(DescriptorData);
        e.stride = sizeof(DescriptorData);
        if (vb.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            ++set->dynamicCount;
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = u32(vkBindings.size());
    info.pBindings = vkBindings.data();
    VkResult result = g_vk.vkCreateDescriptorSetLayout(m_device->vk, &info, nullptr, &set->layout);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateDescriptorSetLayout failed: %s", ResultText(result));
        delete set;
        return nullptr;
    }
    if (!entries.empty())
    {
        VkDescriptorUpdateTemplateCreateInfo t{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        t.descriptorUpdateEntryCount = u32(entries.size());
        t.pDescriptorUpdateEntries = entries.data();
        t.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        t.descriptorSetLayout = set->layout;
        result = g_vk.vkCreateDescriptorUpdateTemplate(m_device->vk, &t, nullptr, &set->update);
        if (result != VK_SUCCESS)
        {
            Log("! vkCreateDescriptorUpdateTemplate failed: %s", ResultText(result));
            g_vk.vkDestroyDescriptorSetLayout(m_device->vk, set->layout, nullptr);
            delete set;
            return nullptr;
        }
    }
    try
    {
        m_sets.emplace(hash, set);
    }
    catch (...)
    {
        if (set->update)
            g_vk.vkDestroyDescriptorUpdateTemplate(m_device->vk, set->update, nullptr);
        g_vk.vkDestroyDescriptorSetLayout(m_device->vk, set->layout, nullptr);
        delete set;
        return nullptr;
    }
    return set;
}

const PipelineLayout* LayoutCache::Graphics(const SetLayout* const (&sets)[kGraphicsStages], bool partial)
{
    u64 hash = partial ? 0x6a8u : 0x6a7u;
    for (const SetLayout* set : sets)
        hash = HashCombine(hash, set ? set->hash : 0);
    std::lock_guard lock(m_lock);
    if (const auto it = m_layouts.find(hash); it != m_layouts.end())
        return it->second;
    auto* layout = new (std::nothrow) PipelineLayout();
    if (!layout)
        return nullptr;
    VkDescriptorSetLayout handles[kGraphicsStages];
    for (u32 i = 0; i < kGraphicsStages; ++i)
    {
        // A library's layout leaves the other part's sets null (independent sets).
        layout->sets[i] = sets[i] ? sets[i] : (partial ? nullptr : m_empty);
        handles[i] = layout->sets[i] ? layout->sets[i]->layout : VK_NULL_HANDLE;
    }
    // Trailing empty sets are left out (fewer sets to bind).
    u32 count = kGraphicsStages;
    while (count > 1 && (!layout->sets[count - 1] || layout->sets[count - 1] == m_empty))
        --count;
    layout->setCount = count;
    layout->hash = hash;
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    // With pipeline libraries every graphics layout has independent sets: library and linked layouts must agree.
    if (m_device->caps.pipelineLibrary)
        info.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    info.setLayoutCount = count;
    info.pSetLayouts = handles;
    const VkResult result = g_vk.vkCreatePipelineLayout(m_device->vk, &info, nullptr, &layout->layout);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreatePipelineLayout failed: %s", ResultText(result));
        delete layout;
        return nullptr;
    }
    try
    {
        m_layouts.emplace(hash, layout);
    }
    catch (...)
    {
        g_vk.vkDestroyPipelineLayout(m_device->vk, layout->layout, nullptr);
        delete layout;
        return nullptr;
    }
    return layout;
}

const PipelineLayout* LayoutCache::Compute(const SetLayout* set)
{
    const u64 hash = HashCombine(0xc0u, set ? set->hash : 0);
    std::lock_guard lock(m_lock);
    if (const auto it = m_layouts.find(hash); it != m_layouts.end())
        return it->second;
    auto* layout = new (std::nothrow) PipelineLayout();
    if (!layout)
        return nullptr;
    layout->sets[0] = set ? set : m_empty;
    layout->setCount = 1;
    layout->hash = hash;
    VkDescriptorSetLayout handle = layout->sets[0]->layout;
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &handle;
    const VkResult result = g_vk.vkCreatePipelineLayout(m_device->vk, &info, nullptr, &layout->layout);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreatePipelineLayout (compute) failed: %s", ResultText(result));
        delete layout;
        return nullptr;
    }
    try
    {
        m_layouts.emplace(hash, layout);
    }
    catch (...)
    {
        g_vk.vkDestroyPipelineLayout(m_device->vk, layout->layout, nullptr);
        delete layout;
        return nullptr;
    }
    return layout;
}

void DescriptorAllocator::Init(Device* device)
{
    m_device = device;
}

void DescriptorAllocator::Shutdown()
{
    if (!m_device)
        return;
    for (Slot& slot : m_slots)
    {
        for (VkDescriptorPool pool : slot.pools)
        {
            Device* device = m_device;
            device->releaser.Defer([device, pool] { g_vk.vkDestroyDescriptorPool(device->vk, pool, nullptr); });
        }
        slot.pools.clear();
    }
    m_device = nullptr;
}

VkDescriptorPool DescriptorAllocator::NewPool()
{
    // Sized for the engine's sets: a few CBs, many textures, some samplers per stage set.
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerPool * 6},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerPool * 12},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerPool * 6},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kSetsPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetsPerPool * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, kSetsPerPool / 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerPool / 2},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kSetsPerPool / 8},
    };
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = kSetsPerPool;
    info.poolSizeCount = m_device->caps.rayQuery ? u32(std::size(sizes)) : u32(std::size(sizes)) - 1;
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkCreateDescriptorPool(m_device->vk, &info, nullptr, &pool);
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkCreateDescriptorPool failed: %s";
        LogOnce(kFormat, kFormat, ResultText(result));
        return VK_NULL_HANDLE;
    }
    m_device->Count(Device::StatPools);
    return pool;
}

VkDescriptorSet DescriptorAllocator::Allocate(const SetLayout* layout, u64 frame)
{
    if (!m_device || !layout)
        return VK_NULL_HANDLE;
    Slot& slot = m_slots[frame % (kFramesInFlight + 1)];
    if (slot.frame != frame)
    {
        // This slot last served frame - (kFramesInFlight + 1), which EndFrame made sure completed.
        for (VkDescriptorPool pool : slot.pools)
            g_vk.vkResetDescriptorPool(m_device->vk, pool, 0);
        slot.current = 0;
        slot.frame = frame;
    }
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout->layout;
    for (;;)
    {
        if (slot.current >= slot.pools.size())
        {
            VkDescriptorPool pool = NewPool();
            if (!pool)
                return VK_NULL_HANDLE;
            try
            {
                slot.pools.push_back(pool);
            }
            catch (...)
            {
                g_vk.vkDestroyDescriptorPool(m_device->vk, pool, nullptr);
                return VK_NULL_HANDLE;
            }
        }
        info.descriptorPool = slot.pools[slot.current];
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = g_vk.vkAllocateDescriptorSets(m_device->vk, &info, &set);
        if (result == VK_SUCCESS)
            return set;
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            static constexpr char kFormat[] = "! vkAllocateDescriptorSets failed: %s";
            LogOnce(kFormat, kFormat, ResultText(result));
            return VK_NULL_HANDLE;
        }
        ++slot.current;
    }
}

// ============================================================================================
// Null resources
// ============================================================================================

namespace
{
bool CreateNullImage(Device* device, VkImageType type, u32 layers, VkImageCreateFlags flags, VkImageUsageFlags usage,
    VkImage& image, Allocation& memory)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_vk.vkCreateImage(device->vk, &info, nullptr, &image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    g_vk.vkGetImageMemoryRequirements(device->vk, image, &req);
    if (!device->memory.Allocate(req, MemoryClass::Device, false, image, VK_NULL_HANDLE, memory))
        return false;
    return g_vk.vkBindImageMemory(device->vk, image, memory.memory, memory.offset) == VK_SUCCESS;
}

VkImageView CreateNullView(Device* device, VkImage image, VkImageViewType type, u32 layers)
{
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    g_vk.vkCreateImageView(device->vk, &info, nullptr, &view);
    return view;
}
} // namespace

bool NullResources::Init(Device* device)
{
    // The zero buffer serves unbound CBs and buffer SRVs even with null descriptors (dynamic CB bindings
    // need a buffer).
    VkBufferCreateInfo binfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    binfo.size = 64 * 1024;
    binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (g_vk.vkCreateBuffer(device->vk, &binfo, nullptr, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    g_vk.vkGetBufferMemoryRequirements(device->vk, buffer, &req);
    if (!device->memory.Allocate(req, MemoryClass::Device, false, VK_NULL_HANDLE, buffer, bufferMemory) ||
        g_vk.vkBindBufferMemory(device->vk, buffer, bufferMemory.memory, bufferMemory.offset) != VK_SUCCESS)
        return false;
    {
        std::vector<u8> zeros(64 * 1024, 0);
        device->init.UploadBuffer(buffer, 0, zeros.data(), zeros.size());
    }
    VkBufferViewCreateInfo tinfo{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    tinfo.buffer = buffer;
    tinfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    tinfo.range = VK_WHOLE_SIZE;
    g_vk.vkCreateBufferView(device->vk, &tinfo, nullptr, &texel);
    tinfo.format = VK_FORMAT_R32_UINT;
    g_vk.vkCreateBufferView(device->vk, &tinfo, nullptr, &storageTexel);

    VkSamplerCreateInfo sinfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // D3D11's default sampler: linear, clamp, no anisotropy, LOD 0..max.
    sinfo.magFilter = sinfo.minFilter = VK_FILTER_LINEAR;
    sinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sinfo.addressModeU = sinfo.addressModeV = sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.maxLod = VK_LOD_CLAMP_NONE;
    sinfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    g_vk.vkCreateSampler(device->vk, &sinfo, nullptr, &sampler);

    if (device->nullDescriptor)
        return texel && storageTexel && sampler;

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!CreateNullImage(device, VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, usage, image2d, memory2d) ||
        !CreateNullImage(device, VK_IMAGE_TYPE_3D, 1, 0, usage, image3d, memory3d))
        return false;
    view1d = VK_NULL_HANDLE; // 1D views need a 1D image: 1D slots read the 2D image through a 2D-array fallback
    view2d = CreateNullView(device, image2d, VK_IMAGE_VIEW_TYPE_2D, 1);
    view2dArray = CreateNullView(device, image2d, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
    viewCube = CreateNullView(device, image2d, VK_IMAGE_VIEW_TYPE_CUBE, 6);
    viewCubeArray = device->cubeArray ? CreateNullView(device, image2d, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6) : VK_NULL_HANDLE;
    view3d = CreateNullView(device, image3d, VK_IMAGE_VIEW_TYPE_3D, 1);
    storage2d = view2d;
    // 1D images are separate objects in Vulkan.
    if (CreateNullImage(device, VK_IMAGE_TYPE_1D, 1, 0, usage, image1d, memory1d))
    {
        view1d = CreateNullView(device, image1d, VK_IMAGE_VIEW_TYPE_1D, 1);
        view1dArray = CreateNullView(device, image1d, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
    }
    Resource r2d;
    r2d.image = image2d;
    r2d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    device->init.InitialLayout(r2d, VK_IMAGE_LAYOUT_GENERAL, true);
    Resource r3d;
    r3d.image = image3d;
    r3d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    device->init.InitialLayout(r3d, VK_IMAGE_LAYOUT_GENERAL, true);
    if (image1d)
    {
        Resource r1d;
        r1d.image = image1d;
        r1d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        device->init.InitialLayout(r1d, VK_IMAGE_LAYOUT_GENERAL, true);
    }
    return view2d && view2dArray && viewCube && view3d;
}

void NullResources::Shutdown(Device* device)
{
    const VkImageView views[] = {view1d, view1dArray, view2d, view2dArray, view3d, viewCube, viewCubeArray};
    for (VkImageView view : views)
    {
        if (view)
            g_vk.vkDestroyImageView(device->vk, view, nullptr);
    }
    for (VkImage image : {image1d, image2d, image3d})
    {
        if (image)
            g_vk.vkDestroyImage(device->vk, image, nullptr);
    }
    device->memory.Free(memory1d);
    device->memory.Free(memory2d);
    device->memory.Free(memory3d);
    if (texel)
        g_vk.vkDestroyBufferView(device->vk, texel, nullptr);
    if (storageTexel)
        g_vk.vkDestroyBufferView(device->vk, storageTexel, nullptr);
    if (buffer)
        g_vk.vkDestroyBuffer(device->vk, buffer, nullptr);
    device->memory.Free(bufferMemory);
    if (sampler)
        g_vk.vkDestroySampler(device->vk, sampler, nullptr);
    *this = NullResources{};
}

VkImageView NullResources::ImageFor(VkImageViewType type, bool storage) const
{
    (void)storage;
    switch (type)
    {
    case VK_IMAGE_VIEW_TYPE_1D: return view1d ? view1d : view2d;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return view1dArray ? view1dArray : view2dArray;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return view2dArray;
    case VK_IMAGE_VIEW_TYPE_3D: return view3d;
    case VK_IMAGE_VIEW_TYPE_CUBE: return viewCube;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return viewCubeArray ? viewCubeArray : viewCube;
    default: return view2d;
    }
}

// ============================================================================================
// Queries
// ============================================================================================

bool QueryPoolSet::Init(Device* device)
{
    m_device = device;
    return true;
}

void QueryPoolSet::Shutdown()
{
    std::lock_guard lock(m_lock);
    for (std::vector<Pool>* list : {&m_occlusion, &m_timestamp})
    {
        for (Pool& pool : *list)
            g_vk.vkDestroyQueryPool(m_device->vk, pool.pool, nullptr);
        list->clear();
    }
}

QueryPoolSet::Slot QueryPoolSet::Allocate(VkQueryType type)
{
    std::lock_guard lock(m_lock);
    std::vector<Pool>& list = type == VK_QUERY_TYPE_OCCLUSION ? m_occlusion : m_timestamp;
    for (Pool& pool : list)
    {
        if (pool.free.empty())
            continue;
        const u32 index = pool.free.back();
        pool.free.pop_back();
        // hostQueryReset: the slot is not in use by the GPU (freed slots come back through the releaser).
        g_vk.vkResetQueryPool(m_device->vk, pool.pool, index, 1);
        return {pool.pool, index};
    }
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = type;
    info.queryCount = kQueriesPerPool;
    Pool pool;
    const VkResult result = g_vk.vkCreateQueryPool(m_device->vk, &info, nullptr, &pool.pool);
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkCreateQueryPool failed: %s";
        LogOnce(kFormat, kFormat, ResultText(result));
        return {};
    }
    g_vk.vkResetQueryPool(m_device->vk, pool.pool, 0, kQueriesPerPool);
    pool.free.reserve(kQueriesPerPool);
    for (u32 i = kQueriesPerPool; i-- > 1;)
        pool.free.push_back(i);
    list.push_back(std::move(pool));
    return {list.back().pool, 0};
}

void QueryPoolSet::Free(VkQueryType type, const Slot& slot)
{
    if (!slot.pool)
        return;
    std::lock_guard lock(m_lock);
    std::vector<Pool>& list = type == VK_QUERY_TYPE_OCCLUSION ? m_occlusion : m_timestamp;
    for (Pool& pool : list)
    {
        if (pool.pool == slot.pool)
        {
            pool.free.push_back(slot.index);
            return;
        }
    }
}
} // namespace xrvk
