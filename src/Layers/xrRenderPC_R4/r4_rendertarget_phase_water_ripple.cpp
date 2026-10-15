// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

#include "da_async.h"

// Declared before the render namespace opens, or the externs would name
// xray::render::*::symbols that nothing defines. Both live in the engine (xr_ioc_cmd.cpp).
// The resolution read here is the one the targets were ACTUALLY created at, published by
// r2_rendertarget.cpp - r__water_ripple itself is a live console var, and taking it here would
// have the solver transform a texel count the targets do not have.
extern ENGINE_API int ps_r__water_ripple_active;
extern ENGINE_API int ps_r__rain_quality;

namespace xray::render::RENDER_NAMESPACE
{
// The two compute passes of the solver, as blenders: a compute shader is bound to its input
// texture by NAME at compile time, so the FFT pass exists twice - element 0 reads scratch a,
// element 1 reads scratch b - and the transform's size is a compile-time constant of the
// shader (the group-shared line), so the pass is compiled once per resolution the preset
// ladder can ask for.
class CBlender_da_ripple_fft : public IBlender
{
public:
    u32 size{512};
    LPCSTR getComment() override { return "INTERNAL: the ripple field's FFT pass"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 1)
            return;
        C.r_ComputePass(size >= 1024 ? "da_ripple_fft_1024" : (size >= 512 ? "da_ripple_fft_512" : "da_ripple_fft_256"));
        C.r_dx11Texture("s_fft_in", C.iElement ? r2_RT_water_ripple_fft "b" : r2_RT_water_ripple_fft "a");
        C.r_End();
    }
};

class CBlender_da_ripple_prop : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: the ripple field's Fourier-space step"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass("da_ripple_propagate");
        C.r_dx11Texture("s_fft_in", r2_RT_water_ripple_fft "a");
        C.r_End();
    }
};

// The sim's own state. File scope like the cloud map's window centre: it belongs to this pass
// and to nothing else, and it is five values.
static float g_ripple_acc = 0.f; // fixed-step accumulator
static u32 g_ripple_step = 0; // steps since the level loaded; the rain hash's clock
static float g_ripple_prev_x = 0.f; // the window centre the field currently holds
static float g_ripple_prev_z = 0.f;
static float g_ripple_held_x = 0.f; // the window centre before this frame's steps (a lost job)
static float g_ripple_held_z = 0.f;
static ID3DTexture2D* g_ripple_primed = nullptr; // the surface the clear below was applied to

// Everything above belongs to ONE level. The targets are not recreated between levels, so the
// prime test below - keyed on the surface, which does not change - would never fire again and
// level 2 would start with level 1's heights still ringing in it. That is this repo's
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

// One compute pass of the solver: the element's shader and input, the given scratch as its
// output, then everything unbound again so the next pass can swap the two.
static void da_ripple_compute(CBackend& cmd_list, SPass& P, ID3D11UnorderedAccessView* out, u32 groups_x,
    u32 groups_y, const std::function<void()>& constants)
{
    cmd_list.set_States(P.state);
    cmd_list.set_CS(P.cs);
    cmd_list.set_Constants(P.constants);
    cmd_list.set_Textures(P.T);
    constants();
    u32 initial = 1;
    auto* ctx = HW.get_context(cmd_list.context_id);
    ctx->CSSetUnorderedAccessViews(0, 1, &out, &initial);
    cmd_list.Compute(groups_x, groups_y, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
    cmd_list.clear_CS_resources();
}

// The real-space half of a step as a compute pass (da_water_ripple.cs, the DirectX 12 async path):
// element 0 reads the published state (the frame's first step), element 1 the previous step's
// output in scratch b - the compute path leaves the per-step copy out and publishes once.
class CBlender_da_water_ripple_cs : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: the ripple field's real-space step, compute"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 1)
            return;
        C.r_ComputePass("da_water_ripple");
        C.r_dx11Texture("s_water_ripple_prev", C.iElement ? r2_RT_water_ripple_fft "b" : r2_RT_water_ripple "0");
        C.r_dx11Texture("s_water_field", r2_RT_water_field);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_End();
    }
};

