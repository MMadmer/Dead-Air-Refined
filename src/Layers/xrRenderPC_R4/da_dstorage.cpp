// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// DirectStorage texture loading and the [texload] numbers (da_dstorage.h).

#include "stdafx.h"

#include "da_dstorage.h"

#include <mutex>

#if defined(XR_D3D12)
#include <condition_variable>
#include <deque>
#include <intrin.h>
#include <thread>
#include <winioctl.h>

#include <dstorage.h>

#include "Common/LevelStructure.hpp"
#include "Layers/xrRenderDX12/d12_api.h"
#include "xrCore/Compression/rt_compressor_prefix.h"
// Its inline seek() narrows size_t to int (C4267 in every includer).
#pragma warning(push)
#pragma warning(disable : 4267)
#include "xrCore/stream_reader.h"
#pragma warning(pop)
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
constexpr size_t kPaths = size_t(DaTexPath::Count);
constexpr pcstr kPathNames[kPaths] = {"cpu", "ds raw", "ds lzo", "failed"};
constexpr size_t kKeptBatchLines = 4;

struct TexloadPath
{
    u32 count{};
    double total_ms{};
    float max_ms{};
    // Every load's time, for the p95 of the synchronous groups.
    xr_vector<float> times;
};

struct TexloadGroup
{
    TexloadPath path[kPaths];
    u32 errors{};
    u64 bytes{};
    double read_ms{};
    double create_ms{};
    double codec_ms{};

    void add(const DaTexloadSample& sample, bool keep_times)
    {
        TexloadPath& p = path[size_t(sample.path)];
        ++p.count;
        p.total_ms += sample.total_ms;
        p.max_ms = std::max(p.max_ms, sample.total_ms);
        if (keep_times)
            p.times.push_back(sample.total_ms);
        errors += sample.error ? 1 : 0;
        bytes += sample.bytes;
        read_ms += sample.read_ms;
        create_ms += sample.create_ms;
        codec_ms += sample.codec_ms;
    }

    u32 count() const
    {
        u32 n = 0;
        for (const TexloadPath& p : path)
            n += p.count;
        return n;
    }

    double total_ms() const
    {
        double ms = 0;
        for (const TexloadPath& p : path)
            ms += p.total_ms;
        return ms;
    }
};

struct TexloadState
{
    std::mutex lock;

    TexloadGroup batch;
    string32 batch_label{};
    size_t batch_textures{};
    size_t batch_workers{};
    u64 batch_start{};
    u64 batch_end{};
    bool batch_running{};
    bool batch_unreported{};
    xr_vector<xr_string> batch_lines;

    // Synchronous loads (outside the pool) while a level loads and once it is in play; level_open from
    // the level's final upload to its unload.
    TexloadGroup load;
    TexloadGroup game;
    bool level_open{};
};

TexloadState& texload()
{
    static TexloadState state;
    return state;
}

thread_local bool t_batch_worker = false;
thread_local bool t_fallback = false;
// The last LoadBuffers of this thread went through DirectStorage (DaGeometryTimer's line).
thread_local bool t_geometry_ds = false;
thread_local u32 t_geometry_buffers = 0;
thread_local u64 t_geometry_bytes = 0;

double qpc_ticks_per_ms()
{
    static const double ticks = []
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return double(frequency.QuadPart) / 1000.0;
    }();
    return ticks;
}

float ticks_to_ms(u64 ticks) { return float(double(ticks) / qpc_ticks_per_ms()); }

float p95(xr_vector<float> times)
{
    if (times.empty())
        return 0.f;
    const size_t k = std::min(times.size() - 1, (times.size() * 95 + 99) / 100 - 1);
    std::nth_element(times.begin(), times.begin() + k, times.end());
    return times[k];
}

// "cpu 35: 120.5 ms, max 12.3, p95 8.1; ds raw 0; ..." for the synchronous groups.
xr_string sync_paths(const TexloadGroup& g)
{
    xr_string text;
    for (size_t i = 0; i < kPaths; ++i)
    {
        const TexloadPath& p = g.path[i];
        string128 part;
        if (p.count)
            xr_sprintf(part, "%s%s %u: %.1f ms, max %.1f, p95 %.1f", i ? "; " : "", kPathNames[i], p.count, p.total_ms,
                p.max_ms, p95(p.times));
        else
            xr_sprintf(part, "%s%s 0", i ? "; " : "", kPathNames[i]);
        text += part;
    }
    if (g.errors)
    {
        string32 part;
        xr_sprintf(part, "; errors %u", g.errors);
        text += part;
    }
    return text;
}

// Caller holds the lock.
void report_batch(TexloadState& s, float wait_ms)
{
    const TexloadGroup& g = s.batch;
    string32 errors{};
    if (g.errors)
        xr_sprintf(errors, ", errors %u", g.errors);
    string512 line;
    xr_sprintf(line,
        "batch %s: %zu textures queued, %u loads (ds raw %u, ds lzo %u, cpu %u, failed %u), %.0f MB, wall %.0f ms, "
        "wait %.0f ms, create %.0f ms, read %.0f ms, codec %.0f ms cpu, %zu workers%s",
        s.batch_label, s.batch_textures, g.count(), g.path[size_t(DaTexPath::DsRaw)].count,
        g.path[size_t(DaTexPath::DsLzo)].count, g.path[size_t(DaTexPath::Cpu)].count,
        g.path[size_t(DaTexPath::DsFailed)].count, double(g.bytes) / 1048576.0, ticks_to_ms(s.batch_end - s.batch_start),
        wait_ms, g.create_ms, g.read_ms, g.codec_ms, s.batch_workers, errors);
    Msg("* [texload] %s", line);
    if (s.batch_lines.size() == kKeptBatchLines)
        s.batch_lines.erase(s.batch_lines.begin());
    s.batch_lines.emplace_back(line);
    s.batch_unreported = false;
}

void report_sync(const TexloadGroup& g, pcstr when)
{
    Msg("* [texload] %s: %u sync loads, %.0f MB, %.0f ms (%s)", when, g.count(), double(g.bytes) / 1048576.0,
        g.total_ms(), sync_paths(g).c_str());
}

// The level is in play: loaded, and its precache frames are over.
bool level_in_play() { return g_pGameLevel && g_pGameLevel->bReady && !Device.dwPrecacheFrame; }

// CPU the codec threads spent on the running batch.
void texload_add_codec(double ms)
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    if (s.batch_running)
        s.batch.codec_ms += ms;
}
} // namespace

#if defined(XR_D3D12)
// ============================================================================================================
// DirectStorage (R5)
// ============================================================================================================
namespace
{
constexpr u32 kStagingSize = 64u << 20;
constexpr u32 kStatusSlots = 8192;
constexpr u32 kNoSlot = ~0u;
constexpr u32 kSubmitEvery = 32;
constexpr size_t kHeaderWindow = 256;
constexpr DWORD kWaitTimeoutMs = 30000;
// Custom format: 0x80 | 1 (LZO) | skipped mips << 1.
constexpr u8 kLzoFormat = DSTORAGE_CUSTOM_COMPRESSION_0 | 1;

u32 FourCC(const char* s) { return u32(u8(s[0])) | (u32(u8(s[1])) << 8) | (u32(u8(s[2])) << 16) | (u32(u8(s[3])) << 24); }

// A DDS whose body goes 1:1 into a texture (no conversion) and the mips a load keeps.
struct DdsLayout
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 width = 0, height = 0; // top mip of the file
    u32 mips = 1;              // in the file
    u32 slices = 1;            // array size, 6 per cube
    bool cube = false;
    u32 dataOffset = 128;
    u32 blockBytes = 0; // 0 = not block compressed
    u32 texelBytes = 0;
    u32 skip = 0; // leading mips not loaded

    u32 kept() const { return mips - skip; }
    u32 subresources() const { return kept() * slices; }

    void tight(u32 mip, u64& pitch, u32& rows) const
    {
        const u32 w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
        if (blockBytes)
        {
            pitch = u64(std::max(1u, (w + 3) / 4)) * blockBytes;
            rows = std::max(1u, (h + 3) / 4);
        }
        else
        {
            pitch = u64(w) * texelBytes;
            rows = h;
        }
    }

    u64 mipBytes(u32 mip) const
    {
        u64 pitch;
        u32 rows;
        tight(mip, pitch, rows);
        return pitch * rows;
    }

    u64 sliceBytes() const
    {
        u64 bytes = 0;
        for (u32 m = 0; m < mips; ++m)
            bytes += mipBytes(m);
        return bytes;
    }

    // Offset of mip `mip` of slice `slice` in the DDS body.
    u64 bodyOffset(u32 slice, u32 mip) const
    {
        u64 offset = u64(slice) * sliceBytes();
        for (u32 m = 0; m < mip; ++m)
            offset += mipBytes(m);
        return offset;
    }

