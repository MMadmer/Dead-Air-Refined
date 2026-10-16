// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The RT lane's GPU objects on either API (da_rt_device.h): native buffers and acceleration structures, their
// builds, compacting copies and compacted sizes, bindless slots and upload memory. DirectX 12: xrD3D12's native
// resources, recorded on the section's command list. Vulkan: xrVulkan's RT buffers and acceleration structures,
// recorded on the immediate context; what the passes read by address or through the bindless set is ordered by
// full barriers, as the layer cannot see it.

#include "stdafx.h"

#include "da_rt_device.h"

namespace xray::render::RENDER_NAMESPACE
{
#if defined(DA_RT_LANE)
struct DaRtSizeQuery
{
#if defined(XR_D3D12)
    ID3D12Resource* sizes{};    // written by the builds (UAV)
    ID3D12Resource* readback{}; // copied to after them
#else
    xrvk::AccelSizeQuery* query{};
    xr_vector<xrvk::Accel*> built; // the structure of each entry, written after the barrier
#endif
    u32 count{};
};

namespace
{
#if defined(XR_D3D12)
ID3D12Resource* native(const DaRtBuffer* b) { return reinterpret_cast<ID3D12Resource*>(const_cast<DaRtBuffer*>(b)); }
ID3D12Resource* native(const DaRtAccel* a) { return reinterpret_cast<ID3D12Resource*>(const_cast<DaRtAccel*>(a)); }

void wide_name(pcstr name, wchar_t (&out)[128]) { swprintf_s(out, L"%hs", name ? name : "rt"); }

D3D12_RAYTRACING_GEOMETRY_DESC geometry_desc(const DaRtTriangles& t)
{
    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = t.alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
    g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.IndexCount = t.index_count;
    g.Triangles.VertexCount = t.vertex_count;
    g.Triangles.IndexBuffer = t.ib;
    g.Triangles.VertexBuffer.StartAddress = t.vb;
    g.Triangles.VertexBuffer.StrideInBytes = t.stride;
    return g;
}

// Bottom level over `geometry`, or a top level of `count` instances.
void inputs_of(const DaRtTriangles* geometry, u32 count, u32 flags, xr_vector<D3D12_RAYTRACING_GEOMETRY_DESC>& descs,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in)
{
    in = {};
    in.Type = geometry ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS(flags);
    in.NumDescs = count;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    if (geometry)
    {
        descs.resize(count);
        for (u32 i = 0; i < count; ++i)
            descs[i] = geometry_desc(geometry[i]);
        in.pGeometryDescs = descs.data();
    }
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!resource || before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void native_uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}
#else
ID3D11Buffer* native(const DaRtBuffer* b) { return reinterpret_cast<ID3D11Buffer*>(const_cast<DaRtBuffer*>(b)); }
xrvk::Accel* native(const DaRtAccel* a) { return reinterpret_cast<xrvk::Accel*>(const_cast<DaRtAccel*>(a)); }

void triangles_of(const DaRtTriangles* geometry, u32 count, xr_vector<xrvk::AccelTriangles>& out)
{
    out.resize(geometry ? count : 0);
    for (u32 i = 0; geometry && i < count; ++i)
    {
        const DaRtTriangles& t = geometry[i];
        xrvk::AccelTriangles& v = out[i];
        v.vertexAddress = t.vb;
        v.vertexStride = t.stride;
        v.vertexCount = t.vertex_count;
        v.indexAddress = t.ib;
        v.indexCount = t.index_count;
        v.opaque = !t.alpha;
    }
}
#endif
} // namespace

// ---- objects ----------------------------------------------------------------------------------------------

bool da_rt_sizes(const DaRtTriangles* geometry, u32 count, u32 flags, DaRtSizes& out)
{
    out = {};
#if defined(XR_D3D12)
    ID3D12Device5* device5 = xrd3d12::NativeDevice5(HW.pDevice);
    if (!device5)
        return false;
    xr_vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
    inputs_of(geometry, count, flags, descs, in);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    out = {info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes, info.UpdateScratchDataSizeInBytes};
    return out.result != 0;
#else
    xr_vector<xrvk::AccelTriangles> meshes;
    triangles_of(geometry, count, meshes);
    xrvk::AccelSizes sizes;
    if (!xrvk::AccelBuildSizes(HW.pDevice, geometry ? meshes.data() : nullptr, count, flags, sizes))
        return false;
    out = {sizes.result, sizes.build, sizes.update};
    return true;
#endif
}

DaRtBuffer* da_rt_buffer_create(u64 size, pcstr name)
{
#if defined(XR_D3D12)
    wchar_t wide[128];
    wide_name(name, wide);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, wide, &buffer)))
        return nullptr;
    return reinterpret_cast<DaRtBuffer*>(buffer);