// The fixed step, and the losses and constants every step of the solver shares (both paths).
static constexpr float ripple_dt = 1.f / 30.f;
// Losses. In the solver, per wavelength: a wavelength-blind trickle that keeps a ring for
// half a minute, and a viscosity that takes the short waves first - a few times water's
// own, because a pond's surface film damps by k^2 as well. Dry ground is a rain puddle:
// centimetres deep over a bed that takes the energy out in a second.
static constexpr float ripple_loss_flat = 0.03f; // 1/s
static constexpr float ripple_viscosity = 5e-5f; // m^2/s
static constexpr float ripple_damping_land = 0.97f; // per step, at 30 Hz
// Surface tension over density, m^3/s^2: what makes a 6 cm ripple faster than a 15 cm one.
static constexpr float ripple_tension = 7.28e-5f;

// Whether the field runs at all this frame: the tier, its targets and a window.
static bool da_ripple_active(const CRenderTarget& target)
{
    if (ps_r__water_ripple_active <= 0 || !target.rt_WaterRipple || !target.rt_WaterRippleFFT[0] ||
        !target.rt_WaterRippleFFT[1])
        return false;
    // Not gated on the level having a water body: a rain puddle is water too, and its rings
    // live in this field. A level with no bake is one sheet of open water.
    if (!g_pGamePersistent)
        return false;
    // Metres across the window, 0 when the tier is off.
    return g_pGamePersistent->Environment().water_ripple_win.z > 0.f;
}

// Whether this frame's solve will take a step: da_ripple_prepare's accumulator test, without
// moving it (the job path records no job without a step).
bool da_water_ripple_due(const CRenderTarget& target)
{
    if (!da_ripple_active(target))
        return false;
    const float acc = g_ripple_primed != target.rt_WaterRipple->pSurface ? 0.f : g_ripple_acc;
    return int((acc + Device.fTimeDelta) / ripple_dt) > 0;
}

// Created on first use rather than in the render target's constructor, the way rt_ui is:
// the constructor is shared ground and this pass is the only thing that wants the shaders.
static void da_ripple_spectral_create(ref_shader& fft, ref_shader& prop, u32 texels)
{
    if (!fft)
    {
        static CBlender_da_ripple_fft b_fft;
        b_fft.size = texels;
        string64 name;
        xr_sprintf(name, "r2" DELIMITER "da_ripple_fft_%u", texels);
        fft.create(&b_fft, name);
    }
    if (!prop)
    {
        static CBlender_da_ripple_prop b_prop;
        prop.create(&b_prop, "r2" DELIMITER "da_ripple_propagate");
    }
}

