// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Placed texture heaps of xrD3D12 (docs/dead-air/dx12-features/dstorage.md 3.6; lane `ds`): SRV-only
// textures suballocated from large heaps instead of one committed resource each. Reached through
// hooks in InitTexture, ReleaseStorage and Device::Shutdown (d12_layer.h, [dx12:ds]).
#include "d12_layer.h"

#include <algorithm>
#include <memory>

namespace xrd3d12
{
namespace
{
constexpr u64 kHeapSize = 256ull << 20;
// Bigger textures stay committed: a few per level, and they would fragment the heaps.
constexpr u64 kMaxPlacedSize = 64ull << 20;

// Guards every Device::texheap (creation and use); allocations are short.
std::mutex g_texheapLock;
thread_local bool t_streamedTextures = false;

u64 AlignUp(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// Bits per texel (per 4x4 block for block-compressed formats); 0 = not handled.
u32 BitsPerElement(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 64;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 128;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
        return 64;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        return 32;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 16;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM:
        return 8;
    default:
        return 0;
    }
}

// 4 KB placement is allowed when the most detailed mip spans at most 16 standard tiles (64 KB); asking
// the runtime with an ineligible description is a debug-layer error, so it is checked first.
bool SmallPlacement(const D3D12_RESOURCE_DESC& desc)
{
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1)
        return false;
    u32 bits = BitsPerElement(desc.Format);
    if (!bits)
        return false;
    u64 width = desc.Width;
    u32 height = desc.Height;
    if (IsBlockCompressed(desc.Format))
    {
        width = (width + 3) / 4;
        height = (height + 3) / 4;
    }
    u32 tileX = 0, tileY = 0;
    switch (bits)
    {
    case 8: tileX = 64; tileY = 64; break;
    case 16: tileX = 64; tileY = 32; break;
    case 32: tileX = 32; tileY = 32; break;
    case 64: tileX = 32; tileY = 16; break;
    case 128: tileX = 16; tileY = 16; break;
    default: return false;
    }
    return ((width + tileX - 1) / tileX) * ((height + tileY - 1) / tileY) <= 16;
}
} // namespace

// One heap and its free ranges (offset, size), sorted by offset and coalesced.
struct TextureHeap
{
    ID3D12Heap* heap = nullptr;
    std::vector<std::pair<u64, u64>> free;
    u64 used = 0;
};

struct TexturePlacement
{
    TextureHeap* heap = nullptr;
    u64 offset = 0;
    u64 size = 0;
};

class TexturePageAllocator
{
public:
    std::vector<std::unique_ptr<TextureHeap>> heaps;
    u32 live = 0;
    u64 liveBytes = 0;
    u64 placedTotal = 0;
    u64 committedTotal = 0;

    // First fit over every heap; a new heap when none has room. Caller holds g_texheapLock.
    TextureHeap* Allocate(ID3D12Device* device, u64 size, u64 alignment, u64& offset)
    {
        for (const std::unique_ptr<TextureHeap>& heap : heaps)
        {
            if (Take(*heap, size, alignment, offset))
                return heap.get();
        }
        auto heap = std::make_unique<TextureHeap>();
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = kHeapSize;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        const HRESULT hr = device->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap->heap));
        if (FAILED(hr))
        {
            static constexpr char kFormat[] = "texture heap: CreateHeap failed, hr 0x%08X; textures stay committed";
            LogOnce(kFormat, kFormat, unsigned(hr));
            return nullptr;
        }
        heap->free.emplace_back(0, kHeapSize);
        TextureHeap* result = heap.get();
        heaps.push_back(std::move(heap));
        return Take(*result, size, alignment, offset) ? result : nullptr;
    }

    // Caller holds g_texheapLock. A heap that became empty goes when another empty one is kept.
    void Free(TextureHeap* heap, u64 offset, u64 size)
    {
        auto& free = heap->free;
        auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(offset, u64(0)));
        it = free.insert(it, {offset, size});
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
        heap->used -= size;
        if (heap->used)
            return;
        const bool anotherEmpty = std::any_of(heaps.begin(), heaps.end(),
            [heap](const std::unique_ptr<TextureHeap>& h) { return h.get() != heap && !h->used; });
        if (!anotherEmpty)
            return;
        heap->heap->Release();
        heaps.erase(std::find_if(heaps.begin(), heaps.end(),
            [heap](const std::unique_ptr<TextureHeap>& h) { return h.get() == heap; }));
    }

private:
    static bool Take(TextureHeap& heap, u64 size, u64 alignment, u64& offset)
    {
        auto& free = heap.free;
        for (size_t i = 0; i < free.size(); ++i)
        {
            const auto [start, length] = free[i];
            const u64 aligned = AlignUp(start, alignment);
            if (aligned + size > start + length)
                continue;
            const u64 tail = start + length - (aligned + size);
            if (aligned > start)
            {
                free[i].second = aligned - start;
                if (tail)
                    free.insert(free.begin() + ptrdiff_t(i) + 1, {aligned + size, tail});
            }
            else if (tail)
                free[i] = {aligned + size, tail};
            else
                free.erase(free.begin() + ptrdiff_t(i));
            heap.used += size;
            offset = aligned;
            return true;
        }
        return false;
    }
};