    D3D12_RESOURCE_DESC desc12() const
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = std::max(1u, width >> skip);
        d.Height = std::max(1u, height >> skip);
        d.DepthOrArraySize = UINT16(slices);
        d.MipLevels = UINT16(kept());
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        return d;
    }
};

DXGI_FORMAT FromDx10(u32 format, DdsLayout& t)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        t.blockBytes = 8;
        return DXGI_FORMAT(format);
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        t.blockBytes = 16;
        return DXGI_FORMAT(format);
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        t.texelBytes = 4;
        return DXGI_FORMAT(format);
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// The header rules of texture_load's two paths (the BC fast path, DirectXTex for the rest), restricted to
// what both create without converting texels. `lod` = get_texture_load_lod (cubes keep every mip).
bool ParseDds(const u8* p, size_t n, int lod, DdsLayout& t)
{
    if (n < 128 || memcmp(p, "DDS ", 4))
        return false;
    u32 hsize, flags, h, w, mips, pfSize, pfFlags, fourcc, bits, rmask, gmask, bmask, amask, caps2;
    memcpy(&hsize, p + 4, 4);
    memcpy(&flags, p + 8, 4);
    memcpy(&h, p + 12, 4);
    memcpy(&w, p + 16, 4);
    memcpy(&mips, p + 28, 4);
    memcpy(&pfSize, p + 76, 4);
    memcpy(&pfFlags, p + 80, 4);
    memcpy(&fourcc, p + 84, 4);
    memcpy(&bits, p + 88, 4);
    memcpy(&rmask, p + 92, 4);
    memcpy(&gmask, p + 96, 4);
    memcpy(&bmask, p + 100, 4);
    memcpy(&amask, p + 104, 4);
    memcpy(&caps2, p + 112, 4);
    // 128 = the legacy D3DX size that counted the magic (texture_load patches it).
    if ((hsize != 124 && hsize != 128) || pfSize != 32 || !w || !h)
        return false;
    // Volumes (DDSD_DEPTH, DDSCAPS2_VOLUME) keep today's path.
    if ((flags & 0x800000) || (caps2 & 0x200000))
        return false;
    t = {};
    t.width = w;
    t.height = h;
    // DirectXTex takes the count whatever DDSD_MIPMAPCOUNT says.
    t.mips = std::max(1u, mips);
    if (caps2 & 0x200)
    {
        if ((caps2 & 0xFC00) != 0xFC00)
            return false;
        t.cube = true;
        t.slices = 6;
    }
    if (pfFlags & 0x4) // DDPF_FOURCC
    {
        if (fourcc == FourCC("DX10"))
        {
            if (n < 148)
                return false;
            u32 dxgi, dim, misc, arr;
            memcpy(&dxgi, p + 128, 4);
            memcpy(&dim, p + 132, 4);
            memcpy(&misc, p + 136, 4);
            memcpy(&arr, p + 140, 4);
            t.dataOffset = 148;
            if (dim != 3 || !arr)
                return false;
            t.cube = (misc & 0x4) != 0;
            t.slices = t.cube ? 6 * arr : arr;
            t.format = FromDx10(dxgi, t);
        }
        else if (fourcc == FourCC("DXT1")) { t.format = DXGI_FORMAT_BC1_UNORM; t.blockBytes = 8; }
        else if (fourcc == FourCC("DXT2") || fourcc == FourCC("DXT3")) { t.format = DXGI_FORMAT_BC2_UNORM; t.blockBytes = 16; }
        else if (fourcc == FourCC("DXT4") || fourcc == FourCC("DXT5")) { t.format = DXGI_FORMAT_BC3_UNORM; t.blockBytes = 16; }
        else if (fourcc == FourCC("ATI1") || fourcc == FourCC("BC4U")) { t.format = DXGI_FORMAT_BC4_UNORM; t.blockBytes = 8; }
        else if (fourcc == FourCC("BC4S")) { t.format = DXGI_FORMAT_BC4_SNORM; t.blockBytes = 8; }
        else if (fourcc == FourCC("ATI2") || fourcc == FourCC("BC5U")) { t.format = DXGI_FORMAT_BC5_UNORM; t.blockBytes = 16; }
        else if (fourcc == FourCC("BC5S")) { t.format = DXGI_FORMAT_BC5_SNORM; t.blockBytes = 16; }
    }
    else if ((pfFlags & 0x40) && bits == 32) // DDPF_RGB: only layouts DirectXTex copies unchanged
    {
        const bool alpha = (pfFlags & 0x1) != 0;
        if (rmask == 0xff0000 && gmask == 0xff00 && bmask == 0xff && alpha && amask == 0xff000000)
            t.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (rmask == 0xff0000 && gmask == 0xff00 && bmask == 0xff && !alpha && !amask)
            t.format = DXGI_FORMAT_B8G8R8X8_UNORM;
        else if (rmask == 0xff && gmask == 0xff00 && bmask == 0xff0000 && alpha && amask == 0xff000000)
            t.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.texelBytes = t.format != DXGI_FORMAT_UNKNOWN ? 4 : 0;
    }
    if (t.format == DXGI_FORMAT_UNKNOWN)
        return false;
    u32 maxMips = 1;
    for (u32 size = std::max(w, h); size > 1; size >>= 1)
        ++maxMips;
    if (t.mips > maxMips)
        return false;
    t.skip = t.cube ? 0 : std::min<u32>(u32(std::max(lod, 0)), t.mips - 1);
    // Block-compressed top mips must be a multiple of 4 (texture_load rounds them up through DirectXTex).
    if (t.blockBytes && (((w >> t.skip) & 3) || ((h >> t.skip) & 3)))
        return false;
    return true;
}

// Rows of the kept subresources from the tight DDS body into placed footprints (dst may be write-combined:
// written front to back, never read).
void Repitch(const DdsLayout& t, const u8* body, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, u8* dst)
{
    for (u32 slice = 0; slice < t.slices; ++slice)
    {
        const u8* src = body + t.bodyOffset(slice, t.skip);
        for (u32 m = 0; m < t.kept(); ++m)
        {
            u64 pitch;
            u32 rows;
            t.tight(m + t.skip, pitch, rows);
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = fp[slice * t.kept() + m];
            u8* out = dst + f.Offset;
            if (pitch == f.Footprint.RowPitch)
                memcpy(out, src, size_t(pitch * rows));
            else
            {
                for (u32 y = 0; y < rows; ++y)
                    memcpy(out + u64(y) * f.Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
            }
            src += pitch * rows;
        }
    }
}

using PFN_DStorageSetConfiguration1 = HRESULT(WINAPI*)(const DSTORAGE_CONFIGURATION1*);
using PFN_DStorageGetFactory = HRESULT(WINAPI*)(REFIID, void**);

// One priority: its file queue, its memory queue (re-pitched tails) and their fences.
struct QueuePair
{
    IDStorageQueue2* file = nullptr;
    IDStorageQueue2* memory = nullptr;
    ID3D12Fence* fileFence = nullptr;
    ID3D12Fence* memoryFence = nullptr;
    u64 fenceValue = 0;
    // A texture's requests and its status entry stay together (a status covers everything since the last).
    std::mutex lock;
    u32 unsubmitted = 0;
    bool memoryUsed = false;

    void release()
    {
        for (IDStorageQueue2** q : {&file, &memory})
        {
            if (*q)
            {
                (*q)->Close();
                (*q)->Release();
                *q = nullptr;
            }
        }
        for (ID3D12Fence** f : {&fileFence, &memoryFence})
        {
            if (*f)
            {
                (*f)->Release();
                *f = nullptr;
            }
        }
    }
};

// Requests that did not complete in time (dstorage.md 8): what they read or write and their status entries
// stay alive until the entries complete, or for the rest of the process.
struct ParkedLoad
{
    u32 slots[2] = {kNoSlot, kNoSlot};
    xr_vector<IUnknown*> resources; // one reference each
    xr_vector<u8> tail;             // a memory queue's source
};

struct Runtime
{
    ID3D12Device* device = nullptr;
    IDStorageFactory* factory = nullptr;
    IDStorageCustomDecompressionQueue1* codecQueue = nullptr;
    IDStorageStatusArray* status = nullptr;
    QueuePair batch;
    QueuePair sync;
    // Level geometry: its own queue, so its wait does not include the texture batch running beside it.
    QueuePair geometry;
    bool buffered = false;
    u32 verify = 0;
    std::atomic<bool> disabled{false}; // a wait timed out: the rest of the session loads the usual way

    std::mutex slotLock;
    xr_vector<u32> freeSlots;

    std::mutex fileLock;
    xr_vector<IDStorageFile*> files; // by vfs index
    xr_vector<u8> fileFailed;

    // Codec threads (custom LZO format).
    std::thread dispatcher;
    xr_vector<std::thread> codecThreads;
    std::mutex codecLock;
    std::condition_variable codecWake;
    std::deque<DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST> codecWork;
    std::atomic<bool> stopping{false};
    std::atomic<u32> codecFailures{0};

    // The textures of the running upload batch (their tickets belong to CTexture::ds_ticket).
    std::mutex batchLock;
    xr_vector<CTexture*> batchTextures;

    std::mutex parkedLock;
    xr_vector<ParkedLoad> parked;

    // Verify readbacks (-dstorage_verify): a copy queue of our own.
    std::mutex verifyLock;
    ID3D12CommandQueue* verifyQueue = nullptr;
    ID3D12CommandAllocator* verifyAllocator = nullptr;
    ID3D12GraphicsCommandList* verifyList = nullptr;
    ID3D12Fence* verifyFence = nullptr;
    u64 verifyValue = 0;
    HANDLE verifyEvent = nullptr;
    bool verifyStuck = false; // a readback copy timed out: its allocator cannot be reset
    u32 verifiedSync = 0, mismatchedSync = 0;
    u32 verifiedBatch = 0, mismatchedBatch = 0;

    // Session totals.
    std::atomic<u64> loadsRaw{0}, loadsLzo{0}, loadsFailed{0}, bytesDirect{0}, bytesTail{0};
};

enum class RuntimeState
{
    Untried,
    Ready,
    Failed
};

std::mutex g_runtimeLock;
RuntimeState g_state = RuntimeState::Untried;
Runtime* g_runtime = nullptr;
HMODULE g_module = nullptr;
xr_string g_reason;

// -dstorage_verify N: every stride-th DirectStorage texture of the running batch, at most N of them
// (set by da_texload_batch_begin before the batch's workers start).
std::atomic<u32> g_verifyStride{1};
std::atomic<u32> g_verifyNext{0};

// Unbuffered reads suit solid state drives; a volume with a seek penalty (a hard disk) goes through the
// OS file cache.
bool SeekPenalty()
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || exe[1] != L':')
        return false;
    const wchar_t volume[] = {L'\\', L'\\', L'.', L'\\', exe[0], L':', 0};
    HANDLE handle = CreateFileW(volume, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &bytes, nullptr);
    CloseHandle(handle);
    return ok && bytes >= sizeof(seek) && seek.IncursSeekPenalty;
}

