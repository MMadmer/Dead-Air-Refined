// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Objects of xrVulkan: private data, resources (buffers, textures, the swap chain's images), views,
// shaders' teardown, queries and command lists.
#include "vk_layer.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace xrvk
{
namespace
{
VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

template <class F>
void DeferAction(Device* device, F&& action)
{
    // Resource storage and views: what a descriptor set names.
    device->DescriptorTargetGone();
    device->releaser.Defer(std::function<void()>(std::forward<F>(action)));
}

u32 FullMipCount(u32 width, u32 height, u32 depth)
{
    u32 size = (std::max)({width, height, depth, 1u});
    u32 mips = 1;
    while (size > 1)
    {
        size >>= 1;
        ++mips;
    }
    return mips;
}

u32 DepthAtMip(const Resource& r, u32 mip)
{
    return (std::max)(1u, r.depth >> mip);
}

void SetResourceCommon(Device* device, Resource& r, D3D11_RESOURCE_DIMENSION dimension, D3D11_USAGE usage, UINT bind,
    UINT cpu, UINT misc)
{
    r.dimension = dimension;
    r.usage = usage;
    r.bindFlags = bind;
    r.cpuAccess = cpu;
    r.miscFlags = misc;
    r.id = device->nextObjectId.fetch_add(1, std::memory_order_relaxed);
}

// Queue families that may touch a resource; CONCURRENT sharing where more than one does (DOOM keeps
// async-compute images concurrent and in GENERAL: no ownership transfers anywhere).
u32 SharingFamilies(Device* device, bool async, bool transfer, u32 (&families)[3])
{
    u32 count = 0;
    families[count++] = device->graphicsFamily;
    if (async && device->computeQueue)
        families[count++] = device->computeFamily;
    if (transfer && device->transferQueue)
        families[count++] = device->transferFamily;
    return count;
}

bool IsTypeless(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
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

// The Vulkan format of a texture: depth-capable typeless formats become their depth format when the
// texture is a depth target.
VkFormat ImageFormat(DXGI_FORMAT format, UINT bind)
{
    if (bind & D3D11_BIND_DEPTH_STENCIL)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:
            return VK_FORMAT_D16_UNORM;
        default:
            break;
        }
    }
    return Format(format).format;
}

VkImageAspectFlags AspectOf(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// Linear CPU-visible storage of staging and dynamic textures: one footprint per subresource.
HRESULT CreateLinear(Device* device, Resource& r, MemoryClass cls)
{
    const FormatInfo& fmt = Format(r.format);
    const u32 blockBytes = fmt.blockBytes ? fmt.blockBytes : 4;
    const u32 blockWidth = fmt.blockWidth;
    // Rows: multiples of the block size and of 16 bytes (copies need block-aligned offsets and pitches).
    const VkDeviceSize rowAlign = blockBytes % 16 == 0 ? blockBytes : (16 % blockBytes == 0 ? 16 : blockBytes * 16);
    VkDeviceSize total = 0;
    r.footprints.resize(r.SubresourceCount());
    for (u32 layer = 0; layer < r.arraySize; ++layer)
    {
        for (u32 mip = 0; mip < r.mipLevels; ++mip)
        {
            Resource::Footprint& f = r.footprints[r.Subresource(mip, layer)];
            f.width = (std::max)(1u, r.width >> mip);
            f.height = (std::max)(1u, r.height >> mip);
            f.depth = DepthAtMip(r, mip);
            const u32 blocksX = (f.width + blockWidth - 1) / blockWidth;
            f.rows = (f.height + blockWidth - 1) / blockWidth;
            f.rowPitch = u32(AlignUp(VkDeviceSize(blocksX) * blockBytes, rowAlign));
            f.depthPitch = f.rowPitch * f.rows;
            total = AlignUp(total, 256);
            f.offset = total;
            total += VkDeviceSize(f.depthPitch) * f.depth;
        }
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = (std::max)(total, VkDeviceSize(256));
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult result = g_vk.vkCreateBuffer(device->vk, &info, nullptr, &r.linear);
    if (result != VK_SUCCESS)
        return E_OUTOFMEMORY;
    VkMemoryRequirements req{};
    g_vk.vkGetBufferMemoryRequirements(device->vk, r.linear, &req);
    if (!device->memory.Allocate(req, cls, false, VK_NULL_HANDLE, r.linear, r.linearMemory) || !r.linearMemory.cpu)
        return E_OUTOFMEMORY;
    result = g_vk.vkBindBufferMemory(device->vk, r.linear, r.linearMemory.memory, r.linearMemory.offset);
    return result == VK_SUCCESS ? S_OK : E_OUTOFMEMORY;
}
} // namespace

// ============================================================================================
// PrivateData
// ============================================================================================

HRESULT PrivateData::Get(REFGUID guid, UINT* size, void* data)
{
    if (!size)
        return E_INVALIDARG;
    std::lock_guard lock(m_lock);
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
            std::memcpy(data, e.bytes.data(), needed);
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
        std::lock_guard lock(m_lock);
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
        std::lock_guard lock(m_lock);
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

// ============================================================================================
// Resources
// ============================================================================================

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

void ReleaseResourceStorage(Device* device, Resource& r)
{
    const VkImage image = r.image;
    const VkBuffer buffer = r.buffer;
    const VkBuffer linear = r.linear;
    const Allocation memory = r.memory;
    const Allocation linearMemory = r.linearMemory;
    const bool ownsImage = image != VK_NULL_HANDLE;
    const VkImageView rateView = r.rateView;
    const VkImageView nativeView = r.nativeView;
    r.rateView = r.nativeView = VK_NULL_HANDLE;
    r.image = VK_NULL_HANDLE;
    r.buffer = VK_NULL_HANDLE;
    r.linear = VK_NULL_HANDLE;
    r.memory = {};
    r.linearMemory = {};
    if (!ownsImage && !buffer && !linear && !memory.Valid() && !linearMemory.Valid() && !rateView && !nativeView)
        return;
    DeferAction(device, [device, image, buffer, linear, memory, linearMemory, ownsImage, rateView, nativeView] {
        if (rateView)
            g_vk.vkDestroyImageView(device->vk, rateView, nullptr);
        if (nativeView)
            g_vk.vkDestroyImageView(device->vk, nativeView, nullptr);
        if (ownsImage)
            g_vk.vkDestroyImage(device->vk, image, nullptr);
        if (buffer)
            g_vk.vkDestroyBuffer(device->vk, buffer, nullptr);
        if (linear)
            g_vk.vkDestroyBuffer(device->vk, linear, nullptr);
        device->memory.Free(memory);
        device->memory.Free(linearMemory);
    });
}

HRESULT Buffer::Create(Device* device, const D3D11_BUFFER_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Buffer** out)
{
    *out = nullptr;
    if (!desc.ByteWidth)
        return E_INVALIDARG;
    if (desc.Usage == D3D11_USAGE_IMMUTABLE && (!data || !data->pSysMem))
        return E_INVALIDARG;
    if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && desc.ByteWidth > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16)
        return E_INVALIDARG;
    if (desc.BindFlags & D3D11_BIND_STREAM_OUTPUT)
    {
        static constexpr char kFormat[] = "! stream output buffers are not supported by the Vulkan renderer";
        LogOnce(kFormat, kFormat);
        return E_NOTIMPL;
    }
    Buffer* b = new (std::nothrow) Buffer(device);
    if (!b)
        return E_OUTOFMEMORY;
    b->m_desc = desc;
    SetResourceCommon(device, *b, D3D11_RESOURCE_DIMENSION_BUFFER, desc.Usage, desc.BindFlags, desc.CPUAccessFlags,
        desc.MiscFlags);
    b->byteWidth = desc.ByteWidth;

    // Dynamic buffers live in upload memory, version by version; so do constant buffers the engine updates
    // with UpdateSubresource (no copy, no split of the rendering).
    b->renamed = desc.Usage == D3D11_USAGE_DEFAULT && desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER;
    if (desc.Usage == D3D11_USAGE_DYNAMIC || b->renamed)
    {
        if (b->renamed)
            b->shadow.assign(desc.ByteWidth, 0);
        if (data && data->pSysMem)
        {
            if (b->renamed)
                std::memcpy(b->shadow.data(), data->pSysMem, desc.ByteWidth);
        }
        if ((data && data->pSysMem) || b->renamed)
        {
            UploadAlloc alloc{};
            if (!device->creationHeap || !device->creationHeap->Allocate(desc.ByteWidth, alloc, nullptr))
            {
                b->Release();
                return E_OUTOFMEMORY;
            }
            std::memcpy(alloc.cpu, b->renamed ? b->shadow.data() : data->pSysMem, desc.ByteWidth);
            b->current.alloc = alloc;
            b->current.heap = device->creationHeap;
            b->current.version = 1;
        }
        *out = b;
        return S_OK;
    }

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = AlignUp(desc.ByteWidth, 4);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (desc.BindFlags & D3D11_BIND_VERTEX_BUFFER)
        info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (desc.BindFlags & D3D11_BIND_INDEX_BUFFER)
        info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)
        info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
        info.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
        info.usage |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (desc.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)
        info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device->caps.rayQuery)
    {
        info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        // Vertex and index data feed bottom-level acceleration structures as they are, and the RT lane reads them as
        // bindless raw buffers.
        if (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE))
            info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        // The RT lane's own buffers (vk_raytracing.cpp).
        if (desc.MiscFlags & kMiscRtBuffer)
            info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        if (desc.MiscFlags & kMiscAccelStorage)
            info.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    }
    u32 families[3];
    const bool staging = desc.Usage == D3D11_USAGE_STAGING;
    const u32 familyCount = staging ? 1 : SharingFamilies(device, true, true, families);
    if (familyCount > 1)
    {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = familyCount;
        info.pQueueFamilyIndices = families;
    }
    VkResult result = g_vk.vkCreateBuffer(device->vk, &info, nullptr, &b->buffer);
    if (result != VK_SUCCESS)
    {
        b->Release();
        return E_OUTOFMEMORY;
    }
    VkMemoryRequirements req{};
    g_vk.vkGetBufferMemoryRequirements(device->vk, b->buffer, &req);
    // Structures, scratch and the lane's tables start at 256-byte addresses.
    if (desc.MiscFlags & (kMiscRtBuffer | kMiscAccelStorage))
        req.alignment = (std::max)(req.alignment, VkDeviceSize(256));
    const MemoryClass cls = !staging ? MemoryClass::Device :
        (desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) ? MemoryClass::Readback : MemoryClass::Staging;
    if (!device->memory.Allocate(req, cls, false, VK_NULL_HANDLE, b->buffer, b->memory) ||
        g_vk.vkBindBufferMemory(device->vk, b->buffer, b->memory.memory, b->memory.offset) != VK_SUCCESS)
    {
        b->Release();
        return E_OUTOFMEMORY;
    }
    if (device->caps.rayQuery)
    {
        VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addr.buffer = b->buffer;
        b->address = g_vk.vkGetBufferDeviceAddress(device->vk, &addr);
    }
    b->gpuWritten = (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    if (data && data->pSysMem)
    {
        if (staging)
            std::memcpy(b->memory.cpu, data->pSysMem, desc.ByteWidth);
        else
            device->init.UploadBuffer(b->buffer, 0, data->pSysMem, desc.ByteWidth);
    }
    else if (!staging && !(desc.MiscFlags & kMiscAccelStorage))
    {
        // D3D11 buffers start zeroed; UAV counters and accumulation buffers rely on it (a structure's storage is
        // written by its build).
        device->init.ZeroBuffer(b->buffer);
    }
    *out = b;
    return S_OK;
}

