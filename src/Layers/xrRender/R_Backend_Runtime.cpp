#include "stdafx.h"
#pragma hdrstop

#include "LightTrack.h"
#include "xrEngine/IRenderable.h"

#if defined(USE_DX11)
#include <DirectXMath.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
void CBackend::OnFrameEnd()
{
    if (!GEnv.isDedicatedServer)
    {
#if defined(USE_DX11)
        HW.get_context(context_id)->ClearState();
#endif
        Invalidate();
    }
}

void CBackend::OnFrameBegin()
{
    if (!GEnv.isDedicatedServer)
    {
        PGO(Msg("PGO:*****frame[%d]*****", Device.dwFrame));

#ifndef USE_DX9
        Invalidate();
        // DX9 sets base rt and base zb by default
#ifndef USE_OGL
        // XXX: Getting broken HUD hands for OpenGL after calling rmNormal()
        RImplementation.rmNormal(*this);
#else
        set_FB(HW.pFB);
#endif
        set_RT(RImplementation.Target->get_base_rt());
        set_ZB(RImplementation.Target->get_base_zb());
#endif

        ZeroMemory(&stat, sizeof(stat));
        set_Stencil(FALSE);
    }
}

void CBackend::Invalidate()
{
    detailRendering = false;
#if defined(USE_DX11)
    constants.discard_pending();
    texture_slice_epoch = 0;
#endif

    pRT[0] = 0;
    pRT[1] = 0;
    pRT[2] = 0;
    pRT[3] = 0;
    pZB = 0;
#if defined(USE_DX11)
    depth_dimensions_zb = nullptr;
#endif
#if defined(USE_OGL)
    pFB = 0;
    pp = 0;
#endif

    decl = nullptr;
    vb = 0;
    ib = 0;
    vb_stride = 0;

    state = nullptr;
    ps = 0;
    vs = 0;
    DX11_ONLY(gs = NULL);
#ifdef USE_DX11
    hs = 0;
    ds = 0;
    cs = 0;
#endif
    ctable = nullptr;

    T = nullptr;
    M = nullptr;
    C = nullptr;

    stencil_enable = u32(-1);
    stencil_func = u32(-1);
    stencil_ref = u32(-1);
    stencil_mask = u32(-1);
    stencil_writemask = u32(-1);
    stencil_fail = u32(-1);
    stencil_pass = u32(-1);
    stencil_zfail = u32(-1);
    cull_mode = u32(-1);
    fill_mode = u32(-1);
    z_enable = u32(-1);
    z_func = u32(-1);
    alpha_ref = u32(-1);
    colorwrite_mask = u32(-1);

    // Since constant buffers are unmapped (for DirecX 10)
    // transform setting handlers should be unmapped too.
    xforms.unmap();

#if defined(USE_DX11)
    m_pInputLayout = NULL;
    m_pInputLayoutDecl = nullptr;
    m_pInputLayoutSignature = nullptr;
    m_PrimitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    m_bChangedRTorZB = false;
    m_pInputSignature = NULL;
    for (int i = 0; i < MaxCBuffers; ++i)
    {
        m_aPixelConstants[i] = 0;
        m_aVertexConstants[i] = 0;
        m_aGeometryConstants[i] = 0;
        m_aHullConstants[i] = 0;
        m_aDomainConstants[i] = 0;
        m_aComputeConstants[i] = 0;
    }
    StateManager.Reset();
    SRVSManager.ResetDeviceState();
    SSManager.ResetContext(context_id);

    for (u32 gs_it = 0; gs_it < CTexture::mtMaxGeometryShaderTextures;)
        textures_gs[gs_it++] = 0;
    for (u32 hs_it = 0; hs_it < CTexture::mtMaxHullShaderTextures;)
        textures_hs[hs_it++] = 0;
    for (u32 ds_it = 0; ds_it < CTexture::mtMaxDomainShaderTextures;)
        textures_ds[ds_it++] = 0;
    for (u32 cs_it = 0; cs_it < CTexture::mtMaxComputeShaderTextures;)
        textures_cs[cs_it++] = 0;
#endif // USE_DX11

    for (u32 ps_it = 0; ps_it < CTexture::mtMaxPixelShaderTextures;)
        textures_ps[ps_it++] = nullptr;
    for (u32 vs_it = 0; vs_it < CTexture::mtMaxVertexShaderTextures;)
        textures_vs[vs_it++] = nullptr;
    for (auto& matrix : matrices)
        matrix = nullptr;

    last_texture_ps = -1;
    last_texture_vs = -1;
    last_texture_gs = -1;
    viewport_valid = false;
    scissor_valid = false;
#if defined(USE_DX11)
    last_texture_hs = -1;
    last_texture_ds = -1;
    last_texture_cs = -1;
#endif
}