u32 VerifyCount()
{
    pcstr arg = strstr(Core.Params, "-dstorage_verify ");
    return arg ? u32(std::max(0, atoi(arg + xr_strlen("-dstorage_verify ")))) : 0;
}

void CodecRequest(Runtime& rt, const DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST& r, xr_vector<u8>& scratch)
{
    ZoneScopedN("dstorage.codec");
    const u64 start = da_texload_ticks();
    HRESULT hr = E_FAIL;
    const int skip = int((u32(r.CompressionFormat) & 0x7F) >> 1);
    u8 header[kHeaderWindow];
    const size_t headerSize = rtc_decompress_prefix(header, sizeof(header), r.SrcBuffer, size_t(r.SrcSize));
    DdsLayout t;
    if (headerSize && ParseDds(header, headerSize, skip, t) && t.skip == u32(skip))
    {
        const u64 need = t.dataOffset + t.sliceBytes() * t.slices;
        scratch.resize(size_t(need));
        const D3D12_RESOURCE_DESC desc = t.desc12();
        xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.subresources());
        UINT64 total = 0;
        rt.device->GetCopyableFootprints(&desc, 0, t.subresources(), 0, fp.data(), nullptr, nullptr, &total);
        if (total == r.DstSize && rtc_decompress_prefix(scratch.data(), size_t(need), r.SrcBuffer, size_t(r.SrcSize)) == need)
        {
            Repitch(t, scratch.data() + t.dataOffset, fp.data(), static_cast<u8*>(r.DstBuffer));
            hr = S_OK;
        }
    }
    if (FAILED(hr))
        rt.codecFailures.fetch_add(1, std::memory_order_relaxed);
    // A rare big texture does not keep its scratch for the rest of the session.
    if (scratch.capacity() > (32u << 20))
        xr_vector<u8>().swap(scratch);
    texload_add_codec(da_texload_ms_since(start));
    DSTORAGE_CUSTOM_DECOMPRESSION_RESULT result{r.Id, hr};
    rt.codecQueue->SetRequestResults(1, &result);
}

void StartCodec(Runtime& rt)
{
    const u32 threads = std::clamp(std::thread::hardware_concurrency(), 4u, 10u) - 2;
    rt.dispatcher = std::thread([&rt]
    {
        const HANDLE event = rt.codecQueue->GetEvent();
        DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST requests[64];
        while (!rt.stopping.load(std::memory_order_acquire))
        {
            if (WaitForSingleObject(event, 50) != WAIT_OBJECT_0)
                continue;
            for (;;)
            {
                UINT32 count = 0;
                if (FAILED(rt.codecQueue->GetRequests1(DSTORAGE_GET_REQUEST_FLAG_SELECT_CUSTOM, 64, requests, &count)) || !count)
                    break;
                {
                    std::lock_guard guard(rt.codecLock);
                    rt.codecWork.insert(rt.codecWork.end(), requests, requests + count);
                }
                rt.codecWake.notify_all();
            }
        }
    });
    for (u32 i = 0; i < threads; ++i)
    {
        rt.codecThreads.emplace_back([&rt]
        {
            xr_vector<u8> scratch;
            for (;;)
            {
                DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST request;
                {
                    std::unique_lock guard(rt.codecLock);
                    rt.codecWake.wait(guard, [&rt] { return rt.stopping.load(std::memory_order_acquire) || !rt.codecWork.empty(); });
                    if (rt.codecWork.empty())
                        return;
                    request = rt.codecWork.front();
                    rt.codecWork.pop_front();
                }
                CodecRequest(rt, request, scratch);
            }
        });
    }
}

void StopCodec(Runtime& rt)
{
    rt.stopping.store(true, std::memory_order_release);
    rt.codecWake.notify_all();
    if (rt.dispatcher.joinable())
        rt.dispatcher.join();
    for (std::thread& thread : rt.codecThreads)
        thread.join();
    rt.codecThreads.clear();
}

bool CreatePair(Runtime& rt, QueuePair& pair, DSTORAGE_PRIORITY priority, UINT16 capacity, pcstr name)
{
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = capacity;
    qd.Priority = priority;
    qd.Name = name;
    qd.Device = rt.device;
    if (FAILED(rt.factory->CreateQueue(&qd, IID_PPV_ARGS(&pair.file))))
        return false;
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    if (FAILED(rt.factory->CreateQueue(&qd, IID_PPV_ARGS(&pair.memory))))
        return false;
    return SUCCEEDED(rt.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pair.fileFence))) &&
        SUCCEEDED(rt.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pair.memoryFence)));
}

void DestroyRuntime(Runtime* rt)
{
    if (!rt)
        return;
    StopCodec(*rt);
    rt->batch.release();
    rt->sync.release();
    rt->geometry.release();
    for (IDStorageFile* file : rt->files)
    {
        if (file)
        {
            file->Close();
            file->Release();
        }
    }
    if (rt->status)
        rt->status->Release();
    if (rt->codecQueue)
        rt->codecQueue->Release();
    if (rt->factory)
        rt->factory->Release();
    if (rt->verifyList)
        rt->verifyList->Release();
    if (rt->verifyAllocator)
        rt->verifyAllocator->Release();
    if (rt->verifyQueue)
        rt->verifyQueue->Release();
    if (rt->verifyFence)
        rt->verifyFence->Release();
    if (rt->verifyEvent)
        CloseHandle(rt->verifyEvent);
    xr_delete(rt);
}