void Buffer::Destroy()
{
    RetireDynamicVersion(this);
    ReleaseResourceStorage(m_device, *this);
    delete this;
}

void Buffer::OnDebugName(const char* name)
{
    if (buffer)
        m_device->Name(VK_OBJECT_TYPE_BUFFER, u64(buffer), name);
}

HRESULT InitTexture(Device* device, Resource& r, D3D11_RESOURCE_DIMENSION dim, u32 width, u32 height, u32 depth,
    u32 mips, u32 arraySize, DXGI_FORMAT format, u32 samples, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc,
    const D3D11_SUBRESOURCE_DATA* data)
{
    if (!width || !height || !depth || !arraySize)
        return E_INVALIDARG;
    if (usage == D3D11_USAGE_IMMUTABLE && (!data || !data->pSysMem))
        return E_INVALIDARG;
    const FormatInfo& fmt = Format(format);
    if (fmt.format == VK_FORMAT_UNDEFINED)
    {
        static constexpr char kFormat[] = "! texture format %u has no Vulkan equivalent: refused";
        LogOnce(&kFormat, kFormat, unsigned(format));
        return E_INVALIDARG;
    }
    SetResourceCommon(device, r, dim, usage, bind, cpu, misc);
    r.format = format;
    r.width = width;
    r.height = height;
    r.depth = depth;
    r.arraySize = arraySize;
    r.samples = (std::max)(1u, samples);
    r.mipLevels = mips ? mips : FullMipCount(width, dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1 : height,
        dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? depth : 1);
    if (usage == D3D11_USAGE_STAGING)
    {
        // CPU-side texture: linear memory only, copies go through it.
        const HRESULT hr = CreateLinear(device, r,
            (cpu & D3D11_CPU_ACCESS_READ) ? MemoryClass::Readback : MemoryClass::Staging);
        if (FAILED(hr))
            return hr;
        if (data)
        {
            for (u32 s = 0; s < r.SubresourceCount(); ++s)
            {
                if (!data[s].pSysMem)
                    continue;
                const Resource::Footprint& f = r.footprints[s];
                const u8* src = static_cast<const u8*>(data[s].pSysMem);
                const u32 rowBytes = (std::min)(f.rowPitch, data[s].SysMemPitch ? data[s].SysMemPitch : f.rowPitch);
                for (u32 z = 0; z < f.depth; ++z)
                {
                    for (u32 y = 0; y < f.rows; ++y)
                        std::memcpy(r.linearMemory.cpu + f.offset + VkDeviceSize(z) * f.depthPitch + VkDeviceSize(y) * f.rowPitch,
                            src + VkDeviceSize(z) * data[s].SysMemSlicePitch + VkDeviceSize(y) * data[s].SysMemPitch, rowBytes);
                }
            }
        }
        return S_OK;
    }

    VkImageCreateInfo& info = r.imageInfo;
    info = VkImageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? VK_IMAGE_TYPE_1D :
        dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = ImageFormat(format, bind);
    info.extent = {width, dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? 1u : height,
        dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? depth : 1u};
    info.mipLevels = r.mipLevels;
    info.arrayLayers = dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1u : arraySize;
    info.samples = VkSampleCountFlagBits(r.samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r.aspect = AspectOf(info.format);
    const bool depthImage = (r.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    if ((misc & D3D11_RESOURCE_MISC_TEXTURECUBE) && arraySize >= 6)
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D && (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS)))
        info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // Typeless colour textures take views of other formats of their class.
    if (IsTypeless(format) && !depthImage)
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((bind & D3D11_BIND_SHADER_RESOURCE) || !depthImage)
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (bind & D3D11_BIND_RENDER_TARGET)
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (bind & D3D11_BIND_DEPTH_STENCIL)
        info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
    {
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        // sRGB formats cannot be storage images: their UNORM twin is (the UAV views it so).
        if (fmt.srgb)
            info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }
    // The engine's shading rate image (R8_UINT, one texel per tile) is used as the rate attachment.
    if (device->caps.shadingRate && format == DXGI_FORMAT_R8_UINT && (bind & D3D11_BIND_UNORDERED_ACCESS) &&
        dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r.samples == 1)
        info.usage |= VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    if (r.samples > 1)
        info.usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);

    const bool initialData = data && data->pSysMem;
    u32 families[3];
    const bool async = (bind & D3D11_BIND_UNORDERED_ACCESS) != 0;
    const bool viaTransfer = initialData && !depthImage;
    const u32 familyCount = SharingFamilies(device, async, viaTransfer, families);
    if (familyCount > 1)
    {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = familyCount;
        info.pQueueFamilyIndices = families;
    }
    VkResult result = g_vk.vkCreateImage(device->vk, &info, nullptr, &r.image);
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkCreateImage failed: %s (format %u, %ux%ux%u, %u mips, %u layers, usage 0x%X)";
        LogOnce(&kFormat, kFormat, ResultText(result), unsigned(info.format), width, height, depth, info.mipLevels,
            info.arrayLayers, info.usage);
        return E_OUTOFMEMORY;
    }
    VkMemoryDedicatedRequirements dedicatedReq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicatedReq};
    VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    reqInfo.image = r.image;
    g_vk.vkGetImageMemoryRequirements2(device->vk, &reqInfo, &req2);
    const bool attachment = (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
    // Render targets get their own allocation where the driver wants it (better compression on NVIDIA).
    const bool dedicated = dedicatedReq.requiresDedicatedAllocation ||
        (dedicatedReq.prefersDedicatedAllocation && attachment) || req2.memoryRequirements.size >= kDedicatedThreshold;
    if (!device->memory.Allocate(req2.memoryRequirements, MemoryClass::Device, dedicated, r.image, VK_NULL_HANDLE, r.memory) ||
        g_vk.vkBindImageMemory(device->vk, r.image, r.memory.memory, r.memory.offset) != VK_SUCCESS)
        return E_OUTOFMEMORY;
    // -vkstats: the targets with what may cost them compression (mutable formats, storage, concurrent sharing).
    if (device->stats && attachment)
    {
        Log("* target %ux%u format %u (vk %u) mips %u layers %u: usage 0x%X%s%s%s%s", width, height, unsigned(format),
            unsigned(info.format), info.mipLevels, info.arrayLayers, info.usage,
            (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ? ", mutable" : "",
            (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) ? ", storage" : "",
            info.sharingMode == VK_SHARING_MODE_CONCURRENT ? ", concurrent" : "", dedicated ? ", dedicated" : "");
    }

    // Initial layout and contents.
    VkImageLayout layout;
    if (bind & D3D11_BIND_UNORDERED_ACCESS)
        layout = VK_IMAGE_LAYOUT_GENERAL;
    else if (attachment)
        layout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    else
        layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    if (initialData && !attachment)
        layout = (bind & D3D11_BIND_UNORDERED_ACCESS) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    r.states.assign(1, ImageState{layout});
    r.uniform = true;
    if (initialData)
        (viaTransfer ? device->init : device->GraphicsInit()).UploadTexture(r, 0, r.SubresourceCount(), data, layout);
    else
        device->GraphicsInit().InitialLayout(r, layout, true);

    if (usage == D3D11_USAGE_DYNAMIC)
    {
        // Map(DISCARD) writes go to frame memory and are copied in at Unmap; the footprints give the pitches.
        const FormatInfo& f = Format(format);
        const u32 blockBytes = f.blockBytes ? f.blockBytes : 4;
        r.footprints.resize(r.SubresourceCount());
        for (u32 layer = 0; layer < r.arraySize; ++layer)
        {
            for (u32 mip = 0; mip < r.mipLevels; ++mip)
            {
                Resource::Footprint& fp = r.footprints[r.Subresource(mip, layer)];
                fp.width = (std::max)(1u, width >> mip);
                fp.height = (std::max)(1u, height >> mip);
                fp.depth = DepthAtMip(r, mip);
                const u32 blocksX = (fp.width + f.blockWidth - 1) / f.blockWidth;
                fp.rows = (fp.height + f.blockWidth - 1) / f.blockWidth;
                fp.rowPitch = u32(AlignUp(VkDeviceSize(blocksX) * blockBytes, blockBytes % 16 == 0 ? blockBytes : 16));
                fp.depthPitch = fp.rowPitch * fp.rows;
            }
        }
    }
    return S_OK;
}

HRESULT Texture1D::Create(Device* device, const D3D11_TEXTURE1D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture1D** out)
{
    *out = nullptr;
    Texture1D* t = new (std::nothrow) Texture1D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    const HRESULT hr = InitTexture(device, *t, D3D11_RESOURCE_DIMENSION_TEXTURE1D, desc.Width, 1, 1, desc.MipLevels,
        desc.ArraySize, desc.Format, 1, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags, data);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    t->m_desc.MipLevels = t->mipLevels;
    *out = t;
    return S_OK;
}

void Texture1D::Destroy()
{
    ReleaseResourceStorage(m_device, *this);
    delete this;
}

void Texture1D::OnDebugName(const char* name)
{
    if (image)
        m_device->Name(VK_OBJECT_TYPE_IMAGE, u64(image), name);
}

HRESULT Texture2D::Create(Device* device, const D3D11_TEXTURE2D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture2D** out)
{
    *out = nullptr;
    Texture2D* t = new (std::nothrow) Texture2D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    const HRESULT hr = InitTexture(device, *t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc.Width, desc.Height, 1,
        desc.MipLevels, desc.ArraySize, desc.Format, desc.SampleDesc.Count, desc.Usage, desc.BindFlags,
        desc.CPUAccessFlags, desc.MiscFlags, data);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    t->m_desc.MipLevels = t->mipLevels;
    device->TextureWritten();
    *out = t;
    return S_OK;
}

Texture2D* Texture2D::CreateForSwapChain(Device* device, VkImage image, const D3D11_TEXTURE2D_DESC& desc)
{
    Texture2D* t = new (std::nothrow) Texture2D(device);
    if (!t)
        return nullptr;
    t->m_desc = desc;
    SetResourceCommon(device, *t, D3D11_RESOURCE_DIMENSION_TEXTURE2D, D3D11_USAGE_DEFAULT, desc.BindFlags, 0, 0);
    t->format = desc.Format;
    t->width = desc.Width;
    t->height = desc.Height;
    t->depth = 1;
    t->mipLevels = 1;
    t->arraySize = 1;
    t->samples = 1;
    t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    t->imageInfo = VkImageCreateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    t->imageInfo.imageType = VK_IMAGE_TYPE_2D;
    t->imageInfo.format = Format(desc.Format).format;
    t->imageInfo.extent = {desc.Width, desc.Height, 1};
    t->imageInfo.mipLevels = 1;
    t->imageInfo.arrayLayers = 1;
    t->imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    t->states.assign(1, ImageState{});
    // One of the swap chain's images: owned by the swap chain, presented in PRESENT_SRC.
    t->image = image;
    t->m_swapChainImage = true;
    return t;
}

void Texture2D::Destroy()
{
    if (m_swapChainImage)
        image = VK_NULL_HANDLE;
    else
        m_device->TextureWritten();
    ReleaseResourceStorage(m_device, *this);
    delete this;
}

void Texture2D::OnDebugName(const char* name)
{
    if (image && !m_swapChainImage)
        m_device->Name(VK_OBJECT_TYPE_IMAGE, u64(image), name);
}

HRESULT Texture3D::Create(Device* device, const D3D11_TEXTURE3D_DESC& desc, const D3D11_SUBRESOURCE_DATA* data, Texture3D** out)
{
    *out = nullptr;
    Texture3D* t = new (std::nothrow) Texture3D(device);
    if (!t)
        return E_OUTOFMEMORY;
    t->m_desc = desc;
    const HRESULT hr = InitTexture(device, *t, D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc.Width, desc.Height, desc.Depth,
        desc.MipLevels, 1, desc.Format, 1, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags, data);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    t->m_desc.MipLevels = t->mipLevels;
    *out = t;
    return S_OK;
}

void Texture3D::Destroy()
{
    ReleaseResourceStorage(m_device, *this);
    delete this;
}

void Texture3D::OnDebugName(const char* name)
{
    if (image)
        m_device->Name(VK_OBJECT_TYPE_IMAGE, u64(image), name);
}

// ============================================================================================
// Views
// ============================================================================================

namespace
{
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

bool SliceRange(const Resource& r, UINT first, UINT count, SubRange& range)
{
    if (first >= r.arraySize)
        return false;
    if (count == UINT(-1))
        count = r.arraySize - first;
    if (!count || count > r.arraySize - first)
        return false;
    range.firstLayer = first;
    range.layerCount = count;
    return true;
}

bool WRange(const Resource& r, u32 mip, UINT first, UINT& size)
{
    const u32 depth = DepthAtMip(r, mip);
    if (first >= depth)
        return false;
    if (size == UINT(-1))
        size = depth - first;
    return size && size <= depth - first;
}

// The format and aspect a view of `r` uses for D3D11 view format `viewFormat`.
bool ViewFormatOf(const Resource& r, DXGI_FORMAT viewFormat, bool depthView, VkFormat& format, VkImageAspectFlags& aspect,
    VkComponentMapping& components)
{
    components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY};
    if (viewFormat == DXGI_FORMAT_UNKNOWN)
        viewFormat = r.format;
    if (r.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
    {
        // Depth images are viewed with their own format; the D3D11 format picks the plane.
        format = r.imageInfo.format;
        if (depthView)
        {
            aspect = r.aspect;
            return true;
        }
        switch (viewFormat)
        {
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
            // D3D reads stencil in .g, Vulkan returns it in .r.
            components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                VK_COMPONENT_SWIZZLE_ONE};
            return (r.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        default:
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            return true;
        }
    }
    if (TypelessOf(viewFormat) != TypelessOf(r.format) && viewFormat != r.format)
        return false;
    const FormatInfo& f = Format(viewFormat);
    if (f.format == VK_FORMAT_UNDEFINED || f.depth)
        return false;
    format = f.format;
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (viewFormat == DXGI_FORMAT_A8_UNORM)
        components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    else if (viewFormat == DXGI_FORMAT_B8G8R8X8_UNORM || viewFormat == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
        viewFormat == DXGI_FORMAT_B8G8R8X8_TYPELESS)
        components.a = VK_COMPONENT_SWIZZLE_ONE;
    return true;
}

HRESULT MakeImageView(Device* device, View& view, VkImage image, VkImageViewType type, VkFormat format,
    VkImageAspectFlags aspect, const VkComponentMapping& components, const SubRange& range, VkImageUsageFlags usage,
    VkImageView& out)
{
    VkImageViewCreateInfo& info = view.viewInfo;
    info = VkImageViewCreateInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = type;
    info.format = format;
    info.components = components;
    info.subresourceRange = {aspect, range.firstMip, range.mipCount, range.firstLayer, range.layerCount};
    VkImageViewUsageCreateInfo usageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (usage)
    {
        // A view of a mutable image only carries the usage its own format supports.
        usageInfo.usage = usage;
        info.pNext = &usageInfo;
    }
    const VkResult result = g_vk.vkCreateImageView(device->vk, &info, nullptr, &out);
    info.pNext = nullptr;
    if (result != VK_SUCCESS)
    {
        static constexpr char kFormat[] = "! vkCreateImageView failed: %s (type %u, format %u)";
        LogOnce(&kFormat, kFormat, ResultText(result), unsigned(type), unsigned(format));
        return E_INVALIDARG;
    }
    view.viewType = type;
    return S_OK;
}

void AttachResource(View& view, ViewKind kind, ID3D11Resource* iface, Resource* r, Device* device)
{
    view.kind = kind;
    view.resourceIface = iface;
    iface->AddRef();
    view.resource = r;
    view.id = device->nextObjectId.fetch_add(1, std::memory_order_relaxed);
}

HRESULT ViewResource(ID3D11Resource* iface, UINT bind, const char* kind, Resource*& out)
{
    out = nullptr;
    Resource* r = Resource::From(iface);
    if (!r)
        return E_INVALIDARG;
    if (r->usage == D3D11_USAGE_STAGING || !(r->bindFlags & bind))
        return E_INVALIDARG;
    if (r->IsBuffer() && r->Versioned())
    {
        LogOnce(kind, "! %s of a dynamic buffer refused (a DISCARD renames it; use a DEFAULT buffer)", kind);
        return E_INVALIDARG;
    }
    out = r;
    return S_OK;
}

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

// Element size of a buffer view: structured (the stride), raw (4 bytes, R32_TYPELESS) or typed.
bool BufferViewLayout(const Buffer& b, DXGI_FORMAT format, bool raw, u32& elementSize, bool& structuredOut)
{
    const D3D11_BUFFER_DESC& desc = b.Desc();
    const bool structured = (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
    structuredOut = raw || structured;
    if (raw)
    {
        if (structured || !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || format != DXGI_FORMAT_R32_TYPELESS)
            return false;
        elementSize = 4;
        return true;
    }
    if (structured)
    {
        if (format != DXGI_FORMAT_UNKNOWN)
            return false;
        elementSize = desc.StructureByteStride;
        return elementSize != 0;
    }
    elementSize = BufferElementSize(format);
    return elementSize && !IsTypeless(format);
}

HRESULT MakeBufferView(Device* device, View& view, Buffer& b, DXGI_FORMAT format, bool raw, UINT first, UINT count,
    bool storage)
{
    u32 elementSize = 0;
    bool structured = false;
    if (!BufferViewLayout(b, format, raw, elementSize, structured) || !count ||
        (u64(first) + count) * elementSize > b.byteWidth)
        return E_INVALIDARG;
    view.format = format;
    view.structured = structured;
    view.bufferOffset = VkDeviceSize(first) * elementSize;
    view.bufferRange = VkDeviceSize(count) * elementSize;
    if (structured)
    {
        if (view.bufferOffset % device->limits.minStorageBufferOffsetAlignment)
        {
            static constexpr char kFormat[] = "! a structured buffer view starts off the storage alignment: refused";
            LogOnce(kFormat, kFormat);
            return E_INVALIDARG;
        }
        return S_OK;
    }
    if (view.bufferOffset % device->limits.minTexelBufferOffsetAlignment)
    {
        static constexpr char kFormat[] = "! a typed buffer view starts off the texel alignment: refused";
        LogOnce(kFormat, kFormat);
        return E_INVALIDARG;
    }
    VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    info.buffer = b.buffer;
    info.format = Format(format).format;
    info.offset = view.bufferOffset;
    info.range = view.bufferRange;
    VkFormatProperties props{};
    g_vk.vkGetPhysicalDeviceFormatProperties(device->physical, info.format, &props);
    const VkFormatFeatureFlags needed = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    if (!(props.bufferFeatures & needed))
        return E_INVALIDARG;
    return g_vk.vkCreateBufferView(device->vk, &info, nullptr, &view.texelView) == VK_SUCCESS ? S_OK : E_OUTOFMEMORY;
}
} // namespace

VkImageView View::ImageViewFor(VkImageViewType declared)
{
    if (declared == viewType || !imageView)
        return imageView;
    // Only these pairs view the same subresources.
    const bool compatible = (declared == VK_IMAGE_VIEW_TYPE_2D_ARRAY && viewType == VK_IMAGE_VIEW_TYPE_2D) ||
        (declared == VK_IMAGE_VIEW_TYPE_2D && viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY && range.layerCount == 1) ||
        (declared == VK_IMAGE_VIEW_TYPE_1D_ARRAY && viewType == VK_IMAGE_VIEW_TYPE_1D) ||
        (declared == VK_IMAGE_VIEW_TYPE_1D && viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY && range.layerCount == 1) ||
        (declared == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (viewType == VK_IMAGE_VIEW_TYPE_CUBE || viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) ||
        (declared == VK_IMAGE_VIEW_TYPE_CUBE && viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY && range.layerCount == 6 &&
            (resource->imageInfo.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT));
    if (!compatible)
        return VK_NULL_HANDLE;
    std::lock_guard lock(adaptLock);
    for (const Adapted& a : adapted)
    {
        if (a.type == declared)
            return a.view;
    }
    VkImageViewCreateInfo info = viewInfo;
    info.pNext = nullptr;
    info.viewType = declared;
    VkImageView created = VK_NULL_HANDLE;
    if (g_vk.vkCreateImageView(resource ? Owner()->vk : VK_NULL_HANDLE, &info, nullptr, &created) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    try
    {
        adapted.push_back({declared, created});
    }
    catch (...)
    {
        g_vk.vkDestroyImageView(Owner()->vk, created, nullptr);
        return VK_NULL_HANDLE;
    }
    return created;
}

View::~View() = default;

void View::ReleaseViews(Device* device)
{
    std::vector<VkImageView> views;
    if (imageView)
        views.push_back(imageView);
    for (const Adapted& a : adapted)
        views.push_back(a.view);
    const VkBufferView texel = texelView;
    const VkBuffer counterBuffer = counter;
    const Allocation counterAlloc = counterMemory;
    imageView = VK_NULL_HANDLE;
    adapted.clear();
    texelView = VK_NULL_HANDLE;
    counter = VK_NULL_HANDLE;
    counterMemory = {};
    if (!views.empty() || texel || counterBuffer)
    {
        DeferAction(device, [device, views = std::move(views), texel, counterBuffer, counterAlloc] {
            for (VkImageView v : views)
                g_vk.vkDestroyImageView(device->vk, v, nullptr);
            if (texel)
                g_vk.vkDestroyBufferView(device->vk, texel, nullptr);
            if (counterBuffer)
                g_vk.vkDestroyBuffer(device->vk, counterBuffer, nullptr);
            device->memory.Free(counterAlloc);
        });
    }
    if (resourceIface)
    {
        resourceIface->Release();
        resourceIface = nullptr;
    }
}

Device* View::Owner() const
{
    return owner;
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

HRESULT ShaderResourceView::Create(Device* device, ID3D11Resource* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
    ShaderResourceView** out)
{
    *out = nullptr;
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_SHADER_RESOURCE, "SRV", r);
    if (FAILED(hr))
        return hr;
    ShaderResourceView* v = new (std::nothrow) ShaderResourceView(device);
    if (!v)
        return E_OUTOFMEMORY;
    v->owner = device;
    AttachResource(*v, ViewKind::SRV, resource, r, device);

    if (r->IsBuffer())
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        Buffer& b = *static_cast<Buffer*>(r);
        if (desc)
            d = *desc;
        else
        {
            const D3D11_BUFFER_DESC& bd = b.Desc();
            if (!(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !bd.StructureByteStride)
            {
                v->Release();
                return E_INVALIDARG;
            }
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
            first = d.BufferEx.FirstElement;
            count = d.BufferEx.NumElements;
            raw = (d.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
        }
        else
        {
            v->Release();
            return E_INVALIDARG;
        }
        hr = MakeBufferView(device, *v, b, d.Format, raw, first, count, false);
        if (FAILED(hr))
        {
            v->Release();
            return hr;
        }
        v->m_desc = d;
        *out = v;
        return S_OK;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    if (desc)
        d = *desc;
    else
    {
        d.Format = r->format;
        switch (r->dimension)
        {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            d.ViewDimension = r->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY : D3D11_SRV_DIMENSION_TEXTURE1D;
            d.Texture1DArray.MipLevels = UINT(-1);
            d.Texture1DArray.ArraySize = r->arraySize;
            break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
            if ((r->miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) && r->arraySize >= 6 && r->samples == 1)
            {
                d.ViewDimension = r->arraySize > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
                d.TextureCubeArray.MipLevels = UINT(-1);
                d.TextureCubeArray.NumCubes = r->arraySize / 6;
            }
            else if (r->samples > 1)
            {
                d.ViewDimension = r->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                d.Texture2DMSArray.ArraySize = r->arraySize;
            }
            else
            {
                d.ViewDimension = r->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
                d.Texture2DArray.MipLevels = UINT(-1);
                d.Texture2DArray.ArraySize = r->arraySize;
            }
            break;
        default:
            d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D.MipLevels = UINT(-1);
            break;
        }
    }

    const bool is1D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples == 1;
    const bool is2DMS = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples > 1;
    const bool is3D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    SubRange range;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    bool ok = true;
    switch (d.ViewDimension)
    {
    case D3D11_SRV_DIMENSION_TEXTURE1D:
        ok = is1D && MipRange(*r, d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels, range);
        type = VK_IMAGE_VIEW_TYPE_1D;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        ok = is1D && MipRange(*r, d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels, range) &&
            SliceRange(*r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        ok = is2D && MipRange(*r, d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels, range);
        type = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        ok = is2D && MipRange(*r, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels, range) &&
            SliceRange(*r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        ok = is2DMS;
        type = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        ok = is2DMS && SliceRange(*r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
        ok = is3D && MipRange(*r, d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels, range);
        type = VK_IMAGE_VIEW_TYPE_3D;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        ok = is2D && r->arraySize >= 6 && (r->imageInfo.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
            MipRange(*r, d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels, range);
        range.firstLayer = 0;
        range.layerCount = 6;
        type = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
    {
        const UINT first = d.TextureCubeArray.First2DArrayFace;
        UINT cubes = d.TextureCubeArray.NumCubes;
        if (cubes == UINT(-1) && first < r->arraySize)
            cubes = (r->arraySize - first) / 6;
        ok = is2D && cubes && cubes <= r->arraySize / 6 && device->cubeArray &&
            (r->imageInfo.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
            MipRange(*r, d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels, range) &&
            SliceRange(*r, first, cubes * 6, range);
        type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        break;
    }
    default:
        ok = false;
        break;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkComponentMapping components{};
    if (!ok || !ViewFormatOf(*r, d.Format, false, format, aspect, components))
    {
        v->Release();
        return E_INVALIDARG;
    }
    v->range = range;
    v->aspect = aspect;
    v->format = d.Format;
    hr = MakeImageView(device, *v, r->image, type, format, aspect, components, range,
        (r->imageInfo.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0, v->imageView);
    if (FAILED(hr))
    {
        v->Release();
        return hr;
    }
    v->m_desc = d;
    *out = v;
    return S_OK;
}

void STDMETHODCALLTYPE ShaderResourceView::GetResource(ID3D11Resource** out)
{
    *out = resourceIface;
    if (resourceIface)
        resourceIface->AddRef();
}

void ShaderResourceView::Destroy()
{
    View::ReleaseViews(m_device);
    delete this;
}

HRESULT RenderTargetView::Create(Device* device, ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc,
    RenderTargetView** out)
{
    *out = nullptr;
    Resource* r = Resource::From(resource);
    if (!r || r->IsBuffer() || !(r->bindFlags & D3D11_BIND_RENDER_TARGET))
        return E_INVALIDARG;
    RenderTargetView* v = new (std::nothrow) RenderTargetView(device);
    if (!v)
        return E_OUTOFMEMORY;
    v->owner = device;
    AttachResource(*v, ViewKind::RTV, resource, r, device);

    D3D11_RENDER_TARGET_VIEW_DESC d{};
    if (desc)
        d = *desc;
    else
    {
        d.Format = r->format;
        if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        {
            d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            d.Texture3D.WSize = r->depth;
        }
        else if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY : D3D11_RTV_DIMENSION_TEXTURE1D;
            d.Texture1DArray.ArraySize = r->arraySize;
        }
        else if (r->samples > 1)
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
            d.Texture2DMSArray.ArraySize = r->arraySize;
        }
        else
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
            d.Texture2DArray.ArraySize = r->arraySize;
        }
    }
    const bool is1D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples == 1;
    const bool is2DMS = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples > 1;
    const bool is3D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    SubRange range;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    bool ok = true;
    switch (d.ViewDimension)
    {
    case D3D11_RTV_DIMENSION_TEXTURE1D:
        ok = is1D && MipRange(*r, d.Texture1D.MipSlice, 1, range);
        type = VK_IMAGE_VIEW_TYPE_1D;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
        ok = is1D && MipRange(*r, d.Texture1DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2D:
        ok = is2D && MipRange(*r, d.Texture2D.MipSlice, 1, range);
        type = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        ok = is2D && MipRange(*r, d.Texture2DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS:
        ok = is2DMS;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
        ok = is2DMS && SliceRange(*r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE3D:
    {
        UINT size = d.Texture3D.WSize;
        ok = is3D && MipRange(*r, d.Texture3D.MipSlice, 1, range) && WRange(*r, range.firstMip, d.Texture3D.FirstWSlice, size);
        // Slices of a 3D image are the layers of a 2D-array view (2D_ARRAY_COMPATIBLE).
        range.firstLayer = d.Texture3D.FirstWSlice;
        range.layerCount = size;
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    }
    default:
        ok = false;
        break;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkComponentMapping components{};
    if (!ok || !ViewFormatOf(*r, d.Format, false, format, aspect, components) || aspect != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        v->Release();
        return E_INVALIDARG;
    }
    // Attachments take no swizzle.
    components = {};
    v->range = range;
    v->aspect = aspect;
    v->format = d.Format;
    v->m_desc = d;
    const HRESULT hr = MakeImageView(device, *v, r->image, type, format, aspect, components, range,
        (r->imageInfo.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0, v->imageView);
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
    *out = resourceIface;
    if (resourceIface)
        resourceIface->AddRef();
}

void RenderTargetView::Destroy()
{
    View::ReleaseViews(m_device);
    delete this;
}

HRESULT DepthStencilView::Create(Device* device, ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
    DepthStencilView** out)
{
    *out = nullptr;
    Resource* r = Resource::From(resource);
    if (!r || r->IsBuffer() || !(r->bindFlags & D3D11_BIND_DEPTH_STENCIL) || r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        return E_INVALIDARG;
    DepthStencilView* v = new (std::nothrow) DepthStencilView(device);
    if (!v)
        return E_OUTOFMEMORY;
    v->owner = device;
    AttachResource(*v, ViewKind::DSV, resource, r, device);
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    if (desc)
        d = *desc;
    else
    {
        d.Format = r->format;
        if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE1DARRAY : D3D11_DSV_DIMENSION_TEXTURE1D;
            d.Texture1DArray.ArraySize = r->arraySize;
        }
        else if (r->samples > 1)
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
            d.Texture2DMSArray.ArraySize = r->arraySize;
        }
        else
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
            d.Texture2DArray.ArraySize = r->arraySize;
        }
    }
    if (d.Flags & ~UINT(D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL))
    {
        v->Release();
        return E_INVALIDARG;
    }
    const bool is1D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples == 1;
    const bool is2DMS = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples > 1;
    SubRange range;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    bool ok = true;
    switch (d.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE1D:
        ok = is1D && MipRange(*r, d.Texture1D.MipSlice, 1, range);
        type = VK_IMAGE_VIEW_TYPE_1D;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
        ok = is1D && MipRange(*r, d.Texture1DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2D:
        ok = is2D && MipRange(*r, d.Texture2D.MipSlice, 1, range);
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        ok = is2D && MipRange(*r, d.Texture2DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        ok = is2DMS;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        ok = is2DMS && SliceRange(*r, d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    default:
        ok = false;
        break;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkComponentMapping components{};
    if (!ok || !(r->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || !ViewFormatOf(*r, d.Format, true, format, aspect, components))
    {
        v->Release();
        return E_INVALIDARG;
    }
    v->range = range;
    v->aspect = aspect;
    v->format = d.Format;
    v->dsvFlags = d.Flags;
    v->m_desc = d;
    const HRESULT hr = MakeImageView(device, *v, r->image, type, format, aspect, {}, range, 0, v->imageView);
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
    *out = resourceIface;
    if (resourceIface)
        resourceIface->AddRef();
}

void DepthStencilView::Destroy()
{
    View::ReleaseViews(m_device);
    delete this;
}

HRESULT UnorderedAccessView::Create(Device* device, ID3D11Resource* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,
    UnorderedAccessView** out)
{
    *out = nullptr;
    Resource* r = nullptr;
    HRESULT hr = ViewResource(resource, D3D11_BIND_UNORDERED_ACCESS, "UAV", r);
    if (FAILED(hr))
        return hr;
    UnorderedAccessView* v = new (std::nothrow) UnorderedAccessView(device);
    if (!v)
        return E_OUTOFMEMORY;
    v->owner = device;
    AttachResource(*v, ViewKind::UAV, resource, r, device);

    if (r->IsBuffer())
    {
        Buffer& b = *static_cast<Buffer*>(r);
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        if (desc)
            d = *desc;
        else
        {
            const D3D11_BUFFER_DESC& bd = b.Desc();
            if (!(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !bd.StructureByteStride)
            {
                v->Release();
                return E_INVALIDARG;
            }
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            d.Buffer.NumElements = bd.ByteWidth / bd.StructureByteStride;
        }
        if (d.ViewDimension != D3D11_UAV_DIMENSION_BUFFER)
        {
            v->Release();
            return E_INVALIDARG;
        }
        if (d.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))
        {
            static constexpr char kFormat[] = "! append/counter buffer UAVs are not supported; view refused";
            LogOnce(kFormat, kFormat);
            v->Release();
            return E_NOTIMPL;
        }
        hr = MakeBufferView(device, *v, b, d.Format, (d.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) != 0,
            d.Buffer.FirstElement, d.Buffer.NumElements, true);
        if (FAILED(hr))
        {
            v->Release();
            return hr;
        }
        v->m_desc = d;
        v->uavFlags = d.Buffer.Flags;
        *out = v;
        return S_OK;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    if (desc)
        d = *desc;
    else
    {
        d.Format = r->format;
        if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
        {
            d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            d.Texture3D.WSize = r->depth;
        }
        else if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY : D3D11_UAV_DIMENSION_TEXTURE1D;
            d.Texture1DArray.ArraySize = r->arraySize;
        }
        else
        {
            d.ViewDimension = r->arraySize > 1 ? D3D11_UAV_DIMENSION_TEXTURE2DARRAY : D3D11_UAV_DIMENSION_TEXTURE2D;
            d.Texture2DArray.ArraySize = r->arraySize;
        }
    }
    const bool is1D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    const bool is2D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && r->samples == 1;
    const bool is3D = r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    SubRange range;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    bool ok = true;
    switch (d.ViewDimension)
    {
    case D3D11_UAV_DIMENSION_TEXTURE1D:
        ok = is1D && MipRange(*r, d.Texture1D.MipSlice, 1, range);
        type = VK_IMAGE_VIEW_TYPE_1D;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        ok = is1D && MipRange(*r, d.Texture1DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
        ok = is2D && MipRange(*r, d.Texture2D.MipSlice, 1, range);
        break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        ok = is2D && MipRange(*r, d.Texture2DArray.MipSlice, 1, range) &&
            SliceRange(*r, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, range);
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
    {
        UINT size = d.Texture3D.WSize;
        ok = is3D && MipRange(*r, d.Texture3D.MipSlice, 1, range) && WRange(*r, range.firstMip, d.Texture3D.FirstWSlice, size);
        // A UAV of a whole 3D mip is the 3D view; a W range of it reads through the whole mip.
        type = VK_IMAGE_VIEW_TYPE_3D;
        break;
    }
    default:
        ok = false;
        break;
    }
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkComponentMapping components{};
    if (!ok || !ViewFormatOf(*r, d.Format, false, format, aspect, components))
    {
        v->Release();
        return E_INVALIDARG;
    }
    // Storage views of sRGB data use the UNORM twin (sRGB is never a storage format).
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_SRGB: format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case VK_FORMAT_B8G8R8A8_SRGB: format = VK_FORMAT_B8G8R8A8_UNORM; break;
    default: break;
    }
    v->range = range;
    v->aspect = aspect;
    v->format = d.Format;
    v->m_desc = d;
    hr = MakeImageView(device, *v, r->image, type, format, aspect, {}, range,
        (r->imageInfo.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) ? VK_IMAGE_USAGE_STORAGE_BIT : 0, v->imageView);
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
    *out = resourceIface;
    if (resourceIface)
        resourceIface->AddRef();
}

void UnorderedAccessView::Destroy()
{
    View::ReleaseViews(m_device);
    delete this;
}

// ============================================================================================
// Shaders
// ============================================================================================

Shader::~Shader() = default;

void Shader::ReleaseObjects(Device* device)
{
    const VkShaderModule m = module;
    const VkPipeline p = computePipeline;
    module = VK_NULL_HANDLE;
    computePipeline = VK_NULL_HANDLE;
    // Graphics pipelines built from this shader stay in the pipeline cache until the device goes (other
    // shader objects with the same code reuse them); the module itself is only needed at creation.
    device->pipelines.Forget(this);
    if (m || p)
    {
        device->releaser.Defer([device, m, p] {
            if (m)
                g_vk.vkDestroyShaderModule(device->vk, m, nullptr);
            if (p)
                g_vk.vkDestroyPipeline(device->vk, p, nullptr);
        });
    }
}

template <class Iface, Stage S>
void ShaderObject<Iface, S>::Destroy()
{
    ReleaseObjects(DeviceChild<Iface>::m_device);
    delete this;
}

template class ShaderObject<ID3D11VertexShader, Stage::VS>;
template class ShaderObject<ID3D11HullShader, Stage::HS>;
template class ShaderObject<ID3D11DomainShader, Stage::DS>;
template class ShaderObject<ID3D11GeometryShader, Stage::GS>;
template class ShaderObject<ID3D11PixelShader, Stage::PS>;
template class ShaderObject<ID3D11ComputeShader, Stage::CS>;

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

VkShaderStageFlagBits VkStageOf(Stage stage, bool mesh)
{
    switch (stage)
    {
    case Stage::VS: return mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT;
    case Stage::PS: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case Stage::GS: return mesh ? VK_SHADER_STAGE_TASK_BIT_EXT : VK_SHADER_STAGE_GEOMETRY_BIT;
    case Stage::HS: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case Stage::DS: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}

// ============================================================================================
// Queries
// ============================================================================================

HRESULT Query::Create(Device* device, const D3D11_QUERY_DESC& desc, Query** out)
{
    *out = nullptr;
    Query* q = new (std::nothrow) Query(device);
    if (!q)
        return E_OUTOFMEMORY;
    q->m_desc = desc;
    switch (desc.Query)
    {
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    case D3D11_QUERY_TIMESTAMP:
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
        break;
    default:
        LogOnce("Query.type", "query type %u is not implemented, its data reads as zeros", unsigned(desc.Query));
        break;
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

void Query::FreeSlots()
{
    Device* device = m_device;
    const VkQueryType type = m_desc.Query == D3D11_QUERY_TIMESTAMP ? VK_QUERY_TYPE_TIMESTAMP : VK_QUERY_TYPE_OCCLUSION;
    std::vector<QueryPoolSet::Slot> slots = std::move(parts);
    parts.clear();
    if (slot.pool)
        slots.push_back(slot);
    slot = {};
    if (slots.empty())
        return;
    // A submitted End may still write them.
    device->releaser.Defer([device, type, slots = std::move(slots)] {
        for (const QueryPoolSet::Slot& s : slots)
            device->queryPools.Free(type, s);
    });
}

void Query::Destroy()
{
    FreeSlots();
    delete this;
}

// ============================================================================================
// Command lists
// ============================================================================================

void CommandList::Destroy()
{
    if (!buffers.empty())
    {
        // Never executed: the GPU has not seen them, the recording context reuses them.
        if (recordedBy)
            recordedBy->ReturnBuffers(buffers, 0);
        buffers.clear();
    }
    // Its staging copies never ran: the references taken when they were recorded go too.
    ReleaseStagingReferences(pendingReadback);
    for (Query* q : endedQueries)
        q->Release();
    endedQueries.clear();
    if (recordedBy)
        recordedBy->Release();
    delete this;
}

void ReleaseStagingReferences(std::vector<Resource*>& resources)
{
    for (Resource* r : resources)
    {
        if (!r)
            continue;
        r->gpuWritePending = false;
        // Staging resources of the list were AddRef'd through their interface when recorded.
        if (r->IsBuffer())
            static_cast<Buffer*>(r)->Release();
        else if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
            static_cast<Texture1D*>(r)->Release();
        else if (r->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            static_cast<Texture2D*>(r)->Release();
        else
            static_cast<Texture3D*>(r)->Release();
    }
    resources.clear();
}
} // namespace xrvk
