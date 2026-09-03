#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Environment.h"
#include "xrCore/FMesh.hpp"
#include "FTreeVisual.h"
#include "Common/OGF_GContainer_Vertices.hpp"

namespace xray::render::RENDER_NAMESPACE
{
shared_str m_xform;
shared_str m_xform_v;
shared_str c_consts;
shared_str c_wave;
shared_str c_wind;
shared_str c_c_bias;
shared_str c_c_scale;
shared_str c_c_sun;

FTreeVisual::FTreeVisual(void) {}
FTreeVisual::~FTreeVisual(void) {}
void FTreeVisual::Release() { dxRender_Visual::Release(); }
void FTreeVisual::Load(const char* N, IReader* data, u32 dwFlags)
{
    dxRender_Visual::Load(N, data, dwFlags);

    const VertexElement* vFormat = nullptr;

    // read vertices
    R_ASSERT(data->find_chunk(OGF_GCONTAINER));
    {
        // verts
        u32 ID = data->r_u32();
        vBase = data->r_u32();
        vCount = data->r_u32();
        vFormat = RImplementation.getVB_Format(ID);

        VERIFY(nullptr == p_rm_Vertices);
        p_rm_Vertices = RImplementation.getVB(ID);
        p_rm_Vertices->AddRef();

        // indices
        dwPrimitives = 0;
        ID = data->r_u32();
        iBase = data->r_u32();
        iCount = data->r_u32();
        dwPrimitives = iCount / 3;

        VERIFY(nullptr == p_rm_Indices);
        p_rm_Indices = RImplementation.getIB(ID);
        p_rm_Indices->AddRef();
    }

    // load tree-def
    R_ASSERT(data->find_chunk(OGF_TREEDEF2));
    {
        data->r(&xform, sizeof(xform));
        data->r(&c_scale, sizeof(c_scale));
        c_scale.rgb.mul(.5f);
        c_scale.hemi *= .5f;
        c_scale.sun *= .5f;
        data->r(&c_bias, sizeof(c_bias));
        c_bias.rgb.mul(.5f);
        c_bias.hemi *= .5f;
        c_bias.sun *= .5f;
        // Msg				("hemi[%f / %f], sun[%f / %f]",c_scale.hemi,c_bias.hemi,c_scale.sun,c_bias.sun);
    }

    // Natural frequency from the real height: f = 1.2 / sqrt(H) Hz is the field regression
    // for trees (a 15 m crown swings at ~0.3 Hz, a 3 m bush at ~0.7). The old per-tree
    // factor came from a position hash and made every tree on the map swing at roughly
    // the same rate, size-blind.
    {
        const float H = std::max(vis.box.vMax.y - vis.box.vMin.y, 1.f);
        m_wind_omega = PI_MUL_2 * 1.2f / _sqrt(H);
    }

    /*if (RImplementation.o.ffp && dcl_equal(vFormat, mu_model_decl_unpacked))
    {
        const size_t vertices_size = vCount * sizeof(mu_model_vert_unpacked);

        const auto new_buffer = xr_new<VertexStagingBuffer>();
        new_buffer->Create(vertices_size);

        auto vert_new = static_cast<mu_model_vert_unpacked*>(new_buffer->Map());
        const auto vert_orig = static_cast<mu_model_vert_unpacked*>(p_rm_Vertices->Map(vBase, vertices_size, true)); // read-back
        CopyMemory(vert_new, vert_orig, vertices_size);

        for (size_t i = 0; i < vCount; ++i)
        {
            //vert_new->P.mul(xform.j);
            ++vert_new;
        }

        new_buffer->Unmap(true);
        p_rm_Vertices->Unmap(false);
        _RELEASE(p_rm_Vertices);
        p_rm_Vertices = new_buffer;
        vBase = 0;
    }*/

    // Geom
    rm_geom.create(vFormat, *p_rm_Vertices, *p_rm_Indices);

    // Get constants
    m_xform = "m_xform";
    m_xform_v = "m_xform_v";
    c_consts = "consts";
    c_wave = "wave";
    c_wind = "wind";
    c_c_bias = "c_bias";
    c_c_scale = "c_scale";
    c_c_sun = "c_sun";
}

struct FTreeVisual_setup
{
    u32 dwFrame;
    float scale;
    Fvector4 wave;
    Fvector4 wind;

    FTreeVisual_setup(): dwFrame(0), scale(0) {}