void CBackend::set_ClipPlanes(u32 _enable, Fplane* _planes /*=NULL */, u32 count /* =0*/)
{
#if defined(USE_DX11) || defined(USE_OGL)
    // TODO: DX11: Implement in the corresponding vertex shaders
    // Use this to set up location, were shader setup code will get data
    // VERIFY(!"CBackend::set_ClipPlanes not implemented!");
    UNUSED(_enable);
    UNUSED(_planes);
    UNUSED(count);
    return;
#else
#   error No graphics API selected or enabled!
#endif
}

#ifndef DEDICATED_SREVER
void CBackend::set_ClipPlanes(u32 _enable, Fmatrix* _xform /*=NULL */, u32 fmask /* =0xff */)
{
    if (0 == HW.Caps.geometry.dwClipPlanes)
        return;
    if (!_enable)
    {
#if defined(USE_DX11) || defined(USE_OGL)
    // TODO: DX11: Implement in the corresponding vertex shaders
    // Use this to set up location, were shader setup code will get data
    // VERIFY(!"CBackend::set_ClipPlanes not implemented!");
#else
#   error No graphics API selected or enabled!
#endif
        return;
    }
    VERIFY(_xform && fmask);
    CFrustum F;
    F.CreateFromMatrix(*_xform, fmask);
    set_ClipPlanes(_enable, F.planes, F.p_count);
}

void CBackend::set_Textures(STextureList* textures_list)
{
    if (T == textures_list)
    {
        if (!textures_list)
            return;

#if defined(USE_DX11)
        const u32 currentSliceEpoch = CTexture::get_slice_epoch();
        if (texture_slice_epoch == currentSliceEpoch)
            return;
#endif

        // A reused list can only require work when a pixel resource switched its array slice.
        for (const auto& [loadId, texture] : *textures_list)
        {
            if (loadId >= CTexture::rstVertex)
                break;

            CTexture* surface = texture._get();
            if (!surface || surface->last_slice == surface->curr_slice)
                continue;

            stat.textures++;
            surface->bind(*this, loadId);
            surface->last_slice = surface->curr_slice;
        }
#if defined(USE_DX11)
        texture_slice_epoch = currentSliceEpoch;
#endif
        return;
    }

    T = textures_list;
    // If resources weren't set at all we should clear from resource #0.
    int _last_ps = -1;
    int _last_vs = -1;
#if defined(USE_DX11)
    int _last_gs = -1;
    int _last_hs = -1;
    int _last_ds = -1;
    int _last_cs = -1;
#endif
    auto it = textures_list->begin();
    const auto end = textures_list->end();

    for (; it != end; ++it)
    {
        std::pair<u32, ref_texture>& loader = *it;
        u32 load_id = loader.first;
        CTexture* load_surf = loader.second._get();
        //if (load_id < 256) {
        if (load_id < CTexture::rstVertex)
        {
            // Set up pixel shader resources
            VERIFY(load_id < CTexture::mtMaxPixelShaderTextures);
            // ordinary pixel surface
            if ((int)load_id > _last_ps)
                _last_ps = load_id;
            if (textures_ps[load_id] != load_surf || (load_surf && (load_surf->last_slice != load_surf->curr_slice)))
            {
                textures_ps[load_id] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Apply(load_id);
                    load_surf->last_slice = load_surf->curr_slice;
                }
            }
        }
        else
#if defined(USE_DX11)
        if (load_id < CTexture::rstGeometry)
#endif
        {
            // Set up pixel shader resources
            VERIFY(load_id < CTexture::rstVertex + CTexture::mtMaxVertexShaderTextures);

            // vertex only //d-map or vertex
            u32 load_id_remapped = load_id - CTexture::rstVertex;
            if ((int)load_id_remapped > _last_vs)
                _last_vs = load_id_remapped;
            if (textures_vs[load_id_remapped] != load_surf)
            {
                textures_vs[load_id_remapped] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Apply(load_id);
                }
            }
        }
