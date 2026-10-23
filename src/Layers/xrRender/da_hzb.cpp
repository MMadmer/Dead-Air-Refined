// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The depth pyramid of the G-buffer stage (da_hzb.h).
#include "stdafx.h"
#include "da_hzb.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderDX11/dx11Native.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// Levels of the pyramid (r3\da_hzb.h da_hzb_lv): down to one texel for a depth of 16384 pixels.
constexpr u32 kLevels = 14;

// Its pass (r3\da_hzb.cs): the first level reads the depth as the G-buffer left it, copied (CRenderTarget::copy_depth)
// since the depth stays bound for the draws.
class CBlender_da_hzb : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: depth pyramid"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass("da_hzb");
        C.r_dx11Texture("s_depth_copy", r2_RT_depth_copy);
        C.r_End();
    }
};

struct State
{
    ref_shader shader;
    bool shader_tried{};
    // The levels side by side in one R32F texture, for the depth of this size; its first level at half of it.
    ID3D11Texture2D* tex{};
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    u32 w{}, h{}, levels{};
    Fvector4 lv[kLevels]{};
    // The frame of the last pyramid: a pass binding it later in another frame finds it stale and binds it off.
    u32 frame{u32(-1)};
};
State g;

void release_texture()
{
    _RELEASE(g.srv);
    _RELEASE(g.uav);
    _RELEASE(g.tex);
    g.w = g.h = g.levels = 0;
}

// Level 0 at half the render size, each next one half the last, rounded up, to one texel; the levels after the first
// stacked down a column to its right.
bool make_texture(u32 w, u32 h)
{
    release_texture();
    u32 lw = (w + 1) / 2, lh = (h + 1) / 2, column = 0;
    const u32 w0 = lw, h0 = lh;
    for (u32 k = 0; k < kLevels; ++k)
    {
        g.lv[k].set(k ? float(w0) : 0.f, k ? float(column) : 0.f, float(lw), float(lh));
        column += k ? lh : 0;
        ++g.levels;
        if (lw == 1 && lh == 1)
            break;
        lw = (lw + 1) / 2;
        lh = (lh + 1) / 2;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w0 + (w0 + 1) / 2;
    desc.Height = std::max(h0, column);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &g.tex)) ||
        FAILED(HW.pDevice->CreateShaderResourceView(g.tex, nullptr, &g.srv)) ||
        FAILED(HW.pDevice->CreateUnorderedAccessView(g.tex, nullptr, &g.uav)))
    {
        release_texture();
        return false;
    }
    g.w = w;
    g.h = h;
    return true;
}
} // namespace

bool da_hzb_build(CBackend& cmd)
{
    if (!g.shader_tried)
    {
        g.shader_tried = true;
        // (An install with older shaders has no pass: compiling a missing file is fatal.)
        if (!da_async_shader_exists("da_hzb.cs"))
            return false;
        static CBlender_da_hzb blender;
        g.shader.create(&blender, "r2" DELIMITER "da_hzb");
    }
    SPass* pass = g.shader && g.shader->E[0] && !g.shader->E[0]->passes.empty() ? &*g.shader->E[0]->passes[0] : nullptr;
    if (!pass || !pass->cs || !pass->cs->sh)
        return false;
    const u32 w = std::max(Device.dwRenderWidth ? Device.dwRenderWidth : Device.dwWidth, 1u);
    const u32 h = std::max(Device.dwRenderHeight ? Device.dwRenderHeight : Device.dwHeight, 1u);
    if ((!g.tex || g.w != w || g.h != h) && !make_texture(w, h))
        return false;
    if (!RImplementation.Target->copy_depth(cmd))
        return false;
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    const UINT initial = ~0u;
    ID3D11UnorderedAccessView* none = nullptr;
    for (u32 k = 0; k < g.levels; ++k)
    {
        const Fvector4& dst = g.lv[k];
        const Fvector4 src = k ? g.lv[k - 1] : Fvector4{-1.f, 0.f, float(w), float(h)};
#if defined(XR_NATIVE)
        // Each level reads the one before from the same texture.
        if (k)
            xrnative::UavBarrier(ctx, nullptr);
#endif
        cmd.set_States(pass->state);
        cmd.set_CS(pass->cs);
        cmd.set_Constants(pass->constants);
        if (pass->T)
            cmd.set_Textures(pass->T);
        cmd.set_c("da_hzb_src", src);
        cmd.set_c("da_hzb_dst", dst);
        ctx->CSSetUnorderedAccessViews(0, 1, &g.uav, &initial);
        cmd.Compute((u32(dst.z) + 7) / 8, (u32(dst.w) + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
        cmd.clear_CS_resources();
        cmd.SRVSManager.Apply(cmd.context_id);
    }
    g.frame = Device.dwFrame;
    return true;
}

void da_hzb_bind(CBackend& cmd, u32 slot, bool on)
{
    on = on && g.srv && g.frame == Device.dwFrame;
    cmd.set_c("da_hzb", on ? 1.f : 0.f, float(g.levels), float(g.w), float(g.h));
    for (u32 k = 0; k < kLevels; ++k)
        cmd.set_ca("da_hzb_lv", k, g.lv[k]);
    // The main view's camera, world to view rows and the projection's terms (TAA's jitter in _31, _32).
    const Fmatrix& V = Device.mView;
    cmd.set_ca("da_hzb_view", 0, V._11, V._21, V._31, V._41);
    cmd.set_ca("da_hzb_view", 1, V._12, V._22, V._32, V._42);
    cmd.set_ca("da_hzb_view", 2, V._13, V._23, V._33, V._43);
    const Fmatrix& P = Device.mProject;
    cmd.set_c("da_hzb_proj", P._11, P._22, P._33, P._43);
    cmd.set_c("da_hzb_proj2", P._31, P._32, -P._43 / P._33, 0.f);
    cmd.SRVSManager.SetCSResource(slot, on ? g.srv : nullptr);
}

void da_hzb_unbind(CBackend& cmd, u32 slot)
{
    cmd.SRVSManager.SetCSResource(slot, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
}

void da_hzb_destroy()
{
    release_texture();
    g.shader.destroy();
    g.shader_tried = false;
    g.frame = u32(-1);
}
} // namespace xray::render::RENDER_NAMESPACE
