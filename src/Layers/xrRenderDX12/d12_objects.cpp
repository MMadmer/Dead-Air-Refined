// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The COM objects of xrD3D12: private data, resources, views, shader reflection, queries and
// command lists (see d12_layer.h for the contract).
#include "d12_layer.h"

#include <d3d12shader.h>

#include <cstring>
#include <new>
#include <utility>

namespace xrd3d12
{
namespace
{
template <class T>
constexpr T Max(T a, T b)
{
    return a < b ? b : a;
}

constexpr u64 AlignUp(u64 value, u64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// ---- lifetime helpers ---------------------------------------------------------------------------

// Hands a reference to the releaser. On allocation failure the object leaks: releasing it now could
// free memory the GPU still reads.
void DeferRelease(Device* device, IUnknown* object)
{
    if (!object)
        return;
    try
    {
        device->releaser.Release(object);
    }
    catch (const std::bad_alloc&)
    {
        LogOnce("DeferRelease", "out of memory in the deferred releaser, a GPU object leaks");
    }
}

template <class F>
void DeferAction(Device* device, F&& action)
{
    try
    {
        device->releaser.Defer(std::function<void()>(std::forward<F>(action)));
    }
    catch (const std::bad_alloc&)
    {
        LogOnce("DeferAction", "out of memory in the deferred releaser, a GPU allocation leaks");
    }
}

void ReportFailure(Device* device, HRESULT hr, const char* what)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        device->ReportRemoval(hr);
    Log("%s failed, hr 0x%08X", what, unsigned(hr));
}

Device* DeviceOf(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    switch (dim)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<Buffer*>(static_cast<ID3D11Buffer*>(resource))->Owner();
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<Texture1D*>(static_cast<ID3D11Texture1D*>(resource))->Owner();
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<Texture2D*>(static_cast<ID3D11Texture2D*>(resource))->Owner();
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return static_cast<Texture3D*>(static_cast<ID3D11Texture3D*>(resource))->Owner();
    default: return nullptr;
    }
}

void SetNativeName(ID3D12Object* object, const char* name)
{
    if (!object || !name)
        return;
    // Names longer than the buffer are cut; a split UTF-8 sequence becomes a replacement char.
    char narrow[256];
    const size_t length = strnlen(name, sizeof(narrow) - 1);
    memcpy(narrow, name, length);
    narrow[length] = 0;
    wchar_t wide[256];
    if (MultiByteToWideChar(CP_UTF8, 0, narrow, -1, wide, 256) > 0)
        object->SetName(wide);
}

// ---- formats -------------------------------------------------------------------------------------

// Fully typeless formats (a view cannot use them; a null view desc of such a resource is invalid).
bool IsTypeless(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool IsDsvFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D16_UNORM ||
        format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// D3D11 casting rules: a view keeps the resource's format family, and only DSVs use depth formats.
// D3D12 would create an undefined descriptor where D3D11 fails the call.
bool ViewFormatFits(const Resource& r, DXGI_FORMAT view, bool depthView)
{
    if (IsTypeless(view) || IsDsvFormat(view) != depthView)
        return false;
    return TypelessOf(view) == TypelessOf(r.format);
}

// D3D12 SRVs of planar depth-stencil formats select the plane explicitly.
UINT SrvPlaneSlice(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT ? 1 : 0;
}

// A typed member of the resource's family to state its optimized clear value in; UNKNOWN when there
// is no obvious one.
DXGI_FORMAT ClearFormat(DXGI_FORMAT format, bool depth)
{
    if (depth)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
        default: return IsDsvFormat(format) ? format : DXGI_FORMAT_UNKNOWN;
        }
    }
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return IsTypeless(format) ? DXGI_FORMAT_UNKNOWN : format;
    }
}

// Nearly every clear of the engine is to black or to depth 1 / stencil 0 (the few to other values
// just take the slow path). A clear to the value named at creation is a metadata-only clear where
// the hardware wants it known up front; a D3D11 driver learns it on its own.
bool OptimizedClear(const D3D12_RESOURCE_DESC& desc, D3D12_CLEAR_VALUE& clear)
{
    const bool depth = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    if (!depth && !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return false;
    clear = {};
    clear.Format = ClearFormat(desc.Format, depth);
    if (depth)
        clear.DepthStencil = {1.0f, 0};
    return clear.Format != DXGI_FORMAT_UNKNOWN;
}

// ---- descriptions --------------------------------------------------------------------------------

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    return heap;
}

D3D12_RESOURCE_DESC BufferDesc(u64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_FLAGS FlagsFromBind(UINT bind)
{
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (bind & D3D11_BIND_RENDER_TARGET)
        flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (bind & D3D11_BIND_DEPTH_STENCIL)
    {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!(bind & D3D11_BIND_SHADER_RESOURCE))
            flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return flags;
}

D3D12_RESOURCE_DESC TextureDesc(D3D12_RESOURCE_DIMENSION dimension, u64 width, u32 height, u32 depthOrArraySize, u32 mips,
    DXGI_FORMAT format, DXGI_SAMPLE_DESC sample, UINT bind)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = dimension;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = UINT16(depthOrArraySize);
    desc.MipLevels = UINT16(mips);
    desc.Format = format;
    desc.SampleDesc = sample;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = FlagsFromBind(bind);
    return desc;
}

u32 FullMipCount(u64 width, u32 height, u32 depth)
{
    u64 size = Max(width, Max<u64>(height, depth));
    u32 count = 1;
    while (size > 1)
    {
        size >>= 1;
        ++count;
    }
    return count;
}

u32 SampleCount(const Resource& r)
{
    return r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? Max<u32>(1, r.nativeDesc.SampleDesc.Count) : 1;
}

u32 DepthAtMip(const Resource& r, u32 mip)
{
    return r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? Max<u32>(1, u32(r.nativeDesc.DepthOrArraySize) >> mip) : 1;
}

void SetResourceCommon(Resource& r, D3D11_RESOURCE_DIMENSION dimension, D3D11_USAGE usage, UINT bind, UINT cpuAccess, UINT misc)
{
    r.dimension = dimension;
    r.usage = usage;
    r.bindFlags = bind;
    r.cpuAccess = cpuAccess;
    r.miscFlags = misc;
}

// ---- resource storage -----------------------------------------------------------------------------

HRESULT CreateCommitted(Device* device, D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES state, ID3D12Resource** out, const char* what, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(heapType);
    const HRESULT hr = device->d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(out));
    if (FAILED(hr))
    {
        *out = nullptr;
        Log("%s: CreateCommittedResource failed, hr 0x%08X (%llux%ux%u, mips %u, format %u, flags 0x%X)", what,
            unsigned(hr), static_cast<unsigned long long>(desc.Width), desc.Height, unsigned(desc.DepthOrArraySize),
            unsigned(desc.MipLevels), unsigned(desc.Format), unsigned(desc.Flags));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            device->ReportRemoval(hr);
    }
    return hr;
}

// A persistently mapped linear buffer: READBACK when the CPU reads it, UPLOAD when it writes it.
HRESULT CreateLinear(Device* device, Resource& r, u64 size, const char* what)
{
    const bool read = (r.cpuAccess & D3D11_CPU_ACCESS_READ) != 0;
    if (read && (r.cpuAccess & D3D11_CPU_ACCESS_WRITE))
        LogOnce("CreateLinear.rw", "staging resource with CPU read+write access lives in a READBACK heap (GPU cannot read it)");
    const D3D12_RESOURCE_DESC desc = BufferDesc(size, D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = CreateCommitted(device, read ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD, desc,
        read ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, &r.linear, what);
    if (FAILED(hr))
        return hr;
    void* cpu = nullptr;
    const D3D12_RANGE nothing{0, 0};
    hr = r.linear->Map(0, read ? nullptr : &nothing, &cpu);
    if (FAILED(hr) || !cpu)
    {
        ReportFailure(device, hr, "Map of a staging buffer");
        return FAILED(hr) ? hr : E_FAIL;
    }
    r.linearCpu = static_cast<u8*>(cpu);
    return S_OK;
}

// Placed footprints of every subresource of the texture (staging memory layout, dynamic uploads).
void FillFootprints(Device* device, Resource& r, D3D12_RESOURCE_DESC desc, u64& total)
{
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    const u32 count = r.SubresourceCount();
    r.footprints.resize(count);
    r.rowCounts.resize(count);
    r.rowSizes.resize(count);
    total = 0;
    device->d12->GetCopyableFootprints(&desc, 0, count, 0, r.footprints.data(), r.rowCounts.data(), r.rowSizes.data(), &total);
}

// D3D11 initial data (tightly or loosely pitched) into the placed footprints of `linearCpu`.
void CopyToFootprints(Resource& r, const D3D11_SUBRESOURCE_DATA* data, u32 count)
{
    for (u32 s = 0; s < count; ++s)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = r.footprints[s];
        const u8* src = static_cast<const u8*>(data[s].pSysMem);
        u8* dst = r.linearCpu + fp.Offset;
        const u32 rows = r.rowCounts[s];
        const size_t rowSize = size_t(r.rowSizes[s]);
        for (u32 z = 0; z < fp.Footprint.Depth; ++z)
        {
            for (u32 y = 0; y < rows; ++y)
            {
                memcpy(dst + (u64(z) * rows + y) * fp.Footprint.RowPitch,
                    src + u64(z) * data[s].SysMemSlicePitch + u64(y) * data[s].SysMemPitch, rowSize);
            }
        }
    }
}

