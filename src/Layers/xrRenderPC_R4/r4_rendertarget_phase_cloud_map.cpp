// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include <DirectXTex.h>
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

namespace xray::render::RENDER_NAMESPACE
{
// Where the map sits this frame: world XZ of its centre and its edge length in metres. The
// da_cloud_map binder hands these to every shader that samples the map.
float g_da_cloud_map_center_x = 0.f;
float g_da_cloud_map_center_z = 0.f;
float g_da_cloud_map_extent = 16000.f;

// The cloud deck field, once per frame: for every texel of a square of the deck plane around
// the camera, the field's density, its coverage and its detail. The sun passes read it per lit
// pixel, the shafts per march sample, the deck per sky pixel - each as one texture fetch where
// they used to evaluate six octaves of noise. 1024x1024 texels of six noise reads is nothing;
// the same reads per screen pixel per pass were fourteen milliseconds.
void CRenderTarget::phase_cloud_map()
{
    if (!s_cloud_map || !rt_cloud_map || !g_pGamePersistent)
        return;

    PIX_EVENT(DA_phase_cloud_map);

    // Snap the centre to the texel grid so the map's contents do not swim under the camera:
    // the field is evaluated in world space, so a snapped centre only moves which texels
    // exist, never what they hold.
    const float texel = g_da_cloud_map_extent / 1024.f;
    const Fvector& eye = Device.vCameraPosition;
    g_da_cloud_map_center_x = floorf(eye.x / texel) * texel;
    g_da_cloud_map_center_z = floorf(eye.z / texel) * texel;

    u_setrt(RCache, rt_cloud_map, nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
    // u_setrt binds the target but leaves the viewport alone (the stock passes set their own);
    // without this the quad rasterizes at screen size and only its top-left lands in the target.
    RCache.SetViewport({ 0.f, 0.f, float(rt_cloud_map->dwWidth), float(rt_cloud_map->dwHeight), 0.f, 1.f });
    RCache.set_Stencil(FALSE);
    RCache.set_Z(FALSE);
    RCache.set_CullMode(CULL_NONE);
    RCache.set_ColorWriteEnable();

    u32 Offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    RCache.set_Element(s_cloud_map->E[0]);
    RCache.set_Geometry(g_combine);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // Back to the device-sized viewport for whatever renders next.
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());

    // The sun's column: the texel the camera-to-sun ray hits, copied to a staging texture and
    // read back a frame later (no stall), so the engine knows how much deck stands between the
    // player and the sun - the lens flare and the sun sprite fade with it.
    auto& env = g_pGamePersistent->Environment();
    Fvector to_sun = env.CurrentEnv.sun_dir;
    to_sun.invert();
    if (to_sun.y > 0.05f)
    {
        auto* ctx = HW.get_context(RCache.context_id);
        for (auto& tex : cloud_readback)
        {
            if (tex)
                continue;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 1;
            desc.Height = 1;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HW.pDevice->CreateTexture2D(&desc, nullptr, &tex);
        }
        static u32 slot = 0;
        if (cloud_readback[0] && cloud_readback[1])
        {
            const float t = (env.eff_cloud_altitude - eye.y) / to_sun.y;
            const float hx = eye.x + to_sun.x * t;
            const float hz = eye.z + to_sun.z * t;
            const float ux = (hx - g_da_cloud_map_center_x) / g_da_cloud_map_extent + 0.5f;
            const float uz = (hz - g_da_cloud_map_center_z) / g_da_cloud_map_extent + 0.5f;
            if (ux > 0.f && ux < 1.f && uz > 0.f && uz < 1.f)
            {
                const u32 px = std::min<u32>(u32(ux * float(rt_cloud_map->dwWidth)), rt_cloud_map->dwWidth - 1);
                const u32 py = std::min<u32>(u32(uz * float(rt_cloud_map->dwHeight)), rt_cloud_map->dwHeight - 1);
                const D3D11_BOX box{px, py, 0, px + 1, py + 1, 1};
                ctx->CopySubresourceRegion(cloud_readback[slot], 0, 0, 0, 0, rt_cloud_map->pSurface, 0, &box);
                // The other slot holds last frame's texel: read it without waiting.
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(cloud_readback[slot ^ 1], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
                {
                    const u8 a = static_cast<const u8*>(mapped.pData)[3];
                    ctx->Unmap(cloud_readback[slot ^ 1], 0);
                    const float target = float(a) / 255.f;
                    env.cloud_sun_visibility += (target - env.cloud_sun_visibility) * 0.15f;
                }
                slot ^= 1;
            }
            else
                env.cloud_sun_visibility += (1.f - env.cloud_sun_visibility) * 0.15f;
        }
    }
    else
        env.cloud_sun_visibility = 1.f;
}

// Diagnostic (r__cloud_map_dump): the map as a PNG next to the screenshots, so a "no clouds
// overhead" report can be checked against what the field actually holds around the camera.
void CRenderTarget::dump_cloud_map()
{
    if (!rt_cloud_map || !rt_cloud_map->pSurface)
        return;
    DirectX::ScratchImage image;
    if (FAILED(DirectX::CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), rt_cloud_map->pSurface, image)))
    {
        Log("! cloud map dump: capture failed");
        return;
    }
    string_path path;
    FS.update_path(path, "$screenshots$", "cloud_map.png");
    wchar_t wpath[MAX_PATH];
    mbstowcs(wpath, path, MAX_PATH);
    const HRESULT hr = DirectX::SaveToWICFile(*image.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), wpath);
    Msg("* cloud map dump -> %s (%s) centre=(%.0f, %.0f) extent=%.0f", path, SUCCEEDED(hr) ? "ok" : "FAILED",
        g_da_cloud_map_center_x, g_da_cloud_map_center_z, g_da_cloud_map_extent);

    // The march buffer too (half-res RGBA16F -> 8-bit, alpha dropped: the colour is what shows).
    if (rt_clouds[0] && rt_clouds[0]->pSurface)
    {
        DirectX::ScratchImage march, converted;
        if (SUCCEEDED(DirectX::CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), rt_clouds[0]->pSurface, march)) &&
            SUCCEEDED(DirectX::Convert(*march.GetImage(0, 0, 0), DXGI_FORMAT_B8G8R8X8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.f, converted)))
        {
            static u32 dump_index = 0; string64 dump_name; xr_sprintf(dump_name, "cloud_march_%u.png", dump_index++); FS.update_path(path, "$screenshots$", dump_name);
            mbstowcs(wpath, path, MAX_PATH);
            const HRESULT hr2 = DirectX::SaveToWICFile(*converted.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), wpath);
            Msg("* cloud march dump -> %s (%s)", path, SUCCEEDED(hr2) ? "ok" : "FAILED");
            // The transmittance too, as grey: white = clear sky, black = opaque cloud.
            DirectX::ScratchImage tgrey, tconv;
            const HRESULT hr3 = DirectX::TransformImage(*march.GetImage(0, 0, 0),
                [](DirectX::XMVECTOR* out, const DirectX::XMVECTOR* in, size_t w, size_t) {
                    for (size_t x = 0; x < w; ++x)
                        out[x] = DirectX::XMVectorSetW(DirectX::XMVectorSplatW(in[x]), 1.f);
                }, tgrey);
            if (SUCCEEDED(hr3) && SUCCEEDED(DirectX::Convert(*tgrey.GetImage(0, 0, 0), DXGI_FORMAT_B8G8R8X8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.f, tconv)))
            {
                xr_sprintf(dump_name, "cloud_march_t_%u.png", dump_index - 1);
                FS.update_path(path, "$screenshots$", dump_name);
                mbstowcs(wpath, path, MAX_PATH);
                DirectX::SaveToWICFile(*tconv.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), wpath);
            }
        }
    }
}

void da_dump_cloud_map()
{
    if (RImplementation.Target)
        RImplementation.Target->dump_cloud_map();
}
} // namespace xray::render::RENDER_NAMESPACE
