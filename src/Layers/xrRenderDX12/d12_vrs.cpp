// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Variable rate shading state of the xrD3D12 contexts (docs/dead-air/dx12-features/vrs.md 3.4;
// lane `vrs`). Reached through hooks in the context and the device (d12_layer.h, [dx12:vrs]).
//
// SetShadingRate is context state like any D3D11 state: kept until changed, cleared by ClearState,
// ExecuteCommandList(FALSE) and FinishCommandList(FALSE) (ReleaseBindings), never seen by native
// sections (BeginNative hands the list out at 1x1). Every draw of a context that ever had a rate
// compares the rate the draw needs with what the open list has and sets only a difference. The rate
// image is dropped for a draw that would misuse it: bound as an output or read by a shader of the
// draw, or a render target the image does not cover within one tile (display-size and shadow-map
// targets); the draw then shades as the combiners give it without an image.
#include "d12_layer.h"

#include <algorithm>
#include <bit>
#include <new>

namespace xrd3d12
{
struct ContextVrs
{
    // What SetShadingRate asked for.
    D3D12_SHADING_RATE base = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER combiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    ID3D11Resource* image = nullptr; // AddRef'd
    Resource* imageResource = nullptr;
    bool active = false; // anything but 1x1 without an image

    // What the open list was given last; valid while listKnown (a native section or a heap change
    // may have replaced it).
    bool listKnown = false;
    D3D12_SHADING_RATE listBase = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER listCombiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    ID3D12Resource* listImage = nullptr;
};

namespace
{
bool IsDefault(D3D12_SHADING_RATE base, const D3D12_SHADING_RATE_COMBINER* combiners, const void* image)
{
    return base == D3D12_SHADING_RATE_1X1 && !image && combiners[0] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
}

// 1x1 without an image on the open list, where the device can set rates at all.
void ResetListRate(Context& c)
{
    ID3D12GraphicsCommandList5* list5 = c.List5();
    if (!c.applied.vrsListNonDefault || !list5 || !c.device()->caps.vrsTier)
        return;
    list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
    if (c.device()->caps.vrsTier >= 2)
        list5->RSSetShadingRateImage(nullptr);
    c.applied.vrsListNonDefault = false;
    if (c.vrs)
        c.vrs->listKnown = false;
    c.device()->Count(Device::StatShadingRate);
}

void ReleaseImage(ContextVrs& v)
{
    if (v.image)
        v.image->Release();
    v.image = nullptr;
    v.imageResource = nullptr;
}

// Width and height of the subresource a render target or depth view writes.
bool ViewSize(const View* v, u32& width, u32& height)
{
    Resource* r = v->resource ? v->resource->Actual() : nullptr;
    if (!r || r->IsBuffer())
        return false;
    const u32 mip = v->range.firstMip;
    width = (std::max)(u32(r->nativeDesc.Width >> mip), 1u);
    height = (std::max)(u32(r->nativeDesc.Height >> mip), 1u);
    return true;
}

bool ReadByGraphicsStage(const Context& c, const Resource* image)
{
    for (u32 s = 0; s < u32(Stage::CS); ++s)
    {
        const StageBindings& sb = c.stages[s];
        if (!sb.shader)
            continue;
        for (u32 m = sb.shader->bindings.srvMask; m; m &= m - 1)
        {
            const u32 slot = u32(std::countr_zero(m));
            if (slot >= kMaxSRVs || !sb.srv[slot])
                continue;
            const View* v = static_cast<const View*>(static_cast<ShaderResourceView*>(sb.srv[slot]));
            if (v->resource == image)
                return true;
        }
    }
    return false;
}

// Whether the draw about to be recorded may use the rate image (safety rules, vrs.md 3.4).
bool ImageUsable(Context& c, Resource* image)
{
    static constexpr char kOutput[] = "shading rate image bound as an output or read by the draw: ignored for the draw";
    static constexpr char kSize[] = "shading rate image does not cover the render target within one tile: ignored for the draw";
    static constexpr char kNoTarget[] = "shading rate image with neither a render target nor a depth target: ignored for the draw";
    SubRange all;
    all.mipCount = image->mipLevels;
    all.sliceCount = image->arraySize;
    all.planeCount = image->planeCount;
    if (c.IsBoundAsOutput(image, all) || ReadByGraphicsStage(c, image))
    {
        LogOnce(kOutput, kOutput);
        return false;
    }
    u32 width = 0, height = 0;
    const bool sized = c.numRTVs && c.rtvs[0] ? ViewSize(static_cast<RenderTargetView*>(c.rtvs[0]), width, height)
                                                : c.dsv && ViewSize(static_cast<DepthStencilView*>(c.dsv), width, height);
    if (!sized)
    {
        LogOnce(kNoTarget, kNoTarget);
        return false;
    }
    const u32 tile = c.device()->caps.vrsTileSize;
    const u64 imageWidth = image->nativeDesc.Width;
    const u32 imageHeight = image->nativeDesc.Height;
    if (!tile || (width + tile - 1) / tile != imageWidth || (height + tile - 1) / tile != imageHeight)
    {
        LogOnce(kSize, kSize);
        return false;
    }
    return true;
}
} // namespace

void SetShadingRate(ID3D11DeviceContext* context, D3D12_SHADING_RATE base, D3D12_SHADING_RATE_COMBINER perPrimitive,
    D3D12_SHADING_RATE_COMBINER image, ID3D11Resource* rateImage)
{
    if (!context)
        return;
    auto* c = static_cast<Context*>(context);
    const NativeCaps& caps = c->device()->caps;
    if (caps.vrsTier < 2)
    {
        static constexpr char kFormat[] = "SetShadingRate below VRS tier 2 ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    if (c->m_inNative)
    {
        static constexpr char kFormat[] = "SetShadingRate inside a native section ignored";
        LogOnce(kFormat, kFormat);
        return;
    }
    // 2x4, 4x2 and 4x4 need the additional rates.
    if (!caps.vrsAdditionalRates &&
        (base == D3D12_SHADING_RATE_2X4 || base == D3D12_SHADING_RATE_4X2 || base == D3D12_SHADING_RATE_4X4))
        base = D3D12_SHADING_RATE_2X2;
    Resource* imageResource = nullptr;
    if (rateImage)
    {
        imageResource = Resource::From(rateImage);
        if (!imageResource || imageResource->dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
            imageResource->format != DXGI_FORMAT_R8_UINT || !imageResource->native || imageResource->nativeDesc.SampleDesc.Count > 1)
        {
            static constexpr char kFormat[] = "SetShadingRate: the rate image is not a single-sample R8_UINT texture; ignored";
            LogOnce(kFormat, kFormat);
            imageResource = nullptr;
            rateImage = nullptr;
        }
    }
    const D3D12_SHADING_RATE_COMBINER combiners[2]{perPrimitive, image};
    if (IsDefault(base, combiners, rateImage))
    {
        if (ContextVrs* v = c->vrs)
        {
            ReleaseImage(*v);
            v->base = D3D12_SHADING_RATE_1X1;
            v->combiners[0] = v->combiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
            v->active = false;
        }
        return;
    }
    if (!c->vrs)
    {
        c->vrs = new (std::nothrow) ContextVrs;
        if (!c->vrs)
            return;
    }
    ContextVrs& v = *c->vrs;
    if (rateImage != v.image)
    {
        if (rateImage)
            rateImage->AddRef();
        ReleaseImage(v);
        v.image = rateImage;
        v.imageResource = imageResource;
    }
    v.base = base;
    v.combiners[0] = perPrimitive;
    v.combiners[1] = image;
    v.active = true;
}

// Nothing device-wide: the capabilities are NativeCaps (d12_native.cpp), the state is per context.
bool Device::VrsInit()
{
    return true;
}

void Device::VrsShutdown()
{
}

void Context::VrsApply(ID3D12GraphicsCommandList* /*list*/)
{
    // The list may have lost what was set (heaps, a native section); VrsRequire sets it again.
    if (vrs)
        vrs->listKnown = false;
    else
        ResetListRate(*this);
    applied.vrsDirty = false;
}

void Context::VrsRequire()
{
    ContextVrs& v = *vrs;
    if (!v.active)
    {
        if (applied.vrsListNonDefault)
            ResetListRate(*this);
        return;
    }
    ID3D12GraphicsCommandList5* list5 = List5();
    if (!list5)
        return;

    D3D12_SHADING_RATE base = v.base;
    D3D12_SHADING_RATE_COMBINER combiners[2]{v.combiners[0], v.combiners[1]};
    Resource* image = v.imageResource && combiners[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH && ImageUsable(*this, v.imageResource)
        ? v.imageResource
        : nullptr;
    if (!image)
    {
        // Without an image the image rate is 1x1: OVERRIDE and MIN give 1x1, the others the base.
        if (combiners[1] == D3D12_SHADING_RATE_COMBINER_OVERRIDE || combiners[1] == D3D12_SHADING_RATE_COMBINER_MIN)
            base = D3D12_SHADING_RATE_1X1;
        combiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    }
    if (IsDefault(base, combiners, image))
    {
        if (applied.vrsListNonDefault)
            ResetListRate(*this);
        return;
    }

    ID3D12Resource* native = image ? image->native : nullptr;
    const bool same = applied.vrsListNonDefault && v.listKnown && v.listBase == base && v.listCombiners[0] == combiners[0] &&
        v.listCombiners[1] == combiners[1] && v.listImage == native;
    if (!same)
    {
        list5->RSSetShadingRate(base, combiners);
        list5->RSSetShadingRateImage(native);
        v.listKnown = true;
        v.listBase = base;
        v.listCombiners[0] = combiners[0];
        v.listCombiners[1] = combiners[1];
        v.listImage = native;
        applied.vrsListNonDefault = true;
        device()->Count(Device::StatShadingRate);
    }
    if (image)
    {
        // The immediate context reads the tracked state; a deferred one records a first requirement.
        constexpr D3D12_RESOURCE_STATES kSource = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        if (IsDeferred() || !image->states.uniform || image->states.all != kSource)
            RequireAll(image, kSource);
    }
}

void Context::VrsReset(bool keepState)
{
    if (keepState)
    {
        // InvalidateApplied: the state stays, the list may have another rate now.
        vrs->listKnown = false;
        return;
    }
    // ReleaseBindings (ClearState, ExecuteCommandList/FinishCommandList(FALSE)): back to 1x1.
    ReleaseImage(*vrs);
    vrs->base = D3D12_SHADING_RATE_1X1;
    vrs->combiners[0] = vrs->combiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    vrs->active = false;
}

void Context::VrsNativeReset()
{
    ResetListRate(*this);
    // The context's rate comes back at the next draw.
    applied.vrsDirty = true;
}

void Context::VrsDestroy()
{
    ReleaseImage(*vrs);
    delete vrs;
    vrs = nullptr;
}
} // namespace xrd3d12
