#include "stdafx.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

// The width the target was actually created at, published by r2_rendertarget.cpp - r__visor_drops
// itself is a live console var and the target is sized exactly once.
extern ENGINE_API int ps_r__visor_drops_active;

namespace xray::render::RENDER_NAMESPACE
{
// The sim's own state. File scope, like the ripple field's window: it belongs to this pass and
// to nothing else.
static float g_visor_acc = 0.f; // fixed-step accumulator
static u32 g_visor_step = 0; // steps since the target was primed; the spawn hash's clock
static ID3DTexture2D* g_visor_primed = nullptr; // the surface the clear below was applied to

// The visor is the actor's, and the actor is a level's. Nothing recreates the target between
// levels, so the prime test below - keyed on the surface - would never fire again and the next
// level would start behind a mask still streaming with the last one's rain.
void da_visor_drops_reset()
{
    g_visor_acc = 0.f;
    g_visor_step = 0;
    g_visor_primed = nullptr;
}

// One or more fixed steps of the water standing on the actor's visor.
//
// The glass in front of the eye is a plate a hand's width across with a few dozen drops on it,
// and every one of them is doing one of three things: sitting where it landed because contact
// angle hysteresis pins it there, growing as more rain lands on it and as it swallows its
// neighbours, or - once it is too heavy for the pinning force to hold - running down and leaving
// a wet trail behind it that the next drop follows. None of that is a pattern. It is a field
// with a memory, so it lives in a target and is stepped: da_visor_drops.ps.
//
// What must not be decided per frame, and is decided here:
//
//   * the STEP is fixed and fed by an accumulator. A film that thins per frame drains at the
//     frame rate, and a drop that falls per frame falls faster on a better machine;
//   * gravity is the WORLD's, resolved into the plane of the glass. The visor turns with the
//     head, so the drops are still in screen space - but which way is down on the glass is not:
//     look up at the sky and the plate goes horizontal, the in-plane component of gravity goes
//     to zero, and the drops stop running and just sit there, which is exactly what they do;
//   * the wetting rate comes from r2_lenswater_val, the one number every driver of this effect
//     has ever pushed - the rain drivers and the actor's own head-under-water ramp. Read as a
//     RATE and not as a level, which is the whole of why the old effect arrived in slabs: the
//     value moves once a second in steps of 0.03, and a level that steps is a picture that
//     steps, while a rate that steps is a field that does not.
void CRenderTarget::phase_visor_drops()
{
    if (ps_r__visor_drops_active <= 0 || !rt_VisorDrops || !rt_VisorDropsTmp)
        return;
    if (!g_pGamePersistent)
        return;

    if (!s_visor_drops)
        s_visor_drops.create("da_visor_drops");
    if (!s_visor_drops)
        return;

    PIX_EVENT(DA_phase_visor_drops);

    const auto& env = g_pGamePersistent->Environment();

    // A fresh target holds whatever the allocator left in it, and this one is fed back into
    // itself every step.
    if (g_visor_primed != rt_VisorDrops->pSurface)
    {
        RCache.ClearRT(rt_VisorDrops, {});
        RCache.ClearRT(rt_VisorDropsTmp, {});
        g_visor_primed = rt_VisorDrops->pSurface;
        g_visor_acc = 0.f;
    }

    //	Twice the ripple field's rate, and for a reason that is not "smoother". A drop running at
    //	twenty centimetres a second crosses a 22 cm visor in about a second, which at 30 Hz is
    //	seven millimetres of travel per step - several times its own width, so it would arrive
    //	ahead of where it was rather than slide, and both the eye and the trail would see a
    //	dotted line. At 60 Hz it moves about its own width.
    constexpr float dt = 1.f / 60.f;
    g_visor_acc += Device.fTimeDelta;
    int steps = int(g_visor_acc / dt);
    if (steps >= 4)
    {
        steps = 4;
        g_visor_acc = 0.f;
    }
    else
        g_visor_acc -= float(steps) * dt;
    if (steps <= 0)
        return;

    // How much of the glass one texel is. The visor is a plate about this wide across the
    // screen's horizontal - the drops have to be a believable SIZE against it, and a millimetre
    // is a millimetre only once something says how many of them the screen spans.
    constexpr float visor_width_m = 0.22f;
    const float texel_mm = visor_width_m * 1000.f / float(rt_VisorDrops->dwWidth);

    // Gravity in the plane of the glass. The visor faces the way the eye does, so the world's
    // down resolved onto the screen's right and up axes IS the direction water runs on it, and
    // the length of that resolved vector is how much of gravity the plate actually feels.
    const Fvector& right = Device.vCameraRight;
    const Fvector& up = Device.vCameraTop;
    const float gx = -right.y; // dot(down, right), down = (0,-1,0)
    const float gy = -up.y;
    // Screen up is +y in the world and the target's v runs downward, so the field's own y is the
    // negative of the screen's.
    const float g_len = _sqrt(gx * gx + gy * gy);
    const float gux = (g_len > 1e-4f) ? gx / g_len : 0.f;
    const float guy = (g_len > 1e-4f) ? -gy / g_len : 1.f;

    // The wetting rate, and the hand. Two sources, and the wetter wins: r2_lenswater_val is the
    // rain drivers' (shelter and the mask are already in it), env.visor.dunk is the actor's own
    // head coming out of the water.
    const float wet = (env.visor.qa_wet >= 0.f)
        ? clampr(env.visor.qa_wet, 0.f, 1.f)
        : clampr(std::max(ps_r2_lenswater_value, env.visor.dunk), 0.f, 1.f);
    const float wipe = env.visor_wipe_phase();
    const float wipe_dir = float(env.visor.wipe_dir);

    const float rt_w = float(rt_VisorDrops->dwWidth);
    const float rt_h = float(rt_VisorDrops->dwHeight);
    auto* ctx = HW.get_context(RCache.context_id);

    for (int s = 0; s < steps; ++s)
    {
        u_setrt(RCache, rt_VisorDropsTmp, nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
        // u_setrt binds the target and leaves the viewport alone, and this one is not screen
        // sized: without this the quad rasterizes at screen size and only a corner of it lands.
        RCache.SetViewport({ 0.f, 0.f, rt_w, rt_h, 0.f, 1.f });
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

        RCache.set_Element(s_visor_drops->E[0]);
        RCache.set_Geometry(g_combine);
        RCache.set_c("da_visor_sim", dt, texel_mm, float(g_visor_step), wet);
        RCache.set_c("da_visor_grav", gux, guy, g_len, rt_w / rt_h);
        // A sweep only exists while it is running; once it is over the hand is gone and the
        // shader must not keep scrubbing the far edge.
        RCache.set_c("da_visor_wipe", (wipe < 1.f) ? 1.f : 0.f, wipe, wipe_dir, 0.f);
        RCache.set_c("da_visor_dim", 1.f / rt_w, 1.f / rt_h, rt_w, rt_h);
        RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

        RCache.set_RT(nullptr, 0);
        // The step just drawn becomes the state every reader binds, under the one name.
        ctx->CopyResource(rt_VisorDrops->pSurface, rt_VisorDropsTmp->pSurface);
        ++g_visor_step;
    }

    // Back to the device-sized viewport and target for whatever renders next.
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
}

// Half floats, read back by hand: the field is A16B16G16R16F and there is no reason to widen a
// shipping target so a diagnostic can read it.
static float da_half_to_float(u16 h)
{
    const u32 sign = u32(h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1fu;
    u32 man = h & 0x3ffu;
    if (exp == 0)
    {
        if (man == 0)
            return sign ? -0.f : 0.f;
        // Subnormal: normalise it into a float exponent.
        exp = 1;
        while (!(man & 0x400u))
        {
            man <<= 1;
            --exp;
        }
        man &= 0x3ffu;
    }
    else if (exp == 31)
        exp = 255 - 112; // inf or nan, and the bit pattern below carries which
    const u32 bits = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// What the visor holds, read back through a staging copy and logged, plus the thickness as a
// picture: a screenshot cannot say whether a field is empty, or there and too faint to see, and
// this feature has already been drawn twice by people who could not tell the two apart.
void da_visor_drops_stats()
{
    if (!RImplementation.Target || !RImplementation.Target->rt_VisorDrops ||
        !RImplementation.Target->rt_VisorDrops->pSurface)
    {
        Msg("* [visor] no field");
        return;
    }
    const ref_rt& rt = RImplementation.Target->rt_VisorDrops;
    D3D11_TEXTURE2D_DESC desc;
    rt->pSurface->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
    {
        Msg("* [visor] no staging copy");
        return;
    }
    auto* ctx = HW.get_context(RCache.context_id);
    ctx->CopyResource(staging, rt->pSurface);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        double sum = 0.0, film = 0.0;
        float max_h = 0.f, max_v = 0.f, max_film = 0.f;
        u32 wet = 0, nans = 0;
        for (u32 y = 0; y < desc.Height; ++y)
        {
            const u16* row = reinterpret_cast<const u16*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
            for (u32 x = 0; x < desc.Width; ++x)
            {
                const float h = da_half_to_float(row[x * 4]);
                const float f = da_half_to_float(row[x * 4 + 1]);
                const float vx = da_half_to_float(row[x * 4 + 2]);
                const float vy = da_half_to_float(row[x * 4 + 3]);
                if (h != h || f != f || vx != vx || vy != vy)
                {
                    ++nans;
                    continue;
                }
                if (h > 0.02f)
                    ++wet;
                sum += h;
                film += f;
                max_h = std::max(max_h, h);
                max_film = std::max(max_film, f);
                max_v = std::max(max_v, _sqrt(vx * vx + vy * vy));
            }
        }
        // The thickness as a picture: black is dry glass, white is a millimetre of water.
        string_path path;
        string64 dump;
        xr_sprintf(dump, "visor_drops_%u.pgm", g_visor_step);
        FS.update_path(path, "$app_data_root$", dump);
        if (IWriter* W = FS.w_open(path))
        {
            string64 head;
            xr_sprintf(head, "P5\n%u %u\n255\n", desc.Width, desc.Height);
            W->w(head, u32(xr_strlen(head)));
            xr_vector<u8> line(desc.Width);
            for (u32 y = 0; y < desc.Height; ++y)
            {
                const u16* row = reinterpret_cast<const u16*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
                for (u32 x = 0; x < desc.Width; ++x)
                    line[x] = u8(clampr(da_half_to_float(row[x * 4]) * 255.f, 0.f, 255.f));
                W->w(line.data(), desc.Width);
            }
            FS.w_close(W);
        }
        ctx->Unmap(staging, 0);
        const double texels = double(desc.Width) * double(desc.Height);
        Msg("* [visor] %ux%u, step %u, %u nan; %.1f%% of the glass wet, mean %.4f mm, film mean %.4f max %.4f mm, "
            "deepest %.3f mm, fastest %.0f mm/s",
            desc.Width, desc.Height, g_visor_step, nans, 100.0 * double(wet) / texels, sum / texels, film / texels,
            max_film, max_h, max_v);
    }
    else
        Msg("* [visor] map failed");
    _RELEASE(staging);
}
} // namespace xray::render::RENDER_NAMESPACE
