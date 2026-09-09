#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Environment.h"
#include "xrCore/FMesh.hpp"
#include "FTreeVisual.h"
#include "Common/OGF_GContainer_Vertices.hpp"
#include "xrCore/Threading/ParallelFor.hpp"

#include <mutex>

namespace xray::render::RENDER_NAMESPACE
{
// One wind state per TREE. A tree in a level is several visuals sharing one root (the trunk,
// the crown, ...); each used to carry its own oscillator and its own height from its own
// bounding box, so the crown swung at a different rate and bent by a different profile than
// the trunk - the foliage "living apart from the trunk". The record is keyed by the root
// position (the placement every visual of the model carries) and refcounted by the visuals.
struct TreeWindShared
{
    u64 key{};
    float top{1.f};           // height of the tallest visual above the root (m)
    float omega{};            // natural angular frequency (rad/s), from top
    float q{1.f};             // response, 1 = following the gust field exactly
    float qd{};
    float gust{};
    float deviation{};
    Fvector sampled_root{};
    std::atomic<u32> frame{u32(-1)};
    std::mutex update_lock;
    u32 refs{};
    bool settled{};           // the state has been set to its first target (no swing on load)
    // A foliage visual (an alpha-tested leaf_wave card) shares this root. The tree format is
    // what every multiple-use model compiles to, and flora\trunk_wave dresses stumps, logs and
    // snags as readily as the trunk under a crown: the wave, the trunk profile, the flutter
    // and the motors all belong to a plant, and a plant has leaves. A trunk_wave visual with
    // no leaf_wave sibling at its root is wood, and wood stands still.
    bool foliage{};
};

static xr_map<u64, TreeWindShared*> g_tree_wind_shared;
static std::mutex g_tree_wind_shared_lock;

static u64 tree_wind_key(const Fvector& root)
{
    // 5 cm cells: the visuals of one model carry the same placement to the bit.
    const auto q = [](float v) { return u64(u32(iFloor(v * 20.f) + 0x100000)) & 0x1FFFFFu; };
    return q(root.x) | (q(root.y) << 21) | (q(root.z) << 42);
}

static TreeWindShared* tree_wind_shared_acquire(const Fvector& root, float top, bool foliage)
{
    std::lock_guard lock(g_tree_wind_shared_lock);
    const u64 key = tree_wind_key(root);
    TreeWindShared*& s = g_tree_wind_shared[key];
    if (!s)
    {
        s = xr_new<TreeWindShared>();
        s->key = key;
    }
    ++s->refs;
    s->top = std::max(s->top, top);
    s->foliage |= foliage;
    // Natural frequency from the real height, f = 1.0 / sqrt(H) Hz: a 15 m crown swings at
    // ~0.26 Hz, a 3 m bush at ~0.6 (the field regression sits a little above; this leans
    // toward the slower sway the trees had before they knew their height).
    s->omega = PI_MUL_2 * 1.0f / _sqrt(std::max(s->top, 1.f));
    return s;
}

static void tree_wind_shared_addref(TreeWindShared* s)
{
    if (!s)
        return;
    std::lock_guard lock(g_tree_wind_shared_lock);
    ++s->refs;
}

static void tree_wind_shared_release(TreeWindShared* s)
{
    if (!s)
        return;
    std::lock_guard lock(g_tree_wind_shared_lock);
    if (--s->refs == 0)
    {
        g_tree_wind_shared.erase(s->key);
        xr_delete(s);
    }
}
} // namespace xray::render::RENDER_NAMESPACE

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
FTreeVisual::~FTreeVisual(void) { tree_wind_shared_release(m_shared); }
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

    // One wind state per tree, registered by the root; the tallest visual sets the height.
    // The box of a level visual is world-space, so the top is measured from the root - a
    // crown visual's own box starts at its lowest branch and said nothing about the tree.
    // Foliage is what the blender compiled as alpha-tested (uber_deffer names that pixel
    // shader "_aref"): the leaf_wave cards of a crown or a bush, never a trunk_wave trunk.
    bool foliage = false;
    if (shader && shader->E[0] && !shader->E[0]->passes.empty())
    {
        const ref_ps& ps = shader->E[0]->passes[0]->ps;
        foliage = ps && ps->cName.c_str() && strstr(ps->cName.c_str(), "_aref");
    }
    m_shared = tree_wind_shared_acquire(xform.c, std::max(vis.box.vMax.y - xform.c.y, 1.f), foliage);

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
        // imperceptible 3 degrees), so the service envelope has to overshoot hard. What it must
        // NOT do is overshoot at the bottom: the curve used to start at half strength, so a
        // tree kept swaying at literally zero wind and a calm morning ran at 85 per cent of
        // nominal. It starts near nothing now and climbs faster, which leaves a storm where it
        // was and makes calm actually calm. The shader caps the total bend at 0.50*H, so storm
        // weathers that author amplitude 0.10 still cannot fold a crown over.
        wind.mul(desc.m_fTreeAmplitude * (0.04f + 3.60f * env.eff_wind_norm));

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