// Caller holds g_runtimeLock.
Runtime* CreateRuntime()
{
    ID3D12Device* device = HW.pDevice ? xrd3d12::NativeDevice(HW.pDevice) : nullptr;
    if (!device)
    {
        g_reason = "no DirectX 12 device";
        return nullptr;
    }
    if (!g_module)
    {
        // By absolute path next to the executable; dstoragecore.dll comes from the same folder.
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = length ? wcsrchr(path, L'\\') : nullptr;
        if (!slash || size_t(slash - path) + 14 >= MAX_PATH)
        {
            g_reason = "no executable folder";
            return nullptr;
        }
        wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"dstorage.dll");
        g_module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_module)
        {
            g_reason = "dstorage.dll not loaded";
            return nullptr;
        }
    }
    const auto setConfiguration = reinterpret_cast<PFN_DStorageSetConfiguration1>(GetProcAddress(g_module, "DStorageSetConfiguration1"));
    const auto getFactory = reinterpret_cast<PFN_DStorageGetFactory>(GetProcAddress(g_module, "DStorageGetFactory"));
    if (!setConfiguration || !getFactory)
    {
        g_reason = "dstorage.dll lacks its entry points";
        return nullptr;
    }

    Runtime* rt = xr_new<Runtime>();
    rt->device = device;
    rt->buffered = strstr(Core.Params, "-dstorage_buffered") || SeekPenalty();
    rt->verify = VerifyCount();
    DSTORAGE_CONFIGURATION1 configuration{};
    // Only GDeflate would use the built-in CPU codec; our LZO runs on our threads.
    configuration.NumBuiltInCpuDecompressionThreads = DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION;
    configuration.DisableTelemetry = TRUE;
    configuration.DisableBypassIO = rt->buffered;
    configuration.ForceFileBuffering = rt->buffered;
    // A second device of the session finds the configuration already applied (it is process-wide).
    setConfiguration(&configuration);
    if (FAILED(getFactory(IID_PPV_ARGS(&rt->factory))) || !rt->factory)
    {
        g_reason = "no DirectStorage factory";
        DestroyRuntime(rt);
        return nullptr;
    }
    rt->factory->SetStagingBufferSize(kStagingSize);
    if (strstr(Core.Params, "-dxdebug"))
        rt->factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS);
    if (FAILED(rt->factory->QueryInterface(IID_PPV_ARGS(&rt->codecQueue))) ||
        FAILED(rt->factory->CreateStatusArray(kStatusSlots, "xfr textures", IID_PPV_ARGS(&rt->status))) ||
        !CreatePair(*rt, rt->batch, DSTORAGE_PRIORITY_NORMAL, DSTORAGE_MAX_QUEUE_CAPACITY, "xfr batch") ||
        !CreatePair(*rt, rt->sync, DSTORAGE_PRIORITY_HIGH, 1024, "xfr sync") ||
        !CreatePair(*rt, rt->geometry, DSTORAGE_PRIORITY_NORMAL, 1024, "xfr geometry"))
    {
        g_reason = "DirectStorage queues not created";
        DestroyRuntime(rt);
        return nullptr;
    }
    rt->freeSlots.reserve(kStatusSlots);
    for (u32 i = kStatusSlots; i--;)
        rt->freeSlots.push_back(i);
    StartCodec(*rt);
    Msg("* [dstorage] runtime ready: %s reads, staging %u MB, %zu codec threads%s", rt->buffered ? "buffered" : "unbuffered",
        kStagingSize >> 20, rt->codecThreads.size(), rt->verify ? ", verify on" : "");
    return rt;
}

// The runtime when DirectStorage may take textures now; loads it on first use.
Runtime* Available(bool forTexture)
{
    if (strstr(Core.Params, "-no_dstorage"))
        return nullptr;
    std::lock_guard guard(g_runtimeLock);
    if (g_state == RuntimeState::Untried)
    {
        g_runtime = CreateRuntime();
        g_state = g_runtime ? RuntimeState::Ready : RuntimeState::Failed;
        if (!g_runtime)
            Msg("! [dstorage] not available (%s): textures load the usual way", g_reason.c_str());
    }
    if (!g_runtime || (forTexture && g_runtime->disabled))
        return nullptr;
    return g_runtime;
}

IDStorageFile* ArchiveFile(Runtime& rt, size_t vfs)
{
    std::lock_guard guard(rt.fileLock);
    if (vfs >= rt.files.size())
    {
        rt.files.resize(vfs + 1, nullptr);
        rt.fileFailed.resize(vfs + 1, 0);
    }
    if (rt.files[vfs] || rt.fileFailed[vfs])
        return rt.files[vfs];
    const auto& archive = FS.m_archives[vfs];
    wchar_t path[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, archive.path.c_str(), -1, path, MAX_PATH) ||
        FAILED(rt.factory->OpenFile(path, IID_PPV_ARGS(&rt.files[vfs]))))
    {
        rt.files[vfs] = nullptr;
        rt.fileFailed[vfs] = 1;
        Msg("! [dstorage] cannot open %s: its textures load the usual way", archive.path.c_str());
    }
    return rt.files[vfs];
}

u32 TakeSlot(Runtime& rt)
{
    std::lock_guard guard(rt.slotLock);
    if (rt.freeSlots.empty())
        return kNoSlot;
    const u32 slot = rt.freeSlots.back();
    rt.freeSlots.pop_back();
    return slot;
}

void GiveSlot(Runtime& rt, u32 slot)
{
    if (slot == kNoSlot)
        return;
    std::lock_guard guard(rt.slotLock);
    rt.freeSlots.push_back(slot);
}

// Waits until the status entry completed; false after the timeout.
bool WaitSlot(Runtime& rt, u32 slot)
{
    if (slot == kNoSlot)
        return true;
    const u64 start = da_texload_ticks();
    for (u32 spin = 0; !rt.status->IsComplete(slot); ++spin)
    {
        if (spin < 64)
            _mm_pause();
        else if (spin < 256)
            SwitchToThread();
        else
            Sleep(0);
        if ((spin & 1023) == 1023 && da_texload_ms_since(start) > float(kWaitTimeoutMs))
            return false;
    }
    return true;
}

bool WaitFence(ID3D12Fence* fence, u64 value, DWORD timeoutMs)
{
    if (fence->GetCompletedValue() >= value)
        return true;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    bool done = SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done || fence->GetCompletedValue() >= value;
}

// Hands everything enqueued so far to the runtime (a batch submits every kSubmitEvery textures only).
void Submit(QueuePair& pair)
{
    std::lock_guard guard(pair.lock);
    pair.file->Submit();
    if (pair.memoryUsed)
        pair.memory->Submit();
    pair.unsubmitted = 0;
}

// Every request of the pair so far is done (fences signalled after them).
bool Drain(QueuePair& pair, DWORD timeoutMs = kWaitTimeoutMs)
{
    u64 value;
    bool memory;
    {
        std::lock_guard guard(pair.lock);
        value = ++pair.fenceValue;
        memory = pair.memoryUsed;
        pair.file->EnqueueSignal(pair.fileFence, value);
        pair.file->Submit();
        if (memory)
        {
            pair.memory->EnqueueSignal(pair.memoryFence, value);
            pair.memory->Submit();
        }
        pair.unsubmitted = 0;
    }
    return WaitFence(pair.fileFence, value, timeoutMs) && (!memory || WaitFence(pair.memoryFence, value, timeoutMs));
}

// A wait timed out: device-removed check, then the rest of the session loads the usual way. One line per
// session, whichever wait timed out first.
void Disable(Runtime& rt, pcstr what)
{
    if (rt.disabled.exchange(true))
        return;
    const HRESULT removed = HW.pDevice->GetDeviceRemovedReason();
    Msg("! [dstorage] %s did not complete in %u s (device %s): DirectStorage is off for this session", what,
        kWaitTimeoutMs / 1000, FAILED(removed) ? "removed" : "alive");
}

void Park(Runtime& rt, ParkedLoad&& load)
{
    std::lock_guard guard(rt.parkedLock);
    rt.parked.push_back(std::move(load));
}

// Parked loads whose status entries completed give back their resources and entries.
void SweepParked(Runtime& rt)
{
    std::lock_guard guard(rt.parkedLock);
    std::erase_if(rt.parked, [&rt](ParkedLoad& load)
    {
        for (u32 slot : load.slots)
        {
            if (slot != kNoSlot && !rt.status->IsComplete(slot))
                return false;
        }
        for (u32 slot : load.slots)
            GiveSlot(rt, slot);
        for (IUnknown* resource : load.resources)
            resource->Release();
        return true;
    });
}

size_t ParkedCount(Runtime& rt)
{
    std::lock_guard guard(rt.parkedLock);
    return rt.parked.size();
}
} // namespace

struct DaDsTicket
{
    ID3D11Texture2D* texture = nullptr;
    u32 memory = 0;
    u32 fileSlot = kNoSlot;
    u32 memorySlot = kNoSlot;
    bool sync = false;
    bool verify = false; // a batch ticket -dstorage_verify picked
    DaTexPath path = DaTexPath::DsRaw;
    DdsLayout layout;
    xr_vector<u8> tail; // re-pitched tail mips, the memory queue's source until the requests completed
    u64 start = 0;
    float readMs = 0, createMs = 0;
    u64 bytes = 0;
    string_path file{};
};

