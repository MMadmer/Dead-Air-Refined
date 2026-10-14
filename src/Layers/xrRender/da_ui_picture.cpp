// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The interface drawn into a picture for the loading screen's own presenter (xrEngine/da_loading_presenter.h):
// what the game draws of a layer goes into a target of the screen's size twice, over black and over white, and
// the two give back each pixel's colour and alpha - whatever blending the interface used.

#include "stdafx.h"
#include "da_ui_picture.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
struct PictureTargets
{
    ID3DTexture2D* target{};
    ID3DRenderTargetView* view{};
    u32 width{}, height{};
    ID3DTexture2D* staging[2]{};
    u32 stagingWidth{}, stagingHeight{};

    void release_target()
    {
        _RELEASE(view);
        _RELEASE(target);
        width = height = 0;
    }
    void release_staging()
    {
        _RELEASE(staging[0]);
        _RELEASE(staging[1]);
        stagingWidth = stagingHeight = 0;
    }
};

PictureTargets targets;

bool make(u32 w, u32 h, bool stage, ID3DTexture2D*& out)
{
    D3D_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = stage ? D3D_USAGE_STAGING : D3D_USAGE_DEFAULT;
    desc.BindFlags = stage ? 0 : D3D_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = stage ? D3D_CPU_ACCESS_READ : 0;
    return SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &out));
}
} // namespace

void da_ui_picture_destroy()
{
    targets.release_staging();
    targets.release_target();
}

bool CRender::RenderUIPicture(const Irect& rect, const std::function<void()>& draw, xr_vector<u32>& rgba)
{
    const u32 w = Device.dwWidth, h = Device.dwHeight;
    const int left = std::max(rect.left, 0), top = std::max(rect.top, 0);
    const int right = std::min(rect.right, int(w)), bottom = std::min(rect.bottom, int(h));
    if (right <= left || bottom <= top)
        return false;
    const u32 pw = u32(right - left), ph = u32(bottom - top);

    if (targets.width != w || targets.height != h)
    {
        targets.release_target();
        if (!make(w, h, false, targets.target) || FAILED(HW.pDevice->CreateRenderTargetView(targets.target, nullptr, &targets.view)))
        {
            targets.release_target();
            return false;
        }
        targets.width = w;
        targets.height = h;
    }
    if (targets.stagingWidth < pw || targets.stagingHeight < ph)
    {
        targets.release_staging();
        const u32 sw = std::max(pw, w / 2), sh = std::max(ph, h / 2);
        if (!make(sw, sh, true, targets.staging[0]) || !make(sw, sh, true, targets.staging[1]))
        {
            targets.release_staging();
            return false;
        }
        targets.stagingWidth = sw;
        targets.stagingHeight = sh;
    }

    auto* context = HW.get_context(CHW::IMM_CTX_ID);
    const D3D11_BOX box{ UINT(left), UINT(top), 0, UINT(right), UINT(bottom), 1 };
    for (u32 pass = 0; pass < 2; ++pass)
    {
        const float c = pass ? 1.f : 0.f;
        const float clear[4] = { c, c, c, 0.f };
        Target->u_setrt(RCache, w, h, targets.view, nullptr, nullptr, nullptr);
        context->ClearRenderTargetView(targets.view, clear);
        draw();
        context->CopySubresourceRegion(targets.staging[pass], 0, 0, 0, 0, targets.target, 0, &box);
    }
    Target->u_setrt(RCache, w, h, Target->get_base_rt(), nullptr, nullptr, Target->get_base_zb());

    D3D_MAPPED_TEXTURE2D over[2]{};
    const bool mapped0 = SUCCEEDED(context->Map(targets.staging[0], 0, D3D_MAP_READ, 0, &over[0]));
    const bool mapped1 = mapped0 && SUCCEEDED(context->Map(targets.staging[1], 0, D3D_MAP_READ, 0, &over[1]));
    if (mapped1)
    {
        // Over black a pixel is c * a, over white c * a + 1 - a: the difference is the alpha's complement.
        rgba.resize(size_t(pw) * ph);
        for (u32 y = 0; y < ph; ++y)
        {
            const u8* b = static_cast<const u8*>(over[0].pData) + size_t(y) * over[0].RowPitch;
            const u8* wt = static_cast<const u8*>(over[1].pData) + size_t(y) * over[1].RowPitch;
            u32* out = &rgba[size_t(y) * pw];
            for (u32 x = 0; x < pw; ++x, b += 4, wt += 4)
            {
                const int gap = (int(wt[0]) - b[0]) + (int(wt[1]) - b[1]) + (int(wt[2]) - b[2]);
                const int a = std::clamp(255 - (gap + 1) / 3, 0, 255);
                if (!a)
                {
                    out[x] = 0;
                    continue;
                }
                const auto channel = [a](u8 v) { return u32(std::min((int(v) * 255 + a / 2) / a, 255)); };
                out[x] = channel(b[0]) | channel(b[1]) << 8 | channel(b[2]) << 16 | u32(a) << 24;
            }
        }
    }
    if (mapped1)
        context->Unmap(targets.staging[1], 0);
    if (mapped0)
        context->Unmap(targets.staging[0], 0);
    return mapped1;
}
} // namespace xray::render::RENDER_NAMESPACE
