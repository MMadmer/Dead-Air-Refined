// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The extensions both native renderers offer behind their D3D11 device (xrD3D12 for R5, xrVulkan for R6)
// under one name: render code written against xrnative:: runs on DirectX 12 and Vulkan alike. Everything
// that needs the API's own objects (DXR, the SDK integrations, DirectStorage) keeps its XR_D3D12 code.
#pragma once

#if defined(XR_NATIVE)
// The Vulkan module uses xrD3D12's DXC service and reflection as well.
#include "Layers/xrRenderDX12/d12_api.h"
#if defined(XR_VULKAN)
#include "Layers/xrRenderVK/vk_api.h"
#endif

namespace xrnative
{
// D3D12_SHADING_RATE_COMBINER values; the Vulkan layer takes the same numbers.
enum ShadingRateCombiner : unsigned
{
    CombinerPassthrough = 0,
    CombinerOverride = 1,
    CombinerMin = 2,
    CombinerMax = 3,
    CombinerSum = 4,
};

struct Caps
{
    bool vrs = false;              // per-draw rate and a rate image
    unsigned vrsTile = 0;          // pixels per rate image texel
    bool meshShaders = false;
    bool rayQuery = false;
    bool asyncCompute = false;
    bool waveOps = false;
    unsigned waveLanesMin = 0, waveLanesMax = 0;
    bool float16 = false;
};

inline bool GetCaps(ID3D11Device* device, Caps& out)
{
    out = Caps{};
#if defined(XR_D3D12)
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(device);
    if (!caps)
        return false;
    out.vrs = caps->vrsTier >= 2;
    out.vrsTile = caps->vrsTileSize;
    out.meshShaders = caps->meshShaderTier >= 10;
    out.rayQuery = caps->raytracingTier >= 11;
    out.asyncCompute = caps->asyncCompute;
    out.waveOps = caps->waveOps;
    out.waveLanesMin = caps->waveLanesMin;
    out.waveLanesMax = caps->waveLanesMax;
    out.float16 = caps->native16Bit;
#else
    const xrvk::NativeCaps* caps = xrvk::GetNativeCaps(device);
    if (!caps)
        return false;
    out.vrs = caps->shadingRate;
    out.vrsTile = caps->shadingRateTexel;
    out.meshShaders = caps->meshShaders;
    out.rayQuery = caps->rayQuery;
    out.asyncCompute = caps->asyncCompute;
    out.waveOps = caps->subgroupSize != 0;
    out.waveLanesMin = caps->subgroupSizeMin ? caps->subgroupSizeMin : caps->subgroupSize;
    out.waveLanesMax = caps->subgroupSizeMax ? caps->subgroupSizeMax : caps->subgroupSize;
    out.float16 = caps->float16;
#endif
    return true;
}

inline void DrawIndexedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count = nullptr, UINT countOffset = 0)
{
#if defined(XR_D3D12)
    xrd3d12::DrawIndexedIndirectCount(context, args, offset, maxCount, count, countOffset);
#else
    xrvk::DrawIndexedIndirectCount(context, args, offset, maxCount, count, countOffset);
#endif
}

inline void DrawInstancedIndirectCount(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset, UINT maxCount,
    ID3D11Buffer* count = nullptr, UINT countOffset = 0)
{
#if defined(XR_D3D12)
    xrd3d12::DrawInstancedIndirectCount(context, args, offset, maxCount, count, countOffset);
#else
    xrvk::DrawInstancedIndirectCount(context, args, offset, maxCount, count, countOffset);
#endif
}

inline bool DispatchMesh(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
#if defined(XR_D3D12)
    return xrd3d12::DispatchMesh(context, x, y, z);
#else
    return xrvk::DispatchMesh(context, x, y, z);
#endif
}

// `base` is a D3D12_SHADING_RATE value, `combiner` how the rate image (null = none) combines with it.
inline void SetShadingRate(ID3D11DeviceContext* context, unsigned base, unsigned combiner, ID3D11Resource* image)
{
#if defined(XR_D3D12)
    xrd3d12::SetShadingRate(context, D3D12_SHADING_RATE(base), D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
        D3D12_SHADING_RATE_COMBINER(combiner), image);
#else
    xrvk::SetShadingRate(context, base, image ? combiner : CombinerPassthrough, image);
#endif
}

inline void UavBarrier(ID3D11DeviceContext* context, ID3D11Resource* resource = nullptr)
{
#if defined(XR_D3D12)
    xrd3d12::UavBarrier(context, resource);
#else
    xrvk::UavBarrier(context, resource);
#endif
}

inline bool QueryVideoMemory(ID3D11Device* device, UINT64& budget, UINT64& usage)
{
#if defined(XR_D3D12)
    return xrd3d12::QueryVideoMemory(device, budget, usage);
#else
    return xrvk::QueryVideoMemory(device, budget, usage);
#endif
}

// The reflection of an engine shader object (DXBC, DXIL or the Vulkan container's DXIL twin).
inline HRESULT Reflect(const void* bytecode, size_t size, REFIID riid, void** reflection)
{
#if defined(XR_VULKAN)
    xrvk::ShaderContainerView view;
    if (xrvk::ParseShaderContainer(bytecode, size, view) && view.dxil)
        return xrd3d12::Reflect(view.dxil, view.dxilSize, riid, reflection);
#endif
    return xrd3d12::Reflect(bytecode, size, riid, reflection);
}

// The DXIL (or DXBC) inside an engine shader object: the input signature and disassembly read it.
inline void ShaderBytecode(const void*& bytecode, size_t& size)
{
#if defined(XR_VULKAN)
    xrvk::ShaderContainerView view;
    if (xrvk::ParseShaderContainer(bytecode, size, view) && view.dxil)
    {
        bytecode = view.dxil;
        size = view.dxilSize;
    }
#else
    (void)bytecode;
    (void)size;
#endif
}
} // namespace xrnative
#endif
