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
        CEnvDescriptor& desc = g_pGamePersistent->Environment().CurrentEnv;

        // Calc wind-vector3, scale
        float tm_rot = PI_MUL_2 * Device.fTimeGlobal / desc.m_fTreeRotation;

        wind.set(_sin(tm_rot), 0, _cos(tm_rot), 0);
        wind.normalize();
        wind.mul(desc.m_fTreeAmplitude); // dir1*amplitude

        scale = 1.f / float(FTreeVisual_quant);

        // setup constants
        wave.set(desc.m_fTreeWave.x, desc.m_fTreeWave.y, desc.m_fTreeWave.z, Device.fTimeGlobal * desc.m_fTreeSpeed); // wave
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
    cmd_list.tree.set_c_sun(s * c_scale.sun, s * c_bias.sun, 0, 0); // sun
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
    data.vectors[8].set(scale * c_scale.sun, scale * c_bias.sun, 0, 0);
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