namespace
{
// The copy queue of the verify readbacks, created on first use. Caller holds verifyLock.
bool EnsureVerifyQueue(Runtime& rt)
{
    if (rt.verifyStuck)
        return false;
    if (rt.verifyQueue)
        return true;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(rt.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&rt.verifyQueue))) ||
        FAILED(rt.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&rt.verifyAllocator))) ||
        FAILED(rt.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, rt.verifyAllocator, nullptr, IID_PPV_ARGS(&rt.verifyList))) ||
        FAILED(rt.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.verifyFence))))
    {
        return false;
    }
    rt.verifyList->Close();
    rt.verifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return !!rt.verifyEvent;
}

// Runs the recorded readback copies. False when they did not complete in time: the copy queue may still
// write the readback, so the caller keeps it alive, and no later verify resets the allocator. Caller holds
// verifyLock.
bool RunVerifyCopy(Runtime& rt)
{
    rt.verifyList->Close();
    ID3D12CommandList* lists[] = {rt.verifyList};
    rt.verifyQueue->ExecuteCommandLists(1, lists);
    rt.verifyQueue->Signal(rt.verifyFence, ++rt.verifyValue);
    if (rt.verifyFence->GetCompletedValue() < rt.verifyValue)
    {
        rt.verifyFence->SetEventOnCompletion(rt.verifyValue, rt.verifyEvent);
        WaitForSingleObject(rt.verifyEvent, kWaitTimeoutMs);
    }
    rt.verifyStuck = rt.verifyFence->GetCompletedValue() < rt.verifyValue;
    if (rt.verifyStuck)
        Msg("! [dstorage] a verify readback did not complete in %u s: verify stops", kWaitTimeoutMs / 1000);
    return !rt.verifyStuck;
}

// -dstorage_verify: the texture read back and compared with its DDS body as today's path reads it.
bool VerifyTicket(Runtime& rt, const DaDsTicket& ticket)
{
    std::lock_guard guard(rt.verifyLock);
    if (!EnsureVerifyQueue(rt))
        return false;
    IReader* reader = FS.r_open(ticket.file);
    if (!reader)
        return false;
    const DdsLayout& t = ticket.layout;
    const u8* dds = static_cast<const u8*>(reader->pointer());
    bool same = size_t(reader->length()) >= t.dataOffset + t.sliceBytes() * t.slices;
    ID3D12Resource* texture = xrd3d12::NativeResource(ticket.texture);
    const D3D12_RESOURCE_DESC desc = t.desc12();
    xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.subresources());
    UINT64 total = 0;
    rt.device->GetCopyableFootprints(&desc, 0, t.subresources(), 0, fp.data(), nullptr, nullptr, &total);
    ID3D12Resource* readback = nullptr;
    if (same && texture)
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        same = SUCCEEDED(rt.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&readback)));
    }
    if (same && readback)
    {
        rt.verifyAllocator->Reset();
        rt.verifyList->Reset(rt.verifyAllocator, nullptr);
        for (u32 s = 0; s < t.subresources(); ++s)
        {
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            from.pResource = texture;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = s;
            to.pResource = readback;
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = fp[s];
            rt.verifyList->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        u8* data = nullptr;
        same = RunVerifyCopy(rt);
        // Left to the copy queue that may still write it.
        if (!same)
            readback = nullptr;
        same = same && SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)));
        if (same)
        {
            const u8* body = dds + t.dataOffset;
            for (u32 slice = 0; slice < t.slices && same; ++slice)
            {
                const u8* src = body + t.bodyOffset(slice, t.skip);
                for (u32 m = 0; m < t.kept() && same; ++m)
                {
                    u64 pitch;
                    u32 rows;
                    t.tight(m + t.skip, pitch, rows);
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = fp[slice * t.kept() + m];
                    for (u32 y = 0; y < rows && same; ++y)
                        same = !memcmp(data + f.Offset + u64(y) * f.Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
                    src += pitch * rows;
                }
            }
            const D3D12_RANGE nothing{0, 0};
            readback->Unmap(0, &nothing);
        }
    }
    if (readback)
        readback->Release();
    FS.r_close(reader);
    if (!same)
        Msg("! [dstorage] verify MISMATCH: %s", ticket.file);
    return same;
}

// Enqueues a texture's requests. Raw entries: the leading mips whose tight layout is the placed footprint
// go straight from the file into the texture, the rest (a few KB) is re-pitched here from the mapping and
// goes through the memory queue. LZO entries: one request through the codec.
bool Enqueue(Runtime& rt, DaDsTicket& ticket, IDStorageFile* file, const u8* mapped, u32 ptr, u32 sizeCompressed, bool lzo)
{
    const DdsLayout& t = ticket.layout;
    ID3D12Resource* resource = xrd3d12::NativeResource(ticket.texture);
    if (!resource)
        return false;
    const D3D12_RESOURCE_DESC desc = t.desc12();
    QueuePair& pair = ticket.sync ? rt.sync : rt.batch;
    xr_vector<DSTORAGE_REQUEST> fileRequests, memoryRequests;

    if (lzo)
    {
        UINT64 total = 0;
        rt.device->GetCopyableFootprints(&desc, 0, t.subresources(), 0, nullptr, nullptr, nullptr, &total);
        if (total > kStagingSize)
            return false;
        DSTORAGE_REQUEST& r = fileRequests.emplace_back();
        r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT(kLzoFormat | (t.skip << 1));
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
        r.Source.File.Source = file;
        r.Source.File.Offset = ptr;
        r.Source.File.Size = sizeCompressed;
        r.Destination.MultipleSubresources.Resource = resource;
        r.Destination.MultipleSubresources.FirstSubresource = 0;
        r.UncompressedSize = u32(total);
    }
    else
    {
        const u32 kept = t.kept();
        xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(kept);
        // Tail offsets per slice into one block.
        struct Tail
        {
            u32 first;
            u32 count;
            u64 offset;
            u64 size;
        };
        xr_vector<Tail> tails;
        u64 tailTotal = 0;
        u64 direct = 0;
        for (u32 slice = 0; slice < t.slices; ++slice)
        {
            const u32 first = slice * kept;
            rt.device->GetCopyableFootprints(&desc, first, kept, 0, fp.data(), nullptr, nullptr, nullptr);
            u32 k = 0;
            u64 tight = 0;
            for (; k < kept; ++k)
            {
                u64 pitch;
                u32 rows;
                t.tight(k + t.skip, pitch, rows);
                if (pitch != fp[k].Footprint.RowPitch || tight != fp[k].Offset)
                    break;
                tight += pitch * rows;
            }
            if (k)
            {
                if (tight > kStagingSize)
                    return false;
                DSTORAGE_REQUEST& r = fileRequests.emplace_back();
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE;
                r.Source.File.Source = file;
                r.Source.File.Offset = u64(ptr) + t.dataOffset + t.bodyOffset(slice, t.skip);
                r.Source.File.Size = u32(tight);
                r.Destination.MultipleSubresourcesRange.Resource = resource;
                r.Destination.MultipleSubresourcesRange.FirstSubresource = first;
                r.Destination.MultipleSubresourcesRange.NumSubresources = k;
                direct += tight;
            }
            if (k < kept)
            {
                UINT64 size = 0;
                rt.device->GetCopyableFootprints(&desc, first + k, kept - k, 0, nullptr, nullptr, nullptr, &size);
                // A request goes through the staging buffer in one piece.
                if (size > kStagingSize)
                    return false;
                tails.push_back({first + k, kept - k, tailTotal, size});
                tailTotal += (size + 511) & ~511ull;
            }
        }
        if (tailTotal)
        {
            ticket.tail.assign(size_t(tailTotal), 0);
            xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> tfp(kept);
            for (const Tail& tail : tails)
            {
                rt.device->GetCopyableFootprints(&desc, tail.first, tail.count, 0, tfp.data(), nullptr, nullptr, nullptr);
                const u32 slice = tail.first / kept;
                const u32 mip0 = tail.first % kept + t.skip;
                const u8* src = mapped + ptr + t.dataOffset + t.bodyOffset(slice, mip0);
                u8* dst = ticket.tail.data() + tail.offset;
                for (u32 i = 0; i < tail.count; ++i)
                {
                    u64 pitch;
                    u32 rows;
                    t.tight(mip0 + i, pitch, rows);
                    for (u32 y = 0; y < rows; ++y)
                        memcpy(dst + tfp[i].Offset + u64(y) * tfp[i].Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
                    src += pitch * rows;
                }
                DSTORAGE_REQUEST& r = memoryRequests.emplace_back();
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE;
                r.Source.Memory.Source = dst;
                r.Source.Memory.Size = u32(tail.size);
                r.Destination.MultipleSubresourcesRange.Resource = resource;
                r.Destination.MultipleSubresourcesRange.FirstSubresource = tail.first;
                r.Destination.MultipleSubresourcesRange.NumSubresources = tail.count;
            }
        }
        rt.bytesDirect.fetch_add(direct, std::memory_order_relaxed);
        rt.bytesTail.fetch_add(tailTotal, std::memory_order_relaxed);
    }

    if (!fileRequests.empty())
        ticket.fileSlot = TakeSlot(rt);
    if (!memoryRequests.empty())
        ticket.memorySlot = TakeSlot(rt);
    if ((!fileRequests.empty() && ticket.fileSlot == kNoSlot) || (!memoryRequests.empty() && ticket.memorySlot == kNoSlot))
        return false;

    std::lock_guard guard(pair.lock);
    for (const DSTORAGE_REQUEST& r : fileRequests)
        pair.file->EnqueueRequest(&r);
    if (!fileRequests.empty())
        pair.file->EnqueueStatus(rt.status, ticket.fileSlot);
    for (const DSTORAGE_REQUEST& r : memoryRequests)
        pair.memory->EnqueueRequest(&r);
    if (!memoryRequests.empty())
    {
        pair.memory->EnqueueStatus(rt.status, ticket.memorySlot);
        pair.memoryUsed = true;
    }
    if (ticket.sync || ++pair.unsubmitted >= kSubmitEvery)
    {
        pair.file->Submit();
        if (pair.memoryUsed)
            pair.memory->Submit();
        pair.unsubmitted = 0;
    }
    return true;
}

void ReleaseTicket(Runtime* rt, DaDsTicket* ticket)
{
    if (rt)
    {
        GiveSlot(*rt, ticket->fileSlot);
        GiveSlot(*rt, ticket->memorySlot);
    }
    if (ticket->texture)
        ticket->texture->Release();
    xr_delete(ticket);
}

// A ticket whose requests did not complete: its texture, tail and status entries go to the parked list.
void ParkTicket(Runtime& rt, DaDsTicket* ticket)
{
    ParkedLoad load;
    load.slots[0] = ticket->fileSlot;
    load.slots[1] = ticket->memorySlot;
    if (ticket->texture)
        load.resources.push_back(ticket->texture);
    ticket->texture = nullptr;
    // A moved vector keeps its block: the memory queue's source address stays valid.
    load.tail = std::move(ticket->tail);
    Park(rt, std::move(load));
    xr_delete(ticket);
}
} // namespace

