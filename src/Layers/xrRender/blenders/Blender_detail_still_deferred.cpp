#include "stdafx.h"
#pragma hdrstop

#include "Blender_detail_still.h"
#include "uber_deffer.h"
#if defined(XR_NATIVE)
#include "Layers/xrRenderPC_R4/da_grass_mesh.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
CBlender_Detail_Still::CBlender_Detail_Still()
{
    description.CLS = B_DETAIL;
    description.version = 0;
}

void CBlender_Detail_Still::Save(IWriter& fs)
{
    IBlender::Save(fs);
    xrPWRITE_PROP(fs, "Alpha-blend", xrPID_BOOL, oBlend);
}

void CBlender_Detail_Still::Load(IReader& fs, u16 version)
{
    IBlender::Load(fs, version);
    xrPREAD_PROP(fs, xrPID_BOOL, oBlend);
}

LPCSTR CBlender_Detail_Still::getComment()
{
    return "LEVEL: detail objects";
}

#if RENDER == R_R2
void CBlender_Detail_Still::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

    switch (C.iElement)
    {
    case SE_R2_NORMAL_HQ: // deffer wave
        uber_deffer(C, false, "detail_w", "base", true);
        break;
    case SE_R2_NORMAL_LQ: // deffer still
        uber_deffer(C, false, "detail_s", "base", true);
        break;
    }
}
#else
// The season's snow under the stock details (da_detail_common.h buries a tuft its depth covers): the two
// textures of r3\da_snow_sample.h by name, and the samplers it may use. Each call looks its name up in
// the compiled shaders' tables and returns silently where none reads it.
// The stock detail cards write the foliage material (deffer_base_aref_flat.ps): the full-screen snow layer
// leaves them to their own season (keep, height, burial in the snow) like the blade grass beside them.
constexpr pcstr k_stock_detail = "DA_STOCK_DETAIL";

static void detail_bind_snow(CBlender_Compile& C)
{
    C.r_dx11Texture("s_da_snow_fine", "$user$da_snow_fine");
    C.r_dx11Texture("s_da_snow_level", "$user$da_snow_level");
    C.r_dx11Sampler("smp_rtlinear");
    C.r_dx11Sampler("smp_nofilter");
    C.r_dx11Sampler("smp_linear");
}

void CBlender_Detail_Still::Compile(CBlender_Compile& C)
{
    IBlender::Compile(C);

    bool bUseATOC = (RImplementation.o.msaa_alphatest == CRender::MSAA_ATEST_DX10_0_ATOC);

    switch (C.iElement)
    {
    case SE_R2_NORMAL_HQ: // deffer wave
        if (bUseATOC)
        {
            uber_deffer(C, false, "detail_w", "base_atoc", true, 0, true);
            detail_bind_snow(C);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_ColorWriteEnable(false, false, false, false);
            C.r_CullMode(D3DCULL_NONE);
            //	Alpha to coverage.
            C.RS.SetRS(XRDX11RS_ALPHATOCOVERAGE, TRUE);
            C.r_End();
        }

        uber_deffer(C, false, "detail_w", "base", true, 0, true, true, nullptr, k_stock_detail);
        detail_bind_snow(C);
        C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        C.r_StencilRef(0x01);
        C.r_CullMode(D3DCULL_NONE);
        if (bUseATOC)
            C.RS.SetRS(D3DRS_ZFUNC, D3DCMP_EQUAL);
        C.r_End();
        break;
    case SE_R2_NORMAL_LQ: // deffer still
        if (bUseATOC)
        {
            uber_deffer(C, false, "detail_s", "base_atoc", true, 0, true);
            detail_bind_snow(C);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
            C.r_ColorWriteEnable(false, false, false, false);
            //	Alpha to coverage.
            C.RS.SetRS(XRDX11RS_ALPHATOCOVERAGE, TRUE);
            C.r_End();
        }

        uber_deffer(C, false, "detail_s", "base", true, 0, true, true, nullptr, k_stock_detail);
        detail_bind_snow(C);
        C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        C.r_StencilRef(0x01);
        C.r_CullMode(D3DCULL_NONE);
        //	Need this for ATOC
        if (bUseATOC)
            C.RS.SetRS(D3DRS_ZFUNC, D3DCMP_EQUAL);
        C.r_End();
        break;
#if defined(XR_NATIVE)
    // Mesh-shader grass (mesh.md 3.6): elements 2 (wave) and 3 (still) are the vertex elements with
    // the grass mesh and amplification shaders; empty where the session has no mesh grass.
    case 2:
    case 3:
        if (da_grass_mesh_blender())
        {
            const bool wave = C.iElement == 2;
            // The option reaches the waving variant's compile; created here, the pass finds it by name.
            if (wave)
                RImplementation.addShaderOption("DETAIL_WAVE", "1");
            constexpr pcstr wave_ms = "deffer_detail_mesh#ms(DETAIL_WAVE,)";
            constexpr pcstr still_ms = "deffer_detail_mesh#ms";
            const ref_vs ms = RImplementation.Resources->_CreateVS(wave ? wave_ms : still_ms);
            RImplementation.clearAllShaderOptions();
            if (ms->sh &&
                uber_deffer(C, false, wave ? "detail_w" : "detail_s", "base", true, 0, true, true, wave ? wave_ms : still_ms,
                    k_stock_detail))
            {
                detail_bind_snow(C);
                C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
                C.r_StencilRef(0x01);
                C.r_CullMode(D3DCULL_NONE);
                C.r_End();
            }
        }
        break;
    // The shadow-pass variants of 2 and 3 (mesh.md 3.10): the same tufts, sway and alpha test, without
    // the G-buffer outputs no shadow pass binds.
    case 4:
    case 5:
        if (da_grass_mesh_blender())
        {
            const bool wave = C.iElement == 4;
            RImplementation.addShaderOption("DETAIL_DEPTH", "1");
            if (wave)
                RImplementation.addShaderOption("DETAIL_WAVE", "1");
            constexpr pcstr wave_ms = "deffer_detail_mesh#ms(DETAIL_WAVE,DETAIL_DEPTH,)";
            constexpr pcstr still_ms = "deffer_detail_mesh#ms(DETAIL_DEPTH,)";
            const ref_vs ms = RImplementation.Resources->_CreateVS(wave ? wave_ms : still_ms);
            RImplementation.clearAllShaderOptions();
            if (ms->sh)
            {
                uber_detail_mesh_depth(C, wave ? wave_ms : still_ms);
                detail_bind_snow(C);
                C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
                C.r_StencilRef(0x01);
                C.r_CullMode(D3DCULL_NONE);
                C.r_End();
            }
        }
        break;
#endif
    }
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
