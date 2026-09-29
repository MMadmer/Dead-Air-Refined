// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `rt` lane (docs/dead-air/dx12-features/dxr.md 9): native ray tracing over
// geometry laid out like the engine's level buffers, and D3D11 view descriptors copied into ring
// tables and bindless slots, the way the ray tracing passes bind engine targets and materials.
// Scenarios() appends this list.
#include "harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

// Self-contained source through the layer's DXC service at HLSL 2021, like the engine's native passes.
bool CompileNative(Result& r, const char* source, const char* entry, const char* target, std::vector<unsigned char>& out)
{
    xrd3d12::ShaderCompile request;
    request.source = source;
    request.size = std::strlen(source);
    request.name = "layertest_rt.hlsl";
    request.entry = entry;
    request.target = target;
    request.preprocessed = true;
    xrd3d12::ShaderOutput output;
    if (!r.Check(xrd3d12::CompileShader(request, output), "%s (%s) does not compile: %s", entry, target, output.messages.c_str()))
        return false;
    out = std::move(output.object);
    return true;
}

D3D12_ROOT_PARAMETER1 TableParam(const D3D12_DESCRIPTOR_RANGE1* range)
{
    D3D12_ROOT_PARAMETER1 p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable = {1, range};
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return p;
}

ID3D12PipelineState* NativePipeline(Gpu& g, Result& r, const D3D12_ROOT_PARAMETER1* params, UINT count, const char* source,
    const char* entry, const char* target, ID3D12RootSignature*& root)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = count;
    desc.Desc_1_1.pParameters = params;
    root = xrd3d12::RootSignature(g.dev, desc, L"layertest rt");
    std::vector<unsigned char> code;
    if (!r.Check(root, "RootSignature failed") || !CompileNative(r, source, entry, target, code))
        return nullptr;
    ID3D12PipelineState* pso = xrd3d12::ComputePipeline(g.dev, root, code.data(), code.size(), L"layertest rt");
    r.Check(pso, "ComputePipeline(%s) failed", entry);
    return pso;
}

