// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Offline DirectStorage probe (docs/dead-air/dx12-features/dstorage.md 2.4, 9): loads the .dds
// entries of one .xdb archive into D3D12 textures and times the paths.
//   cpu      : today's engine path - mapped archive, LZO into the heap, rows copied into upload pages
//              under one lock (InitUploader), copies on a direct queue.
//   ds       : DirectStorage, unbuffered - file ranges read by the runtime, a custom codec (0x80 raw,
//              0x81 LZO) that inflates and re-pitches the DDS body into the runtime's staging memory,
//              the runtime copies into the texture on its own copy queue. --direct reads raw entries
//              straight into the textures (the layout-matching mips) plus a re-pitched tail.
//   dsbuf    : ds through the OS file cache.
//   dsmem    : IO ceiling - stored bytes into system memory, nothing decoded (dsmembuf: buffered).
//   gdeflate : what a GDeflate container would give (re-packed in memory, GPU decompression).
//   stats    : formats and how many entries take the 1:1 path.
//   prefix   : rtc_decompress_prefix check over every LZO entry (xr_lzo::decompress_prefix, or the
//              xrCore.dll export by ordinal with --xrcore).
//   geom     : the vertex/index buffers of level.geom entries (--entry picks the level) into DEFAULT
//              buffers: the engine's path, or DirectStorage straight from the file with --ds.
// A QA tool; not part of the engine build (tools\qa\dstorage\Run-DsProbe.ps1 builds and runs it).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dstorage.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lzo/lzo1x.h"
#include "lzo1x_decompress_safe_fast.inl"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dstorage.lib")

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Clock = std::chrono::steady_clock;

static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED 0x%08X: %s (line %d)\n", unsigned(hr_), #x, __LINE__); exit(1); } } while (0)

// ---- options ---------------------------------------------------------------------------------------
struct Options
{
    u64 limit = 700ull << 20;
    u32 threads = 4;
    u32 verify = 0;
    u32 stagingMb = 128;
    bool placed = false;
    bool direct = false;
    bool debug = false;
    bool ds = false;
    u32 pieceKb = 32768;
    std::string xrcore;
    std::string entry = "level.geom";
};
static Options g_opt;

// ---- archive index (from the engine's vfs-index cache, matched by archive size) --------------------
struct Entry
{
    std::string name;
    u32 ptr, real, comp;
};

static bool LoadIndex(const std::string& appdata, u64 archiveSize, std::vector<Entry>& out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((appdata + "\\vfs-index-*.cache").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do
    {
        const std::string path = appdata + "\\" + fd.cFileName;
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") || !f)
            continue;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 32)
        {
            fclose(f);
            continue;
        }
        std::vector<u8> d(size);
        const size_t got = fread(d.data(), 1, size, f);
        fclose(f);
        if (got != size_t(size))
            continue;
        u32 magic, count;
        u64 asize;
        memcpy(&magic, &d[0], 4);
        memcpy(&asize, &d[8], 8);
        memcpy(&count, &d[28], 4);
        if (magic != 0x31494656 || asize != archiveSize)
            continue;
        size_t off = 32;
        for (u32 i = 0; i < count && off < d.size(); ++i)
        {
            Entry e;
            e.name = reinterpret_cast<const char*>(&d[off]);
            off += e.name.size() + 1;
            if (off + 16 > d.size())
                break;
            memcpy(&e.ptr, &d[off + 4], 4);
            memcpy(&e.real, &d[off + 8], 4);
            memcpy(&e.comp, &d[off + 12], 4);
            off += 16;
            out.push_back(std::move(e));
        }
        FindClose(h);
        return true;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return false;
}

// ---- DDS ---------------------------------------------------------------------------------------
struct TexInfo
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 width = 0, height = 0, mips = 1, array = 1;
    bool cube = false;
    u32 dataOffset = 128;
    u32 blockBytes = 0; // 0 = not block compressed
    u32 texelBytes = 0;
};

static u32 FourCC(const char* s) { return u32(s[0]) | (u32(s[1]) << 8) | (u32(s[2]) << 16) | (u32(s[3]) << 24); }