Fvector4 FTreeVisual::tree_wind_row() const
{
    // w carries two things (decoded by da_tree_row_valid / da_tree_row_gate in the shaders):
    // 1 + gate for a visual on its own analytic field, 3 + gate when the shared root sample
    // is valid; 0 stays the older producers' "no state, full bend". The gate is the root's
    // foliage: a stump or a log reads 0 and the wind passes it by.
    if (!m_shared)
        return Fvector4{1.f, 0.f, 0.f, 2.f};
    const float gate = m_shared->foliage ? 1.f : 0.f;
    const auto& s = *m_shared;
    // A quantized key can also join distinct nearby roots. Those visuals keep their own
    // analytic field instead of borrowing the other root's shader sample.
    const bool same_root = s.sampled_root.x == xform.c.x && s.sampled_root.z == xform.c.z;
    return Fvector4{s.top, s.gust, s.deviation, (same_root ? 3.f : 1.f) + gate};
}

bool FTreeVisual::NeedsWindUpdate() const
{
    return m_shared && m_shared->frame.load(std::memory_order_acquire) != Device.dwFrame;
}

void FTreeVisual::PrepareWind(const xr_vector<FTreeVisual*>& visuals)
{
    const auto integrate = [&visuals](const TaskRange<size_t>& range)
    {
        for (size_t i = range.begin(); i != range.end(); ++i)
            visuals[i]->UpdateWindState();
    };
    // Workers touch only tree-local state and immutable frame inputs. Join before the
    // draw code reads it; small lists stay inline to avoid scheduling overhead.
    if (visuals.size() >= 256 && TaskScheduler && TaskScheduler->GetWorkersCount() > 1)
        xr_parallel_for(TaskRange<size_t>(0, visuals.size(), 64), integrate);
    else
        integrate(TaskRange<size_t>(0, visuals.size(), 64));
}