// The CPU half of a frame's solve, shared by the pixel and the compute path: the priming clears,
// the fixed-step accumulator, the sources and the window's shift. The clears are RTV clears on the
// immediate context, so on the async path they precede the job in queue order. false = no step
// this frame.
//
// What lives here is everything that must not be decided per frame:
//
//   * the STEP is fixed and fed by an accumulator. A step tied to the frame time makes the
//     ripple speed track the frame rate, which is the single most common way this feature is
//     built wrong. The spectral step is exact for any dt - there is no Courant number - so it
//     runs at 30 Hz, which halves what the transforms cost and changes nothing a ring does;
//   * every wavelength moves at its own speed, from the full dispersion relation of water at
//     the body's mean depth. The first version of this field was a leapfrog stencil at one
//     speed, chosen for a pretty Courant number; the second tied that speed to the texel and
//     the third split it into three bands - and each of them drew a ring as one crest or three
//     moving as a block, where a pond draws a train whose long waves run ahead of its short
//     ones. A stencil has one speed; a spectrum has one per wave.
static bool da_ripple_prepare(CRenderTarget& target, DaRippleStep& step)
{
    const auto& env = g_pGamePersistent->Environment();
    step.win = env.water_ripple_win.z;
    step.texels = u32(ps_r__water_ripple_active);
    step.texel_m = step.win / float(step.texels);

    // A fresh target holds whatever the allocator left in it: one garbage texel reading as a
    // NaN spreads over the whole field in one transform. Keyed on the surface the state
    // currently owns rather than on a flag, because a device reset rebuilds the targets and a
    // flag would not notice.
    if (g_ripple_primed != target.rt_WaterRipple->pSurface)
    {
        RCache.ClearRT(target.rt_WaterRipple, {});
        RCache.ClearRT(target.rt_WaterRippleFFT[0], {});
        RCache.ClearRT(target.rt_WaterRippleFFT[1], {});
        g_ripple_primed = target.rt_WaterRipple->pSurface;
        g_ripple_prev_x = env.water_ripple_win.x;
        g_ripple_prev_z = env.water_ripple_win.y;
        g_ripple_acc = 0.f;
    }
    g_ripple_held_x = g_ripple_prev_x;
    g_ripple_held_z = g_ripple_prev_z;

    g_ripple_acc += Device.fTimeDelta;
    int steps = int(g_ripple_acc / ripple_dt);
    if (steps >= 3)
    {
        // A tenth of a second of backlog. Past that the frame is already stuttering and
        // simulating the arrears only makes the next one later still, so the debt is written
        // off instead of carried - the field slows down for a moment, which nobody can see.
        steps = 3;
        g_ripple_acc = 0.f;
    }
    else
        g_ripple_acc -= float(steps) * ripple_dt;
    if (steps <= 0)
        return false;
    step.steps = steps;

    // The water body's mean depth, from the bake. Waves longer than a few times it slow down
    // and lose their dispersion, which is why a marsh pool carries no swell at all.
    step.depth = std::max(env.water_body.x, 0.05f);
    step.speed = env.water_ripple_speed;
    step.edge = env.water_ripple_edge;

    // Rain into the field is the top tier's line in the ladder, and rain_quality is the knob
    // that carries "Extreme" without a second one having to agree with it.
    step.rain_p = 0.f;
    step.rain_amp = 0.f;
    if (ps_r__rain_quality >= 4)
    {
        const float R = env.RainRateMmh();
        if (R > 0.05f)
        {
            // Marshall-Palmer: N(D) = N0*exp(-lambda*D) with N0 = 8000 /m3/mm and lambda in
            // 1/mm, so the drop count is N0/lambda and the flux is that times the fall speed.
            // Counted over D > 3 mm on purpose: a texel here is 3-12 cm and the crater of a
            // 1 mm drop is half a centimetre, so the small stuff cannot be resolved as a ring
            // at all and stays where it already is - the rain-ripple normal map in
            // settings_da_water.h. What is left is the drops that genuinely leave a ring.
            const float lambda = 4.1f * powf(R, -0.21f);
            const float flux = (8000.f / lambda) * 5.f * expf(-lambda * 3.f); // drops/m2/s
            step.rain_p = flux * step.texel_m * step.texel_m * ripple_dt;
            step.rain_amp = 0.006f;
            // Past a third of a chance per texel per step the drops stop being separable, so
            // the surplus goes into the depth of each crater instead of into more of them.
            if (step.rain_p > 0.35f)
            {
                step.rain_amp *= step.rain_p / 0.35f;
                step.rain_p = 0.35f;
            }
        }
    }

    // The window is solved once per frame, so it only moves on the first step of one. Both
    // centres are snapped to whole texels, which is what makes this a copy of the field
    // rather than a resample of it - resampling every frame smears it into mush.
    step.shift_u = (env.water_ripple_win.x - g_ripple_prev_x) / step.win;
    step.shift_v = (env.water_ripple_win.y - g_ripple_prev_z) / step.win;
    // The window the field holds once these steps ran. Only when a step runs: otherwise the
    // field still holds the OLD window, and moving the recorded centre without resampling would
    // shift the whole thing sideways.
    g_ripple_prev_x = env.water_ripple_win.x;
    g_ripple_prev_z = env.water_ripple_win.y;
    return true;
}

// One or more fixed steps of the interaction ripple field (DESIGN2.md section 2). The real-space
// half - the window shift, the losses at the rim and on dry ground, the sources - is
// da_water_ripple.ps; the wave motion is solved in Fourier space by da_ripple_propagate.cs
// between two FFTs (da_ripple_fft.h): Tessendorf's eWave. The readers bind one fixed name: the
// solver's last pass lands in scratch b and is copied into "$user$water_ripple0" - a copy is 8 MB
// at the top tier, and it is what lets every consumer read the finished step under one name
// instead of strobing between two.
void CRenderTarget::phase_water_ripple()
{
    if (!da_ripple_active(*this))
        return;
    if (!s_water_ripple)
        s_water_ripple.create("da_water_ripple");
    da_ripple_spectral_create(s_water_ripple_fft, s_water_ripple_prop, u32(ps_r__water_ripple_active));
    if (!s_water_ripple || !s_water_ripple_fft || !s_water_ripple_prop)
        return;

    PIX_EVENT(DA_phase_water_ripple);

    DaRippleStep step;
    if (!da_ripple_prepare(*this, step))
        return;
    for (int s = 0; s < step.steps; ++s)
        phase_water_ripple_step(RCache, step, s, false);

    // Back to the device-sized viewport and target for whatever renders next.
    u_setrt_scene_base(RCache);
}

