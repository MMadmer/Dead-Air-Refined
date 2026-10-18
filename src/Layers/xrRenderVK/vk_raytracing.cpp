// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The ray tracing part of xrVulkan (vk_api.h, "ray tracing"): the device's bindless set, the lane's buffers and
// upload memory by device address, acceleration structures with their builds, compacting copies and compacted sizes.
// The lane's passes are ordinary compute dispatches of the D3D11 path; what they read by address or through the
// bindless set the layer cannot track, so the lane orders it with AccelBarrier.
#include "vk_layer.h"

#include <algorithm>

namespace xrvk
{
namespace
{
Device* RtDevice(ID3D11Device* device)
{
    if (!IsLayerDevice(device))
        return nullptr;
    Device* d = Device::From(device);
    return d->caps.rayQuery && d->bindless.set && !d->lost ? d : nullptr;
}

// The immediate context of a ray tracing device, outside any rendering (builds and barriers are recorded at once).
Context* RtImmediate(ID3D11DeviceContext* context)
{
    if (!context)
        return nullptr;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    Device* d = RtDevice(device);
    if (device)
        device->Release();
    auto* c = d ? static_cast<Context*>(context) : nullptr;
    if (!c || c->IsDeferred() || !c->Cmd())
        return nullptr;
    c->EndRendering();
    c->FlushPendingClears(nullptr);
    c->FlushBarriers();
    return c;
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) / alignment * alignment; }

template <class F>
void Defer(Device* device, F&& action)
{
    // Acceleration structures and their buffers: a set may name them.
    device->DescriptorTargetGone();
    device->releaser.Defer(std::function<void()>(std::forward<F>(action)));
}

VkAccelerationStructureGeometryKHR TrianglesOf(const AccelTriangles& t)
{
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR& d = g.geometry.triangles;
    d.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    d.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    d.vertexData.deviceAddress = t.vertexAddress;
    d.vertexStride = t.vertexStride;
    d.maxVertex = t.vertexCount ? t.vertexCount - 1 : 0;
    d.indexType = VK_INDEX_TYPE_UINT16;
    d.indexData.deviceAddress = t.indexAddress;
    return g;
}

VkAccelerationStructureGeometryKHR InstancesOf(UINT64 address)
{
    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    g.geometry.instances.data.deviceAddress = address;
    return g;
}

// The geometry of a build or of a size query: meshes, or one instance array.
struct Geometry
{
    std::vector<VkAccelerationStructureGeometryKHR> geometry;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<u32> primitives;
    void Fill(const AccelTriangles* meshes, UINT count, UINT64 instances)
    {
        const u32 n = meshes ? count : 1;
        geometry.resize(n);
        ranges.assign(n, VkAccelerationStructureBuildRangeInfoKHR{});
        primitives.resize(n);
        for (u32 i = 0; i < n; ++i)
        {
            geometry[i] = meshes ? TrianglesOf(meshes[i]) : InstancesOf(instances);
            primitives[i] = meshes ? meshes[i].indexCount / 3 : count;
            ranges[i].primitiveCount = primitives[i];
        }
    }
};
} // namespace

class Accel
{
public:
    Device* device = nullptr;
    ID3D11Buffer* storage = nullptr; // AddRef'd
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    UINT64 size = 0;
    bool top = false;
};

class AccelSizeQuery
{
public:
    Device* device = nullptr;
    VkQueryPool pool = VK_NULL_HANDLE;
    UINT count = 0;
};

// ---- the bindless set ---------------------------------------------------------------------------------------------