static DXGI_FORMAT FromDx10(u32 f, TexInfo& t)
{
    switch (f)
    {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        t.blockBytes = 8; return DXGI_FORMAT(f);
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
        t.blockBytes = 16; return DXGI_FORMAT(f);
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
        t.texelBytes = 4; return DXGI_FORMAT(f);
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Why a texture cannot take the 1:1 path (the engine falls back to DirectXTex for those).
enum class Reject { None, Header, Format, Volume, NotMul4, Truncated };

static Reject ParseDds(const u8* p, size_t n, TexInfo& t)
{
    if (n < 128 || memcmp(p, "DDS ", 4))
        return Reject::Header;
    u32 hsize, flags, h, w, mips, pfFlags, fourcc, bits, rmask, gmask, bmask, amask, caps2;
    memcpy(&hsize, p + 4, 4);
    memcpy(&flags, p + 8, 4);
    memcpy(&h, p + 12, 4);
    memcpy(&w, p + 16, 4);
    memcpy(&mips, p + 28, 4);
    memcpy(&pfFlags, p + 80, 4);
    memcpy(&fourcc, p + 84, 4);
    memcpy(&bits, p + 88, 4);
    memcpy(&rmask, p + 92, 4);
    memcpy(&gmask, p + 96, 4);
    memcpy(&bmask, p + 100, 4);
    memcpy(&amask, p + 104, 4);
    memcpy(&caps2, p + 112, 4);
    if (hsize != 124 && hsize != 128)
        return Reject::Header;
    t.width = w;
    t.height = h;
    t.mips = (flags & 0x20000) && mips ? mips : 1;
    t.dataOffset = 128;
    if (caps2 & 0x200000)
        return Reject::Volume;
    if (caps2 & 0x200)
    {
        if ((caps2 & 0xFC00) != 0xFC00)
            return Reject::Format;
        t.cube = true;
        t.array = 6;
    }
    if (pfFlags & 0x4) // FOURCC
    {
        if (fourcc == FourCC("DX10"))
        {
            if (n < 148)
                return Reject::Header;
            u32 dxgi, dim, misc, arr;
            memcpy(&dxgi, p + 128, 4);
            memcpy(&dim, p + 132, 4);
            memcpy(&misc, p + 136, 4);
            memcpy(&arr, p + 140, 4);
            t.dataOffset = 148;
            if (dim != 3)
                return Reject::Format;
            if (misc & 0x4)
            {
                t.cube = true;
                t.array = 6 * std::max(1u, arr);
            }
            else
                t.array = std::max(1u, arr);
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
    else if ((pfFlags & 0x40) && bits == 32)
    {
        t.texelBytes = 4;
        if (rmask == 0xff0000 && gmask == 0xff00 && bmask == 0xff)
            t.format = (pfFlags & 0x1) && amask ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
        else if (rmask == 0xff && gmask == 0xff00 && bmask == 0xff0000)
            t.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (t.format == DXGI_FORMAT_UNKNOWN)
        return Reject::Format;
    if (t.blockBytes && ((t.width & 3) || (t.height & 3)))
        return Reject::NotMul4;
    return Reject::None;
}

// Tight (DDS) layout of one mip: pitch and rows.
static void Tight(const TexInfo& t, u32 mip, u64& pitch, u32& rows)
{
    const u32 w = std::max(1u, t.width >> mip), h = std::max(1u, t.height >> mip);
    if (t.blockBytes)
    {
        pitch = u64(std::max(1u, (w + 3) / 4)) * t.blockBytes;
        rows = std::max(1u, (h + 3) / 4);
    }
    else
    {
        pitch = u64(w) * t.texelBytes;
        rows = h;
    }
}

static u64 TightTotal(const TexInfo& t)
{
    u64 total = 0;
    for (u32 a = 0; a < t.array; ++a)
        for (u32 m = 0; m < t.mips; ++m)
        {
            u64 pitch;
            u32 rows;
            Tight(t, m, pitch, rows);
            total += pitch * rows;
        }
    return total;
}

static D3D12_RESOURCE_DESC Desc(const TexInfo& t)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = t.width;
    d.Height = t.height;
    d.DepthOrArraySize = UINT16(t.array);
    d.MipLevels = UINT16(t.mips);
    d.Format = t.format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

// Rows of every subresource from the tight DDS body into placed footprints (dst may be write-combined:
// written front to back, never read).
static bool Repitch(ID3D12Device* dev, const TexInfo& t, const u8* body, u8* dst, u64 dstSize)
{
    const u32 n = t.mips * t.array;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);
    UINT64 total = 0;
    const D3D12_RESOURCE_DESC d = Desc(t);
    dev->GetCopyableFootprints(&d, 0, n, 0, fp.data(), nullptr, nullptr, &total);
    if (total != dstSize)
        return false;
    const u8* src = body;
    for (u32 s = 0; s < n; ++s)
    {
        u64 pitch;
        u32 r;
        Tight(t, s % t.mips, pitch, r);
        u8* out = dst + fp[s].Offset;
        if (pitch == fp[s].Footprint.RowPitch)
            memcpy(out, src, size_t(pitch * r));
        else
            for (u32 y = 0; y < r; ++y)
                memcpy(out + u64(y) * fp[s].Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
        src += pitch * r;
    }
    return true;
}

// ---- shared state --------------------------------------------------------------------------------
struct Item
{
    const Entry* e;
    TexInfo t;
    ID3D12Resource* res = nullptr;
    u64 footprintTotal = 0;
};

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static u8* g_map;
static u64 g_mapSize;

static bool Decompress(const Entry& e, std::vector<u8>& out)
{
    out.resize(e.real);
    lzo_uint len = e.real;
    const int r = xr_lzo::decompress_safe(g_map + e.ptr, e.comp, out.data(), &len, nullptr);
    return r == LZO_E_OK && len == e.real;
}

// Header of an LZO entry without inflating all of it: the first output bytes are the header.
static bool PeekLzo(const u8* src, u32 comp, u32 real, std::vector<u8>& scratch, size_t& got, u32& fallbacks)
{
    const size_t want = std::min<size_t>(real, 64 * 1024);
    scratch.resize(want);
    lzo_uint len = want;
    xr_lzo::decompress_safe(src, comp, scratch.data(), &len, nullptr);
    if (len >= 148)
    {
        got = len;
        return true;
    }
    // A literal run longer than the window stops the safe decoder before it writes: inflate it all.
    ++fallbacks;
    scratch.resize(real);
    len = real;
    const int r = xr_lzo::decompress_safe(src, comp, scratch.data(), &len, nullptr);
    got = len;
    return r == LZO_E_OK;
}

// --placed: textures placed into 256 MB heaps (bump allocation) instead of committed.
static std::mutex g_heapLock;
static std::vector<ID3D12Heap*> g_heaps;
static u64 g_heapOffset = 0;

static ID3D12Resource* CreateTex(const TexInfo& t, D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_DESC d = Desc(t);
    ID3D12Resource* r = nullptr;
    if (g_opt.placed)
    {
        constexpr u64 kHeap = 256ull << 20;
        const D3D12_RESOURCE_ALLOCATION_INFO info = g_dev->GetResourceAllocationInfo(0, 1, &d);
        std::lock_guard g(g_heapLock);
        u64 off = (g_heapOffset + info.Alignment - 1) & ~(info.Alignment - 1);
        if (g_heaps.empty() || off + info.SizeInBytes > kHeap)
        {
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes = std::max(kHeap, info.SizeInBytes);
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            ID3D12Heap* h;
            CHECK(g_dev->CreateHeap(&hd, IID_PPV_ARGS(&h)));
            g_heaps.push_back(h);
            off = 0;
        }
        CHECK(g_dev->CreatePlacedResource(g_heaps.back(), off, &d, state, nullptr, IID_PPV_ARGS(&r)));
        g_heapOffset = off + info.SizeInBytes;
        return r;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    CHECK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static void ReleaseHeaps()
{
    for (ID3D12Heap* h : g_heaps)
        h->Release();
    g_heaps.clear();
    g_heapOffset = 0;
}

// ---- fence helper ----------------------------------------------------------------------------------
struct Fence
{
    ID3D12Fence* f = nullptr;
    HANDLE ev = nullptr;
    u64 next = 1;
    void Init()
    {
        CHECK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    u64 Signal(ID3D12CommandQueue* q)
    {
        const u64 v = next++;
        q->Signal(f, v);
        return v;
    }
    void Wait(u64 v)
    {
        if (f->GetCompletedValue() < v)
        {
            f->SetEventOnCompletion(v, ev);
            WaitForSingleObject(ev, INFINITE);
        }
    }
    void Release()
    {
        if (f)
            f->Release();
        if (ev)
            CloseHandle(ev);
        f = nullptr;
        ev = nullptr;
    }
};

struct Stats
{
    double wall = 0, produce = 0;
    u64 bytesReal = 0, bytesDisk = 0;
    u32 textures = 0, fallbacks = 0, failures = 0;
};

// ---- mode cpu: today's path ----------------------------------------------------------------------
struct UploadPage
{
    ID3D12Resource* res;
    u8* cpu;
    u64 size;
    u64 fence;
};

static UploadPage* NewPage(u64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto* p = new UploadPage{};
    CHECK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p->res)));
    D3D12_RANGE none{0, 0};
    CHECK(p->res->Map(0, &none, reinterpret_cast<void**>(&p->cpu)));
    p->size = size;
    return p;
}

static Stats RunCpu(std::vector<Item>& items, u32 threads)
{
    Stats st;
    constexpr u64 kPage = 32ull << 20, kFlush = 256ull << 20;
    std::mutex lock;
    ID3D12CommandAllocator* alloc[8];
    for (auto& a : alloc)
        CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    u64 allocFence[8]{};
    u32 allocIdx = 0;
    ID3D12GraphicsCommandList* list;
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    bool open = false;
    Fence fence;
    fence.Init();
    std::vector<UploadPage*> freePages, inList, inFlight;
    UploadPage* page = nullptr;
    u64 pageOff = 0, pending = 0;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    auto ensure = [&] {
        if (open)
            return;
        fence.Wait(allocFence[allocIdx]);
        alloc[allocIdx]->Reset();
        list->Reset(alloc[allocIdx], nullptr);
        open = true;
        // Recycle completed pages.
        const u64 done = fence.f->GetCompletedValue();
        for (size_t i = 0; i < inFlight.size();)
        {
            if (inFlight[i]->fence <= done)
            {
                if (inFlight[i]->size == kPage && freePages.size() < 4)
                    freePages.push_back(inFlight[i]);
                else
                {
                    inFlight[i]->res->Release();
                    delete inFlight[i];
                }
                inFlight[i] = inFlight.back();
                inFlight.pop_back();
            }
            else
                ++i;
        }
    };
    auto flush = [&] {
        if (!open)
            return;
        if (!barriers.empty())
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        barriers.clear();
        list->Close();
        ID3D12CommandList* l[] = {list};
        g_queue->ExecuteCommandLists(1, l);
        const u64 v = fence.Signal(g_queue);
        allocFence[allocIdx] = v;
        allocIdx = (allocIdx + 1) % 8;
        for (auto* p : inList)
            p->fence = v, inFlight.push_back(p);
        inList.clear();
        page = nullptr;
        pageOff = 0;
        pending = 0;
        open = false;
    };
    auto staging = [&](u64 size, u8*& cpu, ID3D12Resource*& res, u64& off) {
        if (size > kPage)
        {
            UploadPage* p = NewPage((size + 65535) & ~65535ull);
            inList.push_back(p);
            cpu = p->cpu;
            res = p->res;
            off = 0;
            return;
        }
        u64 o = page ? (pageOff + 511) & ~511ull : 0;
        if (!page || o + size > page->size)
        {
            if (!freePages.empty())
                page = freePages.back(), freePages.pop_back();
            else
                page = NewPage(kPage);
            inList.push_back(page);
            o = 0;
        }
        pageOff = o + size;
        cpu = page->cpu + o;
        res = page->res;
        off = o;
    };

    std::atomic<size_t> next{0};
    std::atomic<u64> bytesReal{0}, bytesDisk{0};
    const auto t0 = Clock::now();
    auto worker = [&] {
        std::vector<u8> scratch;
        for (size_t i = next++; i < items.size(); i = next++)
        {
            Item& it = items[i];
            const Entry& e = *it.e;
            const u8* dds = g_map + e.ptr;
            if (e.real != e.comp)
            {
                if (!Decompress(e, scratch))
                    continue;
                dds = scratch.data();
            }
            TexInfo t;
            if (ParseDds(dds, e.real, t) != Reject::None)
                continue;
            it.t = t;
            it.res = CreateTex(t, D3D12_RESOURCE_STATE_COPY_DEST);
            const u32 n = t.mips * t.array;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);
            UINT64 total = 0;
            const D3D12_RESOURCE_DESC d = Desc(t);
            g_dev->GetCopyableFootprints(&d, 0, n, 0, fp.data(), nullptr, nullptr, &total);
            {
                std::lock_guard g(lock);
                ensure();
                u8* cpu;
                ID3D12Resource* sres;
                u64 soff;
                staging(total, cpu, sres, soff);
                const u8* src = dds + t.dataOffset;
                for (u32 s = 0; s < n; ++s)
                {
                    u64 pitch;
                    u32 r;
                    Tight(t, s % t.mips, pitch, r);
                    for (u32 y = 0; y < r; ++y)
                        memcpy(cpu + fp[s].Offset + u64(y) * fp[s].Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
                    src += pitch * r;
                    D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
                    dl.pResource = it.res;
                    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dl.SubresourceIndex = s;
                    sl.pResource = sres;
                    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    sl.PlacedFootprint = fp[s];
                    sl.PlacedFootprint.Offset += soff;
                    list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                }
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = it.res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                barriers.push_back(b);
                pending += total;
                if (pending > kFlush)
                    flush();
            }
            bytesReal += e.real;
            bytesDisk += e.comp;
        }
    };
    std::vector<std::thread> pool;
    for (u32 i = 0; i < threads; ++i)
        pool.emplace_back(worker);
    for (auto& th : pool)
        th.join();
    st.produce = Ms(t0, Clock::now());
    {
        std::lock_guard g(lock);
        flush();
    }
    fence.Wait(fence.next - 1);
    st.wall = Ms(t0, Clock::now());
    st.bytesReal = bytesReal;
    st.bytesDisk = bytesDisk;
    for (auto& it : items)
        if (it.res)
            ++st.textures;
    for (auto* p : inFlight)
    {
        p->res->Release();
        delete p;
    }
    for (auto* p : freePages)
    {
        p->res->Release();
        delete p;
    }
    for (auto& a : alloc)
        a->Release();
    list->Release();
    fence.Release();
    return st;
}

// ---- mode ds -----------------------------------------------------------------------------------
static IDStorageFactory* g_factory;

static IDStorageFactory* Factory(bool buffered)
{
    if (g_factory)
        return g_factory;
    DSTORAGE_CONFIGURATION1 cfg{};
    // Every request uses an application format: no built-in decompression threads.
    cfg.NumBuiltInCpuDecompressionThreads = DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION;
    cfg.DisableBypassIO = buffered;
    cfg.ForceFileBuffering = buffered;
    cfg.DisableTelemetry = TRUE;
    CHECK(DStorageSetConfiguration1(&cfg));
    CHECK(DStorageGetFactory(IID_PPV_ARGS(&g_factory)));
    CHECK(g_factory->SetStagingBufferSize(g_opt.stagingMb << 20));
    if (g_opt.debug)
        g_factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_BREAK_ON_ERROR);
    return g_factory;
}

static Stats RunDs(std::vector<Item>& items, const std::wstring& archivePath, u32 threads, bool buffered, bool memoryOnly)
{
    Stats st;
    IDStorageFactory* factory = Factory(buffered);
    IDStorageCustomDecompressionQueue1* cdq;
    CHECK(factory->QueryInterface(IID_PPV_ARGS(&cdq)));
    IDStorageFile* file;
    CHECK(factory->OpenFile(archivePath.c_str(), IID_PPV_ARGS(&file)));
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "dsprobe";
    qd.Device = g_dev;
    IDStorageQueue2* queue;
    CHECK(factory->CreateQueue(&qd, IID_PPV_ARGS(&queue)));
    // --direct: raw entries skip the codec - the mips whose DDS layout already is the placed footprint
    // layout are read straight into the texture, the small tail goes through a memory queue.
    const bool direct = g_opt.direct && !memoryOnly;
    IDStorageQueue2* memQueue = nullptr;
    if (direct)
    {
        DSTORAGE_QUEUE_DESC md = qd;
        md.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        md.Name = "dsprobe-tail";
        CHECK(factory->CreateQueue(&md, IID_PPV_ARGS(&memQueue)));
    }
    std::deque<std::vector<u8>> tails;
    u64 directBytes = 0, tailBytes = 0;
    Fence fence;
    fence.Init();

    // Custom codec workers.
    std::atomic<bool> stop{false};
    std::atomic<u32> codecFailures{0};
    std::atomic<u64> decodeUs{0};
    std::mutex qlock;
    std::condition_variable qcv;
    std::deque<DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST> work;
    std::thread dispatcher([&] {
        HANDLE ev = cdq->GetEvent();
        DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST req[64];
        while (!stop)
        {
            if (WaitForSingleObject(ev, 5) != WAIT_OBJECT_0)
                continue;
            for (;;)
            {
                UINT32 n = 0;
                cdq->GetRequests1(DSTORAGE_GET_REQUEST_FLAG_SELECT_CUSTOM, 64, req, &n);
                if (!n)
                    break;
                {
                    std::lock_guard g(qlock);
                    work.insert(work.end(), req, req + n);
                }
                qcv.notify_all();
            }
        }
    });
    std::vector<std::thread> workers;
    for (u32 i = 0; i < threads; ++i)
        workers.emplace_back([&] {
            std::vector<u8> scratch;
            for (;;)
            {
                DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST r;
                {
                    std::unique_lock g(qlock);
                    qcv.wait(g, [&] { return stop || !work.empty(); });
                    if (work.empty())
                        return;
                    r = work.front();
                    work.pop_front();
                }
                const auto a = Clock::now();
                const u8* dds = static_cast<const u8*>(r.SrcBuffer);
                size_t ddsSize = size_t(r.SrcSize);
                HRESULT hr = S_OK;
                if (r.CompressionFormat & 1)
                {
                    scratch.resize(size_t(r.DstSize) + 4096);
                    lzo_uint len = scratch.size();
                    const int z = xr_lzo::decompress_safe(dds, lzo_uint(r.SrcSize), scratch.data(), &len, nullptr);
                    if (z != LZO_E_OK)
                        hr = E_FAIL;
                    dds = scratch.data();
                    ddsSize = len;
                }
                TexInfo t;
                bool ok = false;
                if (SUCCEEDED(hr) && ParseDds(dds, ddsSize, t) == Reject::None && t.dataOffset + TightTotal(t) <= ddsSize)
                    ok = Repitch(g_dev, t, dds + t.dataOffset, static_cast<u8*>(r.DstBuffer), r.DstSize);
                if (!ok)
                {
                    hr = E_FAIL;
                    ++codecFailures;
                }
                decodeUs += u64(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
                DSTORAGE_CUSTOM_DECOMPRESSION_RESULT res{r.Id, hr};
                cdq->SetRequestResults(1, &res);
            }
        });

    // Producer: header peek + resource + one request per texture (what CTexture::Load would enqueue).
    double createMs = 0;
    const auto t0 = Clock::now();
    std::vector<u8> peek;
    std::vector<u8> sink;
    u64 bytesReal = 0, bytesDisk = 0;
    for (auto& it : items)
    {
        const Entry& e = *it.e;
        if (memoryOnly)
        {
            // IO ceiling: the stored bytes into system memory, nothing decoded, no GPU.
            if (sink.size() < (256u << 20))
                sink.resize(256u << 20);
            if (e.comp > sink.size())
                continue;
            DSTORAGE_REQUEST r{};
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.File.Source = file;
            r.Source.File.Offset = e.ptr;
            r.Source.File.Size = e.comp;
            r.Destination.Memory.Buffer = sink.data() + (bytesDisk % (sink.size() - e.comp + 1));
            r.Destination.Memory.Size = e.comp;
            queue->EnqueueRequest(&r);
            bytesReal += e.real;
            bytesDisk += e.comp;
            ++st.textures;
            continue;
        }
        const u8* src = g_map + e.ptr;
        const u8* hdr = src;
        size_t hdrSize = e.real;
        const bool lzo = e.real != e.comp;
        if (lzo)
        {
            size_t got = 0;
            if (!PeekLzo(src, e.comp, e.real, peek, got, st.fallbacks))
                continue;
            hdr = peek.data();
            hdrSize = got;
        }
        if (ParseDds(hdr, hdrSize, it.t) != Reject::None)
            continue;
        const auto c0 = Clock::now();
        it.res = CreateTex(it.t, D3D12_RESOURCE_STATE_COMMON);
        createMs += Ms(c0, Clock::now());
        const D3D12_RESOURCE_DESC d = Desc(it.t);
        g_dev->GetCopyableFootprints(&d, 0, it.t.mips * it.t.array, 0, nullptr, nullptr, nullptr, &it.footprintTotal);
        if (direct && !lzo)
        {
            const TexInfo& t = it.t;
            u64 fileOff = u64(e.ptr) + t.dataOffset;
            for (u32 a = 0; a < t.array; ++a)
            {
                const u32 s0 = a * t.mips;
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.mips);
                g_dev->GetCopyableFootprints(&d, s0, t.mips, 0, fp.data(), nullptr, nullptr, nullptr);
                // k = mips whose tight layout (pitch, offset from the slice start) is the footprint.
                u32 k = 0;
                u64 tightOff = 0;
                for (; k < t.mips; ++k)
                {
                    u64 pitch;
                    u32 r;
                    Tight(t, k, pitch, r);
                    if (pitch != fp[k].Footprint.RowPitch || tightOff != fp[k].Offset)
                        break;
                    tightOff += pitch * r;
                }
                u64 sliceBytes = 0;
                for (u32 m = 0; m < t.mips; ++m)
                {
                    u64 pitch;
                    u32 r;
                    Tight(t, m, pitch, r);
                    sliceBytes += pitch * r;
                }
                if (k)
                {
                    DSTORAGE_REQUEST r{};
                    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE;
                    r.Source.File.Source = file;
                    r.Source.File.Offset = fileOff;
                    r.Source.File.Size = u32(tightOff);
                    r.Destination.MultipleSubresourcesRange.Resource = it.res;
                    r.Destination.MultipleSubresourcesRange.FirstSubresource = s0;
                    r.Destination.MultipleSubresourcesRange.NumSubresources = k;
                    queue->EnqueueRequest(&r);
                    directBytes += tightOff;
                }
                if (k < t.mips)
                {
                    // The tail, re-pitched here from the mapped archive (a few KB for most textures).
                    UINT64 tailTotal = 0;
                    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> tfp(t.mips - k);
                    g_dev->GetCopyableFootprints(&d, s0 + k, t.mips - k, 0, tfp.data(), nullptr, nullptr, &tailTotal);
                    std::vector<u8>& buf = tails.emplace_back(size_t(tailTotal), u8(0));
                    const u8* tsrc = g_map + fileOff + tightOff;
                    for (u32 m = k; m < t.mips; ++m)
                    {
                        u64 pitch;
                        u32 r;
                        Tight(t, m, pitch, r);
                        for (u32 y = 0; y < r; ++y)
                            memcpy(buf.data() + tfp[m - k].Offset + u64(y) * tfp[m - k].Footprint.RowPitch, tsrc + u64(y) * pitch, size_t(pitch));
                        tsrc += pitch * r;
                    }
                    DSTORAGE_REQUEST r{};
                    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
                    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE;
                    r.Source.Memory.Source = buf.data();
                    r.Source.Memory.Size = u32(buf.size());
                    r.Destination.MultipleSubresourcesRange.Resource = it.res;
                    r.Destination.MultipleSubresourcesRange.FirstSubresource = s0 + k;
                    r.Destination.MultipleSubresourcesRange.NumSubresources = t.mips - k;
                    memQueue->EnqueueRequest(&r);
                    tailBytes += sliceBytes - tightOff;
                }
                fileOff += sliceBytes;
            }
            bytesReal += e.real;
            bytesDisk += e.comp;
            ++st.textures;
            continue;
        }
        DSTORAGE_REQUEST r{};
        r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT(DSTORAGE_CUSTOM_COMPRESSION_0 + (lzo ? 1 : 0));
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
        r.Source.File.Source = file;
        r.Source.File.Offset = e.ptr;
        r.Source.File.Size = e.comp;
        r.Destination.MultipleSubresources.Resource = it.res;
        r.Destination.MultipleSubresources.FirstSubresource = 0;
        r.UncompressedSize = u32(it.footprintTotal);
        r.Name = e.name.c_str();
        queue->EnqueueRequest(&r);
        bytesReal += e.real;
        bytesDisk += e.comp;
        ++st.textures;
    }
    const u64 v = fence.next++;
    queue->EnqueueSignal(fence.f, v);
    queue->Submit();
    Fence memFence;
    u64 mv = 0;
    if (memQueue)
    {
        memFence.Init();
        mv = memFence.next++;
        memQueue->EnqueueSignal(memFence.f, mv);
        memQueue->Submit();
    }
    st.produce = Ms(t0, Clock::now());
    fence.Wait(v);
    if (memQueue)
        memFence.Wait(mv);
    st.wall = Ms(t0, Clock::now());
    st.bytesReal = bytesReal;
    st.bytesDisk = bytesDisk;
    if (memQueue)
    {
        printf("  direct: %.1f MB read straight into textures, %.2f MB of tails through memory (%zu requests)\n",
            directBytes / 1048576.0, tailBytes / 1048576.0, tails.size());
        DSTORAGE_ERROR_RECORD merr{};
        memQueue->RetrieveErrorRecord(&merr);
        if (merr.FailureCount)
            printf("  tail queue: %u failures (first hr 0x%08X)\n", merr.FailureCount, unsigned(merr.FirstFailure.HResult));
        st.failures += merr.FailureCount;
        memQueue->Close();
        memQueue->Release();
        memFence.Release();
    }

    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    st.failures += err.FailureCount + codecFailures;
    printf("  ds: %u failures (first hr 0x%08X), codec failures %u\n", err.FailureCount,
        unsigned(err.FirstFailure.HResult), codecFailures.load());
    printf("  ds: codec cpu time %.1f ms total over %u workers; texture creation %.1f ms on the producer\n",
        decodeUs / 1000.0, threads, createMs);

    stop = true;
    qcv.notify_all();
    dispatcher.join();
    for (auto& w : workers)
        w.join();
    queue->Close();
    queue->Release();
    file->Close();
    file->Release();
    cdq->Release();
    fence.Release();
    return st;
}

// ---- mode gdeflate: what an opt-in GPU-decompressed container would give -----------------------
// Each texture is re-pitched to its placed footprints and GDeflate-compressed (offline in a real
// pipeline), then loaded from memory through DirectStorage with GPU decompression.
static Stats RunGDeflate(std::vector<Item>& items)
{
    Stats st;
    IDStorageCompressionCodec* codec;
    CHECK(DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(&codec)));
    struct Packed
    {
        std::vector<u8> data;
        u64 total;
    };
    std::vector<Packed> packed(items.size());
    std::vector<u8> scratch, foot;
    u64 real = 0, comp = 0, stored = 0;
    const auto c0 = Clock::now();
    for (size_t i = 0; i < items.size(); ++i)
    {
        const Entry& e = *items[i].e;
        const u8* dds = g_map + e.ptr;
        if (e.real != e.comp)
        {
            if (!Decompress(e, scratch))
                continue;
            dds = scratch.data();
        }
        if (ParseDds(dds, e.real, items[i].t) != Reject::None)
            continue;
        const D3D12_RESOURCE_DESC d = Desc(items[i].t);
        UINT64 total = 0;
        g_dev->GetCopyableFootprints(&d, 0, items[i].t.mips * items[i].t.array, 0, nullptr, nullptr, nullptr, &total);
        foot.assign(size_t(total), 0);
        if (!Repitch(g_dev, items[i].t, dds + items[i].t.dataOffset, foot.data(), total))
            continue;
        packed[i].data.resize(codec->CompressBufferBound(size_t(total)));
        size_t out = 0;
        CHECK(codec->CompressBuffer(foot.data(), size_t(total), DSTORAGE_COMPRESSION_DEFAULT, packed[i].data.data(), packed[i].data.size(), &out));
        packed[i].data.resize(out);
        packed[i].total = total;
        real += total;
        comp += out;
        stored += e.comp;
    }
    const double compressMs = Ms(c0, Clock::now());
    codec->Release();
    printf("  gdeflate: %.1f MB footprint data -> %.1f MB (%.3f), stored LZO/raw was %.1f MB; offline compress %.0f ms\n",
        real / 1048576.0, comp / 1048576.0, real ? double(comp) / real : 0.0, stored / 1048576.0, compressMs);

    IDStorageFactory* factory = Factory(false);
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "dsprobe-gdeflate";
    qd.Device = g_dev;
    IDStorageQueue2* queue;
    CHECK(factory->CreateQueue(&qd, IID_PPV_ARGS(&queue)));
    const DSTORAGE_COMPRESSION_SUPPORT support = queue->GetCompressionSupport(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE);
    printf("  gdeflate support flags 0x%X (1 gpu-optimized, 2 gpu-fallback, 4 cpu, 8 compute queue, 16 copy queue)\n", unsigned(support));
    Fence fence;
    fence.Init();
    const auto t0 = Clock::now();
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (packed[i].data.empty())
            continue;
        items[i].res = CreateTex(items[i].t, D3D12_RESOURCE_STATE_COMMON);
        DSTORAGE_REQUEST r{};
        r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
        r.Source.Memory.Source = packed[i].data.data();
        r.Source.Memory.Size = u32(packed[i].data.size());
        r.Destination.MultipleSubresources.Resource = items[i].res;
        r.UncompressedSize = u32(packed[i].total);
        queue->EnqueueRequest(&r);
        st.bytesReal += items[i].e->real;
        st.bytesDisk += packed[i].data.size();
        ++st.textures;
    }
    const u64 v = fence.next++;
    queue->EnqueueSignal(fence.f, v);
    queue->Submit();
    st.produce = Ms(t0, Clock::now());
    fence.Wait(v);
    st.wall = Ms(t0, Clock::now());
    DSTORAGE_ERROR_RECORD err{};
    queue->RetrieveErrorRecord(&err);
    st.failures += err.FailureCount;
    if (err.FailureCount)
        printf("  gdeflate: %u failures (first hr 0x%08X)\n", err.FailureCount, unsigned(err.FirstFailure.HResult));
    queue->Close();
    queue->Release();
    fence.Release();
    return st;
}

// ---- mode geom: level.geom vertex and index buffers (dstorage.md 3.8, PLAN S9) -----------------------
// The VB (9) and IB (10) chunks of every level.geom* entry whose name contains --entry, as the engine's
// LoadBuffers reads them. cpu: the mapped bytes copied into upload pages and copied into DEFAULT buffers on a
// direct queue (the layer's static buffers); --ds: DirectStorage reads the file ranges straight into them.
struct GeomRange
{
    u64 offset; // in the archive
    u32 size;
    ID3D12Resource* buffer;
};

static u32 DeclTypeSize(u8 type)
{
    switch (type)
    {
    case 0: case 4: case 5: case 6: case 8: case 9: case 11: case 13: case 14: case 15: return 4;
    case 1: case 7: case 10: case 12: case 16: return 8;
    case 2: return 12;
    case 3: return 16;
    default: return 0;
    }
}

static bool GeomRanges(const Entry& e, std::vector<GeomRange>& out)
{
    if (e.real != e.comp)
        return false;
    const u8* p = g_map + e.ptr;
    const u64 size = e.real;
    for (u64 off = 0; off + 8 <= size;)
    {
        u32 id, sz;
        memcpy(&id, p + off, 4);
        memcpy(&sz, p + off + 4, 4);
        const u64 data = off + 8, end = data + sz;
        if ((id & 0x80000000u) || end > size)
            return false;
        if (id == 9 || id == 10)
        {
            u64 q = data;
            u32 count;
            memcpy(&count, p + q, 4);
            q += 4;
            for (u32 i = 0; i < count; ++i)
            {
                u64 bytes = 0;
                if (id == 9)
                {
                    u32 vertexSize = 0;
                    for (;;)
                    {
                        if (q + 8 > end)
                            return false;
                        u16 stream, elementOffset;
                        memcpy(&stream, p + q, 2);
                        memcpy(&elementOffset, p + q + 2, 2);
                        const u8 type = p[q + 4];
                        q += 8;
                        if (stream == 0xFF)
                            break;
                        if (!stream)
                            vertexSize = std::max(vertexSize, u32(elementOffset) + DeclTypeSize(type));
                    }
                    u32 vertices;
                    memcpy(&vertices, p + q, 4);
                    q += 4;
                    bytes = u64(vertices) * vertexSize;
                }
                else
                {
                    u32 indices;
                    memcpy(&indices, p + q, 4);
                    q += 4;
                    bytes = u64(indices) * 2;
                }
                if (q + bytes > end)
                    return false;
                out.push_back({e.ptr + q, u32(bytes), nullptr});
                q += bytes;
            }
        }
        off = end;
    }
    return !out.empty();
}

static ID3D12Resource* NewBuffer(u64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<u64>(256, (size + 255) & ~255ull);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    CHECK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

static Stats RunGeom(std::vector<GeomRange>& ranges, const std::wstring& archivePath, bool ds)
{
    Stats st;
    Fence fence;
    fence.Init();
    // The DirectStorage factory, file and queue exist before the clock starts (the engine creates them once).
    IDStorageFile* file = nullptr;
    IDStorageQueue2* queue = nullptr;
    if (ds)
    {
        IDStorageFactory* factory = Factory(false);
        CHECK(factory->OpenFile(archivePath.c_str(), IID_PPV_ARGS(&file)));
        DSTORAGE_QUEUE_DESC qd{};
        qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        qd.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        qd.Priority = DSTORAGE_PRIORITY_NORMAL;
        qd.Name = "dsprobe-geom";
        qd.Device = g_dev;
        CHECK(factory->CreateQueue(&qd, IID_PPV_ARGS(&queue)));
    }
    const auto t0 = Clock::now();
    if (!ds)
    {
        // One thread (LoadBuffers runs on the main thread), 32 MB upload pages, a submission per 256 MB.
        ID3D12CommandAllocator* alloc;
        CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
        ID3D12GraphicsCommandList* list;
        CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
        std::vector<UploadPage*> pages;
        UploadPage* page = nullptr;
        u64 pageOff = 0, pending = 0;
        for (GeomRange& r : ranges)
        {
            r.buffer = NewBuffer(r.size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            UploadPage* target;
            u64 offset;
            if (r.size > (32u << 20))
            {
                target = NewPage((u64(r.size) + 65535) & ~65535ull);
                pages.push_back(target);
                offset = 0;
            }
            else
            {
                if (!page || pageOff + r.size > page->size)
                {
                    page = NewPage(32u << 20);
                    pages.push_back(page);
                    pageOff = 0;
                }
                target = page;
                offset = pageOff;
                pageOff = (pageOff + r.size + 255) & ~255ull;
            }
            memcpy(target->cpu + offset, g_map + r.offset, r.size);
            list->CopyBufferRegion(r.buffer, 0, target->res, offset, r.size);
            pending += r.size;
            st.bytesReal += r.size;
            ++st.textures;
            if (pending > (256u << 20))
            {
                list->Close();
                ID3D12CommandList* l[] = {list};
                g_queue->ExecuteCommandLists(1, l);
                fence.Wait(fence.Signal(g_queue));
                alloc->Reset();
                list->Reset(alloc, nullptr);
                pending = 0;
            }
        }
        list->Close();
        ID3D12CommandList* l[] = {list};
        g_queue->ExecuteCommandLists(1, l);
        st.produce = Ms(t0, Clock::now());
        fence.Wait(fence.Signal(g_queue));
        for (UploadPage* p : pages)
        {
            p->res->Release();
            delete p;
        }
        list->Release();
        alloc->Release();
    }
    else
    {
        // Pieces within the staging buffer; smaller ones keep more reads in flight.
        const u32 piece = std::min<u32>(g_opt.pieceKb << 10, g_opt.stagingMb << 20);
        for (GeomRange& r : ranges)
        {
            r.buffer = NewBuffer(r.size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            for (u32 done = 0; done < r.size; done += piece)
            {
                DSTORAGE_REQUEST q{};
                q.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                q.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
                q.Source.File.Source = file;
                q.Source.File.Offset = r.offset + done;
                q.Source.File.Size = std::min(piece, r.size - done);
                q.Destination.Buffer.Resource = r.buffer;
                q.Destination.Buffer.Offset = done;
                q.Destination.Buffer.Size = q.Source.File.Size;
                queue->EnqueueRequest(&q);
            }
            st.bytesReal += r.size;
            ++st.textures;
        }
        const u64 v = fence.next++;
        queue->EnqueueSignal(fence.f, v);
        queue->Submit();
        st.produce = Ms(t0, Clock::now());
        fence.Wait(v);
        DSTORAGE_ERROR_RECORD err{};
        queue->RetrieveErrorRecord(&err);
        st.failures += err.FailureCount;
        if (err.FailureCount)
            printf("  geom: %u failures (first hr 0x%08X)\n", err.FailureCount, unsigned(err.FirstFailure.HResult));
        queue->Close();
        queue->Release();
        file->Close();
        file->Release();
    }
    st.wall = Ms(t0, Clock::now());
    st.bytesDisk = st.bytesReal;
    fence.Release();
    return st;
}

// Every buffer read back and compared with the archive bytes.
static u32 VerifyGeom(std::vector<GeomRange>& ranges)
{
    u32 bad = 0;
    Fence fence;
    fence.Init();
    ID3D12CommandAllocator* alloc;
    CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList* list;
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    for (GeomRange& r : ranges)
    {
        ID3D12Resource* rb = NewBuffer(r.size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->CopyBufferRegion(rb, 0, r.buffer, 0, r.size);
        list->Close();
        ID3D12CommandList* l[] = {list};
        g_queue->ExecuteCommandLists(1, l);
        fence.Wait(fence.Signal(g_queue));
        u8* p;
        CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)));
        if (memcmp(p, g_map + r.offset, r.size))
            ++bad;
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        rb->Release();
    }
    printf("  verify: %zu buffers compared, %u mismatches\n", ranges.size(), bad);
    list->Release();
    alloc->Release();
    fence.Release();
    return bad;
}

// ---- verification: texture contents equal the DDS body -------------------------------------------
// The runs leave textures in COMMON (DirectStorage; COPY_SOURCE by implicit promotion) or COPY_SOURCE (cpu).
static u32 Verify(std::vector<Item>& items, size_t count)
{
    u32 bad = 0, checked = 0;
    Fence fence;
    fence.Init();
    ID3D12CommandAllocator* alloc;
    CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList* list;
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    std::vector<u8> scratch;
    size_t live = 0;
    for (const auto& it : items)
        live += it.res ? 1 : 0;
    const size_t stride = std::max<size_t>(1, live / std::max<size_t>(1, count));
    size_t seen = 0;
    for (size_t i = 0; i < items.size() && checked < count; ++i)
    {
        Item& it = items[i];
        if (!it.res || seen++ % stride)
            continue;
        const Entry& e = *it.e;
        const u8* dds = g_map + e.ptr;
        if (e.real != e.comp)
        {
            if (!Decompress(e, scratch))
                continue;
            dds = scratch.data();
        }
        TexInfo t;
        if (ParseDds(dds, e.real, t) != Reject::None)
            continue;
        const u32 n = t.mips * t.array;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);
        UINT64 total = 0;
        const D3D12_RESOURCE_DESC d = Desc(t);
        g_dev->GetCopyableFootprints(&d, 0, n, 0, fp.data(), nullptr, nullptr, &total);
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
        ID3D12Resource* rb;
        CHECK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)));
        alloc->Reset();
        list->Reset(alloc, nullptr);
        for (u32 s = 0; s < n; ++s)
        {
            D3D12_TEXTURE_COPY_LOCATION dl{}, sl{};
            sl.pResource = it.res;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.SubresourceIndex = s;
            dl.pResource = rb;
            dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dl.PlacedFootprint = fp[s];
            list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
        list->Close();
        ID3D12CommandList* l[] = {list};
        g_queue->ExecuteCommandLists(1, l);
        fence.Wait(fence.Signal(g_queue));
        u8* p;
        CHECK(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)));
        const u8* src = dds + t.dataOffset;
        bool same = true;
        for (u32 s = 0; s < n && same; ++s)
        {
            u64 pitch;
            u32 r;
            Tight(t, s % t.mips, pitch, r);
            for (u32 y = 0; y < r && same; ++y)
                same = !memcmp(p + fp[s].Offset + u64(y) * fp[s].Footprint.RowPitch, src + u64(y) * pitch, size_t(pitch));
            src += pitch * r;
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        rb->Release();
        ++checked;
        if (!same)
        {
            ++bad;
            printf("  verify MISMATCH: %s\n", e.name.c_str());
        }
    }
    printf("  verify: %u textures compared, %u mismatches\n", checked, bad);
    list->Release();
    alloc->Release();
    fence.Release();
    return bad;
}

