// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Layer scenarios of the `ds` lane (placed texture heaps: dstorage.md 3.6, 9). Scenarios() appends this list.
#include "harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace layertest
{
namespace
{
#define REQUIRE_OK()             \
    if (!r.failures.empty())     \
        return;

constexpr DXGI_FORMAT kRgba8 = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr u32 kTarget = 128;

// misc.z = mip: every texel of the mip, tiled over the target.
const char* LoadSource()
{
    static const std::string source = std::string(kQuadHlsl) + R"(
Texture2D<float4> t0 : register(t0);

float4 LoadPs(VsOut i) : SV_Target
{
    uint mip = uint(misc.z), w, h, levels;
    t0.GetDimensions(mip, w, h, levels);
    return t0.Load(int3(uint2(i.pos.xy) % uint2(w, h), mip));
}
)";
    return source.c_str();
}

// Deterministic texel bytes of mip `mip` of a texture: rows of `rowBytes`, `rows` rows.
std::vector<u8> Pattern(u32 seed, u32 mip, size_t rowBytes, u32 rows)
{
    std::vector<u8> bytes(rowBytes * rows);
    u32 x = seed * 2654435761u + mip * 40503u + 1;
    for (u8& b : bytes)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = u8(x);
    }
    return bytes;
}

struct TextureData
{
    std::vector<std::vector<u8>> mips;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
};

// RGBA8 or BC1 texels for every mip (and slice) of `desc`.
TextureData MakeData(const D3D11_TEXTURE2D_DESC& desc, u32 seed)
{
    TextureData data;
    for (u32 slice = 0; slice < desc.ArraySize; ++slice)
    {
        for (u32 mip = 0; mip < desc.MipLevels; ++mip)
        {
            const u32 w = (std::max)(1u, desc.Width >> mip), h = (std::max)(1u, desc.Height >> mip);
            const bool bc = desc.Format == DXGI_FORMAT_BC1_UNORM;
            const size_t row = bc ? size_t((std::max)(1u, (w + 3) / 4)) * 8 : size_t(w) * 4;
            const u32 rows = bc ? (std::max)(1u, (h + 3) / 4) : h;
            data.mips.push_back(Pattern(seed + slice * 17, mip, row, rows));
            data.init.push_back({data.mips.back().data(), UINT(row), 0});
        }
    }
    // Pointers are taken after every vector exists (push_back may move them).
    for (size_t i = 0; i < data.init.size(); ++i)
        data.init[i].pSysMem = data.mips[i].data();
    return data;
}

bool IsPlaced(ID3D11Resource* resource)
{
    const xrd3d12::Resource* r = xrd3d12::Resource::From(resource);
    return r && r->placement;
}

struct LoadRig
{
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;