bool Device::InitBindless()
{
    const u32 n = bindless.capacity;
    if (!n)
        return false;
    // 2D textures at 0, raw buffers at 1, one index space; slots are written while frames in flight read others.
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    constexpr VkDescriptorBindingFlags kFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags flags[2] = {kFlags, kFlags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 2;
    flagsInfo.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flagsInfo};
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (g_vk.vkCreateDescriptorSetLayout(vk, &layoutInfo, nullptr, &bindless.layout) != VK_SUCCESS)
    {
        bindless.layout = VK_NULL_HANDLE;
        return false;
    }
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    if (g_vk.vkCreateDescriptorPool(vk, &poolInfo, nullptr, &bindless.pool) != VK_SUCCESS)
    {
        bindless.pool = VK_NULL_HANDLE;
        ShutdownBindless();
        return false;
    }
    alloc.descriptorPool = bindless.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &bindless.layout;
    if (g_vk.vkAllocateDescriptorSets(vk, &alloc, &bindless.set) != VK_SUCCESS)
    {
        bindless.set = VK_NULL_HANDLE;
        ShutdownBindless();
        return false;
    }
    bindless.free = {{0u, n}};
    return true;
}

void Device::ShutdownBindless()
{
    if (bindless.pool)
        g_vk.vkDestroyDescriptorPool(vk, bindless.pool, nullptr);
    if (bindless.layout)
        g_vk.vkDestroyDescriptorSetLayout(vk, bindless.layout, nullptr);
    bindless.pool = VK_NULL_HANDLE;
    bindless.layout = VK_NULL_HANDLE;
    bindless.set = VK_NULL_HANDLE;
    bindless.free.clear();
}

UINT AllocateBindless(ID3D11Device* device, UINT count)
{
    Device* d = RtDevice(device);
    if (!d || !count)
        return kBindlessInvalid;
    std::lock_guard lock(d->bindless.lock);
    auto& free = d->bindless.free;
    for (size_t i = 0; i < free.size(); ++i)
    {
        auto& [first, size] = free[i];
        if (size < count)
            continue;
        const u32 index = first;
        first += count;
        size -= count;
        if (!size)
            free.erase(free.begin() + ptrdiff_t(i));
        return index;
    }
    return kBindlessInvalid;
}

void FreeBindless(ID3D11Device* device, UINT index, UINT count)
{
    Device* d = RtDevice(device);
    if (!d || index == kBindlessInvalid || !count)
        return;
    // Frames in flight may still read the slots.
    Defer(d, [d, index, count] {
        std::lock_guard lock(d->bindless.lock);
        auto& free = d->bindless.free;
        const auto at = std::lower_bound(free.begin(), free.end(), std::make_pair(index, 0u));
        auto it = free.insert(at, {index, count});
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
    });
}

void WriteBindless(ID3D11Device* device, UINT index, ID3D11ShaderResourceView* view)
{
    Device* d = RtDevice(device);
    if (!d || index >= d->bindless.capacity)
        return;
    VkDescriptorImageInfo image{VK_NULL_HANDLE, d->nulls.view2d, VK_IMAGE_LAYOUT_GENERAL};
    if (view)
    {
        View* v = View::From(view);
        Resource* r = v ? v->resource : nullptr;
        const VkImageView iv = r && r->IsImage() ? v->ImageViewFor(VK_IMAGE_VIEW_TYPE_2D) : VK_NULL_HANDLE;
        if (iv)
            image = {VK_NULL_HANDLE, iv, Context::ReadLayout(r)};
    }
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = d->bindless.set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &image;
    std::lock_guard lock(d->bindless.lock);
    g_vk.vkUpdateDescriptorSets(d->vk, 1, &w, 0, nullptr);
}