// The views' CPU descriptors copied in order into ring space of `ctx`.
xrd3d12::DescriptorSpan CopyViews(Gpu& g, ID3D11DeviceContext* ctx, ID3D11View* const* views, UINT count)
{
    const xrd3d12::DescriptorSpan span = xrd3d12::AllocateDescriptors(ctx, count);
    ID3D12Device* device = xrd3d12::NativeDevice(g.dev);
    for (UINT i = 0; i < count && span.count; ++i)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE target{span.cpu.ptr + SIZE_T(i) * span.increment};
        device->CopyDescriptorsSimple(1, target, xrd3d12::NativeDescriptor(views[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    return span;
}

D3D12_GPU_DESCRIPTOR_HANDLE At(const xrd3d12::DescriptorSpan& span, UINT i)
{
    return D3D12_GPU_DESCRIPTOR_HANDLE{span.gpu.ptr + UINT64(i) * span.increment};
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// `bytes` of a native buffer in UNORDERED_ACCESS, through a readback copy (waits for the GPU).
bool ReadUavBuffer(Gpu& g, Result& r, ID3D12Resource* buffer, UINT64 bytes, void* out)
{
    ID3D12Resource* readback = nullptr;
    if (!r.Ok(xrd3d12::CreateNativeBuffer(g.dev, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATE_COPY_DEST, L"layertest rt readback", &readback),
            "CreateNativeBuffer(readback)"))
        return false;
    bool read = false;
    xrd3d12::NativeScope scope;
    if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
    {
        D3D12_RESOURCE_BARRIER b = Transition(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        scope.list->ResourceBarrier(1, &b);
        scope.list->CopyBufferRegion(readback, 0, buffer, 0, bytes);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        scope.list->ResourceBarrier(1, &b);
        xrd3d12::EndNative(g.ctx, 0);
        xrd3d12::Flush(g.ctx, true);
        void* mapped = nullptr;
        const D3D12_RANGE whole{0, SIZE_T(bytes)};
        if (r.Ok(readback->Map(0, &whole, &mapped), "Map(readback)"))
        {
            std::memcpy(out, mapped, SIZE_T(bytes));
            const D3D12_RANGE nothing{0, 0};
            readback->Unmap(0, &nothing);
            read = true;
        }
    }
    xrd3d12::ReleaseNative(g.dev, readback);
    return read;
}

// ---- dxr 9 test 2: native ray tracing ----------------------------------------------------------

constexpr u32 kRtWidth = 128, kRtHeight = 64;

const char kTraceHlsl[] = R"(
RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<uint> hits : register(u0);
[numthreads(8, 8, 1)]
void TraceCs(uint3 id : SV_DispatchThreadID)
{
    RayDesc ray;
    ray.Origin = float3(float2(id.xy) + 0.5, -10.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = 100.0;
    // The lower half traces instance mask 0x01 only.
    const uint mask = id.y < 32 ? 0xFF : 0x01;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
    // Alpha test stand-in: of a non-opaque geometry only triangle 0 is solid.
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CandidatePrimitiveIndex() == 0)
            q.CommitNonOpaqueTriangleHit();
    }
    uint value = 0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        value = (q.CommittedInstanceID() & 15) | (q.CommittedGeometryIndex() << 4) | (q.CommittedPrimitiveIndex() << 8) |
            (uint(q.CommittedRayT() * 16.0 + 0.5) << 16);
    hits[id.xy] = value;
}
)";

// A level vertex: position at offset 0 in a 32-byte record, like r1_decl_lmap / mu_model_decl.
struct LevelVertex
{
    float pos[3];
    u32 normal, tangent, binormal;
    short tc[4];
};
static_assert(sizeof(LevelVertex) == 32);

struct RtQuad
{
    float x0, y0, x1, y1, z;
    bool opaque;
};
// Geometry 0 is opaque, geometry 1 (in front of it) is alpha-tested.
constexpr RtQuad kRtQuads[2] = {{8, 8, 40, 40, 5, true}, {24, 24, 56, 40, 3, false}};

struct RtInstance
{
    float dx;
    u32 id, mask;
    bool forceOpaque;
};
constexpr RtInstance kRtInstances[2] = {{0, 5, 0x01, false}, {64, 9, 0x02, true}};

// Point in triangle with a margin: 1 inside, 0 outside, -1 too close to an edge to call.
int InsideTriangle(const float (&p)[3][2], float x, float y)
{
    const float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]);
    const float sign = area < 0 ? -1.0f : 1.0f;
    constexpr float kMargin = 0.05f;
    int result = 1;
    for (int i = 0; i < 3; ++i)
    {
        const float* a = p[i];
        const float* b = p[(i + 1) % 3];
        const float length = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
        const float distance = sign * ((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])) / length;
        if (distance < -kMargin)
            return 0;
        if (distance <= kMargin)
            result = -1;
    }
    return result;
}

// What TraceCs writes at (x, y); `ambiguous` when a ray passes within the margin of an edge.
u32 ExpectedHit(u32 x, u32 y, bool& ambiguous)
{
    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
    const u32 rayMask = y < 32 ? 0xFFu : 0x01u;
    u32 expected = 0;
    float best = 1e9f;
    for (const RtInstance& instance : kRtInstances)
    {
        if (!(instance.mask & rayMask))
            continue;
        for (u32 geometry = 0; geometry < 2; ++geometry)
        {
            const RtQuad& q = kRtQuads[geometry];
            const float x0 = q.x0 + instance.dx, x1 = q.x1 + instance.dx;
            const float corners[4][2] = {{x0, q.y0}, {x1, q.y0}, {x1, q.y1}, {x0, q.y1}};
            for (u32 triangle = 0; triangle < 2; ++triangle)
            {
                const float points[3][2] = {{corners[0][0], corners[0][1]}, {corners[1 + triangle][0], corners[1 + triangle][1]},
                    {corners[2 + triangle][0], corners[2 + triangle][1]}};
                const int inside = InsideTriangle(points, px, py);
                if (inside < 0)
                    ambiguous = true;
                if (inside <= 0 || (!q.opaque && !instance.forceOpaque && triangle != 0))
                    continue;
                const float t = q.z + 10.0f;
                if (t < best)
                {
                    best = t;
                    expected = (instance.id & 15) | (geometry << 4) | (triangle << 8) | (u32(t * 16.0f + 0.5f) << 16);
                }
            }
        }
    }
    return expected;
}

