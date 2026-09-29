#include "stdafx.h"
#pragma hdrstop

#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/Blender_Recorder.h"
#include "Layers/xrRender/Blender.h"
#include "Layers/xrRender/tss.h"
#include "Layers/xrRenderPC_R4/da_dxc.h"

#include <mutex>

namespace xray::render::RENDER_NAMESPACE
{
void fix_texture_name(pstr fn);

void CBlender_Compile::r_Stencil(BOOL Enable, u32 Func, u32 Mask, u32 WriteMask, u32 Fail, u32 Pass, u32 ZFail)
{
    RS.SetRS(D3DRS_STENCILENABLE, BC(Enable));
    if (!Enable)
        return;
    RS.SetRS(D3DRS_STENCILFUNC, Func);
    RS.SetRS(D3DRS_STENCILMASK, Mask);
    RS.SetRS(D3DRS_STENCILWRITEMASK, WriteMask);
    RS.SetRS(D3DRS_STENCILFAIL, Fail);
    RS.SetRS(D3DRS_STENCILPASS, Pass);
    RS.SetRS(D3DRS_STENCILZFAIL, ZFail);
    //	Since we never really support different options for
    //	CW/CCW stencil use it to mimic DX9 behaviour for
    //	single-sided stencil
    RS.SetRS(D3DRS_CCW_STENCILFUNC, Func);
    RS.SetRS(D3DRS_CCW_STENCILFAIL, Fail);
    RS.SetRS(D3DRS_CCW_STENCILPASS, Pass);
    RS.SetRS(D3DRS_CCW_STENCILZFAIL, ZFail);
}

void CBlender_Compile::r_StencilRef(u32 Ref) { RS.SetRS(D3DRS_STENCILREF, Ref); }
void CBlender_Compile::r_CullMode(D3DCULL Mode) { RS.SetRS(D3DRS_CULLMODE, (u32)Mode); }
void CBlender_Compile::r_dx11Texture(LPCSTR ResourceName, LPCSTR texture, bool recursive /*= false*/)
{
    if (ctable.dx9compatibility && !recursive)
    {
        const u32 stage = r_Sampler(ResourceName, texture);
        if (stage != u16(-1))
            return;
    }

    VERIFY(ResourceName);
    if (!texture)
        return;
    //
    string256 TexName;
    xr_strcpy(TexName, texture);
    fix_texture_name(TexName);

    // Find index
    ref_constant C = ctable.get(ResourceName, ctable.dx9compatibility ? RC_dx11texture : u16(-1));
    // VERIFY(C);
    if (!C)
        return;

    R_ASSERT(C->type == RC_dx11texture);
    u32 stage = C->samp.index;

    passTextures.emplace_back(stage, ref_texture(RImplementation.Resources->_CreateTexture(TexName)));
}

void CBlender_Compile::i_dx11FilterAnizo(u32 s, BOOL value)
{
    VERIFY(s != u32(-1));
    RS.SetSAMP(s, XRDX11SAMP_ANISOTROPICFILTER, value);
}

u32 CBlender_Compile::r_dx11Sampler(LPCSTR ResourceName)
{
    // TODO: DX11: Check if we can use dwStage
    u32 stage = i_Sampler(ResourceName);

    if (stage == u32(-1))
        return u32(-1);

    //	init defaults here:

    //	Use D3DTADDRESS_CLAMP,	D3DTEXF_POINT,			D3DTEXF_NONE,	D3DTEXF_POINT
    if (0 == xr_strcmp(ResourceName, "smp_nofilter"))
    {
        i_Address(stage, D3DTADDRESS_CLAMP);
        i_Filter(stage, D3DTEXF_POINT, D3DTEXF_NONE, D3DTEXF_POINT);
    }

    //	Use D3DTADDRESS_CLAMP,	D3DTEXF_LINEAR,			D3DTEXF_NONE,	D3DTEXF_LINEAR
    else if (0 == xr_strcmp(ResourceName, "smp_rtlinear"))
    {
        i_Address(stage, D3DTADDRESS_CLAMP);
        i_Filter(stage, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
    }

    //	Use	D3DTADDRESS_WRAP,	D3DTEXF_LINEAR,			D3DTEXF_LINEAR,	D3DTEXF_LINEAR
    else if (0 == xr_strcmp(ResourceName, "smp_linear"))
    {
        i_Address(stage, D3DTADDRESS_WRAP);
        i_Filter(stage, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR);
    }

    //	Use D3DTADDRESS_WRAP,	D3DTEXF_ANISOTROPIC, 	D3DTEXF_LINEAR,	D3DTEXF_ANISOTROPIC
    else if (0 == xr_strcmp(ResourceName, "smp_base"))
    {
        i_Address(stage, D3DTADDRESS_WRAP);
        i_dx11FilterAnizo(stage, TRUE);
        // i_Filter(stage, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR);
    }

    //	Use D3DTADDRESS_CLAMP,	D3DTEXF_LINEAR,			D3DTEXF_NONE,	D3DTEXF_LINEAR
    else if (0 == xr_strcmp(ResourceName, "smp_material"))
    {
        i_Address(stage, D3DTADDRESS_CLAMP);
        i_Filter(stage, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
        RS.SetSAMP(stage, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
    }

    else if (0 == xr_strcmp(ResourceName, "smp_smap"))
    {
        i_Address(stage, D3DTADDRESS_CLAMP);
        i_Filter(stage, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
        RS.SetSAMP(stage, XRDX11SAMP_COMPARISONFILTER, TRUE);
        RS.SetSAMP(stage, XRDX11SAMP_COMPARISONFUNC, (u32)D3D_COMPARISON_LESS_EQUAL);
    }

    else if (0 == xr_strcmp(ResourceName, "smp_jitter"))
    {
        i_Address(stage, D3DTADDRESS_WRAP);
        i_Filter(stage, D3DTEXF_POINT, D3DTEXF_NONE, D3DTEXF_POINT);
    }

    return stage;
}

void CBlender_Compile::r_Pass(LPCSTR _vs, LPCSTR _gs, LPCSTR _ps, bool bFog, BOOL bZtest, BOOL bZwrite, BOOL bABlend,
                              D3DBLEND abSRC, D3DBLEND abDST, BOOL aTest, u32 aRef)
{
    RS.Invalidate();
    ctable.clear();
    passTextures.clear();
    passMatrices.clear();
    passConstants.clear();
    dwStage = 0;

    // Setup FF-units (Z-buffer, blender)
    PassSET_ZB(bZtest, bZwrite);
    PassSET_Blend(bABlend, abSRC, abDST, aTest, aRef);
    PassSET_LightFog(FALSE, bFog);

    // Create shaders
    SPS* ps = RImplementation.Resources->_CreatePS(_ps);
    u32 flags = 0;
    if (ps->constants.dx9compatibility)
        flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
    SVS* vs = RImplementation.Resources->_CreateVS(_vs, flags);
    SGS* gs = RImplementation.Resources->_CreateGS(_gs);
    dest.ps = ps;
    dest.vs = vs;
    dest.gs = gs;
#ifdef USE_DX11
    dest.hs = RImplementation.Resources->_CreateHS("null");
    dest.ds = RImplementation.Resources->_CreateDS("null");
    dest.cs = RImplementation.Resources->_CreateCS("null");
    UnifyShaderFormats();
#endif
    ctable.merge(&dest.ps->constants);
    ctable.merge(&dest.vs->constants);
    ctable.merge(&dest.gs->constants);

    // Last Stage - disable
    if (0 == xr_stricmp(_ps, "null"))
    {
        RS.SetTSS(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
        RS.SetTSS(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

#ifdef USE_DX11
void CBlender_Compile::r_TessPass(LPCSTR vs, LPCSTR hs, LPCSTR ds, LPCSTR gs, LPCSTR ps, bool bFog, BOOL bZtest,
    BOOL bZwrite, BOOL bABlend, D3DBLEND abSRC, D3DBLEND abDST, BOOL aTest, u32 aRef)
{
    r_Pass(vs, gs, ps, bFog, bZtest, bZwrite, bABlend, abSRC, abDST, aTest, aRef);

    dest.hs = RImplementation.Resources->_CreateHS(hs);
    dest.ds = RImplementation.Resources->_CreateDS(ds);

    // The tessellation stages may disagree with the ones r_Pass unified: then the table is built again
    // from every stage (no r_Constant has run yet).
    if (UnifyShaderFormats())
    {
        ctable.clear();
        ctable.merge(&dest.ps->constants);
        ctable.merge(&dest.vs->constants);
        ctable.merge(&dest.gs->constants);
    }
    ctable.merge(&dest.hs->constants);
    ctable.merge(&dest.ds->constants);
}

void CBlender_Compile::r_ComputePass(LPCSTR cs)
{
    dest.cs = RImplementation.Resources->_CreateCS(cs);

    ctable.merge(&dest.cs->constants);
}

bool CBlender_Compile::UnifyShaderFormats()
{
#ifdef XR_D3D12
    if (!da_dxc_active())
        return false;
    const auto live = [](const auto& stage) { return stage && stage->sh; };
    bool dxil = false, dxbc = false;
    string64 fell_back{};
    const auto classify = [&](const auto& stage, pcstr stage_name) {
        if (!live(stage))
            return;
        if (stage->dxil)
            dxil = true;
        else
        {
            dxbc = true;
            xr_strcat(fell_back, fell_back[0] ? ", " : "");
            xr_strcat(fell_back, stage_name);
        }
    };
    classify(dest.vs, "vs");
    classify(dest.ps, "ps");
    classify(dest.gs, "gs");
    classify(dest.hs, "hs");
    classify(dest.ds, "ds");
    if (!dxil || !dxbc)
        return false;

    pcstr vs_name = dest.vs ? dest.vs->source.c_str() : nullptr;
    pcstr ps_name = dest.ps ? dest.ps->source.c_str() : nullptr;
    // Mesh and amplification stages exist only as DXIL: such a pass stays as it is and the mesh
    // feature sees its pixel shader's format (mesh.md 3.7) and takes the vertex path.
    pcstr gs_name = dest.gs ? dest.gs->source.c_str() : nullptr;
    const bool mesh = (vs_name && strstr(vs_name, "#ms")) || (gs_name && strstr(gs_name, "#as"));
    if (mesh)
    {
        Msg("! [dxc] pass %s/%s: mesh stages with %s from FXC - the pass is not usable", vs_name ? vs_name : "-",
            ps_name ? ps_name : "-", fell_back);
        return false;
    }

    CResourceManager& resources = *RImplementation.Resources;
    string64 twins{};
    u32 replaced = 0;
    const auto twin = [&](auto& stage, pcstr stage_name, auto create) {
        if (!live(stage) || !stage->dxil)
            return;
        stage = create(stage->source.c_str());
        xr_strcat(twins, twins[0] ? ", " : "");
        xr_strcat(twins, stage_name);
        ++replaced;
    };
    // A pixel shader that needed FXC's backwards compatibility passes it on to its vertex shader.
    const u32 vs_flags = RSC_FXC | (dest.ps && dest.ps->constants.dx9compatibility ? D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY : 0);
    twin(dest.vs, "vs", [&](pcstr name) { return resources._CreateVS(name, vs_flags); });
    twin(dest.ps, "ps", [&](pcstr name) { return resources._CreatePS(name, RSC_FXC); });
    twin(dest.gs, "gs", [&](pcstr name) { return resources._CreateGS(name, RSC_FXC); });
    twin(dest.hs, "hs", [&](pcstr name) { return resources._CreateHS(name, RSC_FXC); });
    twin(dest.ds, "ds", [&](pcstr name) { return resources._CreateDS(name, RSC_FXC); });
    da_dxc_note_twins(replaced);
    // Once per stage combination: blenders build the same pass for several elements.
    static std::mutex logged_lock;
    static xr_vector<xr_string> logged;
    xr_string pass = vs_name ? vs_name : "-";
    pass += '/';
    pass += ps_name ? ps_name : "-";
    pass += twins;
    std::lock_guard lock(logged_lock);
    if (std::find(logged.begin(), logged.end(), pass) == logged.end())
    {
        logged.push_back(pass);
        Msg("! [dxc] pass %s/%s: DXBC twins for %s (%s fell back to FXC)", vs_name ? vs_name : "-",
            ps_name ? ps_name : "-", twins, fell_back);
    }
    return replaced != 0;
#else
    return false;
#endif
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