void WriteBindlessBuffer(ID3D11Device* device, UINT index, ID3D11Buffer* buffer)
{
    Device* d = RtDevice(device);
    if (!d || index >= d->bindless.capacity)
        return;
    VkDescriptorBufferInfo info{d->nulls.buffer, 0, VK_WHOLE_SIZE};
    // A dynamic buffer's bytes move with every version: it reads as zeros.
    if (Resource* r = buffer ? Resource::From(buffer) : nullptr; r && r->IsBuffer() && !r->Versioned() && r->buffer)
        info = {r->buffer, r->bufferOffset, AlignUp(r->byteWidth, 4)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = d->bindless.set;
    w.dstBinding = 1;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    std::lock_guard lock(d->bindless.lock);
    g_vk.vkUpdateDescriptorSets(d->vk, 1, &w, 0, nullptr);
}

// ---- push constants, buffers, upload memory, read states ------------------------------------------------------------

void SetComputePushConstants(ID3D11DeviceContext* context, const void* data, UINT size)
{
    if (!context || !data || size > kRtPushConstantBytes)
        return;
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    const bool ours = IsLayerDevice(device);
    if (device)
        device->Release();
    if (!ours)
        return;
    auto* c = static_cast<Context*>(context);
    std::memcpy(c->computePush, data, size);
    c->computePushSize = size;
    c->applied.computePushDirty = true;
}

HRESULT CreateRtBuffer(ID3D11Device* device, UINT64 size, const void* data, const char* name, ID3D11Buffer** out)
{
    *out = nullptr;
    Device* d = RtDevice(device);
    if (!d || !size || size > 0xFFFFFFF0ull)
        return E_INVALIDARG;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(AlignUp(size, 16));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.MiscFlags = kMiscRtBuffer;
    std::vector<u8> padded;
    D3D11_SUBRESOURCE_DATA initial{};
    if (data)
    {
        // The upload copies the whole (aligned) width.
        padded.assign(desc.ByteWidth, 0);
        std::memcpy(padded.data(), data, size_t(size));
        initial.pSysMem = padded.data();
    }
    const HRESULT hr = d->CreateBuffer(&desc, data ? &initial : nullptr, out);
    if (SUCCEEDED(hr) && name && d->debugUtils)
    {
        Resource* r = Resource::From(*out);
        d->Name(VK_OBJECT_TYPE_BUFFER, u64(r->buffer), name);
    }
    return hr;
}

UINT64 BufferAddress(ID3D11Buffer* buffer)
{
    if (!buffer)
        return 0;
    ID3D11Device* device = nullptr;
    buffer->GetDevice(&device);
    const bool ours = IsLayerDevice(device);
    if (device)
        device->Release();
    if (!ours)
        return 0;
    const Resource* r = Resource::From(buffer);
    return r->IsBuffer() && !r->Versioned() && r->address ? r->address + r->bufferOffset : 0;
}

UploadSpan AllocateUpload(ID3D11DeviceContext* context, UINT64 size, UINT64 alignment)
{
    UploadSpan out;
    Context* c = context ? static_cast<Context*>(context) : nullptr;
    ID3D11Device* device = nullptr;
    if (c)
        c->GetDevice(&device);
    Device* d = RtDevice(device);
    if (device)
        device->Release();
    if (!d || !size)
        return out;
    const UploadAlloc alloc = c->upload.Allocate(size, (std::max)(alignment, UINT64(16)), d->frame.load(std::memory_order_acquire));
    if (!alloc.cpu)
        return out;
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = alloc.buffer;
    out.cpu = alloc.cpu;
    out.address = g_vk.vkGetBufferDeviceAddress(d->vk, &info) + alloc.offset;
    return out;
}

void RequireRead(ID3D11DeviceContext* context, ID3D11Resource* const* resources, UINT count)
{
    Context* c = RtImmediate(context);
    if (!c || !resources)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        Resource* r = resources[i] ? Resource::From(resources[i]) : nullptr;
        if (!r)
            continue;
        if (r->IsImage())
            c->RequireImageAll(r, Context::ReadLayout(r), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        else if (r->gpuWritten)
            c->RequireBuffer(r, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    c->FlushBarriers();
}

UINT64 TextureWriteGeneration(ID3D11Device* device)
{
    return IsLayerDevice(device) ? Device::From(device)->textureWrites.load(std::memory_order_relaxed) : 0;
}

// ---- acceleration structures ------------------------------------------------------------------------------------

bool AccelBuildSizes(ID3D11Device* device, const AccelTriangles* geometry, UINT count, UINT flags, AccelSizes& out)
{
    out = {};
    Device* d = RtDevice(device);
    if (!d || (geometry && !count))
        return false;
    Geometry g;
    g.Fill(geometry, count, 0);
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = geometry ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VkBuildAccelerationStructureFlagsKHR(flags);
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = u32(g.geometry.size());
    info.pGeometries = g.geometry.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    g_vk.vkGetAccelerationStructureBuildSizesKHR(d->vk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
        g.primitives.data(), &sizes);
    out.result = sizes.accelerationStructureSize;
    out.build = sizes.buildScratchSize;
    out.update = sizes.updateScratchSize;
    return out.result != 0;
}

Accel* CreateAccel(ID3D11Device* device, bool top, UINT64 size, const char* name)
{
    Device* d = RtDevice(device);
    if (!d || !size || size > 0xFFFFFFF0ull)
        return nullptr;
    // A raw SRV over its bytes carries the structure to the passes (CreateAccelView).
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(AlignUp(size, 256));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | kMiscAccelStorage;
    ID3D11Buffer* storage = nullptr;
    if (FAILED(d->CreateBuffer(&desc, nullptr, &storage)))
        return nullptr;
    Resource* r = Resource::From(storage);
    VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = r->buffer;
    info.offset = r->bufferOffset;
    info.size = size;
    info.type = top ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    const VkResult result = g_vk.vkCreateAccelerationStructureKHR(d->vk, &info, nullptr, &handle);
    if (result != VK_SUCCESS)
    {
        Log("! vkCreateAccelerationStructureKHR (%llu KB) failed: %s", static_cast<unsigned long long>(size >> 10),
            ResultText(result));
        storage->Release();
        return nullptr;
    }
    auto* accel = new (std::nothrow) Accel();
    if (!accel)
    {
        g_vk.vkDestroyAccelerationStructureKHR(d->vk, handle, nullptr);
        storage->Release();
        return nullptr;
    }
    accel->device = d;
    accel->storage = storage;
    accel->handle = handle;
    accel->size = size;
    accel->top = top;
    VkAccelerationStructureDeviceAddressInfoKHR addr{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addr.accelerationStructure = handle;
    accel->address = g_vk.vkGetAccelerationStructureDeviceAddressKHR(d->vk, &addr);
    if (name && d->debugUtils)
        d->Name(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, u64(handle), name);
    return accel;
}

void ReleaseAccel(Accel* accel)
{
    if (!accel)
        return;
    Device* d = accel->device;
    const VkAccelerationStructureKHR handle = accel->handle;
    if (handle)
        Defer(d, [d, handle] { g_vk.vkDestroyAccelerationStructureKHR(d->vk, handle, nullptr); });
    // The buffer defers its own memory.
    if (accel->storage)
        accel->storage->Release();
    delete accel;
}

UINT64 AccelAddress(const Accel* accel) { return accel ? accel->address : 0; }

HRESULT CreateAccelView(Accel* accel, ID3D11ShaderResourceView** view)
{
    *view = nullptr;
    if (!accel || !accel->storage)
        return E_INVALIDARG;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    desc.BufferEx.NumElements = UINT(AlignUp(accel->size, 256) / 4);
    desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    const HRESULT hr = accel->device->CreateShaderResourceView(accel->storage, &desc, view);
    if (SUCCEEDED(hr))
        View::From(*view)->accel = accel->handle;
    return hr;
}

void BuildAccel(ID3D11DeviceContext* context, const AccelBuild& build)
{
    Context* c = RtImmediate(context);
    if (!c || !build.dst || !build.scratch || (build.update && !build.src))
        return;
    Geometry g;
    g.Fill(build.geometry, build.count, build.instances);
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = build.geometry ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VkBuildAccelerationStructureFlagsKHR(build.flags);
    info.mode = build.update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.srcAccelerationStructure = build.update ? build.src->handle : VK_NULL_HANDLE;
    info.dstAccelerationStructure = build.dst->handle;
    info.geometryCount = u32(g.geometry.size());
    info.pGeometries = g.geometry.data();
    info.scratchData.deviceAddress = build.scratch;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = g.ranges.data();
    g_vk.vkCmdBuildAccelerationStructuresKHR(c->Cmd(), 1, &info, &ranges);
}

void CopyAccelCompact(ID3D11DeviceContext* context, Accel* dst, Accel* src)
{
    Context* c = RtImmediate(context);
    if (!c || !dst || !src)
        return;
    VkCopyAccelerationStructureInfoKHR info{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
    info.src = src->handle;
    info.dst = dst->handle;
    info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    g_vk.vkCmdCopyAccelerationStructureKHR(c->Cmd(), &info);
}

void AccelBarrier(ID3D11DeviceContext* context)
{
    Context* c = RtImmediate(context);
    if (!c)
        return;
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
        VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    g_vk.vkCmdPipelineBarrier2(c->Cmd(), &dep);
}

AccelSizeQuery* CreateAccelSizeQuery(ID3D11Device* device, UINT count)
{
    Device* d = RtDevice(device);
    if (!d || !count)
        return nullptr;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    info.queryCount = count;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (g_vk.vkCreateQueryPool(d->vk, &info, nullptr, &pool) != VK_SUCCESS)
        return nullptr;
    g_vk.vkResetQueryPool(d->vk, pool, 0, count);
    auto* query = new (std::nothrow) AccelSizeQuery();
    if (!query)
    {
        g_vk.vkDestroyQueryPool(d->vk, pool, nullptr);
        return nullptr;
    }
    query->device = d;
    query->pool = pool;
    query->count = count;
    return query;
}

void WriteCompactedSizes(ID3D11DeviceContext* context, AccelSizeQuery* query, Accel* const* accels, UINT first, UINT count)
{
    Context* c = RtImmediate(context);
    if (!c || !query || !accels || first + count > query->count)
        return;
    // Runs of structures; a null one (never built) keeps its query unwritten.
    std::vector<VkAccelerationStructureKHR> run;
    run.reserve(count);
    const auto flush = [&](UINT end) {
        if (!run.empty())
            g_vk.vkCmdWriteAccelerationStructuresPropertiesKHR(c->Cmd(), u32(run.size()), run.data(),
                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query->pool, first + end - u32(run.size()));
        run.clear();
    };
    for (UINT i = 0; i < count; ++i)
    {
        if (accels[i] && accels[i]->handle)
            run.push_back(accels[i]->handle);
        else
            flush(i);
    }
    flush(count);
}

bool ReadCompactedSizes(AccelSizeQuery* query, UINT64* sizes, UINT count)
{
    if (!query || !sizes || count > query->count)
        return false;
    // Value and availability per query: an unwritten one reads 0, the result of a written one is there after the
    // flush that waited.
    std::vector<UINT64> data(size_t(count) * 2, 0);
    const VkResult result = g_vk.vkGetQueryPoolResults(query->device->vk, query->pool, 0, count, data.size() * sizeof(UINT64),
        data.data(), 2 * sizeof(UINT64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY)
        return false;
    for (UINT i = 0; i < count; ++i)
        sizes[i] = data[size_t(i) * 2 + 1] ? data[size_t(i) * 2] : 0;
    return true;
}

void ReleaseAccelSizeQuery(AccelSizeQuery* query)
{
    if (!query)
        return;
    Device* d = query->device;
    const VkQueryPool pool = query->pool;
    Defer(d, [d, pool] { g_vk.vkDestroyQueryPool(d->vk, pool, nullptr); });
    delete query;
}
} // namespace xrvk