// Engine-style vertex and index buffers (static D3D11 buffers in the layer's pages, geometry at an
// offset, 16-bit indices from an odd start) -> NativeBufferRange -> a compacted BLAS with an opaque
// and an alpha-tested geometry -> a TLAS with masks, IDs and FORCE_OPAQUE -> inline RayQuery in a
// native compute pass writing a D3D11 UAV texture, checked texel by texel against a CPU model.
void ScenarioNativeRaytracing(Gpu& g, Result& r)
{
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(g.dev);
    ID3D12Device5* dev5 = xrd3d12::NativeDevice5(g.dev);
    if (!caps || caps->raytracingTier < 11 || xrd3d12::DxcShaderModel() < 0x65 || !dev5)
    {
        std::printf("      skipped: no inline ray tracing (tier %u, DXC 0x%X)\n", caps ? caps->raytracingTier : 0u,
            xrd3d12::DxcShaderModel());
        return;
    }

    // Three unrelated vertices and indices first: the visuals start at vBase 3 and iBase 3 / 9.
    constexpr u32 kVBase[2] = {3, 7}, kIBase[2] = {3, 9};
    LevelVertex vertices[11]{};
    u16 indices[15]{};
    for (u32 q = 0; q < 2; ++q)
    {
        const RtQuad& quad = kRtQuads[q];
        const float corners[4][2] = {{quad.x0, quad.y0}, {quad.x1, quad.y0}, {quad.x1, quad.y1}, {quad.x0, quad.y1}};
        for (u32 v = 0; v < 4; ++v)
        {
            LevelVertex& vertex = vertices[kVBase[q] + v];
            vertex.pos[0] = corners[v][0];
            vertex.pos[1] = corners[v][1];
            vertex.pos[2] = quad.z;
            vertex.normal = 0x7F7FFFu;
            vertex.tc[0] = short(v * 1000);
        }
        const u16 quadIndices[6] = {0, 1, 2, 0, 2, 3};
        std::memcpy(indices + kIBase[q], quadIndices, sizeof(quadIndices));
    }
    auto vb = MakeBuffer(g, r, sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, vertices);
    auto ib = MakeBuffer(g, r, sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, indices);
    REQUIRE_OK();
    xrd3d12::NativeBuffer vbRange, ibRange;
    if (!r.Check(xrd3d12::NativeBufferRange(vb.Get(), vbRange) && xrd3d12::NativeBufferRange(ib.Get(), ibRange) && vbRange.address &&
                ibRange.address,
            "NativeBufferRange of the static vertex / index buffer"))
        return;

    D3D12_RAYTRACING_GEOMETRY_DESC geometries[2]{};
    for (u32 q = 0; q < 2; ++q)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC& geometry = geometries[q];
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = kRtQuads[q].opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometry.Triangles.VertexBuffer = {vbRange.address + kVBase[q] * sizeof(LevelVertex), sizeof(LevelVertex)};
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = 4;
        geometry.Triangles.IndexBuffer = ibRange.address + kIBase[q] * sizeof(u16);
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        geometry.Triangles.IndexCount = 6;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
    bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
    bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottom.NumDescs = 2;
    bottom.pGeometryDescs = geometries;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs = UINT(std::size(kRtInstances));
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo{}, topInfo{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottomInfo);
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);

    ID3D12Resource* blas = nullptr;
    ID3D12Resource* compacted = nullptr;
    ID3D12Resource* tlas = nullptr;
    ID3D12Resource* scratch = nullptr;
    ID3D12Resource* postbuild = nullptr;
    const auto releaseAll = [&] {
        for (ID3D12Resource* object : {blas, compacted, tlas, scratch, postbuild})
        {
            if (object)
                xrd3d12::ReleaseNative(g.dev, object);
        }
    };
    r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, bottomInfo.ResultDataMaxSizeInBytes, L"layertest rt blas", &blas), "BLAS buffer");
    r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, topInfo.ResultDataMaxSizeInBytes, L"layertest rt tlas", &tlas), "TLAS buffer");
    r.Ok(xrd3d12::CreateNativeBuffer(g.dev, (std::max)(bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes),
             D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             L"layertest rt scratch", &scratch),
        "scratch buffer");
    r.Ok(xrd3d12::CreateNativeBuffer(g.dev, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"layertest rt postbuild", &postbuild),
        "postbuild info buffer");
    if (!r.failures.empty())
    {
        releaseAll();
        return;
    }

    // BLAS with its compacted size, read back like the scene build does after its Flush(wait).
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope) && scope.list4, "BeginNative refused or no ID3D12GraphicsCommandList4"))
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = bottom;
            build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC info{postbuild->GetGPUVirtualAddress(),
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE};
            scope.list4->BuildRaytracingAccelerationStructure(&build, 1, &info);
            xrd3d12::EndNative(g.ctx, 0);
        }
    }
    UINT64 compactedSize = 0;
    ReadUavBuffer(g, r, postbuild, sizeof(compactedSize), &compactedSize);
    r.Check(compactedSize && compactedSize <= bottomInfo.ResultDataMaxSizeInBytes, "compacted BLAS size %llu (build size %llu)",
        static_cast<unsigned long long>(compactedSize), static_cast<unsigned long long>(bottomInfo.ResultDataMaxSizeInBytes));
    if (r.failures.empty())
        r.Ok(xrd3d12::CreateAccelerationStructureBuffer(g.dev, compactedSize, L"layertest rt blas compacted", &compacted), "compacted BLAS buffer");
    if (!r.failures.empty())
    {
        releaseAll();
        return;
    }

    // Compaction, the uncompacted storage released behind the copy, then the TLAS over the copy.
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope) && scope.list4, "BeginNative refused"))
        {
            scope.list4->CopyRaytracingAccelerationStructure(compacted->GetGPUVirtualAddress(), blas->GetGPUVirtualAddress(),
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            scope.list->ResourceBarrier(1, &uav);
            const xrd3d12::UploadSpan instances = xrd3d12::AllocateUpload(g.ctx, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 2,
                D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
            if (r.Check(instances.cpu, "AllocateUpload(instances) failed"))
            {
                D3D12_RAYTRACING_INSTANCE_DESC descs[2]{};
                for (u32 i = 0; i < 2; ++i)
                {
                    descs[i].Transform[0][0] = descs[i].Transform[1][1] = descs[i].Transform[2][2] = 1.0f;
                    descs[i].Transform[0][3] = kRtInstances[i].dx;
                    descs[i].InstanceID = kRtInstances[i].id;
                    descs[i].InstanceMask = kRtInstances[i].mask;
                    descs[i].Flags = kRtInstances[i].forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
                    descs[i].AccelerationStructure = compacted->GetGPUVirtualAddress();
                }
                std::memcpy(instances.cpu, descs, sizeof(descs));
                top.InstanceDescs = instances.gpu;
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                build.Inputs = top;
                build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
                build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
                scope.list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
                scope.list->ResourceBarrier(1, &uav);
            }
            xrd3d12::EndNative(g.ctx, 0);
        }
        xrd3d12::ReleaseNative(g.dev, blas);
        blas = nullptr;
    }

    // Inline RayQuery from a native compute pass: the TLAS through an acceleration structure view,
    // the D3D11 UAV texture through its copied descriptor.
    ComPtr<ID3D11ShaderResourceView> sceneView;
    r.Ok(xrd3d12::CreateAccelerationStructureView(g.dev, tlas, &sceneView), "CreateAccelerationStructureView(tlas)");
    auto hits = MakeTexture2D(g, r, Desc2D(kRtWidth, kRtHeight, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11UnorderedAccessView> hitsUav;
    if (hits)
        r.Ok(g.dev->CreateUnorderedAccessView(hits.Get(), nullptr, &hitsUav), "CreateUnorderedAccessView(hits)");
    const D3D12_DESCRIPTOR_RANGE1 sceneRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    const D3D12_DESCRIPTOR_RANGE1 hitsRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
    const D3D12_ROOT_PARAMETER1 params[2] = {TableParam(&sceneRange), TableParam(&hitsRange)};
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* trace = r.failures.empty() ? NativePipeline(g, r, params, 2, kTraceHlsl, "TraceCs", "cs_6_5", root) : nullptr;
    if (!r.failures.empty())
    {
        releaseAll();
        return;
    }
    {
        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
        {
            xrd3d12::RequireView(g.ctx, hitsUav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D11View* const views[2] = {sceneView.Get(), hitsUav.Get()};
            const xrd3d12::DescriptorSpan span = CopyViews(g, g.ctx, views, 2);
            if (r.Check(span.count == 2, "AllocateDescriptors failed"))
            {
                scope.list->SetComputeRootSignature(root);
                scope.list->SetPipelineState(trace);
                scope.list->SetComputeRootDescriptorTable(0, At(span, 0));
                scope.list->SetComputeRootDescriptorTable(1, At(span, 1));
                scope.list->Dispatch(kRtWidth / 8, kRtHeight / 8, 1);
            }
            xrd3d12::EndNative(g.ctx, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        }
    }
    const Image image = Read2D(g, r, hits.Get(), 0, "native ray tracing hits");
    releaseAll();
    if (image.texels.size() != kRtWidth * kRtHeight * 4)
        return;

    u32 wrong = 0, ambiguousCount = 0, hitCount = 0, firstX = 0, firstY = 0, firstGot = 0, firstWant = 0;
    for (u32 y = 0; y < kRtHeight; ++y)
    {
        for (u32 x = 0; x < kRtWidth; ++x)
        {
            u32 got = 0;
            std::memcpy(&got, image.texels.data() + (y * kRtWidth + x) * 4, 4);
            bool ambiguous = false;
            const u32 want = ExpectedHit(x, y, ambiguous);
            if (ambiguous)
            {
                ++ambiguousCount;
                continue;
            }
            hitCount += want != 0;
            if (got != want && !wrong++)
            {
                firstX = x;
                firstY = y;
                firstGot = got;
                firstWant = want;
            }
        }
    }
    r.Check(!wrong, "%u texels differ from the CPU model (first at %u,%u: 0x%08X, expected 0x%08X)", wrong, firstX, firstY, firstGot,
        firstWant);
    r.Check(hitCount > 1000 && ambiguousCount < kRtWidth * kRtHeight / 20, "model: %u hits, %u texels on edges", hitCount,
        ambiguousCount);
    r.Value("compacted BLAS smaller than built", compactedSize < bottomInfo.ResultDataMaxSizeInBytes);
}

// ---- dxr 9 test 2: view descriptor copy --------------------------------------------------------

constexpr u32 kViewSize = 16;

#define LAYERTEST_RT_COMBINE                                                                                   \
    "float4 Combine(float4 c, float m, float4 s, uint w, float4 d)\n"                                          \
    "{\n"                                                                                                      \
    "    return float4(c.r + m, c.g + s.g, c.b + float(w & 255u) / 255.0, c.a + d.r + s.a);\n"                 \
    "}\n"

// Colours (texture with initial data), mip 1 of a chain, slice 1 of an array, a raw buffer and a
// render target the D3D11 path drew into just before.
const char kViewsHlsl[] = LAYERTEST_RT_COMBINE R"(
Texture2D<float4> colors : register(t0);
Texture2D<float> mip : register(t1);
Texture2DArray<float4> slice : register(t2);
ByteAddressBuffer words : register(t3);
Texture2D<float4> drawn : register(t4);
RWTexture2D<float4> result : register(u0);
[numthreads(8, 8, 1)]
void TableCs(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    result[p] = Combine(colors.Load(int3(p, 0)), mip.Load(int3(p, 0)), slice.Load(int4(p, 0, 0)),
        words.Load(4 * (p.x + 16 * p.y)), drawn.Load(int3(p, 0)));
}
)";

// The same through bindless slots: three unbounded tables of different view types at one base.
const char kViewsBindlessHlsl[] = LAYERTEST_RT_COMBINE R"(
cbuffer Slots : register(b0)
{
    uint4 slots;
    uint4 slots2;
};
Texture2D<float4> textures[] : register(t0, space100);
ByteAddressBuffer buffers[] : register(t0, space101);
Texture2DArray<float4> arrays[] : register(t0, space102);
RWTexture2D<float4> result : register(u0);
[numthreads(8, 8, 1)]
void BindlessCs(uint3 id : SV_DispatchThreadID)
{
    const uint2 p = id.xy;
    result[p] = Combine(textures[slots.x].Load(int3(p, 0)), textures[slots.y].Load(int3(p, 0)).x, arrays[slots.z].Load(int4(p, 0, 0)),
        buffers[slots.w].Load(4 * (p.x + 16 * p.y)), textures[slots2.x].Load(int3(p, 0)));
}
)";

// D3D11 views of five kinds read by compute. Reference: bound through D3D11 (twice). Layer: their
// NativeDescriptor copies in a ring table, then the same views in bindless slots (WriteBindless,
// WriteBindlessBuffer), with RequireView putting only the viewed subresources into shader read.
void ScenarioViewDescriptorCopy(Gpu& g, Result& r)
{
    // Colours: RGBA8 with initial data.
    u32 colors[kViewSize * kViewSize];
    for (u32 y = 0; y < kViewSize; ++y)
        for (u32 x = 0; x < kViewSize; ++x)
            colors[y * kViewSize + x] = (x * 16) | ((y * 16) << 8) | (((x ^ y) * 16) << 16) | (0xC0u << 24);
    D3D11_SUBRESOURCE_DATA colorData{colors, kViewSize * 4, 0};
    auto colorTex = MakeTexture2D(g, r, Desc2D(kViewSize, kViewSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE), &colorData);

    // Mip 1 of a 32x32 R32F chain.
    std::vector<float> mip0(4 * kViewSize * kViewSize), mip1(kViewSize * kViewSize);
    for (size_t i = 0; i < mip0.size(); ++i)
        mip0[i] = 1000.0f + float(i);
    for (u32 i = 0; i < kViewSize * kViewSize; ++i)
        mip1[i] = float(i % kViewSize) * 0.25f + float(i / kViewSize) * 0.0625f;
    const D3D11_SUBRESOURCE_DATA mipData[2] = {{mip0.data(), 2 * kViewSize * 4, 0}, {mip1.data(), kViewSize * 4, 0}};
    auto mipTex = MakeTexture2D(g, r, Desc2D(2 * kViewSize, 2 * kViewSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, 2), mipData);

    // Slice 1 of a two-slice RGBA32F array.
    std::vector<float> slices(2 * kViewSize * kViewSize * 4);
    for (size_t i = 0; i < slices.size(); ++i)
        slices[i] = i < slices.size() / 2 ? -7.0f : float(i % 97) / 64.0f;
    const D3D11_SUBRESOURCE_DATA sliceData[2] = {{slices.data(), kViewSize * 16, 0}, {slices.data() + slices.size() / 2, kViewSize * 16, 0}};
    auto sliceTex = MakeTexture2D(g, r, Desc2D(kViewSize, kViewSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, 1, 2), sliceData);

    // A raw buffer.
    u32 words[kViewSize * kViewSize];
    for (u32 i = 0; i < kViewSize * kViewSize; ++i)
        words[i] = i * 37 + 11;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(words);
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const D3D11_SUBRESOURCE_DATA wordData{words, 0, 0};
    ComPtr<ID3D11Buffer> wordBuffer;
    r.Ok(g.dev->CreateBuffer(&bufferDesc, &wordData, &wordBuffer), "CreateBuffer(raw)");

    // A render target drawn by D3D11: cleared, then a red quad.
    auto drawnTex = MakeTexture2D(g, r, Desc2D(kViewSize, kViewSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    auto drawnRtv = drawnTex ? MakeRtv(g, r, drawnTex.Get()) : nullptr;
    auto cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
    auto quadVs = MakeVs(g, r, kQuadHlsl, "QuadVs");
    auto colorPs = MakePs(g, r, kQuadHlsl, "ColorPs");

    auto result = MakeTexture2D(g, r, Desc2D(kViewSize, kViewSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE));
    auto result2 = MakeTexture2D(g, r, Desc2D(kViewSize, kViewSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();

    ComPtr<ID3D11ShaderResourceView> views[5];
    views[0] = MakeSrv(g, r, colorTex.Get());
    D3D11_SHADER_RESOURCE_VIEW_DESC mipView{};
    mipView.Format = DXGI_FORMAT_R32_FLOAT;
    mipView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    mipView.Texture2D = {1, 1};
    views[1] = MakeSrv(g, r, mipTex.Get(), &mipView);
    D3D11_SHADER_RESOURCE_VIEW_DESC sliceView{};
    sliceView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sliceView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sliceView.Texture2DArray = {0, 1, 1, 1};
    views[2] = MakeSrv(g, r, sliceTex.Get(), &sliceView);
    D3D11_SHADER_RESOURCE_VIEW_DESC rawView{};
    rawView.Format = DXGI_FORMAT_R32_TYPELESS;
    rawView.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    rawView.BufferEx = {0, kViewSize * kViewSize, D3D11_BUFFEREX_SRV_FLAG_RAW};
    views[3] = MakeSrv(g, r, wordBuffer.Get(), &rawView);
    views[4] = MakeSrv(g, r, drawnTex.Get());
    ComPtr<ID3D11UnorderedAccessView> resultUav, result2Uav;
    r.Ok(g.dev->CreateUnorderedAccessView(result.Get(), nullptr, &resultUav), "CreateUnorderedAccessView(result)");
    r.Ok(g.dev->CreateUnorderedAccessView(result2.Get(), nullptr, &result2Uav), "CreateUnorderedAccessView(result2)");
    REQUIRE_OK();

    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    g.ctx->ClearRenderTargetView(drawnRtv.Get(), clear);
    const QuadConstants q = QuadPx(4, 2, 12, 10, kViewSize, kViewSize, 1, 0, 0, 1);
    Upload(g, cb.Get(), &q, sizeof(q));
    g.ctx->OMSetRenderTargets(1, drawnRtv.GetAddressOf(), nullptr);
    SetViewport(g, 0, 0, float(kViewSize), float(kViewSize));
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(quadVs.Get(), nullptr, 0);
    g.ctx->PSSetShader(colorPs.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
    g.ctx->Draw(6, 0);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    if (!g.layer)
    {
        auto cs = MakeCs(g, r, kViewsHlsl, "TableCs");
        REQUIRE_OK();
        ID3D11ShaderResourceView* srvs[5] = {views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get(), views[4].Get()};
        g.ctx->CSSetShader(cs.Get(), nullptr, 0);
        g.ctx->CSSetShaderResources(0, 5, srvs);
        for (ID3D11UnorderedAccessView* uav : {resultUav.Get(), result2Uav.Get()})
        {
            g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            g.ctx->Dispatch(kViewSize / 8, kViewSize / 8, 1);
        }
        ID3D11ShaderResourceView* noSrvs[5]{};
        ID3D11UnorderedAccessView* noUav = nullptr;
        g.ctx->CSSetShaderResources(0, 5, noSrvs);
        g.ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    else
    {
        const D3D12_DESCRIPTOR_RANGE1 inputs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
        const D3D12_DESCRIPTOR_RANGE1 output{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
        const D3D12_ROOT_PARAMETER1 tableParams[2] = {TableParam(&inputs), TableParam(&output)};
        ID3D12RootSignature* tableRoot = nullptr;
        ID3D12PipelineState* table = NativePipeline(g, r, tableParams, 2, kViewsHlsl, "TableCs", "cs_6_0", tableRoot);

        const D3D12_DESCRIPTOR_RANGE1 bindless[3] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 100, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 101, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 102, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0}};
        D3D12_ROOT_PARAMETER1 bindlessParams[5] = {{}, TableParam(&bindless[0]), TableParam(&bindless[1]), TableParam(&bindless[2]),
            TableParam(&output)};
        bindlessParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bindlessParams[0].Constants = {0, 0, 8};
        bindlessParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ID3D12RootSignature* bindlessRoot = nullptr;
        ID3D12PipelineState* slotted = NativePipeline(g, r, bindlessParams, 5, kViewsBindlessHlsl, "BindlessCs", "cs_6_0", bindlessRoot);
        const u32 base = xrd3d12::AllocateBindless(g.dev, 5);
        r.Check(base != xrd3d12::kBindlessInvalid, "AllocateBindless failed");
        REQUIRE_OK();
        xrd3d12::WriteBindless(g.dev, base + 0, views[0].Get());
        xrd3d12::WriteBindless(g.dev, base + 1, views[1].Get());
        xrd3d12::WriteBindless(g.dev, base + 2, views[2].Get());
        xrd3d12::WriteBindlessBuffer(g.dev, base + 3, wordBuffer.Get());
        xrd3d12::WriteBindless(g.dev, base + 4, views[4].Get());

        xrd3d12::NativeScope scope;
        if (r.Check(xrd3d12::BeginNative(g.ctx, scope), "BeginNative refused"))
        {
            for (const ComPtr<ID3D11ShaderResourceView>& view : views)
                xrd3d12::RequireView(g.ctx, view.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            xrd3d12::RequireView(g.ctx, resultUav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            xrd3d12::RequireView(g.ctx, result2Uav.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D11View* const tableViews[7] = {views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get(), views[4].Get(),
                resultUav.Get(), result2Uav.Get()};
            const xrd3d12::DescriptorSpan span = CopyViews(g, g.ctx, tableViews, 7);
            if (r.Check(span.count == 7, "AllocateDescriptors failed"))
            {
                scope.list->SetComputeRootSignature(tableRoot);
                scope.list->SetPipelineState(table);
                scope.list->SetComputeRootDescriptorTable(0, At(span, 0));
                scope.list->SetComputeRootDescriptorTable(1, At(span, 5));
                scope.list->Dispatch(kViewSize / 8, kViewSize / 8, 1);

                const u32 slots[8] = {base + 0, base + 1, base + 2, base + 3, base + 4, 0, 0, 0};
                scope.list->SetComputeRootSignature(bindlessRoot);
                scope.list->SetPipelineState(slotted);
                scope.list->SetComputeRoot32BitConstants(0, 8, slots, 0);
                for (UINT param = 1; param <= 3; ++param)
                    scope.list->SetComputeRootDescriptorTable(param, scope.bindless);
                scope.list->SetComputeRootDescriptorTable(4, At(span, 6));
                scope.list->Dispatch(kViewSize / 8, kViewSize / 8, 1);
            }
            xrd3d12::EndNative(g.ctx, xrd3d12::TouchedComputeRoot | xrd3d12::TouchedPipeline);
        }
        xrd3d12::FreeBindless(g.dev, base, 5);
    }
    r.images.push_back(Read2D(g, r, result.Get(), 0, "D3D11 views through ring tables", 1e-5));
    r.images.push_back(Read2D(g, r, result2.Get(), 0, "D3D11 views through bindless slots", 1e-5));
}
} // namespace

const std::vector<Scenario>& RtScenarios()
{
    static const std::vector<Scenario> list = {
        {"native raytracing: engine VB/IB, compacted BLAS, TLAS masks, inline RayQuery vs CPU", ScenarioNativeRaytracing, true},
        {"view descriptor copy: D3D11 views in ring tables and bindless slots, native compute", ScenarioViewDescriptorCopy, false},
    };
    return list;
}
} // namespace layertest