// The same steps as compute passes on the given context (the async path): no per-step copy, the
// last step stays in scratch b until phase_water_ripple_publish. -1 = no compute step here (the
// caller runs phase_water_ripple), else the steps run this frame.
int CRenderTarget::phase_water_ripple_cs(CBackend& cmd_list, bool verify)
{
    m_ripple_publish = false;
    if (!da_ripple_active(*this))
        return 0;
    if (!rt_WaterRippleFFT[0]->pUAView || !rt_WaterRippleFFT[1]->pUAView)
        return -1;
    // Asked for once: a content without the shader keeps the pixel step.
    static const bool shader_exists = da_async_shader_exists("da_water_ripple.cs");
    if (!s_water_ripple_cs && shader_exists)
    {
        static CBlender_da_water_ripple_cs b_ripple_cs;
        s_water_ripple_cs.create(&b_ripple_cs, "r2" DELIMITER "da_water_ripple_cs");
    }
    da_ripple_spectral_create(s_water_ripple_fft, s_water_ripple_prop, u32(ps_r__water_ripple_active));
    if (!s_water_ripple_cs || !s_water_ripple_fft || !s_water_ripple_prop)
        return -1;
    if (verify && !s_water_ripple)
        s_water_ripple.create("da_water_ripple");

    PIX_EVENT_CTX(cmd_list, DA_phase_water_ripple_cs);

    DaRippleStep step;
    if (!da_ripple_prepare(*this, step))
        return 0;
    // Checks on the first step, both from the same inputs; the step clock (the rain's hash) is put
    // back after the extra run. Port: the pixel step, which also publishes into the state, so the
    // state is restored too. Schedule: the compute step inline on the immediate context (it
    // publishes nothing), and the job's own step is kept for the compare after the join.
    const bool schedule = verify && da_async_verify_schedule();
    const bool port = verify && !schedule && s_water_ripple;
    for (int s = 0; s < step.steps; ++s)
    {
        if (s == 0 && (port || schedule))
        {
            const u32 clock = g_ripple_step;
            if (port)
            {
                da_async_verify_keep(cmd_list, "ripple_state", rt_WaterRipple->pSurface);
                phase_water_ripple_step(cmd_list, step, 0, false);
                da_async_verify_keep(cmd_list, da_async_verify_outputs[2], rt_WaterRippleFFT[1]->pSurface);
                da_async_verify_restore(cmd_list, "ripple_state", rt_WaterRipple->pSurface);
            }
            else
            {
                phase_water_ripple_step(RCache, step, 0, true);
                da_async_verify_keep(RCache, da_async_verify_outputs[2], rt_WaterRippleFFT[1]->pSurface);
            }
            g_ripple_step = clock;
        }
        phase_water_ripple_step(cmd_list, step, s, true);
        if (s == 0 && port)
            da_async_verify_compare(cmd_list, da_async_verify_outputs[2], rt_WaterRippleFFT[1]->pSurface);
        if (s == 0 && schedule)
        {
            da_async_verify_keep(cmd_list, da_async_verify_ripple_job, rt_WaterRippleFFT[1]->pSurface);
            da_async_verify_expect(da_async_verify_outputs[2], da_async_verify_kept(da_async_verify_ripple_job));
        }
    }
    m_ripple_publish = true;
    return step.steps;
}

// The accumulator and the step clock stay advanced: the field only falls behind by those steps.
void da_water_ripple_lost(CRenderTarget& target)
{
    target.m_ripple_publish = false;
    g_ripple_prev_x = g_ripple_held_x;
    g_ripple_prev_z = g_ripple_held_z;
}