    void Init(Gpu& g, Result& r)
    {
        target = MakeTexture2D(g, r, Desc2D(kTarget, kTarget, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        vs = MakeVs(g, r, LoadSource(), "FullscreenVs");
        ps = MakePs(g, r, LoadSource(), "LoadPs");
        cb = MakeBuffer(g, r, sizeof(QuadConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER);
        if (target)
            rtv = MakeRtv(g, r, target.Get());
    }

    // Draws mip `mip` of `srv` over the target on `dc` (immediate or deferred).
    void Draw(ID3D11DeviceContext* dc, ID3D11ShaderResourceView* srv, u32 mip)
    {
        const float clear[4] = {0, 0, 0, 0};
        dc->ClearRenderTargetView(rtv.Get(), clear);
        dc->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, float(kTarget), float(kTarget), 0, 1};
        dc->RSSetViewports(1, &vp);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(vs.Get(), nullptr, 0);
        dc->PSSetShader(ps.Get(), nullptr, 0);
        QuadConstants q{};
        q.misc[2] = float(mip);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(dc->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &q, sizeof(q));
            dc->Unmap(cb.Get(), 0);
        }
        dc->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        dc->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        dc->PSSetShaderResources(0, 1, &srv);
        dc->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        dc->PSSetShaderResources(0, 1, &none);
    }
};

// 1. Textures with initial data live in the shared heaps (RGBA8 with mips, BC1, a 4 KB-placed small one,
// a cube); freed ranges come back after the frame fence and are reused.
void ScenarioPlacedWithData(Gpu& g, Result& r)
{
    LoadRig rig;
    rig.Init(g, r);
    REQUIRE_OK();

    struct Case
    {
        const char* label;
        D3D11_TEXTURE2D_DESC desc;
    };
    D3D11_TEXTURE2D_DESC mipped = Desc2D(256, 256, kRgba8, D3D11_BIND_SHADER_RESOURCE, 9);
    D3D11_TEXTURE2D_DESC bc1 = Desc2D(128, 128, DXGI_FORMAT_BC1_UNORM, D3D11_BIND_SHADER_RESOURCE, 6);
    D3D11_TEXTURE2D_DESC tiny = Desc2D(16, 16, kRgba8, D3D11_BIND_SHADER_RESOURCE);
    D3D11_TEXTURE2D_DESC defaultUsage = Desc2D(64, 32, kRgba8, D3D11_BIND_SHADER_RESOURCE, 2);
    mipped.Usage = bc1.Usage = tiny.Usage = D3D11_USAGE_IMMUTABLE;
    const Case cases[] = {{"rgba8 256 x9 mips", mipped}, {"bc1 128 x6 mips", bc1}, {"rgba8 16 (4 KB placement)", tiny},
        {"default usage 64x32 x2 mips", defaultUsage}};

    for (int round = 0; round < 2; ++round)
    {
        for (const Case& c : cases)
        {
            TextureData data = MakeData(c.desc, u32(round * 31 + c.desc.Width + c.desc.MipLevels));
            auto texture = MakeTexture2D(g, r, c.desc, data.init.data());
            REQUIRE_OK();
            if (g.layer)
                r.Check(IsPlaced(texture.Get()), "%s: not placed in a texture heap", c.label);
            auto srv = MakeSrv(g, r, texture.Get());
            REQUIRE_OK();
            for (u32 mip = 0; mip < c.desc.MipLevels; mip += 2)
            {
                rig.Draw(g.ctx, srv.Get(), mip);
                char label[128];
                std::snprintf(label, sizeof(label), "round %d %s mip %u", round, c.label, mip);
                r.images.push_back(Read2D(g, r, rig.target.Get(), 0, label));
            }
        }
        // The textures of round 0 went: their ranges return after the fence and round 1 reuses them.
        EndFrame(g);
        WaitIdle(g);
        EndFrame(g);
    }

    // A cube with data.
    D3D11_TEXTURE2D_DESC cube = Desc2D(32, 32, kRgba8, D3D11_BIND_SHADER_RESOURCE, 1, 6);
    cube.Usage = D3D11_USAGE_IMMUTABLE;
    cube.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    TextureData cubeData = MakeData(cube, 99);
    auto cubeTexture = MakeTexture2D(g, r, cube, cubeData.init.data());
    REQUIRE_OK();
    if (g.layer)
        r.Check(IsPlaced(cubeTexture.Get()), "cube: not placed in a texture heap");
    for (u32 face = 0; face < 6; ++face)
    {
        char label[64];
        std::snprintf(label, sizeof(label), "cube face %u", face);
        r.images.push_back(Read2D(g, r, cubeTexture.Get(), face, label));
    }

    // Not placeable: render targets, and DEFAULT textures without data outside a streamed scope.
    auto rt = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    auto empty = MakeTexture2D(g, r, Desc2D(64, 64, kRgba8, D3D11_BIND_SHADER_RESOURCE));
    REQUIRE_OK();
    if (g.layer)
    {
        r.Check(!IsPlaced(rt.Get()), "a render target was placed");
        r.Check(!IsPlaced(empty.Get()), "a texture without data was placed outside a streamed scope");
        xrd3d12::TextureHeapStats stats;
        xrd3d12::GetTextureHeapStats(g.dev, stats);
        r.Check(stats.heaps >= 1 && stats.placed >= 1 && stats.placedTotal >= stats.placed, "heap stats: %u heaps, %u placed",
            stats.heaps, stats.placed);
    }
    g.ctx->ClearState();
}

// 2. A streamed texture (placed without data, left in COMMON) written by a native copy-queue copy and first
// sampled from a deferred list. D3D11 writes the same texels with UpdateSubresource.
void ScenarioStreamedCopyQueue(Gpu& g, Result& r)
{
    LoadRig rig;
    rig.Init(g, r);
    REQUIRE_OK();

    const D3D11_TEXTURE2D_DESC desc = Desc2D(128, 64, kRgba8, D3D11_BIND_SHADER_RESOURCE, 4);
    TextureData data = MakeData(desc, 7);
    ComPtr<ID3D11Texture2D> texture;
    {
        const bool previous = g.layer ? xrd3d12::SetStreamedTextures(true) : false;
        texture = MakeTexture2D(g, r, desc);
        if (g.layer)
            xrd3d12::SetStreamedTextures(previous);
    }
    REQUIRE_OK();

    if (g.layer)
    {
        r.Check(IsPlaced(texture.Get()), "streamed texture: not placed");
        ID3D12Device* device = xrd3d12::NativeDevice(g.dev);
        ID3D12Resource* dst = xrd3d12::NativeResource(texture.Get());
        if (!r.Check(device && dst, "native device or resource missing"))
            return;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        r.Ok(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue(COPY)");
        r.Ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        REQUIRE_OK();
        r.Ok(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "CreateCommandList");
        r.Ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        REQUIRE_OK();

        const D3D12_RESOURCE_DESC rd = dst->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[4];
        UINT rows[4];
        UINT64 rowSizes[4];
        UINT64 total = 0;
        device->GetCopyableFootprints(&rd, 0, 4, 0, fp, rows, rowSizes, &total);
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        r.Ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(&upload)),
            "upload buffer");
        REQUIRE_OK();
        u8* cpu = nullptr;
        r.Ok(upload->Map(0, nullptr, reinterpret_cast<void**>(&cpu)), "Map");
        REQUIRE_OK();
        for (u32 mip = 0; mip < 4; ++mip)
        {
            for (u32 y = 0; y < rows[mip]; ++y)
                std::memcpy(cpu + fp[mip].Offset + u64(y) * fp[mip].Footprint.RowPitch,
                    data.mips[mip].data() + size_t(y) * data.init[mip].SysMemPitch, size_t(rowSizes[mip]));
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            to.pResource = dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = mip;
            from.pResource = upload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = fp[mip];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        upload->Unmap(0, nullptr);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), 1);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence->GetCompletedValue() < 1 && event)
        {
            fence->SetEventOnCompletion(1, event);
            WaitForSingleObject(event, 10000);
        }
        if (event)
            CloseHandle(event);
        r.Check(fence->GetCompletedValue() >= 1, "copy queue did not finish");
    }
    else
    {
        for (u32 mip = 0; mip < 4; ++mip)
            g.ctx->UpdateSubresource(texture.Get(), mip, nullptr, data.mips[mip].data(), data.init[mip].SysMemPitch, 0);
    }
    REQUIRE_OK();

    auto srv = MakeSrv(g, r, texture.Get());
    ComPtr<ID3D11DeviceContext> def;
    r.Ok(g.dev->CreateDeferredContext(0, &def), "CreateDeferredContext");
    REQUIRE_OK();
    for (u32 mip = 0; mip < 4; ++mip)
    {
        // The first use of the texture is in a deferred list (its entry requirement leaves COMMON).
        rig.Draw(def.Get(), srv.Get(), mip);
        ComPtr<ID3D11CommandList> commands;
        r.Ok(def->FinishCommandList(FALSE, &commands), "FinishCommandList");
        REQUIRE_OK();
        g.ctx->ExecuteCommandList(commands.Get(), FALSE);
        char label[64];
        std::snprintf(label, sizeof(label), "streamed texture mip %u from a deferred list", mip);
        r.images.push_back(Read2D(g, r, rig.target.Get(), 0, label));
    }
    g.ctx->ClearState();
}
} // namespace

const std::vector<Scenario>& TexheapScenarios()
{
    static const std::vector<Scenario> list = {
        {"texheap: textures with data placed in shared heaps (mips, BC1, 4 KB, cube), ranges reused", ScenarioPlacedWithData, false},
        {"texheap: streamed texture written by a native copy queue, first sampled from a deferred list", ScenarioStreamedCopyQueue, false},
    };
    return list;
}
} // namespace layertest