// The storage of a texture; `r` has its D3D11 fields set. Never throws.
HRESULT InitTexture(Device* device, Resource& r, const D3D12_RESOURCE_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, const char* what)
{
    try
    {
        r.format = desc.Format;
        r.mipLevels = desc.MipLevels;
        r.arraySize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
        r.planeCount = Max<u32>(1, PlaneCount(device->d12, desc.Format));
        r.nativeDesc = desc;
        // D3D11 initial data covers mips x slices of plane 0.
        const u32 dataCount = r.mipLevels * r.arraySize;
        if (data)
        {
            for (u32 i = 0; i < dataCount; ++i)
            {
                if (!data[i].pSysMem)
                    return E_INVALIDARG;
            }
        }

        if (r.usage == D3D11_USAGE_STAGING)
        {
            u64 total = 0;
            FillFootprints(device, r, desc, total);
            if (!total || total == ~0ull)
                return E_INVALIDARG;
            const HRESULT hr = CreateLinear(device, r, total, what);
            if (FAILED(hr))
                return hr;
            if (data)
                CopyToFootprints(r, data, dataCount);
            return S_OK;
        }

        // Dynamic textures get their footprints too: the context uploads them on every Map.
        if (r.usage == D3D11_USAGE_DYNAMIC)
        {
            u64 total = 0;
            FillFootprints(device, r, desc, total);
        }

        // Depth-stencil data stays on the direct queue: copy engines need not write depth layouts.
        InitUploader* uploader = data ? &device->Uploader((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) : nullptr;
        const bool copyQueue = uploader && uploader->CopyQueue();
        D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
        // The copy queue accepts only resources in COMMON; they promote to COPY_DEST there.
        if (data)
            initial = copyQueue ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_COPY_DEST;
        else if (r.bindFlags & D3D11_BIND_RENDER_TARGET)
            initial = D3D12_RESOURCE_STATE_RENDER_TARGET;
        else if (r.bindFlags & D3D11_BIND_DEPTH_STENCIL)
            initial = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        D3D12_CLEAR_VALUE clear;
        const D3D12_CLEAR_VALUE* optimized = OptimizedClear(desc, clear) ? &clear : nullptr;
        // Placement keys on COPY_DEST for "has initial data"; a copy-queue upload writes every
        // subresource before the first use, as a streamed load does.
        const bool streamed = copyQueue && SetStreamedTextures(true);
        const bool placed = device->TexturePlace(r, desc, initial, optimized, &r.native); // [dx12:ds] hook
        if (copyQueue)
            SetStreamedTextures(streamed);
        if (!placed)
        {
            const HRESULT hr = CreateCommitted(device, D3D12_HEAP_TYPE_DEFAULT, desc, initial, &r.native, what, optimized);
            if (FAILED(hr))
                return hr;
        }
        device->TextureWritten();
        r.states.uniform = true;
        r.states.per.clear();
        if (uploader)
        {
            // A depth buffer without SRV bind denies shader access, so it cannot rest in a read state.
            // Copy-queue textures decay to COMMON after the copy.
            const D3D12_RESOURCE_STATES rest = copyQueue || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ?
                D3D12_RESOURCE_STATE_COMMON :
                kStateShaderRead;
            uploader->UploadTexture(r.native, desc, 0, dataCount, data, rest);
            r.states.all = rest;
        }
        else
            r.states.all = initial;
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

void ReleaseStorage(Device* device, Resource& r)
{
    if (r.native && !r.IsBuffer())
        device->TextureWritten();
    if (r.placement)
        device->TextureRelease(r); // [dx12:ds] hook
    DeferRelease(device, r.native);
    r.native = nullptr;
    DeferRelease(device, r.linear);
    r.linear = nullptr;
    r.linearCpu = nullptr;
}

// ---- view descriptions -----------------------------------------------------------------------------

bool MipRange(const Resource& r, UINT first, UINT count, SubRange& range)
{
    if (first >= r.mipLevels)
        return false;
    if (count == UINT(-1))
        count = r.mipLevels - first;
    if (!count || count > r.mipLevels - first)
        return false;
    range.firstMip = first;
    range.mipCount = count;
    return true;
}

bool MipSlice(const Resource& r, UINT mip, SubRange& range)
{
    return MipRange(r, mip, 1, range);
}

bool SliceRange(const Resource& r, UINT first, UINT count, SubRange& range)
{
    if (first >= r.arraySize)
        return false;
    if (count == UINT(-1))
        count = r.arraySize - first;
    if (!count || count > r.arraySize - first)
        return false;
    range.firstSlice = first;
    range.sliceCount = count;
    return true;
}

// W range of a 3D view at `mip` (-1 = the rest).
bool WRange(const Resource& r, u32 mip, UINT first, UINT& size)
{
    const u32 depth = DepthAtMip(r, mip);
    if (first >= depth)
        return false;
    if (size == UINT(-1))
        size = depth - first;
    return size && size <= depth - first;
}

D3D11_SHADER_RESOURCE_VIEW_DESC DefaultSrv(const Resource& r)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = r.format;
    switch (r.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray.MipLevels = r.mipLevels;
            d.Texture1DArray.ArraySize = r.arraySize;
        }
        else
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
            d.Texture1D.MipLevels = r.mipLevels;
        }
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        if ((r.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && r.arraySize >= 6 && SampleCount(r) == 1)
        {
            if (r.arraySize > 6)
            {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
                d.TextureCubeArray.MipLevels = r.mipLevels;
                d.TextureCubeArray.NumCubes = r.arraySize / 6;
            }
            else
            {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                d.TextureCube.MipLevels = r.mipLevels;
            }
        }
        else if (SampleCount(r) > 1)
        {
            if (r.arraySize > 1)
            {
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray.ArraySize = r.arraySize;
            }
            else
                d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MipLevels = r.mipLevels;
            d.Texture2DArray.ArraySize = r.arraySize;
        }
        else
        {
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = r.mipLevels;
        }
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        d.Texture3D.MipLevels = r.mipLevels;
        break;
    default:
        break;
    }
    return d;
}

HRESULT ConvertSrv(const Resource& r, const D3D11_SHADER_RESOURCE_VIEW_DESC& d, D3D12_SHADER_RESOURCE_VIEW_DESC& nd, SubRange& range)
{
    const bool is1D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) == 1;
    const bool is2DMS = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) > 1;
    const bool is3D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    nd = {};
    nd.Format = d.Format;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    range = SubRange{};
    // The layer transitions depth-stencil resources as a whole.
    range.planeCount = r.planeCount;
    const UINT plane = SrvPlaneSlice(d.Format);
    switch (d.ViewDimension)
    {
    case D3D11_SRV_DIMENSION_TEXTURE1D:
        if (!is1D || !MipRange(r, d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        nd.Texture1D.MostDetailedMip = range.firstMip;
        nd.Texture1D.MipLevels = range.mipCount;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        if (!is1D || !MipRange(r, d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels, range) ||
            !SliceRange(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        nd.Texture1DArray.MostDetailedMip = range.firstMip;
        nd.Texture1DArray.MipLevels = range.mipCount;
        nd.Texture1DArray.FirstArraySlice = range.firstSlice;
        nd.Texture1DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        if (!is2D || !MipRange(r, d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Texture2D.MostDetailedMip = range.firstMip;
        nd.Texture2D.MipLevels = range.mipCount;
        nd.Texture2D.PlaneSlice = plane;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        if (!is2D || !MipRange(r, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels, range) ||
            !SliceRange(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        nd.Texture2DArray.MostDetailedMip = range.firstMip;
        nd.Texture2DArray.MipLevels = range.mipCount;
        nd.Texture2DArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DArray.ArraySize = range.sliceCount;
        nd.Texture2DArray.PlaneSlice = plane;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        if (!is2DMS)
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        if (!is2DMS || !SliceRange(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        nd.Texture2DMSArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DMSArray.ArraySize = range.sliceCount;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
        if (!is3D || !MipRange(r, d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        nd.Texture3D.MostDetailedMip = range.firstMip;
        nd.Texture3D.MipLevels = range.mipCount;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        if (!is2D || r.arraySize < 6 || !MipRange(r, d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        nd.TextureCube.MostDetailedMip = range.firstMip;
        nd.TextureCube.MipLevels = range.mipCount;
        range.firstSlice = 0;
        range.sliceCount = 6;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
    {
        const UINT first = d.TextureCubeArray.First2DArrayFace;
        UINT cubes = d.TextureCubeArray.NumCubes;
        if (cubes == UINT(-1) && first < r.arraySize)
            cubes = (r.arraySize - first) / 6;
        if (!is2D || !cubes || cubes > r.arraySize / 6 ||
            !MipRange(r, d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels, range) ||
            !SliceRange(r, first, cubes * 6, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        nd.TextureCubeArray.MostDetailedMip = range.firstMip;
        nd.TextureCubeArray.MipLevels = range.mipCount;
        nd.TextureCubeArray.First2DArrayFace = first;
        nd.TextureCubeArray.NumCubes = cubes;
        break;
    }
    default:
        return E_INVALIDARG;
    }
    return S_OK;
}

D3D11_RENDER_TARGET_VIEW_DESC DefaultRtv(const Resource& r)
{
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = r.format;
    switch (r.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE1D;
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        if (SampleCount(r) > 1)
        {
            if (r.arraySize > 1)
            {
                d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray.ArraySize = r.arraySize;
            }
            else
                d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        }
        else if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D.WSize = DepthAtMip(r, 0);
        break;
    default:
        break;
    }
    return d;
}

HRESULT ConvertRtv(const Resource& r, const D3D11_RENDER_TARGET_VIEW_DESC& d, D3D12_RENDER_TARGET_VIEW_DESC& nd, SubRange& range)
{
    const bool is1D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) == 1;
    const bool is2DMS = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) > 1;
    const bool is3D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    nd = {};
    nd.Format = d.Format;
    range = SubRange{};
    switch (d.ViewDimension)
    {
    case D3D11_RTV_DIMENSION_TEXTURE1D:
        if (!is1D || !MipSlice(r, d.Texture1D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
        nd.Texture1D.MipSlice = range.firstMip;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        if (!is1D || !MipSlice(r, d.Texture1DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
        nd.Texture1DArray.MipSlice = range.firstMip;
        nd.Texture1DArray.FirstArraySlice = range.firstSlice;
        nd.Texture1DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2D:
        if (!is2D || !MipSlice(r, d.Texture2D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        nd.Texture2D.MipSlice = range.firstMip;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        if (!is2D || !MipSlice(r, d.Texture2DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        nd.Texture2DArray.MipSlice = range.firstMip;
        nd.Texture2DArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        if (!is2DMS)
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        if (!is2DMS || !SliceRange(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        nd.Texture2DMSArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DMSArray.ArraySize = range.sliceCount;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE3D:
    {
        UINT size = d.Texture3D.WSize;
        if (!is3D || !MipSlice(r, d.Texture3D.MipSlice, range) || !WRange(r, range.firstMip, d.Texture3D.FirstWSlice, size))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        nd.Texture3D.MipSlice = range.firstMip;
        nd.Texture3D.FirstWSlice = d.Texture3D.FirstWSlice;
        nd.Texture3D.WSize = size;
        break;
    }
    default:
        return E_INVALIDARG;
    }
    return S_OK;
}

D3D11_DEPTH_STENCIL_VIEW_DESC DefaultDsv(const Resource& r)
{
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = r.format;
    switch (r.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE1D;
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        if (SampleCount(r) > 1)
        {
            if (r.arraySize > 1)
            {
                d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                d.Texture2DMSArray.ArraySize = r.arraySize;
            }
            else
                d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
        else if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        break;
    default:
        // 3D resources have no depth-stencil views.
        d.ViewDimension = D3D11_DSV_DIMENSION_UNKNOWN;
        break;
    }
    return d;
}

HRESULT ConvertDsv(const Resource& r, const D3D11_DEPTH_STENCIL_VIEW_DESC& d, D3D12_DEPTH_STENCIL_VIEW_DESC& nd, SubRange& range)
{
    const bool is1D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) == 1;
    const bool is2DMS = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) > 1;
    if (!ViewFormatFits(r, d.Format, true) || (d.Flags & ~UINT(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL)))
        return E_INVALIDARG;
    nd = {};
    nd.Format = d.Format;
    nd.Flags = D3D12_DSV_FLAGS(d.Flags);
    range = SubRange{};
    range.planeCount = r.planeCount;
    switch (d.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE1D:
        if (!is1D || !MipSlice(r, d.Texture1D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
        nd.Texture1D.MipSlice = range.firstMip;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        if (!is1D || !MipSlice(r, d.Texture1DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
        nd.Texture1DArray.MipSlice = range.firstMip;
        nd.Texture1DArray.FirstArraySlice = range.firstSlice;
        nd.Texture1DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2D:
        if (!is2D || !MipSlice(r, d.Texture2D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        nd.Texture2D.MipSlice = range.firstMip;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        if (!is2D || !MipSlice(r, d.Texture2DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        nd.Texture2DArray.MipSlice = range.firstMip;
        nd.Texture2DArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        if (!is2DMS)
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        if (!is2DMS || !SliceRange(r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        nd.Texture2DMSArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DMSArray.ArraySize = range.sliceCount;
        break;
    default:
        return E_INVALIDARG;
    }
    return S_OK;
}

D3D11_UNORDERED_ACCESS_VIEW_DESC DefaultUav(const Resource& r)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = r.format;
    switch (r.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1DARRAY;
            d.Texture1DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE1D;
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        if (r.arraySize > 1)
        {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.ArraySize = r.arraySize;
        }
        else
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D.WSize = DepthAtMip(r, 0);
        break;
    default:
        break;
    }
    return d;
}

HRESULT ConvertUav(const Resource& r, const D3D11_UNORDERED_ACCESS_VIEW_DESC& d, D3D12_UNORDERED_ACCESS_VIEW_DESC& nd, SubRange& range)
{
    const bool is1D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && SampleCount(r) == 1;
    const bool is3D = r.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    nd = {};
    nd.Format = d.Format;
    range = SubRange{};
    switch (d.ViewDimension)
    {
    case D3D11_UAV_DIMENSION_TEXTURE1D:
        if (!is1D || !MipSlice(r, d.Texture1D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        nd.Texture1D.MipSlice = range.firstMip;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        if (!is1D || !MipSlice(r, d.Texture1DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        nd.Texture1DArray.MipSlice = range.firstMip;
        nd.Texture1DArray.FirstArraySlice = range.firstSlice;
        nd.Texture1DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
        if (!is2D || !MipSlice(r, d.Texture2D.MipSlice, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        nd.Texture2D.MipSlice = range.firstMip;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        if (!is2D || !MipSlice(r, d.Texture2DArray.MipSlice, range) ||
            !SliceRange(r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        nd.Texture2DArray.MipSlice = range.firstMip;
        nd.Texture2DArray.FirstArraySlice = range.firstSlice;
        nd.Texture2DArray.ArraySize = range.sliceCount;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
    {
        UINT size = d.Texture3D.WSize;
        if (!is3D || !MipSlice(r, d.Texture3D.MipSlice, range) || !WRange(r, range.firstMip, d.Texture3D.FirstWSlice, size))
            return E_INVALIDARG;
        nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        nd.Texture3D.MipSlice = range.firstMip;
        nd.Texture3D.FirstWSlice = d.Texture3D.FirstWSlice;
        nd.Texture3D.WSize = size;
        break;
    }
    default:
        return E_INVALIDARG;
    }
    return S_OK;
}

// ---- view plumbing --------------------------------------------------------------------------------

// The resource of a new view and the checks every view kind shares.
HRESULT ViewResource(ID3D11Resource* iface, UINT bind, const char* kind, Resource*& out)
{
    out = nullptr;
    if (!iface)
        return E_INVALIDARG;
    Resource* r = Resource::From(iface);
    if (!r)
        return E_INVALIDARG;
    if (r->usage == D3D11_USAGE_STAGING || !(r->bindFlags & bind))
        return E_INVALIDARG;
    if (r->IsBuffer() && r->usage == D3D11_USAGE_DYNAMIC)
    {
        // Every DISCARD renames a dynamic buffer: a descriptor would keep reading the old version.
        LogOnce(kind, "! %s of a dynamic buffer refused (a DISCARD renames it; use a DEFAULT buffer)", kind);
        return E_INVALIDARG;
    }
    out = r;
    return S_OK;
}

// Bytes of one element of a typed buffer view; 0 for formats buffers cannot hold.
u32 BufferElementSize(DXGI_FORMAT format)
{
    switch (TypelessOf(format))
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS: return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS: return 4;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS: return 2;
    case DXGI_FORMAT_R8_TYPELESS: return 1;
    default: break;
    }
    return format == DXGI_FORMAT_R11G11B10_FLOAT ? 4 : 0;
}

// Element size and D3D12 format of a buffer view: structured (the buffer's stride, format UNKNOWN),
// raw (4 bytes, R32_TYPELESS) or typed. False when D3D11 would refuse the combination.
bool BufferViewLayout(const Buffer& b, DXGI_FORMAT format, bool raw, u32& elementSize, DXGI_FORMAT& nativeFormat)
{
    const D3D11_BUFFER_DESC& desc = b.Desc();
    const bool structured = (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
    if (raw)
    {
        if (structured || !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || format != DXGI_FORMAT_R32_TYPELESS)
            return false;
        elementSize = 4;
        nativeFormat = DXGI_FORMAT_R32_TYPELESS;
        return true;
    }
    if (structured)
    {
        if (format != DXGI_FORMAT_UNKNOWN)
            return false;
        elementSize = desc.StructureByteStride;
        nativeFormat = DXGI_FORMAT_UNKNOWN;
        return elementSize != 0;
    }
    elementSize = BufferElementSize(format);
    nativeFormat = format;
    return elementSize && !IsTypeless(format);
}

bool BufferRangeFits(const Resource& r, UINT first, UINT count, u32 elementSize)
{
    return count && (u64(first) + count) * elementSize <= r.byteWidth;
}

// D3D11 buffer SRV (BUFFER or BUFFEREX) -> D3D12. A null description is valid for structured buffers
// only (the whole buffer).
HRESULT ConvertBufferSrv(const Buffer& b, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, D3D11_SHADER_RESOURCE_VIEW_DESC& d,
    D3D12_SHADER_RESOURCE_VIEW_DESC& nd)
{
    const D3D11_BUFFER_DESC& bd = b.Desc();
    if (desc)
        d = *desc;
    else
    {
        if (!(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !bd.StructureByteStride)
            return E_INVALIDARG;
        d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx.NumElements = bd.ByteWidth / bd.StructureByteStride;
    }
    UINT first = 0, count = 0;
    bool raw = false;
    if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
    {
        first = d.Buffer.FirstElement;
        count = d.Buffer.NumElements;
    }
    else if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
    {
        if (d.BufferEx.Flags & ~UINT(D3D11_BUFFEREX_SRV_FLAG_RAW))
            return E_INVALIDARG;
        first = d.BufferEx.FirstElement;
        count = d.BufferEx.NumElements;
        raw = (d.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
    }
    else
        return E_INVALIDARG;
    u32 elementSize = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (!BufferViewLayout(b, d.Format, raw, elementSize, format) || !BufferRangeFits(b, first, count, elementSize))
        return E_INVALIDARG;
    nd = {};
    nd.Format = format;
    nd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.Buffer.FirstElement = first;
    nd.Buffer.NumElements = count;
    nd.Buffer.StructureByteStride = format == DXGI_FORMAT_UNKNOWN ? elementSize : 0;
    nd.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
    return S_OK;
}

// D3D11 buffer UAV -> D3D12. Append and counter UAVs are refused (the renderer uses neither).
HRESULT ConvertBufferUav(const Buffer& b, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, D3D11_UNORDERED_ACCESS_VIEW_DESC& d,
    D3D12_UNORDERED_ACCESS_VIEW_DESC& nd)
{
    const D3D11_BUFFER_DESC& bd = b.Desc();
    if (desc)
        d = *desc;
    else
    {
        if (!(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !bd.StructureByteStride)
            return E_INVALIDARG;
        d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = bd.ByteWidth / bd.StructureByteStride;
    }
    if (d.ViewDimension != D3D11_UAV_DIMENSION_BUFFER)
        return E_INVALIDARG;
    if (d.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))
    {
        static const char message[] = "! append/counter buffer UAVs are not supported; view refused";
        LogOnce(message, message);
        return E_NOTIMPL;
    }
    if (d.Buffer.Flags & ~UINT(D3D11_BUFFER_UAV_FLAG_RAW))
        return E_INVALIDARG;
    const bool raw = (d.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) != 0;
    u32 elementSize = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (!BufferViewLayout(b, d.Format, raw, elementSize, format) || !BufferRangeFits(b, d.Buffer.FirstElement, d.Buffer.NumElements, elementSize))
        return E_INVALIDARG;
    nd = {};
    nd.Format = format;
    nd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    nd.Buffer.FirstElement = d.Buffer.FirstElement;
    nd.Buffer.NumElements = d.Buffer.NumElements;
    nd.Buffer.StructureByteStride = format == DXGI_FORMAT_UNKNOWN ? elementSize : 0;
    nd.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
    return S_OK;
}

void AttachResource(View& view, ViewKind kind, ID3D11Resource* iface, Resource* r, Device* device)
{
    view.kind = kind;
    view.resourceIface = iface;
    iface->AddRef();
    view.resource = r;
    view.id = device->nextViewId.fetch_add(1, std::memory_order_relaxed);
}

CpuDescriptorHeap& HeapOf(Device* device, ViewKind kind)
{
    switch (kind)
    {
    case ViewKind::RTV: return device->cpuRtv;
    case ViewKind::DSV: return device->cpuDsv;
    default: return device->cpuSrv;
    }
}

// Allocates the view's descriptor(s) and fills them through `make(native, handle)`. A view of the back
// buffer proxy gets one descriptor per swap chain buffer. Never throws.
template <class Make>
HRESULT CreateDescriptors(Device* device, View& view, Make&& make)
{
    CpuDescriptorHeap& heap = HeapOf(device, view.kind);
    Resource* r = view.resource;
    if (!r->IsBackBufferProxy())
    {
        if (!r->native)
            return E_INVALIDARG;
        view.cpu = heap.Allocate();
        if (!view.cpu.ptr)
            return E_OUTOFMEMORY;
        make(r->native, view.cpu);
        return S_OK;
    }

    SwapChain* chain = r->swapChain;
    IDXGISwapChain4* native = chain->Native();
    const u32 count = chain->BufferCount();
    if (!native || !count)
        return E_FAIL;
    try
    {
        view.perBuffer.reserve(count);
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    for (u32 i = 0; i < count; ++i)
    {
        ID3D12Resource* buffer = nullptr;
        const HRESULT hr = native->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer));
        if (FAILED(hr))
        {
            ReportFailure(device, hr, "GetBuffer for a back buffer view");
            return hr;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = heap.Allocate();
        if (!handle.ptr)
        {
            buffer->Release();
            return E_OUTOFMEMORY;
        }
        view.perBuffer.push_back(handle); // reserved above
        make(buffer, handle);
        buffer->Release();
    }
    // Code that reads `cpu` directly still gets a valid (buffer 0) descriptor.
    view.cpu = view.perBuffer[0];
    return S_OK;
}

// ---- reflection ----------------------------------------------------------------------------------

D3D12_SRV_DIMENSION SrvDimensionOf(D3D_SRV_DIMENSION dim)
{
    switch (dim)
    {
    case D3D_SRV_DIMENSION_BUFFER:
    case D3D_SRV_DIMENSION_BUFFEREX: return D3D12_SRV_DIMENSION_BUFFER;
    case D3D_SRV_DIMENSION_TEXTURE1D: return D3D12_SRV_DIMENSION_TEXTURE1D;
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    case D3D_SRV_DIMENSION_TEXTURE2D: return D3D12_SRV_DIMENSION_TEXTURE2D;
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    case D3D_SRV_DIMENSION_TEXTURE2DMS: return D3D12_SRV_DIMENSION_TEXTURE2DMS;
    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    case D3D_SRV_DIMENSION_TEXTURE3D: return D3D12_SRV_DIMENSION_TEXTURE3D;
    case D3D_SRV_DIMENSION_TEXTURECUBE: return D3D12_SRV_DIMENSION_TEXTURECUBE;
    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    default: return D3D12_SRV_DIMENSION_UNKNOWN;
    }
}

D3D12_UAV_DIMENSION UavDimensionOf(D3D_SRV_DIMENSION dim)
{
    switch (dim)
    {
    case D3D_SRV_DIMENSION_BUFFER:
    case D3D_SRV_DIMENSION_BUFFEREX: return D3D12_UAV_DIMENSION_BUFFER;
    case D3D_SRV_DIMENSION_TEXTURE1D: return D3D12_UAV_DIMENSION_TEXTURE1D;
    case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
    case D3D_SRV_DIMENSION_TEXTURE2D: return D3D12_UAV_DIMENSION_TEXTURE2D;
    case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    case D3D_SRV_DIMENSION_TEXTURE3D: return D3D12_UAV_DIMENSION_TEXTURE3D;
    default: return D3D12_UAV_DIMENSION_UNKNOWN;
    }
}

void BindSlot(ShaderBindings& b, D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dim, u32 slot)
{
    switch (type)
    {
    case D3D_SIT_CBUFFER:
        if (slot >= kMaxCBs)
        {
            LogOnce("Reflect.cb", "shader constant buffer slot b%u is beyond the layer limit, ignored", slot);
            return;
        }
        b.cbMask = u16(b.cbMask | (1u << slot));
        b.cbCount = u8(Max<u32>(b.cbCount, slot + 1));
        return;
    // A tbuffer, structured and byte-address buffers and acceleration structures (inline ray queries)
    // occupy t registers like textures (D3D11).
    case D3D_SIT_TBUFFER:
    case D3D_SIT_TEXTURE:
    case D3D_SIT_STRUCTURED:
    case D3D_SIT_BYTEADDRESS:
    case D3D_SIT_RTACCELERATIONSTRUCTURE:
        if (slot >= kMaxSRVs)
        {
            LogOnce("Reflect.srv", "shader resource slot t%u is beyond the layer limit, ignored", slot);
            return;
        }
        b.srvMask |= 1u << slot;
        b.srvDims[slot] = type == D3D_SIT_TEXTURE                ? SrvDimensionOf(dim)
            : type == D3D_SIT_RTACCELERATIONSTRUCTURE ? D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE
                                                      : D3D12_SRV_DIMENSION_BUFFER;
        b.srvCount = u8(Max<u32>(b.srvCount, slot + 1));
        return;
    case D3D_SIT_SAMPLER:
        if (slot >= kMaxSamplers)
        {
            LogOnce("Reflect.smp", "shader sampler slot s%u is beyond the layer limit, ignored", slot);
            return;
        }
        b.samplerMask = u16(b.samplerMask | (1u << slot));
        b.samplerCount = u8(Max<u32>(b.samplerCount, slot + 1));
        return;
    case D3D_SIT_UAV_RWTYPED:
    case D3D_SIT_UAV_RWSTRUCTURED:
    case D3D_SIT_UAV_RWBYTEADDRESS:
    case D3D_SIT_UAV_APPEND_STRUCTURED:
    case D3D_SIT_UAV_CONSUME_STRUCTURED:
    case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
        if (slot >= kMaxUAVs)
        {
            LogOnce("Reflect.uav", "shader UAV slot u%u is beyond the layer limit, ignored", slot);
            return;
        }
        b.uavMask = u8(b.uavMask | (1u << slot));
        b.uavDims[slot] = type == D3D_SIT_UAV_RWTYPED ? UavDimensionOf(dim) : D3D12_UAV_DIMENSION_BUFFER;
        b.uavCount = u8(Max<u32>(b.uavCount, slot + 1));
        return;
    default:
        LogOnce("Reflect.type", "shader binding type %u is not supported, ignored", unsigned(type));
        return;
    }
}
} // namespace

// ================================================================================================
// PrivateData
// ================================================================================================

HRESULT PrivateData::Get(REFGUID guid, UINT* size, void* data)
{
    if (!size)
        return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(m_lock);
    for (const Entry& e : m_entries)
    {
        if (!IsEqualGUID(e.guid, guid))
            continue;
        const UINT needed = e.object ? UINT(sizeof(IUnknown*)) : UINT(e.bytes.size());
        if (!data)
        {
            *size = needed;
            return S_OK;
        }
        if (*size < needed)
        {
            *size = needed;
            return DXGI_ERROR_MORE_DATA;
        }
        *size = needed;
        if (e.object)
        {
            e.object->AddRef();
            *static_cast<IUnknown**>(data) = e.object;
        }
        else if (needed)
            memcpy(data, e.bytes.data(), needed);
        return S_OK;
    }
    *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}

HRESULT PrivateData::Set(REFGUID guid, UINT size, const void* data)
{
    if (!data && size)
        return E_INVALIDARG;
    IUnknown* dropped = nullptr;
    try
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_entries.begin();
        while (it != m_entries.end() && !IsEqualGUID(it->guid, guid))
            ++it;
        if (!data)
        {
            if (it != m_entries.end())
            {
                dropped = it->object;
                m_entries.erase(it);
            }
        }
        else
        {
            const u8* bytes = static_cast<const u8*>(data);
            if (it == m_entries.end())
                m_entries.push_back(Entry{guid, std::vector<u8>(bytes, bytes + size), nullptr});
            else
            {
                it->bytes.assign(bytes, bytes + size);
                dropped = it->object;
                it->object = nullptr;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
    // Outside the lock: the object's Release may call back into us.
    if (dropped)
        dropped->Release();
    return S_OK;
}

HRESULT PrivateData::SetInterface(REFGUID guid, const IUnknown* data)
{
    IUnknown* object = const_cast<IUnknown*>(data);
    IUnknown* dropped = nullptr;
    if (object)
        object->AddRef();
    try
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_entries.begin();
        while (it != m_entries.end() && !IsEqualGUID(it->guid, guid))
            ++it;
        if (!object)
        {
            if (it != m_entries.end())
            {
                dropped = it->object;
                m_entries.erase(it);
            }
        }
        else if (it == m_entries.end())
            m_entries.push_back(Entry{guid, {}, object});
        else
        {
            dropped = it->object;
            it->object = object;
            it->bytes.clear();
        }
    }
    catch (const std::bad_alloc&)
    {
        if (object)
            object->Release();
        return E_OUTOFMEMORY;
    }
    if (dropped)
        dropped->Release();
    return S_OK;
}

PrivateData::~PrivateData()
{
    for (Entry& e : m_entries)
    {
        if (e.object)
            e.object->Release();
    }
}

// ================================================================================================
// Resources
// ================================================================================================

Resource::~Resource() = default;

Resource* Resource::From(ID3D11Resource* resource)
{
    if (!resource)
        return nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    switch (dim)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<Buffer*>(static_cast<ID3D11Buffer*>(resource));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<Texture1D*>(static_cast<ID3D11Texture1D*>(resource));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<Texture2D*>(static_cast<ID3D11Texture2D*>(resource));
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return static_cast<Texture3D*>(static_cast<ID3D11Texture3D*>(resource));
    default: return nullptr;
    }
}

Resource* Resource::Actual()
{
    return swapChain ? static_cast<Resource*>(swapChain->CurrentBuffer()) : this;
}

// ---- Buffer ---------------------------------------------------------------------------------------

HRESULT Buffer::Create(Device* device, const D3D11_BUFFER_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Buffer** out)
{
    if (!desc.ByteWidth || (data && !data->pSysMem))
        return E_INVALIDARG;
    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && (desc.ByteWidth % 16))
        return E_INVALIDARG;
    if (desc.Usage == D3D11_USAGE_IMMUTABLE && !data)
        return E_INVALIDARG;
    if (desc.Usage == D3D11_USAGE_STAGING && desc.BindFlags)
        return E_INVALIDARG;
    // D3D11's rules for the view-related flags: structured excludes raw views and indirect arguments,
    // its stride is a multiple of 4 up to 2048.
    const bool structured = (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
    if (structured && ((desc.MiscFlags & (D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)) ||
                          !desc.StructureByteStride || desc.StructureByteStride % 4 || desc.StructureByteStride > 2048))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    Buffer* b = new (std::nothrow) Buffer(device);
    if (!b)
        return E_OUTOFMEMORY;
    b->m_desc = desc;
    if (!structured)
        b->m_desc.StructureByteStride = 0;
    SetResourceCommon(*b, D3D11_RESOURCE_DIMENSION_BUFFER, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    b->byteWidth = desc.ByteWidth;
    // 256: constant buffer views and root CBV addresses need it; it costs nothing elsewhere.
    const u64 size = AlignUp(desc.ByteWidth, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const bool uav = (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    // Views address their buffer from offset 0 (a structured view of a page range would need the page
    // offset to be a multiple of the stride): buffers with views get their own resource.
    const bool views = uav || (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE);
    b->nativeDesc = BufferDesc(size, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = S_OK;
    try
    {
        if (desc.Usage == D3D11_USAGE_DYNAMIC)
        {
            // The first version lives until the buffer dies; every DISCARD renames into frame memory.
            BufferPageAllocator::Range range;
            const bool allocated = device->dynamicFirstVersions.Allocate(size, range);
            if (allocated)
                b->range = range; // freed by Destroy from here on
            if (!allocated || !range.cpu)
                hr = allocated ? E_FAIL : E_OUTOFMEMORY;
            else
            {
                if (data)
                {
                    memcpy(range.cpu, data->pSysMem, desc.ByteWidth);
                    memset(range.cpu + desc.ByteWidth, 0, size_t(size - desc.ByteWidth));
                }
                else
                    memset(range.cpu, 0, size_t(size));
                b->current.alloc.resource = range.resource;
                b->current.alloc.offset = range.offset;
                b->current.alloc.gpu = range.gpu;
                b->current.alloc.cpu = range.cpu;
                b->current.alloc.size = size;
                b->current.version = 1;
            }
        }
        else if (desc.Usage == D3D11_USAGE_STAGING)
        {
            hr = CreateLinear(device, *b, size, "staging buffer");
            if (SUCCEEDED(hr) && data)
                memcpy(b->linearCpu, data->pSysMem, desc.ByteWidth);
        }
        else if (data && !views)
        {
            // Static data: a range of a DEFAULT page, never written by the GPU again.
            BufferPageAllocator::Range range;
            if (!device->bufferPages.Allocate(size, range))
                hr = E_OUTOFMEMORY;
            else
            {
                b->range = range;
                device->Uploader(false).UploadBuffer(range.resource, range.offset, data->pSysMem, desc.ByteWidth);
            }
        }
        else
        {
            hr = CreateCommitted(device, D3D12_HEAP_TYPE_DEFAULT, b->nativeDesc, D3D12_RESOURCE_STATE_COMMON, &b->native, "buffer");
            if (SUCCEEDED(hr))
            {
                b->states.uniform = true;
                b->states.all = D3D12_RESOURCE_STATE_COMMON;
                device->TrackBuffer(b);
                if (data)
                    device->Uploader(false).UploadBuffer(b->native, 0, data->pSysMem, desc.ByteWidth);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr))
    {
        b->Release();
        return hr;
    }
    *out = b;
    return S_OK;
}

void Buffer::Destroy()
{
    Device* device = m_device;
    // A DISCARD version from a context's persistent heap goes back to it.
    RetireDynamicVersion(this);
    device->UntrackBuffer(this);
    ReleaseStorage(device, *this);
    if (range.resource)
    {
        BufferPageAllocator* pages = usage == D3D11_USAGE_DYNAMIC ? &device->dynamicFirstVersions : &device->bufferPages;
        const BufferPageAllocator::Range freed = range;
        DeferAction(device, [pages, freed] { pages->Free(freed); });
        range = {};
    }
    delete this;
}

void Buffer::OnDebugName(const char* name)
{
    SetNativeName(native, name);
    SetNativeName(linear, name);
}

// ---- Textures -------------------------------------------------------------------------------------

HRESULT Texture1D::Create(Device* device, const D3D11_TEXTURE1D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture1D** out)
{
    const u32 fullMips = FullMipCount(desc.Width, 1, 1);
    const u32 mips = desc.MipLevels ? desc.MipLevels : fullMips;
    if (!desc.Width || !desc.ArraySize || desc.ArraySize > D3D12_REQ_TEXTURE1D_ARRAY_AXIS_DIMENSION || mips > fullMips)
        return E_INVALIDARG;
    if ((desc.Usage == D3D11_USAGE_IMMUTABLE && !data) || (desc.Usage == D3D11_USAGE_STAGING && desc.BindFlags))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    Texture1D* t = new (std::nothrow) Texture1D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    t->m_desc.MipLevels = mips;
    SetResourceCommon(*t, D3D11_RESOURCE_DIMENSION_TEXTURE1D, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    const D3D12_RESOURCE_DESC rd = TextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE1D, desc.Width, 1, desc.ArraySize, mips,
        desc.Format, {1, 0}, desc.BindFlags);
    const HRESULT hr = InitTexture(device, *t, rd, data, "Texture1D");
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

void Texture1D::Destroy()
{
    ReleaseStorage(m_device, *this);
    delete this;
}

void Texture1D::OnDebugName(const char* name)
{
    SetNativeName(native, name);
    SetNativeName(linear, name);
}

HRESULT Texture2D::Create(Device* device, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture2D** out)
{
    const u32 fullMips = FullMipCount(desc.Width, desc.Height, 1);
    const u32 mips = desc.MipLevels ? desc.MipLevels : fullMips;
    const bool multisampled = desc.SampleDesc.Count > 1;
    if (!desc.Width || !desc.Height || !desc.ArraySize || desc.ArraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        mips > fullMips || !desc.SampleDesc.Count)
        return E_INVALIDARG;
    if ((desc.Usage == D3D11_USAGE_IMMUTABLE && !data) || (desc.Usage == D3D11_USAGE_STAGING && desc.BindFlags))
        return E_INVALIDARG;
    // Multisampled resources have one mip, no initial data and live on the GPU only.
    if (multisampled && (mips != 1 || data || desc.Usage == D3D11_USAGE_STAGING || desc.Usage == D3D11_USAGE_DYNAMIC))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    Texture2D* t = new (std::nothrow) Texture2D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    t->m_desc.MipLevels = mips;
    SetResourceCommon(*t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    const D3D12_RESOURCE_DESC rd = TextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, desc.Width, desc.Height, desc.ArraySize,
        mips, desc.Format, desc.SampleDesc, desc.BindFlags);
    const HRESULT hr = InitTexture(device, *t, rd, data, "Texture2D");
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

Texture2D* Texture2D::CreateForSwapChain(Device* device, SwapChain* chain, ID3D12Resource* buffer, const D3D11_TEXTURE2D_DESC& desc)
{
    Texture2D* t = new (std::nothrow) Texture2D(device);
    if (!t)
        return nullptr;
    D3D11_TEXTURE2D_DESC d = desc;
    d.MipLevels = 1;
    d.ArraySize = 1;
    if (!d.SampleDesc.Count)
        d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    t->m_desc = d;
    SetResourceCommon(*t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, d.Usage, d.BindFlags, 0, 0);
    t->format = d.Format;
    t->mipLevels = 1;
    t->arraySize = 1;
    t->planeCount = 1;
    if (buffer)
    {
        buffer->AddRef();
        t->native = buffer;
        t->nativeDesc = buffer->GetDesc();
        t->states.uniform = true;
        t->states.all = D3D12_RESOURCE_STATE_PRESENT;
    }
    else
    {
        // The proxy: no storage, every command resolves the current buffer through Actual().
        t->swapChain = chain;
        t->nativeDesc = TextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, d.Width, d.Height, 1, 1, d.Format, d.SampleDesc, d.BindFlags);
    }
    return t;
}

void Texture2D::Destroy()
{
    ReleaseStorage(m_device, *this);
    delete this;
}

void Texture2D::OnDebugName(const char* name)
{
    SetNativeName(native, name);
    SetNativeName(linear, name);
}

HRESULT Texture3D::Create(Device* device, const D3D11_TEXTURE3D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture3D** out)
{
    const u32 fullMips = FullMipCount(desc.Width, desc.Height, desc.Depth);
    const u32 mips = desc.MipLevels ? desc.MipLevels : fullMips;
    if (!desc.Width || !desc.Height || !desc.Depth || desc.Depth > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION || mips > fullMips)
        return E_INVALIDARG;
    if ((desc.Usage == D3D11_USAGE_IMMUTABLE && !data) || (desc.Usage == D3D11_USAGE_STAGING && desc.BindFlags))
        return E_INVALIDARG;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    Texture3D* t = new (std::nothrow) Texture3D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    t->m_desc.MipLevels = mips;
    SetResourceCommon(*t, D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
    const D3D12_RESOURCE_DESC rd = TextureDesc(D3D12_RESOURCE_DIMENSION_TEXTURE3D, desc.Width, desc.Height, desc.Depth, mips,
        desc.Format, {1, 0}, desc.BindFlags);
    const HRESULT hr = InitTexture(device, *t, rd, data, "Texture3D");
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

void Texture3D::Destroy()
{
    ReleaseStorage(m_device, *this);
    delete this;
}

void Texture3D::OnDebugName(const char* name)
{
    SetNativeName(native, name);
    SetNativeName(linear, name);
}

// ================================================================================================
// Views
// ================================================================================================

D3D12_CPU_DESCRIPTOR_HANDLE View::Descriptor()
{
    if (perBuffer.empty())
        return cpu;
    const u32 index = resource->swapChain->CurrentIndex();
    return index < perBuffer.size() ? perBuffer[index] : perBuffer[0];
}

D3D12_CPU_DESCRIPTOR_HANDLE View::AdaptedSrv(D3D12_SRV_DIMENSION wanted, bool& ok)
{
    ok = false;
    if (kind != ViewKind::SRV)
        return {};
    if (wanted == srvDimension)
    {
        ok = true;
        return Descriptor();
    }
    // Back buffer views change their resource every frame; they are never adapted.
    if (!perBuffer.empty() || !resource->native)
        return {};

    D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
    nd.Format = format;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.ViewDimension = wanted;
    switch (wanted)
    {
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        // A Texture2D declaration reads the first slice of the view: only slice 0 is expressible.
        if (srvDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY || range.firstSlice != 0)
            return {};
        nd.Texture2D.MostDetailedMip = range.firstMip;
        nd.Texture2D.MipLevels = range.mipCount;
        nd.Texture2D.PlaneSlice = SrvPlaneSlice(format);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        if (srvDimension != D3D12_SRV_DIMENSION_TEXTURE2D)
            return {};
        nd.Texture2DArray.MostDetailedMip = range.firstMip;
        nd.Texture2DArray.MipLevels = range.mipCount;
        nd.Texture2DArray.FirstArraySlice = 0;
        nd.Texture2DArray.ArraySize = 1;
        nd.Texture2DArray.PlaneSlice = SrvPlaneSlice(format);
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        if (srvDimension != D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY || range.firstSlice != 0)
            return {};
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        if (srvDimension != D3D12_SRV_DIMENSION_TEXTURE2DMS)
            return {};
        nd.Texture2DMSArray.FirstArraySlice = 0;
        nd.Texture2DMSArray.ArraySize = 1;
        break;
    default:
        return {};
    }

    std::lock_guard<std::mutex> lock(m_adaptLock);
    for (const auto& entry : m_adapted)
    {
        if (entry.first == wanted)
        {
            ok = true;
            return entry.second;
        }
    }
    Device* device = DeviceOf(resourceIface);
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = device->cpuSrv.Allocate();
    if (!handle.ptr)
        return {};
    try
    {
        m_adapted.emplace_back(wanted, handle);
    }
    catch (const std::bad_alloc&)
    {
        device->cpuSrv.Free(handle);
        return {};
    }
    device->d12->CreateShaderResourceView(resource->native, &nd, handle);
    ok = true;
    return handle;
}

View::~View()
{
    if (!resourceIface)
        return;
    // CPU descriptors are read at record time only (copies into the ring, OM/clear calls), and a bound
    // view is referenced by its context, so they can go back to the heap right away.
    Device* device = DeviceOf(resourceIface);
    // Bindless copies are read by the GPU: they are nulled once it is done.
    if (!bindlessSlots.empty())
        device->bindless.ForgetView(*this);
    // Work recorded with the view may still read the acceleration structure.
    DeferRelease(device, accelerationStructure);
    accelerationStructure = nullptr;
    try
    {
        CpuDescriptorHeap& heap = HeapOf(device, kind);
        if (perBuffer.empty())
        {
            if (cpu.ptr)
                heap.Free(cpu);
        }
        else
        {
            for (const D3D12_CPU_DESCRIPTOR_HANDLE& handle : perBuffer)
                heap.Free(handle);
        }
        for (const auto& entry : m_adapted)
            device->cpuSrv.Free(entry.second);
    }
    catch (const std::bad_alloc&)
    {
        LogOnce("View.free", "out of memory while freeing view descriptors, they leak");
    }
    resourceIface->Release();
}

View* View::From(ID3D11ShaderResourceView* view)
{
    return view ? static_cast<ShaderResourceView*>(view) : nullptr;
}

View* View::From(ID3D11RenderTargetView* view)
{
    return view ? static_cast<RenderTargetView*>(view) : nullptr;
}

View* View::From(ID3D11DepthStencilView* view)
{
    return view ? static_cast<DepthStencilView*>(view) : nullptr;
}

View* View::From(ID3D11UnorderedAccessView* view)
{
    return view ? static_cast<UnorderedAccessView*>(view) : nullptr;
}

// ---- ShaderResourceView ----------------------------------------------------------------------------

HRESULT ShaderResourceView::Create(Device* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
    ShaderResourceView** out)
{
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_SHADER_RESOURCE, "ShaderResourceView", r);
    if (FAILED(hr))
        return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
    SubRange range;
    if (r->IsBuffer())
        hr = ConvertBufferSrv(*static_cast<Buffer*>(r), desc, d, nd);
    else
    {
        d = desc ? *desc : DefaultSrv(*r);
        if (d.Format == DXGI_FORMAT_UNKNOWN)
            d.Format = r->format;
        if (!ViewFormatFits(*r, d.Format, false))
            return E_INVALIDARG;
        hr = ConvertSrv(*r, d, nd, range);
    }
    if (FAILED(hr))
        return hr;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    ShaderResourceView* v = new (std::nothrow) ShaderResourceView(device);
    if (!v)
        return E_OUTOFMEMORY;
    AttachResource(*v, ViewKind::SRV, resource, r, device);
    v->m_desc = d;
    v->range = range;
    v->format = d.Format;
    v->srvDimension = nd.ViewDimension;
    hr = CreateDescriptors(device, *v, [device, &nd](ID3D12Resource* native, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        { device->d12->CreateShaderResourceView(native, &nd, handle); });
    if (FAILED(hr))
    {
        v->Release();
        return hr;
    }
    *out = v;
    return S_OK;
}

HRESULT ShaderResourceView::CreateAccelerationStructure(Device* device, ID3D12Resource* tlas, ShaderResourceView** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!device->caps.raytracingTier)
        return DXGI_ERROR_UNSUPPORTED;
    // A small static page buffer stands in for the D3D11 resource: untracked, so the view is never
    // transitioned (acceleration structures stay in their own state).
    static const u8 zeros[256]{};
    D3D11_BUFFER_DESC carrierDesc{};
    carrierDesc.ByteWidth = sizeof(zeros);
    carrierDesc.Usage = D3D11_USAGE_IMMUTABLE;
    carrierDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA carrierData{zeros, 0, 0};
    Buffer* carrier = nullptr;
    HRESULT hr = Buffer::Create(device, carrierDesc, &carrierData, &carrier);
    if (FAILED(hr))
        return hr;
    ShaderResourceView* v = new (std::nothrow) ShaderResourceView(device);
    if (!v)
    {
        carrier->Release();
        return E_OUTOFMEMORY;
    }
    AttachResource(*v, ViewKind::SRV, static_cast<ID3D11Buffer*>(carrier), carrier, device);
    carrier->Release(); // the view holds its own reference
    v->m_desc.Format = DXGI_FORMAT_UNKNOWN;
    v->m_desc.ViewDimension = D3D11_SRV_DIMENSION_UNKNOWN;
    v->srvDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    v->cpu = device->cpuSrv.Allocate();
    if (!v->cpu.ptr)
    {
        v->Release();
        return E_OUTOFMEMORY;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
    nd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.RaytracingAccelerationStructure.Location = tlas ? tlas->GetGPUVirtualAddress() : 0;
    device->d12->CreateShaderResourceView(nullptr, &nd, v->cpu);
    if (tlas)
    {
        tlas->AddRef();
        v->accelerationStructure = tlas;
    }
    *out = v;
    return S_OK;
}

void STDMETHODCALLTYPE ShaderResourceView::GetResource(ID3D11Resource** out)
{
    if (!out)
        return;
    *out = resourceIface;
    resourceIface->AddRef();
}

void ShaderResourceView::Destroy()
{
    delete this;
}

// ---- RenderTargetView -------------------------------------------------------------------------------

HRESULT RenderTargetView::Create(Device* device, ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc,
    RenderTargetView** out)
{
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_RENDER_TARGET, "RenderTargetView", r);
    if (FAILED(hr))
        return hr;
    D3D11_RENDER_TARGET_VIEW_DESC d = desc ? *desc : DefaultRtv(*r);
    if (d.Format == DXGI_FORMAT_UNKNOWN)
        d.Format = r->format;
    if (!ViewFormatFits(*r, d.Format, false))
        return E_INVALIDARG;
    D3D12_RENDER_TARGET_VIEW_DESC nd{};
    SubRange range;
    hr = ConvertRtv(*r, d, nd, range);
    if (FAILED(hr))
        return hr;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    RenderTargetView* v = new (std::nothrow) RenderTargetView(device);
    if (!v)
        return E_OUTOFMEMORY;
    AttachResource(*v, ViewKind::RTV, resource, r, device);
    v->m_desc = d;
    v->range = range;
    v->format = d.Format;
    hr = CreateDescriptors(device, *v, [device, &nd](ID3D12Resource* native, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        { device->d12->CreateRenderTargetView(native, &nd, handle); });
    if (FAILED(hr))
    {
        v->Release();
        return hr;
    }
    *out = v;
    return S_OK;
}

void STDMETHODCALLTYPE RenderTargetView::GetResource(ID3D11Resource** out)
{
    if (!out)
        return;
    *out = resourceIface;
    resourceIface->AddRef();
}

void RenderTargetView::Destroy()
{
    delete this;
}

// ---- DepthStencilView -------------------------------------------------------------------------------

HRESULT DepthStencilView::Create(Device* device, ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
    DepthStencilView** out)
{
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_DEPTH_STENCIL, "DepthStencilView", r);
    if (FAILED(hr))
        return hr;
    D3D11_DEPTH_STENCIL_VIEW_DESC d = desc ? *desc : DefaultDsv(*r);
    if (d.Format == DXGI_FORMAT_UNKNOWN)
        d.Format = r->format;
    D3D12_DEPTH_STENCIL_VIEW_DESC nd{};
    SubRange range;
    hr = ConvertDsv(*r, d, nd, range);
    if (FAILED(hr))
        return hr;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    DepthStencilView* v = new (std::nothrow) DepthStencilView(device);
    if (!v)
        return E_OUTOFMEMORY;
    AttachResource(*v, ViewKind::DSV, resource, r, device);
    v->m_desc = d;
    v->range = range;
    v->format = d.Format;
    v->dsvFlags = d.Flags;
    hr = CreateDescriptors(device, *v, [device, &nd](ID3D12Resource* native, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        { device->d12->CreateDepthStencilView(native, &nd, handle); });
    if (FAILED(hr))
    {
        v->Release();
        return hr;
    }
    *out = v;
    return S_OK;
}

void STDMETHODCALLTYPE DepthStencilView::GetResource(ID3D11Resource** out)
{
    if (!out)
        return;
    *out = resourceIface;
    resourceIface->AddRef();
}

void DepthStencilView::Destroy()
{
    delete this;
}

// ---- UnorderedAccessView ----------------------------------------------------------------------------

HRESULT UnorderedAccessView::Create(Device* device, ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
    UnorderedAccessView** out)
{
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_UNORDERED_ACCESS, "UnorderedAccessView", r);
    if (FAILED(hr))
        return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC nd{};
    SubRange range;
    if (r->IsBuffer())
        hr = ConvertBufferUav(*static_cast<Buffer*>(r), desc, d, nd);
    else
    {
        d = desc ? *desc : DefaultUav(*r);
        if (d.Format == DXGI_FORMAT_UNKNOWN)
            d.Format = r->format;
        if (!ViewFormatFits(*r, d.Format, false))
            return E_INVALIDARG;
        hr = ConvertUav(*r, d, nd, range);
    }
    if (FAILED(hr))
        return hr;
    if (!out)
        return S_FALSE;
    *out = nullptr;

    UnorderedAccessView* v = new (std::nothrow) UnorderedAccessView(device);
    if (!v)
        return E_OUTOFMEMORY;
    AttachResource(*v, ViewKind::UAV, resource, r, device);
    v->m_desc = d;
    v->nativeDesc = nd;
    v->range = range;
    v->format = d.Format;
    hr = CreateDescriptors(device, *v, [device, &nd](ID3D12Resource* native, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        { device->d12->CreateUnorderedAccessView(native, nullptr, &nd, handle); });
    if (SUCCEEDED(hr) && nd.ViewDimension == D3D12_UAV_DIMENSION_BUFFER && nd.Format == DXGI_FORMAT_UNKNOWN)
    {
        // The raw alias of a structured view: same bytes in 32-bit words.
        const u64 stride = nd.Buffer.StructureByteStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
        raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        raw.Buffer.FirstElement = nd.Buffer.FirstElement * stride / 4;
        raw.Buffer.NumElements = UINT(u64(nd.Buffer.NumElements) * stride / 4);
        raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        v->rawAlias = device->cpuSrv.Allocate();
        if (!v->rawAlias.ptr)
            hr = E_OUTOFMEMORY;
        else
            device->d12->CreateUnorderedAccessView(r->native, nullptr, &raw, v->rawAlias);
    }
    if (FAILED(hr))
    {
        v->Release();
        return hr;
    }
    *out = v;
    return S_OK;
}

void STDMETHODCALLTYPE UnorderedAccessView::GetResource(ID3D11Resource** out)
{
    if (!out)
        return;
    *out = resourceIface;
    resourceIface->AddRef();
}

void UnorderedAccessView::Destroy()
{
    // Read only while a clear is recorded, like the view's own descriptor.
    if (rawAlias.ptr)
    {
        try
        {
            m_device->cpuSrv.Free(rawAlias);
        }
        catch (const std::bad_alloc&)
        {
            LogOnce("UnorderedAccessView.free", "out of memory while freeing a view descriptor, it leaks");
        }
    }
    delete this;
}

// ================================================================================================
// State objects
// ================================================================================================

void SamplerState::Destroy()
{
    if (cpu.ptr)
    {
        // Sampler tables are built from these descriptors while recording; free after the GPU caught up.
        Device* device = m_device;
        const D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu;
        DeferAction(device, [device, handle] { device->cpuSampler.Free(handle); });
        cpu = {};
    }
    delete this;
}

// ================================================================================================
// Shaders
// ================================================================================================

bool Shader::Reflect()
{
    bindings = ShaderBindings{};
    inputs.clear();
    if (bytecode.empty())
        return false;
    hash = Hash64(bytecode.data(), bytecode.size());

    // DXBC and DXIL alike (d12_dxc.cpp): the D3D12 reflection interface.
    ID3D12ShaderReflection* reflection = nullptr;
    const HRESULT hr = xrd3d12::Reflect(bytecode.data(), bytecode.size(), __uuidof(ID3D12ShaderReflection),
        reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection)
    {
        Log("shader reflection failed, hr 0x%08X (stage %u, %u bytes)", unsigned(hr), unsigned(stage), unsigned(bytecode.size()));
        return false;
    }

    bool ok = true;
    try
    {
        D3D12_SHADER_DESC desc{};
        reflection->GetDesc(&desc);
        for (UINT i = 0; i < desc.BoundResources; ++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC bind{};
            if (FAILED(reflection->GetResourceBindingDesc(i, &bind)))
                continue;
            // The emulated D3D11 bindings live in space 0.
            if (bind.Space)
            {
                LogOnce("Reflect.space", "shader '%s': binding '%s' in register space %u ignored", name.c_str(),
                    bind.Name ? bind.Name : "?", bind.Space);
                continue;
            }
            const UINT count = bind.BindCount ? bind.BindCount : 1;
            for (UINT slot = bind.BindPoint; slot < bind.BindPoint + count; ++slot)
                BindSlot(bindings, bind.Type, bind.Dimension, slot);
        }
        if (stage == Stage::VS)
        {
            for (UINT i = 0; i < desc.InputParameters; ++i)
            {
                D3D12_SIGNATURE_PARAMETER_DESC param{};
                if (FAILED(reflection->GetInputParameterDesc(i, &param)) || param.SystemValueType != D3D_NAME_UNDEFINED)
                    continue;
                std::string name = param.SemanticName ? param.SemanticName : "";
                for (char& c : name)
                {
                    if (c >= 'a' && c <= 'z')
                        c = char(c - 'a' + 'A');
                }
                inputs.emplace_back(std::move(name), param.SemanticIndex);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        ok = false;
    }
    reflection->Release();
    return ok;
}

Shader* Shader::From(ID3D11DeviceChild* shader, Stage stage)
{
    if (!shader)
        return nullptr;
    switch (stage)
    {
    case Stage::VS: return static_cast<VertexShader*>(static_cast<ID3D11VertexShader*>(shader));
    case Stage::HS: return static_cast<HullShader*>(static_cast<ID3D11HullShader*>(shader));
    case Stage::DS: return static_cast<DomainShader*>(static_cast<ID3D11DomainShader*>(shader));
    case Stage::GS: return static_cast<GeometryShader*>(static_cast<ID3D11GeometryShader*>(shader));
    case Stage::PS: return static_cast<PixelShader*>(static_cast<ID3D11PixelShader*>(shader));
    case Stage::CS: return static_cast<ComputeShader*>(static_cast<ID3D11ComputeShader*>(shader));
    default: return nullptr;
    }
}

// ================================================================================================
// Queries
// ================================================================================================

HRESULT Query::Create(Device* device, const D3D11_QUERY_DESC& desc, Query** out)
{
    if (!out)
        return S_FALSE;
    *out = nullptr;
    Query* q = new (std::nothrow) Query(device);
    if (!q)
        return E_OUTOFMEMORY;
    q->m_desc = desc;
    bool needsSlot = false;
    try
    {
        switch (desc.Query)
        {
        case D3D11_QUERY_OCCLUSION:
        case D3D11_QUERY_OCCLUSION_PREDICATE:
            needsSlot = true;
            q->slot = device->queryHeaps.Allocate(D3D12_QUERY_HEAP_TYPE_OCCLUSION);
            break;
        case D3D11_QUERY_TIMESTAMP:
            needsSlot = true;
            q->slot = device->queryHeaps.Allocate(D3D12_QUERY_HEAP_TYPE_TIMESTAMP);
            break;
        case D3D11_QUERY_EVENT:
        case D3D11_QUERY_TIMESTAMP_DISJOINT:
            break;
        default:
            LogOnce("Query.type", "query type %u is not implemented, its data reads as zeros", unsigned(desc.Query));
            break;
        }
    }
    catch (const std::bad_alloc&)
    {
        q->Release();
        return E_OUTOFMEMORY;
    }
    if (needsSlot && !q->slot.heap)
    {
        LogOnce("Query.slot", "no query heap slot for query type %u", unsigned(desc.Query));
        q->Release();
        return E_OUTOFMEMORY;
    }
    *out = q;
    return S_OK;
}

UINT STDMETHODCALLTYPE Query::GetDataSize()
{
    switch (m_desc.Query)
    {
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
    case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: return sizeof(BOOL);
    case D3D11_QUERY_PIPELINE_STATISTICS: return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3: return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
    default: return 0;
    }
}

void Query::Destroy()
{
    for (const QueryHeapPool::Slot& part : parts)
    {
        Device* device = m_device;
        const QueryHeapPool::Slot freed = part;
        DeferAction(device, [device, freed] { device->queryHeaps.Free(D3D12_QUERY_HEAP_TYPE_OCCLUSION, freed); });
    }
    parts.clear();
    if (slot.heap)
    {
        // A submitted End may still resolve into the slot.
        Device* device = m_device;
        const D3D12_QUERY_HEAP_TYPE type =
            m_desc.Query == D3D11_QUERY_TIMESTAMP ? D3D12_QUERY_HEAP_TYPE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        const QueryHeapPool::Slot freed = slot;
        DeferAction(device, [device, type, freed] { device->queryHeaps.Free(type, freed); });
        slot = {};
    }
    delete this;
}

// ================================================================================================
// Command lists
// ================================================================================================

void CommandList::Destroy()
{
    if (list)
    {
        // Never executed: the GPU has not seen it, the recording context may reuse it.
        ID3D12GraphicsCommandList* unused = list;
        list = nullptr;
        bool returned = false;
        if (recordedBy)
        {
            try
            {
                recordedBy->ReturnList(unused);
                returned = true;
            }
            catch (const std::bad_alloc&)
            {
            }
        }
        if (!returned)
            unused->Release();
    }
    // Its staging copies never ran: the references taken when they were recorded go too.
    ReleaseStagingReferences(pendingReadback);
    if (asyncStamps != ~0u)
        m_device->AsyncDropStamps(asyncStamps);
    delete this;
}
} // namespace xrd3d12