static void ReleaseItems(std::vector<Item>& items)
{
    for (auto& it : items)
        if (it.res)
            it.res->Release(), it.res = nullptr;
    ReleaseHeaps();
}

// ---- mode prefix: rtc_decompress_prefix against the full decode ---------------------------------------
// Every LZO .dds entry is inflated whole; each window must come back as exactly min(window, size) bytes
// equal to the start of the whole. "safe short" counts the windows where the safe decoder (which stops
// before a run that does not fit) produced less than a DDS header.
using PrefixFn = size_t (*)(void*, size_t, const void*, size_t);

static int RunPrefix(const std::vector<const Entry*>& dds)
{
    PrefixFn exported = nullptr;
    if (!g_opt.xrcore.empty())
    {
        HMODULE core = LoadLibraryExA(g_opt.xrcore.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core)
            return printf("prefix: cannot load %s (error %lu)\n", g_opt.xrcore.c_str(), GetLastError()), 1;
        exported = reinterpret_cast<PrefixFn>(GetProcAddress(core, MAKEINTRESOURCEA(1519)));
        const auto byName = reinterpret_cast<PrefixFn>(GetProcAddress(core, "?rtc_decompress_prefix@@YA_KPEAX_KPEBX1@Z"));
        if (!exported || exported != byName)
            return printf("prefix: ordinal 1519 of %s is not rtc_decompress_prefix\n", g_opt.xrcore.c_str()), 1;
        printf("prefix: export ordinal 1519 = rtc_decompress_prefix of %s\n", g_opt.xrcore.c_str());
    }
    // Odd sizes clip literal runs and matches mid-way; 256 is the engine's header window, 64 KB the probe's.
    constexpr size_t kWindows[] = {1, 37, 148, 256, 4097, 65536};
    constexpr size_t kCount = std::size(kWindows);
    u32 lzo = 0, broken = 0, bad = 0, safeShort[kCount]{};
    std::vector<u8> whole, window(65536 + 64);
    for (const Entry* e : dds)
    {
        if (e->real == e->comp)
            continue;
        ++lzo;
        if (!Decompress(*e, whole))
        {
            ++broken;
            printf("  prefix BROKEN stream: %s\n", e->name.c_str());
            continue;
        }
        const u8* src = g_map + e->ptr;
        for (size_t w = 0; w < kCount; ++w)
        {
            const size_t want = std::min<size_t>(kWindows[w], e->real);
            lzo_uint safeLen = want;
            xr_lzo::decompress_safe(src, e->comp, window.data(), &safeLen, nullptr);
            if (safeLen < std::min<size_t>(want, 148))
            {
                ++safeShort[w];
                if (kWindows[w] == 65536)
                    printf("  safe decoder short at 64 KB: %s (%zu of %zu bytes)\n", e->name.c_str(), size_t(safeLen), want);
            }
            // A canary after the window catches a write past its end.
            window[want] = 0xA5;
            lzo_uint len = want;
            const int r = xr_lzo::decompress_prefix(src, e->comp, window.data(), &len);
            bool ok = r == LZO_E_OK && len == want && window[want] == 0xA5 && !memcmp(window.data(), whole.data(), want);
            if (exported)
            {
                window[want] = 0x5A;
                const size_t got = exported(window.data(), want, src, e->comp);
                ok = ok && got == want && window[want] == 0x5A && !memcmp(window.data(), whole.data(), want);
            }
            if (!ok)
            {
                ++bad;
                printf("  prefix MISMATCH: %s window %zu (inline r=%d len=%zu)\n", e->name.c_str(), kWindows[w], r, size_t(len));
            }
        }
    }
    printf("prefix: %u lzo dds, %u broken streams, %u mismatches%s; safe decoder short (< header):", lzo, broken, bad,
        exported ? " (inline + export)" : " (inline)");
    for (size_t w = 0; w < kCount; ++w)
        printf(" %zu B: %u%s", kWindows[w], safeShort[w], w + 1 < kCount ? "," : "\n");
    return broken || bad ? 1 : 0;
}