#if defined(USE_DX11)
        else if (load_id < CTexture::rstHull)
        {
            // Set up pixel shader resources
            VERIFY(load_id < CTexture::rstGeometry + CTexture::mtMaxGeometryShaderTextures);

            // vertex only //d-map or vertex
            u32 load_id_remapped = load_id - CTexture::rstGeometry;
            if ((int)load_id_remapped > _last_gs)
                _last_gs = load_id_remapped;
            if (textures_gs[load_id_remapped] != load_surf)
            {
                textures_gs[load_id_remapped] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Apply(load_id);
                }
            }
        }
        else if (load_id < CTexture::rstDomain)
        {
            //  Set up pixel shader resources
            VERIFY(load_id < CTexture::rstHull + CTexture::mtMaxHullShaderTextures);

            // vertex only //d-map or vertex
            u32 load_id_remapped = load_id - CTexture::rstHull;
            if ((int)load_id_remapped > _last_hs)
                _last_hs = load_id_remapped;
            if (textures_hs[load_id_remapped] != load_surf)
            {
                textures_hs[load_id_remapped] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Apply(load_id);
                }
            }
        }
        else if (load_id < CTexture::rstCompute)
        {
            // Set up pixel shader resources
            VERIFY(load_id < CTexture::rstDomain + CTexture::mtMaxDomainShaderTextures);

            // vertex only //d-map or vertex
            u32 load_id_remapped = load_id - CTexture::rstDomain;
            if ((int)load_id_remapped > _last_ds)
                _last_ds = load_id_remapped;
            if (textures_ds[load_id_remapped] != load_surf)
            {
                textures_ds[load_id_remapped] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Apply(load_id);
                }
            }
        }
        else if (load_id < CTexture::rstInvalid)
        {
            // Set up pixel shader resources
            VERIFY(load_id < CTexture::rstCompute + CTexture::mtMaxComputeShaderTextures);

            // vertex only //d-map or vertex
            u32 load_id_remapped = load_id - CTexture::rstCompute;
            if ((int)load_id_remapped > _last_cs)
                _last_cs = load_id_remapped;
            if (textures_cs[load_id_remapped] != load_surf)
            {
                textures_cs[load_id_remapped] = load_surf;
                stat.textures++;

                if (load_surf)
                {
                    PGO(Msg("PGO:tex%d:%s", load_id, load_surf->cName.c_str()));
                    load_surf->bind(*this, load_id);
                    //load_surf->Applyload_id);
                }
            }
        }
        else
        {
            VERIFY("Invalid enum");
        }
#endif // USE_DX11
    }

    // clear remaining stages (PS)
    const int newLastPs = _last_ps;
    for (++_last_ps; _last_ps <= last_texture_ps; ++_last_ps)
    {
        if (!textures_ps[_last_ps])
            continue;

        textures_ps[_last_ps] = nullptr;
#if defined(USE_DX11)
        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        // HW.pDevice->PSSetShaderResources(_last_ps, 1, &pRes);
        SRVSManager.SetPSResource(_last_ps, pRes);
#elif defined(USE_OGL)
        CHK_GL(glActiveTexture(GL_TEXTURE0 + _last_ps));
        CHK_GL(glBindTexture(GL_TEXTURE_2D, 0));
        if (RImplementation.o.msaa)
            CHK_GL(glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0));
        CHK_GL(glBindTexture(GL_TEXTURE_3D, 0));
        CHK_GL(glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
#else
#   error No graphics API selected or enabled!
#endif
    }
    last_texture_ps = static_cast<s8>(newLastPs);

    // clear remaining stages (VS)
    const int newLastVs = _last_vs;
    for (++_last_vs; _last_vs <= last_texture_vs; ++_last_vs)
    {
        if (!textures_vs[_last_vs])
            continue;

        textures_vs[_last_vs] = nullptr;
#if defined(USE_DX11)
        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        // HW.pDevice->VSSetShaderResources(_last_vs, 1, &pRes);
        SRVSManager.SetVSResource(_last_vs, pRes);
#elif defined(USE_OGL)
        CHK_GL(glActiveTexture(GL_TEXTURE0 + CTexture::rstVertex + _last_vs));
        CHK_GL(glBindTexture(GL_TEXTURE_2D, 0));
        if (RImplementation.o.msaa)
            CHK_GL(glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0));
        CHK_GL(glBindTexture(GL_TEXTURE_3D, 0));
        CHK_GL(glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
#else
#   error No graphics API selected or enabled!
#endif
    }
    last_texture_vs = static_cast<s8>(newLastVs);