DaDsTicket* da_dstorage_begin(pcstr name)
{
    if (!ps_r__dstorage || !HW.pDevice || !name || !name[0])
        return nullptr;
    Runtime* rt = Available(true);
    if (!rt)
        return nullptr;
    ZoneScopedN("dstorage.begin");
    const u64 start = da_texload_ticks();
    string_path fn;
    // Quiet: a missing texture or bump map goes today's way, which logs its fallback once.
    if (!texture_resolve_name(name, fn, true))
        return nullptr;
    xr_strlwr(fn);
    // Loose files and module overlays (no archive entry) keep today's path.
    const CLocatorAPI::file* desc = FS.GetFileDesc(fn);
    if (!desc || desc->vfs >= FS.m_archives.size() || !desc->size_real || !desc->size_compressed)
        return nullptr;
    const u8* mapped = FS.m_archives[desc->vfs].mappedBase;
    if (!mapped)
        return nullptr;
    const bool lzo = desc->size_real != desc->size_compressed;
    // A single LZO texture waits for the codec round trip and unbuffered reads (S6: p95 14.5-16.5 ms against
    // 11.0-13.6 ms inline from the mapped archive): synchronous loads take DirectStorage for raw entries only.
    if (lzo && !t_batch_worker)
        return nullptr;
    const u8* src = mapped + desc->ptr;
    u8 header[kHeaderWindow];
    size_t headerSize = std::min<size_t>(sizeof(header), desc->size_real);
    if (lzo)
        headerSize = rtc_decompress_prefix(header, headerSize, src, desc->size_compressed);
    else
        memcpy(header, src, headerSize);
    const int lod = get_texture_load_lod(fn);
    DdsLayout layout;
    if (!ParseDds(header, headerSize, lod, layout) || layout.dataOffset + layout.sliceBytes() * layout.slices > desc->size_real)
        return nullptr;
    IDStorageFile* file = ArchiveFile(*rt, desc->vfs);
    if (!file)
        return nullptr;

    auto* ticket = xr_new<DaDsTicket>();
    ticket->sync = !t_batch_worker;
    ticket->layout = layout;
    ticket->start = start;
    ticket->bytes = desc->size_real;
    ticket->path = lzo ? DaTexPath::DsLzo : DaTexPath::DsRaw;
    ticket->memory = calc_texture_size(lod, layout.kept(), desc->size_real);
    xr_strcpy(ticket->file, fn);
    ticket->readMs = da_texload_ms_since(start);

    // Placed without data (the requests write every subresource), left in COMMON for DirectStorage.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = std::max(1u, layout.width >> layout.skip);
    td.Height = std::max(1u, layout.height >> layout.skip);
    td.MipLevels = layout.kept();
    td.ArraySize = layout.slices;
    td.Format = layout.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = layout.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    const u64 createStart = da_texload_ticks();
    const bool previous = xrd3d12::SetStreamedTextures(true);
    const HRESULT hr = HW.pDevice->CreateTexture2D(&td, nullptr, &ticket->texture);
    xrd3d12::SetStreamedTextures(previous);
    ticket->createMs = da_texload_ms_since(createStart);
    if (FAILED(hr) || !ticket->texture || !Enqueue(*rt, *ticket, file, mapped, desc->ptr, desc->size_compressed, lzo))
    {
        ReleaseTicket(rt, ticket);
        return nullptr;
    }
    // Picked here, verified by da_dstorage_end, which owns the ticket.
    if (rt->verify && !ticket->sync)
    {
        const u32 n = g_verifyNext.fetch_add(1, std::memory_order_relaxed);
        const u32 stride = g_verifyStride.load(std::memory_order_relaxed);
        ticket->verify = n % stride == 0 && n / stride < rt->verify;
    }
    return ticket;
}

bool da_dstorage_batch_thread() { return t_batch_worker; }

void da_dstorage_batch_add(CTexture* texture)
{
    Runtime* rt = g_runtime;
    if (!rt || !texture)
        return;
    std::lock_guard guard(rt->batchLock);
    rt->batchTextures.push_back(texture);
}

ID3DBaseTexture* da_dstorage_end(DaDsTicket* ticket, u32& memory)
{
    memory = 0;
    if (!ticket)
        return nullptr;
    ZoneScopedN("dstorage.finish");
    Runtime* rt = g_runtime;
    // A batch texture needed before its batch completes: its requests may still sit unsubmitted.
    const auto pending = [rt](u32 slot) { return slot != kNoSlot && !rt->status->IsComplete(slot); };
    if (rt && !ticket->sync && (pending(ticket->fileSlot) || pending(ticket->memorySlot)))
        Submit(rt->batch);
    // After a timeout nothing waits again: what is not complete now loads the usual way.
    bool complete = false;
    if (rt)
    {
        complete = rt->disabled ? !pending(ticket->fileSlot) && !pending(ticket->memorySlot)
                                : WaitSlot(*rt, ticket->fileSlot) && WaitSlot(*rt, ticket->memorySlot);
    }
    if (!complete && rt)
        Disable(*rt, ticket->sync ? "a texture load" : "a batch texture");
    bool ok = complete;
    if (ok)
    {
        for (u32 slot : {ticket->fileSlot, ticket->memorySlot})
        {
            const HRESULT hr = slot == kNoSlot ? S_OK : rt->status->GetHResult(slot);
            if (FAILED(hr))
            {
                ok = false;
                Msg("! [dstorage] %s: request failed, hr 0x%08X; loaded the usual way", ticket->file, u32(hr));
            }
        }
    }
    // -dstorage_verify: every synchronous load and the picked batch tickets, before the texture becomes visible.
    if (ok && (ticket->sync || ticket->verify) && rt->verify)
    {
        const bool same = VerifyTicket(*rt, *ticket);
        std::lock_guard guard(rt->verifyLock);
        ++(ticket->sync ? rt->verifiedSync : rt->verifiedBatch);
        (ticket->sync ? rt->mismatchedSync : rt->mismatchedBatch) += same ? 0 : 1;
    }

    DaTexloadSample sample;
    sample.total_ms = da_texload_ms_since(ticket->start);
    sample.read_ms = ticket->readMs;
    sample.create_ms = ticket->createMs;
    sample.bytes = ticket->bytes;
    sample.path = ticket->path;
    ID3DBaseTexture* texture = nullptr;
    if (ok)
    {
        texture = ticket->texture;
        ticket->texture = nullptr;
        memory = ticket->memory;
        da_texload_note(sample);
        (ticket->path == DaTexPath::DsLzo ? rt->loadsLzo : rt->loadsRaw).fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // texture_load notes the fallback as failed.
        t_fallback = true;
        if (rt)
            rt->loadsFailed.fetch_add(1, std::memory_order_relaxed);
    }
    // Requests still in flight keep what they read and write.
    if (rt && !complete)
        ParkTicket(*rt, ticket);
    else
        ReleaseTicket(rt, ticket);
    return texture;
}