static int Usage()
{
    printf("dsprobe <archive> <appdata> <cpu|ds|dsbuf|dsmem|dsmembuf|gdeflate|stats|prefix|geom> [--limit MB] [--threads N]\n"
           "        [--verify N] [--placed] [--direct] [--staging MB] [--debug] [--xrcore <xrCore.dll>] [--ds] [--entry <name part>]\n");
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 4)
        return Usage();
    const std::string archive = argv[1], appdata = argv[2], mode = argv[3];
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--limit" && hasValue)
            g_opt.limit = _strtoui64(argv[++i], nullptr, 10) << 20;
        else if (a == "--threads" && hasValue)
            g_opt.threads = std::max(1, atoi(argv[++i]));
        else if (a == "--verify" && hasValue)
            g_opt.verify = u32(atoi(argv[++i]));
        else if (a == "--staging" && hasValue)
            g_opt.stagingMb = u32(std::max(1, atoi(argv[++i])));
        else if (a == "--xrcore" && hasValue)
            g_opt.xrcore = argv[++i];
        else if (a == "--placed")
            g_opt.placed = true;
        else if (a == "--direct")
            g_opt.direct = true;
        else if (a == "--debug")
            g_opt.debug = true;
        else if (a == "--ds")
            g_opt.ds = true;
        else if (a == "--piece" && hasValue)
            g_opt.pieceKb = u32(std::max(64, atoi(argv[++i])));
        else if (a == "--entry" && hasValue)
            g_opt.entry = argv[++i];
        else
            return Usage();
    }

    HANDLE hf = CreateFileA(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return printf("cannot open %s\n", archive.c_str()), 1;
    LARGE_INTEGER sz;
    GetFileSizeEx(hf, &sz);
    g_mapSize = u64(sz.QuadPart);
    HANDLE hm = CreateFileMappingA(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_map = static_cast<u8*>(MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0));
    if (!g_map)
        return printf("cannot map %s\n", archive.c_str()), 1;

    std::vector<Entry> entries;
    if (!LoadIndex(appdata, g_mapSize, entries))
        return printf("no vfs-index cache for an archive of %llu bytes in %s\n", g_mapSize, appdata.c_str()), 1;
    std::vector<const Entry*> dds;
    for (auto& e : entries)
    {
        const size_t n = e.name.size();
        if (n > 4 && !_stricmp(e.name.c_str() + n - 4, ".dds") && e.real && u64(e.ptr) + e.comp <= g_mapSize)
            dds.push_back(&e);
    }
    std::sort(dds.begin(), dds.end(), [](const Entry* a, const Entry* b) { return a->name < b->name; });

    if (mode == "stats")
    {
        // Formats and how many textures the 1:1 path takes (headers inflated from the map).
        u32 counts[6]{}, lzo = 0;
        std::vector<u8> scratch;
        u32 fb = 0;
        for (const Entry* e : dds)
        {
            const u8* h = g_map + e->ptr;
            size_t hs = e->real;
            if (e->real != e->comp)
            {
                ++lzo;
                size_t got;
                if (!PeekLzo(h, e->comp, e->real, scratch, got, fb))
                    continue;
                h = scratch.data();
                hs = got;
            }
            TexInfo t;
            ++counts[int(ParseDds(h, hs, t))];
        }
        printf("%zu dds (%u lzo): ok %u, header %u, format %u, volume %u, not-mul4 %u; peek fallbacks %u\n", dds.size(), lzo,
            counts[0], counts[1], counts[2], counts[3], counts[4], fb);
        return 0;
    }
    if (mode == "prefix")
        return RunPrefix(dds);

    std::vector<GeomRange> geom;
    if (mode == "geom")
    {
        u64 entryBytes = 0;
        for (const Entry& e : entries)
        {
            if (e.name.find(g_opt.entry) == std::string::npos || e.name.find("level.geom") == std::string::npos)
                continue;
            if (!GeomRanges(e, geom))
                return printf("geom: %s is not a raw level.geom with VB/IB chunks\n", e.name.c_str()), 1;
            entryBytes += e.real;
        }
        if (geom.empty())
            return printf("geom: no level.geom entry matches '%s'\n", g_opt.entry.c_str()), 1;
        u64 bytes = 0;
        for (const GeomRange& r : geom)
            bytes += r.size;
        printf("geom: %zu buffers, %.1f MB of %.1f MB in the matching entries\n", geom.size(), bytes / 1048576.0,
            entryBytes / 1048576.0);
    }

    std::vector<Item> items;
    u64 total = 0;
    for (const Entry* e : dds)
    {
        if (total >= g_opt.limit)
            break;
        items.push_back({e});
        total += e->real;
    }

    if (g_opt.debug)
    {
        ID3D12Debug* dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }
    IDXGIFactory6* dxgi;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi)));
    IDXGIAdapter1* adapter;
    CHECK(dxgi->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)));

    Stats st;
    if (mode == "geom")
    {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, archive.c_str(), -1, w, MAX_PATH);
        st = RunGeom(geom, w, g_opt.ds);
        printf("%-5s %-60s buffers %4u  real %7.1f MB  enqueue/produce %8.1f ms  wall %8.1f ms  %6.2f GB/s\n",
            g_opt.ds ? "gds" : "gcpu", archive.substr(archive.find_last_of("\\/") + 1).c_str(), st.textures,
            st.bytesReal / 1048576.0, st.produce, st.wall, st.bytesReal / (st.wall / 1000.0) / 1e9);
        const u32 bad = g_opt.verify ? VerifyGeom(geom) : 0;
        for (GeomRange& r : geom)
            if (r.buffer)
                r.buffer->Release();
        if (g_factory)
            g_factory->Release();
        g_queue->Release();
        g_dev->Release();
        adapter->Release();
        dxgi->Release();
        UnmapViewOfFile(g_map);
        CloseHandle(hm);
        CloseHandle(hf);
        return st.failures || bad ? 1 : 0;
    }
    if (mode == "cpu")
        st = RunCpu(items, g_opt.threads);
    else if (mode == "gdeflate")
        st = RunGDeflate(items);
    else if (mode == "ds" || mode == "dsbuf" || mode == "dsmem" || mode == "dsmembuf")
    {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, archive.c_str(), -1, w, MAX_PATH);
        st = RunDs(items, w, g_opt.threads, mode == "dsbuf" || mode == "dsmembuf", mode == "dsmem" || mode == "dsmembuf");
    }
    else
        return Usage();
    printf("%-5s %-60s textures %4u  real %7.1f MB  disk %7.1f MB  enqueue/produce %8.1f ms  wall %8.1f ms  %6.2f GB/s real  peek-fallbacks %u%s%s\n",
        mode.c_str(), archive.substr(archive.find_last_of("\\/") + 1).c_str(), st.textures, st.bytesReal / 1048576.0,
        st.bytesDisk / 1048576.0, st.produce, st.wall, st.bytesReal / (st.wall / 1000.0) / 1e9, st.fallbacks,
        g_opt.placed ? "  placed" : "", g_opt.direct ? "  direct" : "");
    u32 mismatches = 0;
    if (g_opt.verify && mode != "dsmem" && mode != "dsmembuf")
        mismatches = Verify(items, g_opt.verify);
    ReleaseItems(items);
    if (g_factory)
        g_factory->Release();
    g_queue->Release();
    g_dev->Release();
    adapter->Release();
    dxgi->Release();
    UnmapViewOfFile(g_map);
    CloseHandle(hm);
    CloseHandle(hf);
    return st.failures || mismatches ? 1 : 0;
}