void FTreeVisual::UpdateWindState() const
{
    TreeWindShared* s = m_shared;
    if (!s)
        return;
    const u32 frame = Device.dwFrame;
    if (s->frame.load(std::memory_order_acquire) == frame)
        return;
    // Publish completion after integration, not before it. Concurrent cascades may share
    // this tree; only the first caller integrates, while readers acquire the finished state.
    std::lock_guard lock(s->update_lock);
    if (s->frame.load(std::memory_order_relaxed) == frame)
        return;
    if (!g_pGamePersistent || s->omega <= 0.f)
        return;
    const auto& env = g_pGamePersistent->Environment();
    s->gust = env.SampleWindGust(xform.c.x, xform.c.z);
    s->deviation = env.SampleWindDeviation(xform.c.x, xform.c.z);
    s->sampled_root = xform.c;

    // The target is the gust field at THIS root - the tongue passing over it - lifted by a
    // gust event. The shader uses the state IN PLACE of the field's instantaneous value, so
    // the crown follows the tongues through its own inertia (lags one, overshoots a little
    // after it). Multiplying the field by a ringing copy of itself, as before, squared the
    // lull-to-tongue contrast and rocked every crown.
    const float target = (0.60f + 0.65f * s->gust) * (1.f + 0.6f * env.eff_wind_gust_event);

    if (!s->settled)
    {
        // The first frame after a load starts AT the field, not at 1: every crown on the level
        // swinging down to its lull at once was a visible settle.
        s->q = target;
        s->qd = 0.f;
        s->settled = true;
    }

    float dt = Device.fTimeDelta;
    if (dt <= 0.f || dt > 0.25f)
        dt = 0.016f;
    // Semi-implicit Euler, sub-stepped so a stiff bush (omega ~4) stays stable at any frame
    // rate. zeta = 0.30: foliage damps a crown hard (0.04-0.09 is the bare trunk) - one
    // visible overshoot after a gust, then it settles. 0.06 rang for cycles and read as
    // rocking, in calm air too.
    constexpr float zeta = 0.30f;
    const float w = s->omega;
    const u32 steps = std::max(1u, u32(dt * w * 4.f) + 1);
    const float h = dt / float(steps);
    for (u32 i = 0; i < steps; ++i)
    {
        const float acc = w * w * (target - s->q) - 2.f * zeta * w * s->qd;
        s->qd += acc * h;
        s->q += s->qd * h;
    }
    // Wide enough for the natural undershoot after a tongue passes: pinning at the floor held
    // the crown at a flat half-sway for a second.
    s->q = clampr(s->q, 0.35f, 1.6f);

    // wind_dbg: one tree's response against its target, twice a second, so the ring-down can
    // be read as numbers rather than trusted from a screenshot.
    extern ENGINE_API int ps_e_wind_dbg;
    if (ps_e_wind_dbg)
    {
        static std::mutex debug_lock;
        std::lock_guard debug_guard(debug_lock);
        static u64 watched = s->key;
        static float next = 0.f;
        if (watched == s->key && Device.fTimeGlobal >= next)
        {
            next = Device.fTimeGlobal + 0.5f;
            Msg("* [wind-tree] target=%.3f q=%.3f qd=%.3f omega=%.2f top=%.1f", target, s->q, s->qd, s->omega, s->top);
        }
    }
    s->frame.store(frame, std::memory_order_release);
}

Fvector4 FTreeVisual::wind_state_row(float s) const
{
    UpdateWindState();
    // The shader's sway phase advances at the weather's tree speed; the factor turns that
    // into this tree's own natural frequency.
    const float speed = std::max(g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.m_fTreeSpeed : 1.f, 0.2f);
    const float omega = m_shared ? m_shared->omega : 0.f;
    const float q = m_shared ? m_shared->q : 0.f; // 0 = no state, the shader falls back to the field
    const float freq_k = omega > 0.f ? clampr(omega / speed, 0.4f, 6.f) : 0.f;
    return Fvector4{s * c_scale.sun, s * c_bias.sun, q, freq_k};
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
    // The shadow passes take the same wind: a crown's shadow is the crown's, not a stiff copy.
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
    const Fvector4 tree = tree_wind_row();
    cmd_list.tree.set_c_tree(tree.x, tree.y, tree.z, tree.w);
}

#ifdef USE_DX11
bool FTreeVisual::GetInstancedDraw(float /*LOD*/, FTreeVisualInstancedDraw& /*draw*/) { return false; }

void FTreeVisual::SetupInstancedGlobals(CBackend& cmd_list)
{
    FTreeVisual_setup& tvs = GetTreeVisualSetup();
    cmd_list.tree.set_consts(tvs.scale, tvs.scale, 0, 0);
    cmd_list.tree.set_wave(tvs.wave);
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
    data.vectors[9] = tree_wind_row();
}
#endif

#define PCOPY(a) a = pFrom->a
void FTreeVisual::Copy(dxRender_Visual* pSrc)
{
    dxRender_Visual::Copy(pSrc);

    FTreeVisual* pFrom = dynamic_cast<FTreeVisual*>(pSrc);

    PCOPY(rm_geom);
    PCOPY(m_shared);
    tree_wind_shared_addref(m_shared);
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

bool FTreeVisual::rigid() const { return !m_shared || !m_shared->foliage; }

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