#else
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(xrvk::CreateRtBuffer(HW.pDevice, size, nullptr, name, &buffer)))
        return nullptr;
    return reinterpret_cast<DaRtBuffer*>(buffer);
#endif
}

DaRtAddress da_rt_address(const DaRtBuffer* buffer)
{
    if (!buffer)
        return 0;
#if defined(XR_D3D12)
    return native(buffer)->GetGPUVirtualAddress();
#else
    return xrvk::BufferAddress(native(buffer));
#endif
}

u32 da_rt_bindless_raw(DaRtBuffer* buffer, u64 size)
{
    if (!buffer)
        return da_rt_bindless_invalid;
    const u32 index = da_rt_bindless_alloc(1);
    if (index == da_rt_bindless_invalid)
        return index;
#if defined(XR_D3D12)
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = UINT(size / 4);
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    xrd3d12::WriteBindlessNative(HW.pDevice, index, native(buffer), desc);
#else
    UNUSED(size);
    xrvk::WriteBindlessBuffer(HW.pDevice, index, native(buffer));
#endif
    return index;
}

DaRtAccel* da_rt_accel_create(u64 size, bool top, pcstr name)
{
#if defined(XR_D3D12)
    UNUSED(top);
    wchar_t wide[128];
    wide_name(name, wide);
    ID3D12Resource* accel = nullptr;
    if (FAILED(xrd3d12::CreateAccelerationStructureBuffer(HW.pDevice, size, wide, &accel)))
        return nullptr;
    return reinterpret_cast<DaRtAccel*>(accel);
#else
    return reinterpret_cast<DaRtAccel*>(xrvk::CreateAccel(HW.pDevice, top, size, name));
#endif
}

DaRtAddress da_rt_address(const DaRtAccel* accel)
{
    if (!accel)
        return 0;
#if defined(XR_D3D12)
    return native(accel)->GetGPUVirtualAddress();
#else
    return xrvk::AccelAddress(native(accel));
#endif
}

ID3D11ShaderResourceView* da_rt_accel_view(DaRtAccel* accel)
{
    ID3D11ShaderResourceView* view = nullptr;
#if defined(XR_D3D12)
    if (FAILED(xrd3d12::CreateAccelerationStructureView(HW.pDevice, native(accel), &view)))
        return nullptr;
#else
    if (!accel || FAILED(xrvk::CreateAccelView(native(accel), &view)))
        return nullptr;
#endif
    return view;
}

void da_rt_release(DaRtBuffer*& buffer)
{
    if (buffer)
    {
#if defined(XR_D3D12)
        xrd3d12::ReleaseNative(HW.pDevice, native(buffer));
#else
        // The layer keeps the memory until the GPU is done with it.
        native(buffer)->Release();
#endif
    }
    buffer = nullptr;
}

void da_rt_release(DaRtAccel*& accel)
{
    if (accel)
    {
#if defined(XR_D3D12)
        xrd3d12::ReleaseNative(HW.pDevice, native(accel));
#else
        xrvk::ReleaseAccel(native(accel));
#endif
    }
    accel = nullptr;
}

DaRtSizeQuery* da_rt_size_query(u32 count)
{
    if (!count)
        return nullptr;
    auto* query = xr_new<DaRtSizeQuery>();
    query->count = count;
#if defined(XR_D3D12)
    const u64 bytes = (count * 8ull + 255) & ~255ull;
    if (FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON, L"rt compacted sizes", &query->sizes)) ||
        FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, L"rt compacted sizes readback", &query->readback)))
        da_rt_release(query);
#else
    query->query = xrvk::CreateAccelSizeQuery(HW.pDevice, count);
    query->built.assign(count, nullptr);
    if (!query->query)
        da_rt_release(query);
#endif
    return query;
}

void da_rt_release(DaRtSizeQuery*& query)
{
    if (!query)
        return;
#if defined(XR_D3D12)
    if (query->sizes)
        xrd3d12::ReleaseNative(HW.pDevice, query->sizes);
    if (query->readback)
        xrd3d12::ReleaseNative(HW.pDevice, query->readback);
#else
    xrvk::ReleaseAccelSizeQuery(query->query);
#endif
    xr_delete(query);
}