bool Device::TexturePlace(Resource& r, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clear, ID3D12Resource** out)
{
    // SRV-only, single-sampled textures without shared or tiled storage; without initial data only for
    // streamed textures (placed memory keeps what an earlier texture left there).
    if (clear || desc.Flags != D3D12_RESOURCE_FLAG_NONE || desc.SampleDesc.Count > 1 ||
        r.bindFlags != D3D11_BIND_SHADER_RESOURCE || (r.usage != D3D11_USAGE_IMMUTABLE && r.usage != D3D11_USAGE_DEFAULT) ||
        (r.miscFlags & ~UINT(D3D11_RESOURCE_MISC_TEXTURECUBE)) ||
        (initial != D3D12_RESOURCE_STATE_COPY_DEST && !t_streamedTextures))
    {
        return false;
    }

    D3D12_RESOURCE_DESC placed = desc;
    placed.Alignment = SmallPlacement(desc) ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : 0;
    D3D12_RESOURCE_ALLOCATION_INFO info = d12->GetResourceAllocationInfo(0, 1, &placed);
    if (placed.Alignment && info.Alignment != placed.Alignment)
    {
        placed.Alignment = 0;
        info = d12->GetResourceAllocationInfo(0, 1, &placed);
    }

    TexturePlacement* placement = nullptr;
    {
        std::lock_guard lock(g_texheapLock);
        if (!texheap)
            texheap = new (std::nothrow) TexturePageAllocator();
        if (!texheap)
            return false;
        if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes || info.SizeInBytes > kMaxPlacedSize)
        {
            ++texheap->committedTotal;
            return false;
        }
        u64 offset = 0;
        TextureHeap* heap = texheap->Allocate(d12, info.SizeInBytes, info.Alignment, offset);
        if (!heap)
        {
            ++texheap->committedTotal;
            return false;
        }
        placement = new (std::nothrow) TexturePlacement{heap, offset, info.SizeInBytes};
        if (!placement)
        {
            texheap->Free(heap, offset, info.SizeInBytes);
            return false;
        }
        ++texheap->live;
        texheap->liveBytes += info.SizeInBytes;
        ++texheap->placedTotal;
    }

    const HRESULT hr = d12->CreatePlacedResource(placement->heap->heap, placement->offset, &placed, initial, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(out));
    if (FAILED(hr))
    {
        *out = nullptr;
        static constexpr char kFormat[] = "texture heap: CreatePlacedResource failed, hr 0x%08X (%llux%u, format %u); committed instead";
        LogOnce(kFormat, kFormat, unsigned(hr), static_cast<unsigned long long>(desc.Width), desc.Height, unsigned(desc.Format));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            ReportRemoval(hr);
        std::lock_guard lock(g_texheapLock);
        texheap->Free(placement->heap, placement->offset, placement->size);
        --texheap->live;
        texheap->liveBytes -= placement->size;
        --texheap->placedTotal;
        ++texheap->committedTotal;
        delete placement;
        return false;
    }
    r.placement = placement;
    return true;
}

void Device::TextureRelease(Resource& r)
{
    // The resource and its range go together once the GPU is done with them.
    TexturePlacement* placement = r.placement;
    ID3D12Resource* native = r.native;
    r.placement = nullptr;
    r.native = nullptr;
    releaser.Defer([this, placement, native]
    {
        if (native)
            native->Release();
        std::lock_guard lock(g_texheapLock);
        if (texheap)
        {
            texheap->Free(placement->heap, placement->offset, placement->size);
            --texheap->live;
            texheap->liveBytes -= placement->size;
        }
        delete placement;
    });
}

void Device::TexheapShutdown()
{
    std::lock_guard lock(g_texheapLock);
    if (!texheap)
        return;
    if (texheap->live)
        Log("texture heap: %u placed textures (%llu MB) still alive at shutdown", texheap->live,
            static_cast<unsigned long long>(texheap->liveBytes >> 20));
    for (const std::unique_ptr<TextureHeap>& heap : texheap->heaps)
        heap->heap->Release();
    delete texheap;
    texheap = nullptr;
}

bool SetStreamedTextures(bool on)
{
    const bool previous = t_streamedTextures;
    t_streamedTextures = on;
    return previous;
}

void GetTextureHeapStats(ID3D11Device* device, TextureHeapStats& stats)
{
    stats = {};
    Device* d = device ? Device::From(device) : nullptr;
    if (!d)
        return;
    std::lock_guard lock(g_texheapLock);
    if (!d->texheap)
        return;
    stats.heaps = UINT(d->texheap->heaps.size());
    stats.heapBytes = u64(stats.heaps) * kHeapSize;
    stats.placed = d->texheap->live;
    stats.placedBytes = d->texheap->liveBytes;
    stats.placedTotal = d->texheap->placedTotal;
    stats.committedTotal = d->texheap->committedTotal;
}
} // namespace xrd3d12