void da_dstorage_batch_complete()
{
    Runtime* rt = g_runtime;
    if (!rt)
        return;
    xr_vector<CTexture*> textures;
    {
        std::lock_guard guard(rt->batchLock);
        textures.swap(rt->batchTextures);
    }
    if (textures.empty())
        return;
    // After a timeout elsewhere nothing waits again: the finish below takes what completed.
    if (rt->disabled)
        Submit(rt->batch);
    else if (!Drain(rt->batch))
        Disable(*rt, "an upload batch");
    // A texture another thread needed first is finished already; its ticket is gone.
    for (CTexture* texture : textures)
        texture->ds_finish();
    // The picked tickets verified themselves in da_dstorage_end (all of them ended by now).
    if (rt->verify)
    {
        std::lock_guard guard(rt->verifyLock);
        Msg("* [dstorage] verify: %u textures, %u mismatches (batch of %zu)", rt->verifiedBatch, rt->mismatchedBatch,
            textures.size());
        rt->verifiedBatch = rt->mismatchedBatch = 0;
    }
    SweepParked(*rt);
    if (const size_t parked = ParkedCount(*rt))
        Msg("! [dstorage] %zu loads still in flight: their resources are kept until they complete", parked);
}

namespace
{
// Requests within the staging buffer.
constexpr u32 kGeometryPiece = 16u << 20;

void SeekTo(CStreamReader* fs, size_t position) { fs->advance(int(position) - int(fs->tell())); }

struct GeometryPiece
{
    ID3D12Resource* resource;
    u64 fileOffset;     // in the archive
    size_t entryOffset; // in level.geom (verify)
    u32 size;
};

// -dstorage_verify: a buffer read back through our copy queue equals the bytes today's path reads.
bool VerifyBuffer(Runtime& rt, ID3D12Resource* buffer, const void* expected, u32 size)
{
    std::lock_guard guard(rt.verifyLock);
    if (!EnsureVerifyQueue(rt))
        return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    if (FAILED(rt.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))))
    {
        return false;
    }
    rt.verifyAllocator->Reset();
    rt.verifyList->Reset(rt.verifyAllocator, nullptr);
    rt.verifyList->CopyBufferRegion(readback, 0, buffer, 0, size);
    // Left to the copy queue that may still write it.
    if (!RunVerifyCopy(rt))
        return false;
    u8* data = nullptr;
    bool same = SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)));
    if (same)
    {
        same = !memcmp(data, expected, size);
        const D3D12_RANGE nothing{0, 0};
        readback->Unmap(0, &nothing);
    }
    readback->Release();
    return same;
}
} // namespace

bool da_dstorage_load_buffers(CRender& render, CStreamReader* fs, bool alternative)
{
    t_geometry_ds = false;
#ifdef DEBUG
    // The staging buffers of debug builds assert initial data (dx11BufferUtils.cpp): today's path there.
    UNUSED(render);
    UNUSED(fs);
    UNUSED(alternative);
    return false;
#else
    if (!ps_r__dstorage || !fs || !HW.pDevice)
        return false;
    Runtime* rt = Available(true);
    if (!rt)
        return false;
    ZoneScopedN("dstorage.geometry");
    string_path fn;
    FS.update_path(fn, "$level$", alternative ? "level.geomx" : "level.geom");
    xr_strlwr(fn);
    const CLocatorAPI::file* desc = FS.GetFileDesc(fn);
    if (!desc || desc->vfs >= FS.m_archives.size() || desc->size_real != desc->size_compressed || fs->length() != desc->size_real)
        return false;
    IDStorageFile* file = ArchiveFile(*rt, desc->vfs);
    if (!file)
        return false;

    // Top-level chunks (id, size): where the vertex and the index buffer chunks start. A compressed chunk
    // carries a flag in its id and matches neither.
    size_t vbAt = 0, ibAt = 0;
    SeekTo(fs, 0);
    while (fs->elapsed() >= 8)
    {
        const u32 id = fs->r_u32();
        const u32 size = fs->r_u32();
        const size_t at = fs->tell();
        if (at + size > fs->length())
            break;
        if (id == fsL_VB)
            vbAt = at;
        else if (id == fsL_IB)
            ibAt = at;
        SeekTo(fs, at + size);
    }
    SeekTo(fs, 0);
    if (!vbAt || !ibAt)
        return false;

    auto& decls = alternative ? render.xDC : render.nDC;
    auto& vbuffers = alternative ? render.xVB : render.nVB;
    auto& ibuffers = alternative ? render.xIB : render.nIB;
    xr_vector<GeometryPiece> pieces;
    bool ok = true;
    // LoadBuffers' parse; each buffer is created without data (its own resource in COMMON) and read into.
    SeekTo(fs, vbAt);
    const u32 vbCount = fs->r_u32();
    decls.resize(vbCount);
    vbuffers.resize(vbCount);
    for (u32 i = 0; i < vbCount && ok; ++i)
    {
        VertexElement dcl[MAXD3DDECLLENGTH + 1];
        fs->r(dcl, sizeof(dcl));
        fs->advance(-int(sizeof(dcl)));
        const u32 length = GetDeclLength(dcl) + 1;
        decls[i].resize(length);
        fs->r(decls[i].begin(), length * sizeof(VertexElement));
        const u32 vertices = fs->r_u32();
        const size_t bytes = size_t(vertices) * GetDeclVertexSize(dcl, 0);
        const size_t at = fs->tell();
#ifndef MASTER_GOLD
        Msg("* [Loading VB] %u verts, %zu Kb", vertices, bytes / 1024);
#endif
        ok = bytes && bytes <= std::numeric_limits<u32>::max();
        if (ok)
        {
            vbuffers[i].CreateFromData(nullptr, bytes);
            ID3D12Resource* resource = xrd3d12::NativeResource(static_cast<VertexBufferHandle>(vbuffers[i]));
            ok = !!resource;
            pieces.push_back({resource, desc->ptr + at, at, u32(bytes)});
        }
        fs->advance(int(bytes));
    }
    if (ok)
    {
        SeekTo(fs, ibAt);
        const u32 ibCount = fs->r_u32();
        ibuffers.resize(ibCount);
        for (u32 i = 0; i < ibCount && ok; ++i)
        {
            const size_t bytes = size_t(fs->r_u32()) * 2;
            const size_t at = fs->tell();
#ifndef MASTER_GOLD
            Msg("* [Loading IB] %zu indices, %zu Kb", bytes / 2, bytes / 1024);
#endif
            ok = bytes && bytes <= std::numeric_limits<u32>::max();
            if (ok)
            {
                ibuffers[i].CreateFromData(nullptr, bytes);
                ID3D12Resource* resource = xrd3d12::NativeResource(static_cast<IndexBufferHandle>(ibuffers[i]));
                ok = !!resource;
                pieces.push_back({resource, desc->ptr + at, at, u32(bytes)});
            }
            fs->advance(int(bytes));
        }
    }

    u64 total = 0;
    const u32 slot = ok ? TakeSlot(*rt) : kNoSlot;
    ok = ok && slot != kNoSlot;
    if (ok)
    {
        std::lock_guard guard(rt->geometry.lock);
        for (const GeometryPiece& piece : pieces)
        {
            for (u32 done = 0; done < piece.size; done += kGeometryPiece)
            {
                DSTORAGE_REQUEST r{};
                r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
                r.Source.File.Source = file;
                r.Source.File.Offset = piece.fileOffset + done;
                r.Source.File.Size = std::min(kGeometryPiece, piece.size - done);
                r.Destination.Buffer.Resource = piece.resource;
                r.Destination.Buffer.Offset = done;
                r.Destination.Buffer.Size = r.Source.File.Size;
                rt->geometry.file->EnqueueRequest(&r);
            }
            total += piece.size;
        }
        rt->geometry.file->EnqueueStatus(rt->status, slot);
    }
    bool drained = true;
    if (ok)
    {
        drained = Drain(rt->geometry);
        if (!drained)
            Disable(*rt, "level geometry");
        ok = drained && SUCCEEDED(rt->status->GetHResult(slot));
    }
    if (drained)
        GiveSlot(*rt, slot);

    if (ok && rt->verify)
    {
        u32 mismatches = 0;
        for (const GeometryPiece& piece : pieces)
        {
            SeekTo(fs, piece.entryOffset);
            mismatches += VerifyBuffer(*rt, piece.resource, fs->pointer_contiguous(piece.size), piece.size) ? 0 : 1;
        }
        Msg("* [dstorage] verify: %zu geometry buffers, %u mismatches (%s)", pieces.size(), mismatches, fn);
        ok = !mismatches;
    }
    SeekTo(fs, 0);
    if (!ok)
    {
        // Requests still in flight keep their buffers and status entry.
        if (!drained)
        {
            ParkedLoad load;
            load.slots[0] = slot;
            for (const VertexStagingBuffer& vb : vbuffers)
                load.resources.push_back(static_cast<VertexBufferHandle>(vb));
            for (const IndexStagingBuffer& ib : ibuffers)
                load.resources.push_back(static_cast<IndexBufferHandle>(ib));
            for (IUnknown* buffer : load.resources)
                buffer->AddRef();
            Park(*rt, std::move(load));
        }
        // Today's path creates them again from the stream.
        decls.clear();
        vbuffers.clear();
        ibuffers.clear();
        Msg("! [dstorage] level geometry %s: not loaded through DirectStorage, loaded the usual way", fn);
        return false;
    }
    t_geometry_ds = true;
    t_geometry_buffers = u32(pieces.size());
    t_geometry_bytes = total;
    return true;
#endif
}