#if defined(USE_DX11)
    // clear remaining stages (VS)
    const int newLastGs = _last_gs;
    for (++_last_gs; _last_gs <= last_texture_gs; ++_last_gs)
    {
        if (!textures_gs[_last_gs])
            continue;

        textures_gs[_last_gs] = 0;

        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        // HW.pDevice->GSSetShaderResources(_last_gs, 1, &pRes);
        SRVSManager.SetGSResource(_last_gs, pRes);
    }
    last_texture_gs = static_cast<s8>(newLastGs);

    const int newLastHs = _last_hs;
    for (++_last_hs; _last_hs <= last_texture_hs; ++_last_hs)
    {
        if (!textures_hs[_last_hs])
            continue;

        textures_hs[_last_hs] = 0;

        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        SRVSManager.SetHSResource(_last_hs, pRes);
    }
    last_texture_hs = static_cast<s8>(newLastHs);

    const int newLastDs = _last_ds;
    for (++_last_ds; _last_ds <= last_texture_ds; ++_last_ds)
    {
        if (!textures_ds[_last_ds])
            continue;

        textures_ds[_last_ds] = 0;

        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        SRVSManager.SetDSResource(_last_ds, pRes);
    }
    last_texture_ds = static_cast<s8>(newLastDs);

    const int newLastCs = _last_cs;
    for (++_last_cs; _last_cs <= last_texture_cs; ++_last_cs)
    {
        if (!textures_cs[_last_cs])
            continue;

        textures_cs[_last_cs] = 0;

        // TODO: DX11: Optimise: set all resources at once
        ID3DShaderResourceView* pRes = 0;
        SRVSManager.SetCSResource(_last_cs, pRes);
    }
    last_texture_cs = static_cast<s8>(newLastCs);
#endif // USE_DX11

#if defined(USE_DX11)
    texture_slice_epoch = CTexture::get_slice_epoch();
#endif
}

#if defined(USE_DX11)
void CBackend::clear_CS_resources()
{
    if (last_texture_cs < 0)
        return;

    for (int slot = 0; slot <= last_texture_cs; ++slot)
    {
        if (!textures_cs[slot])
            continue;

        textures_cs[slot] = nullptr;
        SRVSManager.SetCSResource(slot, nullptr);
    }

    last_texture_cs = -1;
    T = nullptr;
    SRVSManager.Apply(context_id);
}
#endif
#else

void CBackend::set_ClipPlanes(u32 _enable, Fmatrix* _xform /*=NULL */, u32 fmask /* =0xff */) {}
void CBackend::set_Textures(STextureList* textures_list) {}

#endif // DEDICATED SERVER

void CBackend::SetupStates()
{
    set_CullMode(CULL_CCW);
#if defined(USE_DX11)
    SSManager.SetMaxAnisotropy(ps_r__tf_Anisotropic);
    SSManager.SetMipLODBias(ps_r__tf_Mipbias);
#elif defined(USE_OGL)
    // TODO: OGL: Implement SetupStates().
#else
#   error No graphics API selected or enabled!
#endif
}


// Device dependance
void CBackend::OnDeviceCreate()
{
    ZoneScoped;

#if defined(USE_DX11)
    HW.get_context(context_id)->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), reinterpret_cast<void**>(&pAnnotation));
#endif

    // Debug Draw
    InitializeDebugDraw();

    // invalidate caching
    Invalidate();
}

void CBackend::OnDeviceDestroy()
{
    // Debug Draw
    DestroyDebugDraw();

#if defined(USE_DX11)
    //  Destroy state managers
    StateManager.Reset();
#endif

#if defined(USE_DX11)
    _RELEASE(pAnnotation);
#endif
}

void CBackend::apply_lmaterial()
{
    R_constant* C = get_c(c_sbase)._get(); // get sampler
    if (!C)
        return;

    VERIFY(RC_dest_sampler == C->destination);
#if defined(USE_DX11)
    VERIFY(RC_dx11texture == C->type);
#elif defined(USE_OGL)
    VERIFY(RC_sampler == C->type);
#else
#   error No graphics API selected or enabled!
#endif

    CTexture* T = get_ActiveTexture(u32(C->samp.index));
    VERIFY(T);
    float mtl = T->m_material;
#ifdef DEBUG
    if (ps_r2_ls_flags.test(R2FLAG_GLOBALMATERIAL))
        mtl = ps_r2_gmaterial;
#endif
    hemi.set_material(o_hemi, o_sun, 0, (mtl + .5f) / 4.f);
    hemi.set_pos_faces(o_hemi_cube[CROS_impl::CUBE_FACE_POS_X],
                                o_hemi_cube[CROS_impl::CUBE_FACE_POS_Y],
                                o_hemi_cube[CROS_impl::CUBE_FACE_POS_Z]);
    hemi.set_neg_faces(o_hemi_cube[CROS_impl::CUBE_FACE_NEG_X],
                                o_hemi_cube[CROS_impl::CUBE_FACE_NEG_Y],
                                o_hemi_cube[CROS_impl::CUBE_FACE_NEG_Z]);
}
} // namespace xray::render::RENDER_NAMESPACE
