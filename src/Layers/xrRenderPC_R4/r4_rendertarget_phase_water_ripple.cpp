#include "stdafx.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

// Declared before the render namespace opens, or the externs would name
// xray::render::*::symbols that nothing defines. All three live in the engine (xr_ioc_cmd.cpp).
// The resolution read here is the one the targets were ACTUALLY created at, published by
// r2_rendertarget.cpp - r__water_ripple itself is a live console var, and taking it here would
// have the sim solve its Laplacian against a texel count the targets do not have.
extern ENGINE_API int ps_r__water_ripple_active;
extern ENGINE_API int ps_r__rain_quality;
extern ENGINE_API bool g_da_level_has_water;

namespace xray::render::RENDER_NAMESPACE
{
// The sim's own state. File scope like the cloud map's window centre: it belongs to this pass
// and to nothing else, and it is five values.
static float g_ripple_acc = 0.f; // fixed-step accumulator
static u32 g_ripple_step = 0; // steps since the level loaded; the rain hash's clock
static float g_ripple_prev_x = 0.f; // the window centre the field currently holds
static float g_ripple_prev_z = 0.f;
static ID3DTexture2D* g_ripple_primed = nullptr; // the surface the clear below was applied to

// Everything above belongs to ONE level. The target pair is not recreated between levels, so
// the prime test below - keyed on the surface, which does not change - would never fire again
// and level 2 would start with level 1's heights still ringing in it. That is this repo's
// second-load bug class, and file-scope sim state is exactly how it happens.
//
// Called from da_water_field_update() (r4_water_field.cpp), which is the one place in the
// renderer that notices the bake was replaced.
void da_water_ripple_reset()
{
    g_ripple_acc = 0.f;
    g_ripple_step = 0;
    g_ripple_prev_x = 0.f;
    g_ripple_prev_z = 0.f;
    g_ripple_primed = nullptr;
}

// One or more fixed steps of the interaction ripple field (DESIGN2.md section 2). The wave
// equation itself, its boundaries and its sources are in da_water_ripple.ps; what lives here is
// everything that must not be decided per frame:
//
//   * the STEP is fixed and fed by an accumulator. A step tied to the frame time makes the
//     ripple speed track the frame rate, which is the single most common way this feature is
//     built wrong;
//   * r2 is solved from a fixed wave SPEED rather than pinned at the design's 0.25. That
//     number is what (c*dt/dx)^2 comes to at 256 texels over 32 m; pinning the ratio instead
//     would run the ripples at twice the metres per second on the 128-texel tier;
//   * the pair does not ping-pong its NAMES. Each step writes half 0 and is copied back into
//     half 1, so "$user$water_ripple0" is always the current state and every consumer can bind
//     one fixed name. A true ping-pong puts the current step under a different name on
//     alternate steps, and a consumer that binds statically then strobes at the step rate. The
//     copy is 128-256 KB and buys that outright.
void CRenderTarget::phase_water_ripple()
{
    if (ps_r__water_ripple_active <= 0 || !rt_WaterRipple[0] || !rt_WaterRipple[1])
        return;
    if (!g_da_level_has_water || !g_pGamePersistent)
        return;

    const auto& env = g_pGamePersistent->Environment();
    const float win = env.water_ripple_win.z; // metres across the window, 0 when the tier is off
    if (win <= 0.f)
        return;

    // Created on first use rather than in the render target's constructor, the way rt_ui is:
    // the constructor is shared ground and this pass is the only thing that wants the shader.
    if (!s_water_ripple)
        s_water_ripple.create("da_water_ripple");
    if (!s_water_ripple)
        return;

    PIX_EVENT(DA_phase_water_ripple);

    const float texels = float(ps_r__water_ripple_active);
    const float texel_m = win / texels;

    // A fresh pair holds whatever the allocator left in it, and these are float16 targets: one
    // garbage texel reading as a NaN spreads over the whole field within a few dozen steps.
    // Keyed on the surface the pair currently owns rather than on a flag, because a device
    // reset rebuilds the targets and a flag would not notice.
    if (g_ripple_primed != rt_WaterRipple[0]->pSurface)
    {
        RCache.ClearRT(rt_WaterRipple[0], {});
        RCache.ClearRT(rt_WaterRipple[1], {});
        g_ripple_primed = rt_WaterRipple[0]->pSurface;
        g_ripple_prev_x = env.water_ripple_win.x;
        g_ripple_prev_z = env.water_ripple_win.y;
        g_ripple_acc = 0.f;
    }

    constexpr float dt = 1.f / 60.f;
    g_ripple_acc += Device.fTimeDelta;
    int steps = int(g_ripple_acc / dt);
    if (steps >= 4)
    {
        // A fifth of a second of backlog. Past that the frame is already stuttering and
        // simulating the arrears only makes the next one later still, so the debt is written
        // off instead of carried - the field slows down for a moment, which nobody can see.
        steps = 4;
        g_ripple_acc = 0.f;
    }
    else
        g_ripple_acc -= float(steps) * dt;
    if (steps <= 0)
        return;

    // FTCS is stable in 2D up to r2 = 0.5; 3.75 m/s is where the design's 0.25 lands at the top
    // tier, and the min is what keeps the coarse tier from being asked for more than it can
    // carry if the window or the step ever change.
    constexpr float wave_c = 3.75f;
    const float courant = wave_c * dt / texel_m;
    const float r2 = std::min(courant * courant, 0.25f);
    constexpr float damping = 0.995f; // ~3 s to fade a ring at 60 steps a second

    // Rain into the field is the top tier's line in the ladder, and rain_quality is the knob
    // that carries "Extreme" without a second one having to agree with it.
    float rain_p = 0.f, rain_amp = 0.f;
    if (ps_r__rain_quality >= 4)
    {
        const float R = env.RainRateMmh();
        if (R > 0.05f)
        {
            // Marshall-Palmer: N(D) = N0*exp(-lambda*D) with N0 = 8000 /m3/mm and lambda in
            // 1/mm, so the drop count is N0/lambda and the flux is that times the fall speed.
            // Counted over D > 3 mm on purpose: a texel here is 8-25 cm and the crater of a
            // 1 mm drop is half a centimetre, so the small stuff cannot be resolved as a ring
            // at all and stays where it already is - the rain-ripple normal map in
            // settings_da_water.h. What is left is the drops that genuinely leave a ring.
            const float lambda = 4.1f * powf(R, -0.21f);
            const float flux = (8000.f / lambda) * 5.f * expf(-lambda * 3.f); // drops/m2/s
            rain_p = flux * texel_m * texel_m * dt;
            rain_amp = 0.006f;
            // Past a third of a chance per texel per step the drops stop being separable, so
            // the surplus goes into the depth of each crater instead of into more of them.
            if (rain_p > 0.35f)
            {
                rain_amp *= rain_p / 0.35f;
                rain_p = 0.35f;
            }
        }
    }

    auto* ctx = HW.get_context(RCache.context_id);
    const float rt_w = float(rt_WaterRipple[0]->dwWidth);
    const float rt_h = float(rt_WaterRipple[0]->dwHeight);

    for (int s = 0; s < steps; ++s)
    {
        // The window is solved once per frame, so it only moves on the first step of one. Both
        // centres are snapped to whole texels, which is what makes this a copy of the field
        // rather than a resample of it - resampling every frame smears it into mush.
        const float shift_u = (s == 0) ? (env.water_ripple_win.x - g_ripple_prev_x) / win : 0.f;
        const float shift_v = (s == 0) ? (env.water_ripple_win.y - g_ripple_prev_z) / win : 0.f;

        u_setrt(RCache, rt_WaterRipple[0], nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
        // u_setrt binds the target but leaves the viewport alone, and the field is not screen
        // sized: without this the quad rasterizes at screen size and only a corner of it lands.
        RCache.SetViewport({ 0.f, 0.f, rt_w, rt_h, 0.f, 1.f });
        RCache.set_Stencil(FALSE);
        RCache.set_Z(FALSE);
        RCache.set_CullMode(CULL_NONE);
        RCache.set_ColorWriteEnable();

        // FVF::TL and the same quad as our other fullscreen passes: a vertex layout that does
        // not match the vertex shader is dropped by DirectX without a single message.
        u32 Offset = 0;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

        RCache.set_Element(s_water_ripple->E[0]);
        RCache.set_Geometry(g_combine);
        RCache.set_c("da_ripple_sim", r2, damping, texels, float(g_ripple_step));
        RCache.set_c("da_ripple_src", shift_u, shift_v, rain_p, rain_amp);
        // Every source in the sim is a depth in metres; these say how many steps that depth is
        // spread over. Feeding it once per step instead drives the field into its clamp.
        RCache.set_c("da_ripple_sim2", dt, 0.12f, 0.15f, 0.f);
        RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
        ++g_ripple_step;

        // Hand the state that was just written to whoever reads it next - the following step,
        // or the surface shader this frame. See the note above the function for why this is a
        // copy and not a swap of the two halves.
        ctx->CopyResource(rt_WaterRipple[1]->pSurface, rt_WaterRipple[0]->pSurface);
    }

    // Only now: if no step ran this frame the field still holds the OLD window, and moving the
    // recorded centre without resampling would shift the whole thing sideways.
    g_ripple_prev_x = env.water_ripple_win.x;
    g_ripple_prev_z = env.water_ripple_win.y;

    // Back to the device-sized viewport and target for whatever renders next.
    u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), nullptr, nullptr, get_base_zb());
}
} // namespace xray::render::RENDER_NAMESPACE