// Whether DirectStorage can run this session, not whether r__dstorage is ticked (the Video row greys itself
// on 0 and must stay tickable). A wait that timed out turns it off for the rest of the session.
bool da_dstorage_available()
{
    if (!HW.pDevice)
        return false;
    const Runtime* rt = Available(false);
    return rt && !rt->disabled;
}

void da_dstorage_shutdown()
{
    std::lock_guard guard(g_runtimeLock);
    if (Runtime* rt = g_runtime)
    {
        g_runtime = nullptr;
        // Nothing may still write into textures or read our tails. After a timeout one short wait: a runtime
        // still busy then is left alive (queues open, parked loads kept) rather than holding up the exit.
        const DWORD wait = rt->disabled ? 1000 : kWaitTimeoutMs;
        if (Drain(rt->batch, wait) && Drain(rt->sync, wait) && Drain(rt->geometry, wait))
        {
            SweepParked(*rt);
            DestroyRuntime(rt);
        }
        else
        {
            StopCodec(*rt);
            Msg("! [dstorage] requests still in flight at shutdown: the runtime is left alive");
        }
    }
    // A new device (vid_restart) tries again.
    g_state = RuntimeState::Untried;
}

#else // R4: every texture loads the usual way.

DaDsTicket* da_dstorage_begin(pcstr) { return nullptr; }
bool da_dstorage_batch_thread() { return t_batch_worker; }
void da_dstorage_batch_add(CTexture*) {}
ID3DBaseTexture* da_dstorage_end(DaDsTicket*, u32& memory)
{
    memory = 0;
    return nullptr;
}
void da_dstorage_batch_complete() {}
bool da_dstorage_load_buffers(CRender&, CStreamReader*, bool) { return false; }
bool da_dstorage_available() { return false; }
void da_dstorage_shutdown() {}
#endif

void da_dstorage_log_stats()
{
#if defined(XR_D3D12)
    if (Runtime* rt = g_runtime)
    {
        Msg("* [texload] DirectStorage: %s, %s reads, loads raw %llu, lzo %llu, failed %llu; %.0f MB direct, %.1f MB tails; codec failures %u",
            ps_r__dstorage && !rt->disabled ? "on" : "off", rt->buffered ? "buffered" : "unbuffered",
            rt->loadsRaw.load(), rt->loadsLzo.load(), rt->loadsFailed.load(), double(rt->bytesDirect.load()) / 1048576.0,
            double(rt->bytesTail.load()) / 1048576.0, rt->codecFailures.load());
        SweepParked(*rt);
        if (const size_t parked = ParkedCount(*rt))
            Msg("* [texload] DirectStorage: %zu loads still in flight (resources kept)", parked);
    }
    else
        Msg("* [texload] DirectStorage: %s", strstr(Core.Params, "-no_dstorage") ? "off (-no_dstorage)" : "not loaded");
    if (HW.pDevice)
    {
        xrd3d12::TextureHeapStats heaps;
        xrd3d12::GetTextureHeapStats(HW.pDevice, heaps);
        Msg("* [texload] texture heaps: %u (%llu MB), %u placed textures (%llu MB), %llu placed and %llu too big since start",
            heaps.heaps, heaps.heapBytes >> 20, heaps.placed, heaps.placedBytes >> 20, heaps.placedTotal, heaps.committedTotal);
    }
#else
    Msg("* [texload] DirectStorage: DirectX 12 only");
#endif
    Msg("* [texload] live textures: %u", da_texload_live_textures());
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    for (const xr_string& line : s.batch_lines)
        Msg("* [texload] %s", line.c_str());
    if (s.level_open)
    {
        report_sync(s.load, "level load so far");
        report_sync(s.game, "in game so far");
    }
    else
        Msg("* [texload] no level loaded");
}

DaGeometryTimer::DaGeometryTimer(bool alternative) : m_start(da_texload_ticks()), m_alternative(alternative)
{
    t_geometry_ds = false;
}

DaGeometryTimer::~DaGeometryTimer()
{
    const float ms = da_texload_ms_since(m_start);
    const pcstr file = m_alternative ? "level.geomx" : "level.geom";
    if (t_geometry_ds)
        Msg("* [texload] geometry %s: %u buffers, %.0f MB, %.0f ms (ds)", file, t_geometry_buffers,
            double(t_geometry_bytes) / 1048576.0, ms);
    else
        Msg("* [texload] geometry %s: %.0f ms (cpu)", file, ms);
    t_geometry_ds = false;
}

u64 da_texload_ticks()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return u64(now.QuadPart);
}

float da_texload_ms_since(u64 ticks) { return ticks_to_ms(da_texload_ticks() - ticks); }

void da_texload_note(const DaTexloadSample& sample)
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    if (t_batch_worker)
        s.batch.add(sample, false);
    else if (s.level_open && level_in_play())
        s.game.add(sample, true);
    else
        s.load.add(sample, true);
}

bool da_texload_take_fallback()
{
    const bool fallback = t_fallback;
    t_fallback = false;
    return fallback;
}

void da_texload_batch_worker() { t_batch_worker = true; }

void da_texload_batch_begin(pcstr label, size_t textures, size_t workers)
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    // A batch that finished while nobody joined it.
    if (s.batch_unreported && !s.batch_running)
        report_batch(s, 0.f);
    s.batch = {};
    xr_strcpy(s.batch_label, label);
    s.batch_textures = textures;
    s.batch_workers = workers;
    s.batch_start = da_texload_ticks();
    s.batch_end = s.batch_start;
    s.batch_running = true;
    s.batch_unreported = true;
#if defined(XR_D3D12)
    // -dstorage_verify N: N textures spread over the batch, picked as their requests go out.
    if (const u32 verify = VerifyCount())
    {
        g_verifyStride.store(u32(std::max<size_t>(1, textures / verify)), std::memory_order_relaxed);
        g_verifyNext.store(0, std::memory_order_relaxed);
    }
#endif
}

void da_texload_batch_end()
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    s.batch_end = da_texload_ticks();
    s.batch_running = false;
}

void da_texload_batch_joined(float wait_ms)
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    if (s.batch_unreported && !s.batch_running)
        report_batch(s, wait_ms);
}

void da_texload_game_begin(float upload_ms)
{
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    if (s.level_open)
        return;
    Msg("* [texload] final upload call %.0f ms", upload_ms);
    report_sync(s.load, "level load until the final upload");
    s.level_open = true;
}

void da_texload_game_end()
{
#if defined(XR_D3D12)
    if (Runtime* rt = g_runtime)
    {
        if (rt->verify)
        {
            std::lock_guard guard(rt->verifyLock);
            if (rt->verifiedSync)
                Msg("* [dstorage] verify: %u textures, %u mismatches (synchronous loads)", rt->verifiedSync, rt->mismatchedSync);
            rt->verifiedSync = rt->mismatchedSync = 0;
        }
        SweepParked(*rt);
    }
#endif
    TexloadState& s = texload();
    std::lock_guard guard(s.lock);
    if (s.level_open)
    {
        report_sync(s.load, "level unload: while loading");
        report_sync(s.game, "level unload: in game");
    }
    s.level_open = false;
    s.game = {};
    s.load = {};
}
} // namespace xray::render::RENDER_NAMESPACE