bool da_rt_read_sizes(DaRtSizeQuery* query, u64* sizes, u32 count)
{
    if (!query || !sizes || count > query->count)
        return false;
#if defined(XR_D3D12)
    const D3D12_RANGE range{0, SIZE_T(count * 8ull)};
    void* mapped = nullptr;
    if (FAILED(query->readback->Map(0, &range, &mapped)) || !mapped)
        return false;
    std::memcpy(sizes, mapped, count * 8ull);
    query->readback->Unmap(0, nullptr);
    return true;
#else
    return xrvk::ReadCompactedSizes(query->query, sizes, count);
#endif
}

DaRtAddress da_rt_engine_address(ID3D11Buffer* buffer, bool* tracked)
{
    if (tracked)
        *tracked = false;
    if (!buffer)
        return 0;
#if defined(XR_D3D12)
    xrd3d12::NativeBuffer range;
    if (!xrd3d12::NativeBufferRange(buffer, range))
        return 0;
    if (tracked)
        *tracked = range.tracked;
    return range.address;
#else
    return xrvk::BufferAddress(buffer);
#endif
}

u32 da_rt_bindless_alloc(u32 count)
{
#if defined(XR_D3D12)
    const u32 index = xrd3d12::AllocateBindless(HW.pDevice, count);
    return index == xrd3d12::kBindlessInvalid ? da_rt_bindless_invalid : index;
#else
    const u32 index = xrvk::AllocateBindless(HW.pDevice, count);
    return index == xrvk::kBindlessInvalid ? da_rt_bindless_invalid : index;
#endif
}

void da_rt_bindless_texture(u32 index, ID3D11ShaderResourceView* view)
{
#if defined(XR_D3D12)
    xrd3d12::WriteBindless(HW.pDevice, index, view);
#else
    xrvk::WriteBindless(HW.pDevice, index, view);
#endif
}

void da_rt_bindless_buffer(u32 index, ID3D11Buffer* buffer)
{
#if defined(XR_D3D12)
    xrd3d12::WriteBindlessBuffer(HW.pDevice, index, buffer);
#else
    xrvk::WriteBindlessBuffer(HW.pDevice, index, buffer);
#endif
}

void da_rt_bindless_free(u32 index, u32 count)
{
    if (index == da_rt_bindless_invalid || !count)
        return;
#if defined(XR_D3D12)
    xrd3d12::FreeBindless(HW.pDevice, index, count);
#else
    xrvk::FreeBindless(HW.pDevice, index, count);
#endif
}

u64 da_rt_texture_generation()
{
#if defined(XR_D3D12)
    return xrd3d12::TextureWriteGeneration(HW.pDevice);
#else
    return xrvk::TextureWriteGeneration(HW.pDevice);
#endif
}

bool da_rt_video_memory(u64& budget, u64& usage)
{
    UINT64 b = 0, u = 0;
#if defined(XR_D3D12)
    const bool ok = xrd3d12::QueryVideoMemory(HW.pDevice, b, u);
#else
    const bool ok = xrvk::QueryVideoMemory(HW.pDevice, b, u);
#endif
    budget = b;
    usage = u;
    return ok;
}

void da_rt_flush_wait()
{
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
#if defined(XR_D3D12)
    xrd3d12::Flush(context, true);
#else
    xrvk::Flush(context, true);
#endif
}

// ---- the section's native work ------------------------------------------------------------------------------