// The compute path's result under the name every reader binds. The readers of the new step all
// sit in phase_combine (water, puddle reflections, combine_2), so it runs at the join; the
// G-buffer and the rain read the previous step on both paths.
void CRenderTarget::phase_water_ripple_publish()
{
    if (!m_ripple_publish)
        return;
    m_ripple_publish = false;
    HW.get_context(RCache.context_id)->CopyResource(rt_WaterRipple->pSurface, rt_WaterRippleFFT[1]->pSurface);
}

void CRenderTarget::phase_water_ripple_step(CBackend& cmd_list, const DaRippleStep& step, int index, bool compute)
{
    const u32 texels = step.texels;
    const float shift_u = index == 0 ? step.shift_u : 0.f;
    const float shift_v = index == 0 ? step.shift_v : 0.f;
    // Every source in the sim is a depth in metres; sim2.yz say how many steps that depth is
    // spread over. Feeding it once per step instead drives the field into its clamp. sim2.w is
    // the clock the slots' ring radius runs on.
    const auto real_space_constants = [&] {
        cmd_list.set_c("da_ripple_sim", ripple_damping_land, 0.f, float(texels), float(g_ripple_step));
        cmd_list.set_c("da_ripple_src", shift_u, shift_v, step.rain_p, step.rain_amp);
        cmd_list.set_c("da_ripple_sim2", ripple_dt, 0.10f, 0.15f, step.speed);
        cmd_list.set_c("da_ripple_sim3", step.edge, step.texel_m, 0.f, 0.f);
    };

    // ---- real space: shift, losses, sources, into scratch a ----------------------------------
    if (!compute)
    {
        u_setrt(cmd_list, rt_WaterRippleFFT[0], nullptr, nullptr, (ID3DDepthStencilView*)nullptr);
        // u_setrt binds the target but leaves the viewport alone, and the field is not screen
        // sized: without this the quad rasterizes at screen size and only a corner of it lands.
        cmd_list.SetViewport({ 0.f, 0.f, float(rt_WaterRipple->dwWidth), float(rt_WaterRipple->dwHeight), 0.f, 1.f });
        cmd_list.set_Stencil(FALSE);
        cmd_list.set_Z(FALSE);
        cmd_list.set_CullMode(CULL_NONE);
        cmd_list.set_ColorWriteEnable();

        // FVF::TL and the same quad as our other fullscreen passes: a vertex layout that does
        // not match the vertex shader is dropped by DirectX without a single message.
        u32 Offset = 0;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
        pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
        pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

        cmd_list.set_Element(s_water_ripple->E[0]);
        cmd_list.set_Geometry(g_combine);
        real_space_constants();
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

        // The scratch just drawn into is the compute passes' input and, two passes on, their
        // output: a target still bound as a render target cannot be bound as either.
        cmd_list.set_RT(nullptr, 0);
        cmd_list.set_RT(nullptr, 1);
        cmd_list.set_RT(nullptr, 2);
        cmd_list.set_RT(nullptr, 3);
    }
    else
    {
        const u32 groups = (texels + 15) / 16;
        da_ripple_compute(cmd_list, *s_water_ripple_cs->E[index ? 1 : 0]->passes[0], rt_WaterRippleFFT[0]->pUAView,
            groups, groups, real_space_constants);
    }

    // ---- Fourier space: a -> b -> a (forward), step, b -> a -> b (inverse) ----------------
    const u32 groups_prop = (texels + 15) / 16;
    const float scale_inv = 1.f / float(texels);
    const float k_per_index = PI_MUL_2 / step.win;
    const auto fft = [&](u32 element, ID3D11UnorderedAccessView* out, float axis, float inverse, float scale) {
        da_ripple_compute(cmd_list, *s_water_ripple_fft->E[element]->passes[0], out, texels, 1,
            [&] { cmd_list.set_c("da_fft_ctl", axis, inverse, scale, 0.f); });
    };
    fft(0, rt_WaterRippleFFT[1]->pUAView, 0.f, 0.f, 1.f);
    fft(1, rt_WaterRippleFFT[0]->pUAView, 1.f, 0.f, 1.f);
    da_ripple_compute(cmd_list, *s_water_ripple_prop->E[0]->passes[0], rt_WaterRippleFFT[1]->pUAView, groups_prop,
        groups_prop, [&] {
            cmd_list.set_c("da_ripple_disp", k_per_index, step.depth, ripple_dt, float(texels));
            cmd_list.set_c("da_ripple_disp2", ripple_tension, ripple_viscosity, ripple_loss_flat, 0.f);
        });
    fft(1, rt_WaterRippleFFT[0]->pUAView, 0.f, 1.f, scale_inv);
    fft(0, rt_WaterRippleFFT[1]->pUAView, 1.f, 1.f, scale_inv);

    // Hand the finished step to whoever reads it next - the following step, or the surface
    // shader this frame - under the one name every reader binds.
    if (!compute)
        HW.get_context(cmd_list.context_id)->CopyResource(rt_WaterRipple->pSurface, rt_WaterRippleFFT[1]->pSurface);
    ++g_ripple_step;
}