    void calculate()
    {
        dwFrame = Device.dwFrame;
        auto& env = g_pGamePersistent->Environment();
        CEnvDescriptor& desc = env.CurrentEnv;

        // The wind heading used to spin full circle every m_fTreeRotation seconds - crowns leaned
        // east, then north, then west on a windless noon. Now it comes from the effective-wind
        // service (weather heading + bounded wander).
        const float dir = env.eff_wind_dir;
        wind.set(_sin(dir), 0, _cos(dir), 0);
        wind.normalize();
        // The authored per-weather amplitude is tiny (DA weathers sit at ~0.05 rad, an
        // imperceptible 3 degrees), so the service envelope has to overshoot hard - but from
        // the authored look in calm air: a clear day stays near x1 (the first curve made even
        // clear weather "sway pretty hard"), a storm gust reaches ~x4. Field-test driven
        // three times: too weak at (0.55 + 0.80*var), trunk-slide territory at
        // (0.80 + 4.50*norm); the shader also hard-caps the total bend at 0.38*H now, so
        // storm weathers that author amplitude 0.10 cannot fold a crown over.
        wind.mul(desc.m_fTreeAmplitude * (0.50f + 3.20f * env.eff_wind_norm));

        scale = 1.f / float(FTreeVisual_quant);

        // setup constants: the wave phase comes from the service accumulator, which advances
        // faster in strong wind (crowns whip quicker in a gust, they do not just lean further).
        wave.set(desc.m_fTreeWave.x, desc.m_fTreeWave.y, desc.m_fTreeWave.z, env.eff_tree_phase); // wave
        wave.div(PI_MUL_2);
    }
};

FTreeVisual_setup& GetTreeVisualSetup()
{
    // thread_local is only race-free while calculate() stays a pure function of
    // frame-constant inputs (Device time/frame, CurrentEnv wind). If accumulated
    // wind state is ever added, switch to shared one-writer publication instead.
    static thread_local FTreeVisual_setup setup;
    if (setup.dwFrame != Device.dwFrame)
        setup.calculate();
    return setup;
}

void FTreeVisual::UpdateWindState() const
{
    // Once per frame per tree, whichever pass gets here first (the main pass and the cascades
    // run on different contexts; the exchange makes the first caller the integrator and the
    // others readers).
    const u32 frame = Device.dwFrame;
    if (m_wind_frame.exchange(frame, std::memory_order_acq_rel) == frame)
        return;
    if (!g_pGamePersistent || m_wind_omega <= 0.f)
        return;
    const auto& env = g_pGamePersistent->Environment();

    // The target is the wind at THIS root: the gust-field tongue passing over it, lifted by
    // a gust event. The shader multiplies its own bend by q, so 1 means "exactly the wind".
    const float target = env.SampleWindField(xform.c.x, xform.c.z) * (1.f + 0.6f * env.eff_wind_gust_event);

    float dt = Device.fTimeDelta;
    if (dt <= 0.f || dt > 0.25f)
        dt = 0.016f;
    // Semi-implicit Euler, sub-stepped so a stiff bush (omega ~4) stays stable at any frame
    // rate. zeta = 0.06: two to four visible cycles of ring-down after a gust.
    constexpr float zeta = 0.06f;
    const float w = m_wind_omega;
    const u32 steps = std::max(1u, u32(dt * w * 4.f) + 1);
    const float h = dt / float(steps);
    for (u32 i = 0; i < steps; ++i)
    {
        const float acc = w * w * (target - m_wind_q) - 2.f * zeta * w * m_wind_qd;
        m_wind_qd += acc * h;
        m_wind_q += m_wind_qd * h;
    }
    m_wind_q = clampr(m_wind_q, 0.05f, 2.5f);

    // wind_dbg: one tree's response against its target, twice a second, so the ring-down can
    // be read as numbers rather than trusted from a screenshot.
    extern ENGINE_API int ps_e_wind_dbg;
    if (ps_e_wind_dbg)
    {
        static const FTreeVisual* watched = nullptr;
        static float next = 0.f;
        if (!watched)
            watched = this;
        if (watched == this && Device.fTimeGlobal >= next)
        {
            next = Device.fTimeGlobal + 0.5f;
            Msg("* [wind-tree] target=%.3f q=%.3f qd=%.3f omega=%.2f", target, m_wind_q, m_wind_qd, m_wind_omega);
        }
    }
}

Fvector4 FTreeVisual::wind_state_row(float s) const
{
    UpdateWindState();
    // The shader's sway phase advances at the weather's tree speed; the factor turns that
    // into this tree's own natural frequency.
    const float speed = std::max(g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.m_fTreeSpeed : 1.f, 0.2f);
    const float freq_k = clampr(m_wind_omega / speed, 0.4f, 6.f);
    return Fvector4{s * c_scale.sun, s * c_bias.sun, m_wind_q, freq_k};
}

void FTreeVisual::Render(CBackend& cmd_list, float /*LOD*/, bool use_fast_geo)
{
    FTreeVisual_setup& tvs = GetTreeVisualSetup();
// setup constants
#if RENDER != R_R1
    Fmatrix xform_v;
    xform_v.mul_43(cmd_list.get_xform_view(), xform);
    cmd_list.tree.set_m_xform_v(xform_v); // matrix
#endif
    float s = ps_r__Tree_SBC;
    cmd_list.tree.set_m_xform(xform); // matrix
    cmd_list.tree.set_consts(tvs.scale, tvs.scale, 0, 0); // consts/scale
    cmd_list.tree.set_wave(tvs.wave); // wave
    // Crowns freeze in the SHADOW pass on the lower presets for the same reason the grass does
    // (see dx11DetailManager_VS.cpp): sub-texel smap motion turns into specular shimmer on
    // whatever the canopy shades. On High and Extreme (r__tree_shadow_sway) the shadow sways
    // with the crown - the cost is the wind chain once more per cascade.
#if RENDER != R_R1
    if (!ps_r__tree_shadow_sway && RImplementation.get_context(cmd_list.context_id).o.phase == CRender::PHASE_SMAP)
    {
        // set_wind takes a mutable ref; the value itself never changes.
        static Fvector4 wind_zero{};
        cmd_list.tree.set_wind(wind_zero);
    }
    else
#endif
        cmd_list.tree.set_wind(tvs.wind); // wind
#if RENDER != R_R1
    s *= 1.3333f;
    cmd_list.tree.set_c_scale(s * c_scale.rgb.x, s * c_scale.rgb.y, s * c_scale.rgb.z, s * c_scale.hemi); // scale
    cmd_list.tree.set_c_bias(s * c_bias.rgb.x, s * c_bias.rgb.y, s * c_bias.rgb.z, s * c_bias.hemi); // bias
#else
    const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
    cmd_list.tree.set_c_scale(s * c_scale.rgb.x, s * c_scale.rgb.y, s * c_scale.rgb.z, s * c_scale.hemi); // scale
    cmd_list.tree.set_c_bias(s * c_bias.rgb.x + desc.ambient.x, s * c_bias.rgb.y + desc.ambient.y,
        s * c_bias.rgb.z + desc.ambient.z, s * c_bias.hemi); // bias
#endif
    const Fvector4 row = wind_state_row(s);
    cmd_list.tree.set_c_sun(row.x, row.y, row.z, row.w); // sun + crown wind state
}

#ifdef USE_DX11
bool FTreeVisual::GetInstancedDraw(float /*LOD*/, FTreeVisualInstancedDraw& /*draw*/) { return false; }

void FTreeVisual::SetupInstancedGlobals(CBackend& cmd_list)
{
    FTreeVisual_setup& tvs = GetTreeVisualSetup();
    cmd_list.tree.set_consts(tvs.scale, tvs.scale, 0, 0);
    cmd_list.tree.set_wave(tvs.wave);
    // Same rule as the scalar path in Render(): instanced batches drawn into a shadow map
    // take the frozen wind. Without this the batched trees swayed in the cascades while the
    // scalar-path trees stood still, and the mismatch showed as a crown's shadow sliding
    // across the ground it stands on.
    if (!ps_r__tree_shadow_sway && RImplementation.get_context(cmd_list.context_id).o.phase == CRender::PHASE_SMAP)
    {
        static Fvector4 wind_zero{};
        cmd_list.tree.set_wind(wind_zero);
    }
    else
        cmd_list.tree.set_wind(tvs.wind);
}

void FTreeVisual::FillInstanceData(CBackend& cmd_list, FTreeVisualInstanceData& data) const
{
    Fmatrix xform_v;
    xform_v.mul_43(cmd_list.get_xform_view(), xform);

    // Match the existing DX11 constant-buffer matrix packing.
    data.vectors[0].set(xform._11, xform._21, xform._31, xform._41);
    data.vectors[1].set(xform._12, xform._22, xform._32, xform._42);
    data.vectors[2].set(xform._13, xform._23, xform._33, xform._43);
    data.vectors[3].set(xform_v._11, xform_v._21, xform_v._31, xform_v._41);
    data.vectors[4].set(xform_v._12, xform_v._22, xform_v._32, xform_v._42);
    data.vectors[5].set(xform_v._13, xform_v._23, xform_v._33, xform_v._43);

    const float scale = ps_r__Tree_SBC * 1.3333f;
    data.vectors[6].set(scale * c_scale.rgb.x, scale * c_scale.rgb.y, scale * c_scale.rgb.z,
        scale * c_scale.hemi);
    data.vectors[7].set(scale * c_bias.rgb.x, scale * c_bias.rgb.y, scale * c_bias.rgb.z,
        scale * c_bias.hemi);
    data.vectors[8] = wind_state_row(scale);
}
#endif

#define PCOPY(a) a = pFrom->a
void FTreeVisual::Copy(dxRender_Visual* pSrc)
{
    dxRender_Visual::Copy(pSrc);

    FTreeVisual* pFrom = dynamic_cast<FTreeVisual*>(pSrc);

    PCOPY(rm_geom);
    PCOPY(p_rm_Vertices);
    if (p_rm_Vertices)
        p_rm_Vertices->AddRef();
    PCOPY(vBase);
    PCOPY(vCount);
    PCOPY(vStride);
    PCOPY(p_rm_Indices);
    if (p_rm_Indices)
        p_rm_Indices->AddRef();
    PCOPY(iBase);
    PCOPY(iCount);
    PCOPY(dwPrimitives);

    PCOPY(xform);
    PCOPY(c_scale);
    PCOPY(c_bias);
}

//-----------------------------------------------------------------------------------
// Stripified Tree
//-----------------------------------------------------------------------------------
FTreeVisual_ST::FTreeVisual_ST(void) {}
FTreeVisual_ST::~FTreeVisual_ST(void) {}
void FTreeVisual_ST::Release() { inherited::Release(); }
void FTreeVisual_ST::Load(const char* N, IReader* data, u32 dwFlags) { inherited::Load(N, data, dwFlags); }
void FTreeVisual_ST::Render(CBackend& cmd_list, float LOD, bool use_fast_geo)
{
    inherited::Render(cmd_list, LOD, use_fast_geo);
    cmd_list.set_Geometry(rm_geom);
    cmd_list.Render(D3DPT_TRIANGLELIST, vBase, 0, vCount, iBase, dwPrimitives);
    cmd_list.stat.r.s_flora.add(vCount);
}
#ifdef USE_DX11
bool FTreeVisual_ST::GetInstancedDraw(float /*LOD*/, FTreeVisualInstancedDraw& draw)
{
    if (!rm_geom)
        return false;

    draw.geometry = &*rm_geom;
    draw.base_vertex = vBase;
    draw.vertex_count = vCount;
    draw.start_index = iBase;
    draw.primitive_count = dwPrimitives;
    return true;
}
#endif
void FTreeVisual_ST::Copy(dxRender_Visual* pSrc) { inherited::Copy(pSrc); }
//-----------------------------------------------------------------------------------
// Progressive Tree
//-----------------------------------------------------------------------------------
FTreeVisual_PM::FTreeVisual_PM(void)
{
    pSWI = nullptr;
    last_lod = 0;
}
FTreeVisual_PM::~FTreeVisual_PM(void) {}
void FTreeVisual_PM::Release() { inherited::Release(); }
void FTreeVisual_PM::Load(const char* N, IReader* data, u32 dwFlags)
{
    inherited::Load(N, data, dwFlags);
    R_ASSERT(data->find_chunk(OGF_SWICONTAINER));
    {
        u32 ID = data->r_u32();
        pSWI = RImplementation.getSWI(ID);
    }
}
u32 FTreeVisual_PM::SelectLOD(float LOD)
{
    int lod_id = last_lod;
    if (LOD >= 0.f)
    {
        lod_id = iFloor((1.f - LOD) * float(pSWI->count - 1) + 0.5f);
        last_lod = lod_id;
    }
    VERIFY(lod_id >= 0 && lod_id < int(pSWI->count));
    return u32(lod_id);
}
void FTreeVisual_PM::Render(CBackend& cmd_list, float LOD, bool use_fast_geo)
{
    inherited::Render(cmd_list, LOD, use_fast_geo);
    FSlideWindow& SW = pSWI->sw[SelectLOD(LOD)];
    cmd_list.set_Geometry(rm_geom);
    cmd_list.Render(D3DPT_TRIANGLELIST, vBase, 0, SW.num_verts, iBase + SW.offset, SW.num_tris);
    cmd_list.stat.r.s_flora.add(SW.num_verts);
}
#ifdef USE_DX11
bool FTreeVisual_PM::GetInstancedDraw(float LOD, FTreeVisualInstancedDraw& draw)
{
    if (!rm_geom || !pSWI || !pSWI->count)
        return false;

    FSlideWindow& window = pSWI->sw[SelectLOD(LOD)];
    draw.geometry = &*rm_geom;
    draw.base_vertex = vBase;
    draw.vertex_count = window.num_verts;
    draw.start_index = iBase + window.offset;
    draw.primitive_count = window.num_tris;
    return true;
}
#endif
void FTreeVisual_PM::Copy(dxRender_Visual* pSrc)
{
    inherited::Copy(pSrc);
    FTreeVisual_PM* pFrom = dynamic_cast<FTreeVisual_PM*>(pSrc);
    PCOPY(pSWI);
}
} // namespace xray::render::RENDER_NAMESPACE