DaRtBuffer* DaRtSection::upload_buffer(const void* data, u64 size, pcstr name)
{
    if (!m_open || !size)
        return nullptr;
#if defined(XR_D3D12)
    wchar_t wide[128];
    wide_name(name, wide);
    ID3D12Resource* buffer = nullptr;
    // Created in COMMON: the copy below promotes it to COPY_DEST.
    if (FAILED(xrd3d12::CreateNativeBuffer(HW.pDevice, (size + 255) & ~255ull, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COMMON, wide, &buffer)))
        return nullptr;
    const xrd3d12::UploadSpan span = xrd3d12::AllocateUpload(m_context, size, 16);
    if (!span.cpu)
    {
        xrd3d12::ReleaseNative(HW.pDevice, buffer);
        return nullptr;
    }
    std::memcpy(span.cpu, data, size);
    m_scope.list->CopyBufferRegion(buffer, 0, span.resource, span.offset, size);
    transition(m_scope.list, buffer, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return reinterpret_cast<DaRtBuffer*>(buffer);
#else
    // Filled through the layer's creation uploads, which the next submission waits for.
    ID3D11Buffer* buffer = nullptr;
    if (FAILED(xrvk::CreateRtBuffer(HW.pDevice, size, data, name, &buffer)))
        return nullptr;
    return reinterpret_cast<DaRtBuffer*>(buffer);
#endif
}

DaRtUpload DaRtSection::upload(u64 size, u64 alignment)
{
    if (!m_open)
        return {};
#if defined(XR_D3D12)
    const xrd3d12::UploadSpan span = xrd3d12::AllocateUpload(m_context, size, alignment);
    return {span.cpu, span.gpu};
#else
    const xrvk::UploadSpan span = xrvk::AllocateUpload(m_context, size, alignment);
    return {span.cpu, span.address};
#endif
}

void DaRtSection::build(const DaRtBuild& b)
{
    if (!m_open || !b.dst)
        return;
#if defined(XR_D3D12)
    xr_vector<D3D12_RAYTRACING_GEOMETRY_DESC> descs;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    inputs_of(b.geometry, b.count, b.flags, descs, d.Inputs);
    d.DestAccelerationStructureData = native(b.dst)->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = b.scratch;
    if (!b.geometry)
        d.Inputs.InstanceDescs = b.instances;
    if (b.update)
    {
        d.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        d.SourceAccelerationStructureData = native(b.src)->GetGPUVirtualAddress();
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{};
    if (b.sizes)
    {
        post.DestBuffer = b.sizes->sizes->GetGPUVirtualAddress() + b.size_index * 8ull;
        post.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    }
    m_scope.list4->BuildRaytracingAccelerationStructure(&d, b.sizes ? 1 : 0, b.sizes ? &post : nullptr);
#else
    xr_vector<xrvk::AccelTriangles> meshes;
    triangles_of(b.geometry, b.count, meshes);
    xrvk::AccelBuild build;
    build.dst = native(b.dst);
    build.src = native(b.src);
    build.geometry = b.geometry ? meshes.data() : nullptr;
    build.count = b.count;
    build.instances = b.instances;
    build.flags = b.flags;
    build.update = b.update;
    build.scratch = b.scratch;
    xrvk::BuildAccel(m_context, build);
    if (b.sizes && b.size_index < b.sizes->count)
        b.sizes->built[b.size_index] = native(b.dst);
#endif
}

void DaRtSection::compact(DaRtAccel* dst, DaRtAccel* src)
{
    if (!m_open || !dst || !src)
        return;
#if defined(XR_D3D12)
    m_scope.list4->CopyRaytracingAccelerationStructure(native(dst)->GetGPUVirtualAddress(), native(src)->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
#else
    xrvk::CopyAccelCompact(m_context, native(dst), native(src));
#endif
}

void DaRtSection::accel_barrier(DaRtAccel* accel)
{
    if (!m_open)
        return;
#if defined(XR_D3D12)
    native_uav_barrier(m_scope.list, native(accel));
#else
    UNUSED(accel);
    xrvk::AccelBarrier(m_context);
#endif
}

void DaRtSection::scratch_barrier(DaRtBuffer* scratch)
{
    if (!m_open)
        return;
#if defined(XR_D3D12)
    native_uav_barrier(m_scope.list, native(scratch));
#else
    UNUSED(scratch);
    xrvk::AccelBarrier(m_context);
#endif
}

void DaRtSection::written(DaRtBuffer* buffer)
{
    if (!m_open || !buffer)
        return;
#if defined(XR_D3D12)
    transition(m_scope.list, native(buffer), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
#else
    xrvk::AccelBarrier(m_context);
#endif
}

void DaRtSection::copy_sizes(DaRtSizeQuery* query, u32 count)
{
    if (!m_open || !query || count > query->count)
        return;
#if defined(XR_D3D12)
    transition(m_scope.list, query->sizes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_scope.list->CopyBufferRegion(query->readback, 0, query->sizes, 0, count * 8ull);
#else
    xrvk::WriteCompactedSizes(m_context, query->query, query->built.data(), 0, count);
#endif
}

void DaRtSection::require_read(ID3D11Resource* const* resources, u32 count)
{
    if (!m_open || !resources || !count)
        return;
#if defined(XR_D3D12)
    xrd3d12::RequireMany(m_context, resources, UINT(count), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
#else
    xrvk::RequireRead(m_context, resources, count);
#endif
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