// What the field holds, read back through a staging copy and logged: the extent, the largest
// height and velocity and the height's rms over the three surfaces. r__water_ripple_stats, for
// the QA rig - a screenshot cannot say whether a texture is empty or merely too faint to see.
static void da_water_ripple_stats_one(pcstr name, const ref_rt& rt, pcstr dump = nullptr)
{
    if (!rt || !rt->pSurface)
    {
        Msg("* [ripple] %s: no target", name);
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    rt->pSurface->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
    {
        Msg("* [ripple] %s: no staging copy", name);
        return;
    }
    auto* ctx = HW.get_context(RCache.context_id);
    ctx->CopyResource(staging, rt->pSurface);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        double sum2 = 0.0;
        float max_h = 0.f, max_v = 0.f;
        u32 nonzero = 0, nans = 0;
        for (u32 y = 0; y < desc.Height; ++y)
        {
            const float* row = reinterpret_cast<const float*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
            for (u32 x = 0; x < desc.Width; ++x)
            {
                const float h = row[x * 2], v = row[x * 2 + 1];
                if (h != h || v != v)
                {
                    ++nans;
                    continue;
                }
                if (h != 0.f || v != 0.f)
                    ++nonzero;
                sum2 += double(h) * double(h);
                max_h = std::max(max_h, _abs(h));
                max_v = std::max(max_v, _abs(v));
            }
        }
        // The height as a picture, for the rig: 128 is flat, two centimetres either way is the
        // full range. A number cannot say whether a field is rings or a smear.
        if (dump)
        {
            string_path path;
            FS.update_path(path, "$app_data_root$", dump);
            if (IWriter* W = FS.w_open(path))
            {
                string64 head;
                xr_sprintf(head, "P5\n%u %u\n255\n", desc.Width, desc.Height);
                W->w(head, u32(xr_strlen(head)));
                xr_vector<u8> line(desc.Width);
                for (u32 y = 0; y < desc.Height; ++y)
                {
                    const float* row = reinterpret_cast<const float*>(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch);
                    for (u32 x = 0; x < desc.Width; ++x)
                        line[x] = u8(clampr(128.f + row[x * 2] * (127.f / 0.02f), 0.f, 255.f));
                    W->w(line.data(), desc.Width);
                }
                FS.w_close(W);
            }
        }
        ctx->Unmap(staging, 0);
        Msg("* [ripple] %s: %ux%u, %u nonzero, %u nan, max |h| %.4f m, max |v| %.3f m/s, rms h %.5f", name, desc.Width,
            desc.Height, nonzero, nans, max_h, max_v, float(sqrt(sum2 / double(desc.Width * desc.Height))));
    }
    else
        Msg("* [ripple] %s: map failed", name);
    _RELEASE(staging);
}

void da_water_ripple_stats()
{
    if (!RImplementation.Target)
        return;
    Msg("* [ripple] active %d texels, step %u, accumulator %.3f", ps_r__water_ripple_active, g_ripple_step, g_ripple_acc);
    string64 dump;
    xr_sprintf(dump, "water_ripple_%u.pgm", g_ripple_step);
    da_water_ripple_stats_one("state", RImplementation.Target->rt_WaterRipple, dump);
    da_water_ripple_stats_one("scratch a", RImplementation.Target->rt_WaterRippleFFT[0]);
    da_water_ripple_stats_one("scratch b", RImplementation.Target->rt_WaterRippleFFT[1]);
}
} // namespace xray::render::RENDER_NAMESPACE
