#pragma once

#include "StateManager/dx11ShaderResourceStateCache.h"

namespace xray::render::RENDER_NAMESPACE
{
IC void CBackend::set_xform(u32 ID, const Fmatrix& M)
{
    stat.xforms++;
    //  TODO: DX11: Implement CBackend::set_xform
    // VERIFY(!"Implement CBackend::set_xform");
}

IC void CBackend::set_RT(ID3DRenderTargetView* RT, u32 ID)
{
    if (RT != pRT[ID])
    {
        PGO(Msg("PGO:setRT"));
        stat.target_rt++;
        pRT[ID] = RT;
        //  Mark RT array dirty
        // HW.pDevice->OMSetRenderTargets(sizeof(pRT)/sizeof(pRT[0]), pRT, 0);
        // HW.pDevice->OMSetRenderTargets(sizeof(pRT)/sizeof(pRT[0]), pRT, pZB);
        //  Reset all RT's here to allow RT to be bounded as input
        if (!m_bChangedRTorZB)
            HW.get_context(context_id)->OMSetRenderTargets(0, 0, 0);

        m_bChangedRTorZB = true;
    }
}

IC void CBackend::set_ZB(ID3DDepthStencilView* ZB)
{
    if (ZB != pZB)
    {
        PGO(Msg("PGO:setZB"));
        stat.target_zb++;
        pZB = ZB;
        // HW.pDevice->OMSetRenderTargets(0, 0, pZB);
        // HW.pDevice->OMSetRenderTargets(sizeof(pRT)/sizeof(pRT[0]), pRT, pZB);
        //  Reset all RT's here to allow RT to be bounded as input
        if (!m_bChangedRTorZB)
            HW.get_context(context_id)->OMSetRenderTargets(0, 0, 0);
        m_bChangedRTorZB = true;
    }
}

IC void CBackend::get_ZB_dimensions(ID3DDepthStencilView* ZB, bool msaa, u32& width, u32& height)
{
    VERIFY(ZB);
    if (depth_dimensions_zb != ZB)
    {
        D3D_DEPTH_STENCIL_VIEW_DESC viewDescription;
        ZB->GetDesc(&viewDescription);
        if (!msaa)
        {
            VERIFY(viewDescription.ViewDimension == D3D_DSV_DIMENSION_TEXTURE2D ||
                viewDescription.ViewDimension == D3D_DSV_DIMENSION_TEXTURE2DARRAY);
        }

        ID3DResource* resource;
        ZB->GetResource(&resource);
        auto* texture = static_cast<ID3DTexture2D*>(resource);
        D3D_TEXTURE2D_DESC textureDescription;
        texture->GetDesc(&textureDescription);
        _RELEASE(resource);

        depth_dimensions_zb = ZB;
        depth_dimensions_width = textureDescription.Width;
        depth_dimensions_height = textureDescription.Height;
    }

    width = depth_dimensions_width;
    height = depth_dimensions_height;
}

IC void CBackend::ClearRT(ID3DRenderTargetView* rt, const Fcolor& color)
{
    HW.get_context(context_id)->ClearRenderTargetView(rt, reinterpret_cast<const FLOAT*>(&color));
}

IC void CBackend::ClearZB(ID3DDepthStencilView* zb, float depth)
{
    HW.get_context(context_id)->ClearDepthStencilView(zb, D3D_CLEAR_DEPTH, depth, 0);
}

IC void CBackend::ClearZB(ID3DDepthStencilView* zb, float depth, u8 stencil)
{
    HW.get_context(context_id)->ClearDepthStencilView(zb, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, depth, stencil);
}

IC bool CBackend::ClearRTRect(ID3DRenderTargetView* rt, const Fcolor& color, size_t numRects, const Irect* rects)
{
#ifdef USE_DX11
    if (HW.pContext1)
    {
        HW.pContext1->ClearView(rt, reinterpret_cast<const FLOAT*>(&color),
            reinterpret_cast<const D3D_RECT*>(rects), numRects);
        return true;
    }
#else
    UNUSED(numRects);
    UNUSED(rects);
#endif

    return false;
}

IC bool CBackend::ClearZBRect(ID3DDepthStencilView* zb, float depth, size_t numRects, const Irect* rects)
{
#ifdef USE_DX11
    if (HW.pContext1)
    {
        Fcolor color = { depth, depth, depth, depth };
        HW.pContext1->ClearView(zb, reinterpret_cast<FLOAT*>(&color),
            reinterpret_cast<const D3D_RECT*>(rects), numRects);
        return true;
    }
#else
    UNUSED(numRects);
    UNUSED(rects);
#endif

    return false;
}

ICF void CBackend::set_Format(SDeclaration* _decl)
{
    if (decl != _decl)
    {
        PGO(Msg("PGO:v_format:%x", _decl));
        stat.decl++;
        decl = _decl;
    }
}

ICF void CBackend::set_PS(ID3DPixelShader* _ps, LPCSTR _n)
{
    if (ps != _ps)
    {
        PGO(Msg("PGO:Pshader:%x", _ps));
        stat.ps++;
        ps = _ps;
#ifdef USE_DX11
        HW.get_context(context_id)->PSSetShader(ps, 0, 0);
#else
        HW.pContext->PSSetShader(ps);
#endif

#ifdef DEBUG
        ps_name = _n;
#endif
    }
}

ICF void CBackend::set_GS(ID3DGeometryShader* _gs, LPCSTR _n)
{
    if (gs != _gs)
    {
        PGO(Msg("PGO:Gshader:%x", _ps));
        stat.gs++;
        gs = _gs;
#ifdef USE_DX11
        HW.get_context(context_id)->GSSetShader(gs, 0, 0);
#else
        HW.pContext->GSSetShader(gs);
#endif

#ifdef DEBUG
        gs_name = _n;
#endif
    }
}

#ifdef USE_DX11
ICF void CBackend::set_HS(ID3D11HullShader* _hs, LPCSTR _n)
{
    if (hs != _hs)
    {
        PGO(Msg("PGO:Hshader:%x", _ps));
        stat.hs++;
        hs = _hs;
        HW.get_context(context_id)->HSSetShader(hs, 0, 0);

#ifdef DEBUG
        hs_name = _n;
#endif
    }
}

ICF void CBackend::set_DS(ID3D11DomainShader* _ds, LPCSTR _n)
{
    if (ds != _ds)
    {
        PGO(Msg("PGO:Dshader:%x", _ps));
        stat.ds++;
        ds = _ds;
        HW.get_context(context_id)->DSSetShader(ds, 0, 0);

#ifdef DEBUG
        ds_name = _n;
#endif
    }
}

ICF void CBackend::set_CS(ID3D11ComputeShader* _cs, LPCSTR _n)
{
    if (cs != _cs)
    {
        PGO(Msg("PGO:Cshader:%x", _ps));
        stat.cs++;
        cs = _cs;
        HW.get_context(context_id)->CSSetShader(cs, 0, 0);

#ifdef DEBUG
        cs_name = _n;
#endif
    }
}

ICF bool CBackend::is_TessEnabled() { return HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0 && (ds != 0 || hs != 0); }
#endif

ICF void CBackend::set_VS(ID3DVertexShader* _vs, LPCSTR _n)
{
    if (vs != _vs)
    {
        PGO(Msg("PGO:Vshader:%x", _vs));
        stat.vs++;
        vs = _vs;
#ifdef USE_DX11
        HW.get_context(context_id)->VSSetShader(vs, 0, 0);
#else
        HW.pContext->VSSetShader(vs);
#endif

        vs_name = _n; // release too - the input-layout failure report needs the culprit
    }
}

ICF void CBackend::set_Vertices(ID3DVertexBuffer* _vb, u32 _vb_stride)
{
    if ((vb != _vb) || (vb_stride != _vb_stride))
    {
        PGO(Msg("PGO:VB:%x,%d", _vb, _vb_stride));
        stat.vb++;
        vb = _vb;
        vb_stride = _vb_stride;
        // CHK_DX           (HW.pDevice->SetStreamSource(0,vb,0,vb_stride));
        // UINT StreamNumber,
        // ID3DVertexBuffer * pStreamData,
        // UINT OffsetInBytes,
        // UINT Stride

        // UINT StartSlot,
        // UINT NumBuffers,
        // ID3DxxBuffer *const *ppVertexBuffers,
        // const UINT *pStrides,
        // const UINT *pOffsets
        u32 iOffset = 0;
        HW.get_context(context_id)->IASetVertexBuffers(0, 1, &vb, &_vb_stride, &iOffset);
    }
}

ICF void CBackend::set_Indices(ID3DIndexBuffer* _ib)
{
    if (ib != _ib)
    {
        PGO(Msg("PGO:IB:%x", _ib));
        stat.ib++;
        ib = _ib;
        HW.get_context(context_id)->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    }
}

IC D3D_PRIMITIVE_TOPOLOGY TranslateTopology(D3DPRIMITIVETYPE T)
{
    static D3D_PRIMITIVE_TOPOLOGY translateTable[] = {
        D3D_PRIMITIVE_TOPOLOGY_UNDEFINED, // None
        D3D_PRIMITIVE_TOPOLOGY_POINTLIST, // D3DPT_POINTLIST = 1,
        D3D_PRIMITIVE_TOPOLOGY_LINELIST, // D3DPT_LINELIST = 2,
        D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, // D3DPT_LINESTRIP = 3,
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, // D3DPT_TRIANGLELIST = 4,
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, // D3DPT_TRIANGLESTRIP = 5,
        D3D_PRIMITIVE_TOPOLOGY_UNDEFINED, // D3DPT_TRIANGLEFAN = 6,
    };

    VERIFY(T < sizeof(translateTable) / sizeof(translateTable[0]));
    VERIFY(T >= 0);

    D3D_PRIMITIVE_TOPOLOGY result = translateTable[T];

    VERIFY(result != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);

    return result;
}

IC u32 GetIndexCount(D3DPRIMITIVETYPE T, u32 iPrimitiveCount)
{
    switch (T)
    {
    case D3DPT_POINTLIST: return iPrimitiveCount;
    case D3DPT_LINELIST: return iPrimitiveCount * 2;
    case D3DPT_LINESTRIP: return iPrimitiveCount + 1;
    case D3DPT_TRIANGLELIST: return iPrimitiveCount * 3;
    case D3DPT_TRIANGLESTRIP: return iPrimitiveCount + 2;
    default: NODEFAULT;
#ifdef DEBUG
        return 0;
#endif // #ifdef DEBUG
    }
}

IC void CBackend::ApplyPrimitieTopology(D3D_PRIMITIVE_TOPOLOGY Topology)
{
    if (m_PrimitiveTopology != Topology)
    {
        m_PrimitiveTopology = Topology;
        HW.get_context(context_id)->IASetPrimitiveTopology(m_PrimitiveTopology);
    }
}

#ifdef USE_DX11
IC void CBackend::Compute(u32 ThreadGroupCountX, u32 ThreadGroupCountY, u32 ThreadGroupCountZ)
{
    stat.compute.calls++;
    stat.compute.groups_x = ThreadGroupCountX;
    stat.compute.groups_y = ThreadGroupCountY;
    stat.compute.groups_z = ThreadGroupCountZ;

    SRVSManager.Apply(context_id);
    StateManager.Apply();
    //  State manager may alter constants
    constants.flush();
    HW.get_context(context_id)->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}
#endif

IC void CBackend::Render(D3DPRIMITIVETYPE T, u32 baseV, u32 startV, u32 countV, u32 startI, u32 PC)
{
    // VERIFY(vs);
    // HW.pDevice->VSSetShader(vs);
    // HW.pDevice->GSSetShader(0);

    D3D_PRIMITIVE_TOPOLOGY Topology = TranslateTopology(T);
    u32 iIndexCount = GetIndexCount(T, PC);

//!!! HACK !!!
#ifdef USE_DX11
    if (hs != 0 || ds != 0)
    {
        R_ASSERT(Topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Topology = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    }
#endif

    stat.render.calls++;
    stat.render.verts += countV;
    stat.render.polys += PC;

    ApplyPrimitieTopology(Topology);

    // CHK_DX(HW.pDevice->DrawIndexedPrimitive(T,baseV, startV, countV,startI,PC));
    // D3DPRIMITIVETYPE Type,
    // INT BaseVertexIndex,
    // UINT MinIndex,
    // UINT NumVertices,
    // UINT StartIndex,
    // UINT PriResmitiveCount

    // UINT IndexCount,
    // UINT StartIndexLocation,
    // INT BaseVertexLocation
    SRVSManager.Apply(context_id);
    ApplyRTandZB();
    ApplyVertexLayout();
    StateManager.Apply();
    //  State manager may alter constants
    constants.flush();
    //  Msg("DrawIndexed: Start");
    //  Msg("iIndexCount=%d, startI=%d, baseV=%d", iIndexCount, startI, baseV);
    HW.get_context(context_id)->DrawIndexed(iIndexCount, startI, baseV);
    //  Msg("DrawIndexed: End\n");

    PGO(Msg("PGO:DIP:%dv/%df", countV, PC));
}

IC void CBackend::RenderInstanced(
    D3DPRIMITIVETYPE T, u32 baseV, u32 /*startV*/, u32 countV, u32 startI, u32 PC,
    u32 instanceCount)
{
    VERIFY(instanceCount);

    D3D_PRIMITIVE_TOPOLOGY topology = TranslateTopology(T);
    const u32 indexCount = GetIndexCount(T, PC);

    if (hs || ds)
    {
        R_ASSERT(topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        topology = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    }

    stat.render.calls++;
    stat.render.verts += countV * instanceCount;
    stat.render.polys += PC * instanceCount;

    ApplyPrimitieTopology(topology);
    SRVSManager.Apply(context_id);
    ApplyRTandZB();
    ApplyVertexLayout();
    StateManager.Apply();
    constants.flush();
    HW.get_context(context_id)->DrawIndexedInstanced(
        indexCount, instanceCount, startI, baseV, 0);

    PGO(Msg("PGO:DIP:%dv/%df", countV * instanceCount, PC * instanceCount));
}

IC void CBackend::Render(D3DPRIMITIVETYPE T, u32 startV, u32 PC)
{
    //  TODO: DX11: Remove triangle fan usage from the engine
    if (T == D3DPT_TRIANGLEFAN)
        return;

    // VERIFY(vs);
    // HW.pDevice->VSSetShader(vs);

    D3D_PRIMITIVE_TOPOLOGY Topology = TranslateTopology(T);
    u32 iVertexCount = GetIndexCount(T, PC);

    stat.render.calls++;
    stat.render.verts += 3 * PC;
    stat.render.polys += PC;

    ApplyPrimitieTopology(Topology);
    SRVSManager.Apply(context_id);
    ApplyRTandZB();
    ApplyVertexLayout();
    StateManager.Apply();
    //  State manager may alter constants
    constants.flush();
    //  Msg("Draw: Start");
    //  Msg("iVertexCount=%d, startV=%d", iVertexCount, startV);
    // CHK_DX               (HW.pDevice->DrawPrimitive(T, startV, PC));
    HW.get_context(context_id)->Draw(iVertexCount, startV);
    //  Msg("Draw: End\n");
    PGO(Msg("PGO:DIP:%dv/%df", 3 * PC, PC));
}

IC void CBackend::set_Geometry(SGeometry* _geom)
{
    set_Format(&*_geom->dcl);

    set_Vertices(_geom->vb, _geom->vb_stride);
    set_Indices(_geom->ib);
}

IC void CBackend::set_Scissor(const Irect* R)
{
    if (R)
    {
        if (scissor_valid && scissor_enabled &&
            scissor_cache.left == R->left && scissor_cache.top == R->top &&
            scissor_cache.right == R->right && scissor_cache.bottom == R->bottom)
        {
            return;
        }

        // CHK_DX       (HW.pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE));
        StateManager.EnableScissoring();
        const RECT* clip = reinterpret_cast<const RECT*>(R);
        HW.get_context(context_id)->RSSetScissorRects(1, clip);
        scissor_cache = *R;
        scissor_enabled = true;
        scissor_valid = true;
    }
    else
    {
        if (scissor_valid && !scissor_enabled)
            return;

        // CHK_DX       (HW.pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
        StateManager.EnableScissoring(FALSE);
        HW.get_context(context_id)->RSSetScissorRects(0, 0);
        scissor_enabled = false;
        scissor_valid = true;
    }
}

IC void CBackend::SetViewport(const D3D_VIEWPORT& viewport) const
{
    if (viewport_valid &&
        viewport_cache.TopLeftX == viewport.TopLeftX && viewport_cache.TopLeftY == viewport.TopLeftY &&
        viewport_cache.Width == viewport.Width && viewport_cache.Height == viewport.Height &&
        viewport_cache.MinDepth == viewport.MinDepth && viewport_cache.MaxDepth == viewport.MaxDepth)
    {
        return;
    }

    HW.get_context(context_id)->RSSetViewports(1, &viewport);
    viewport_cache = viewport;
    viewport_valid = true;
}

IC void CBackend::set_Stencil(
    u32 _enable, u32 _func, u32 _ref, u32 _mask, u32 _writemask, u32 _fail, u32 _pass, u32 _zfail)
{
    StateManager.SetStencil(_enable, _func, _ref, _mask, _writemask, _fail, _pass, _zfail);
    // Simple filter
    // if (stencil_enable       != _enable)     { stencil_enable=_enable;       CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILENABLE,     _enable             )); }
    // if (!stencil_enable)                 return;
    // if (stencil_func     != _func)       { stencil_func=_func;           CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILFUNC,
    // _func                )); }
    // if (stencil_ref          != _ref)        { stencil_ref=_ref;             CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILREF,
    // _ref
    // )); }
    // if (stencil_mask     != _mask)       { stencil_mask=_mask;           CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILMASK,
    // _mask                )); }
    // if (stencil_writemask    != _writemask)  { stencil_writemask=_writemask; CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILWRITEMASK,  _writemask          )); }
    // if (stencil_fail     != _fail)       { stencil_fail=_fail;           CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILFAIL,
    // _fail                )); }
    // if (stencil_pass     != _pass)       { stencil_pass=_pass;           CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILPASS,
    // _pass                )); }
    // if (stencil_zfail        != _zfail)      { stencil_zfail=_zfail;         CHK_DX(HW.pDevice->SetRenderState   (
    // D3DRS_STENCILZFAIL,
    // _zfail               )); }
}

IC void CBackend::set_Z(u32 _enable)
{
    StateManager.SetDepthEnable(_enable);
    // if (z_enable != _enable)
    //{
    //  z_enable=_enable;
    //  CHK_DX(HW.pDevice->SetRenderState   ( D3DRS_ZENABLE, _enable ));
    //}
}

IC void CBackend::set_ZFunc(u32 _func)
{
    StateManager.SetDepthFunc(_func);
    // if (z_func!=_func)
    //{
    //  z_func = _func;
    //  CHK_DX(HW.pDevice->SetRenderState( D3DRS_ZFUNC, _func));
    //}
}

IC void CBackend::set_AlphaRef(u32 _value)
{
    //  TODO: DX11: Implement rasterizer state update to support alpha ref
    VERIFY(!"Not implemented.");
    // if (alpha_ref != _value)
    //{
    //  alpha_ref = _value;
    //  CHK_DX(HW.pDevice->SetRenderState(D3DRS_ALPHAREF,_value));
    //}
}

IC void CBackend::set_ColorWriteEnable(u32 _mask)
{
    StateManager.SetColorWriteEnable(_mask);
    // if (colorwrite_mask      != _mask)       {
    //  colorwrite_mask=_mask;
    //  CHK_DX(HW.pDevice->SetRenderState   ( D3DRS_COLORWRITEENABLE,   _mask   ));
    //  CHK_DX(HW.pDevice->SetRenderState   ( D3DRS_COLORWRITEENABLE1,  _mask   ));
    //  CHK_DX(HW.pDevice->SetRenderState   ( D3DRS_COLORWRITEENABLE2,  _mask   ));
    //  CHK_DX(HW.pDevice->SetRenderState   ( D3DRS_COLORWRITEENABLE3,  _mask   ));
    //}
}
ICF void CBackend::set_CullMode(u32 _mode)
{
    StateManager.SetCullMode(_mode);
    cull_mode = _mode;
}

ICF void CBackend::set_FillMode(u32 _mode)
{
    StateManager.SetFillMode(_mode);
}

ICF void CBackend::SetTextureFactor(u32 /*factor*/) const
{
    // Not supported
}

ICF void CBackend::SetAmbient(u32 /*factor*/) const
{
    // Not supported
}

IC void CBackend::ApplyVertexLayout()
{
    VERIFY(vs);
    VERIFY(decl);
    VERIFY(m_pInputSignature);

    if (m_pInputLayout && m_pInputLayoutDecl == decl && m_pInputLayoutSignature == m_pInputSignature)
        return;

    // This backend's own cache: inserting into a tree shared between contexts races the
    // other worker backends walking it (see vs_to_layout).
    auto& layouts = decl->vs_to_layout[context_id];
    auto it = layouts.find(m_pInputSignature);

    if (it == layouts.end())
    {
        // A failed layout creation used to be INVISIBLE: CHK_DX expands to a bare call in
        // release, the pointer stayed uninitialized and was cached UNCONDITIONALLY - so a
        // null settled in forever for this declaration+signature pair, and every following
        // frame bound an empty layout and drew nothing. DX9 read missing shader inputs as
        // zeros; DX11 demands an exact match and refuses, which is why the same level data
        // rendered fine on the original 32-bit engine (the Cordon mast case).
        ID3DInputLayout* pLayout = nullptr;

        const HRESULT hr = HW.pDevice->CreateInputLayout(&decl->dx11_dcl_code[0],
            decl->dx11_dcl_code.size() - 1, m_pInputSignature->GetBufferPointer(),
            m_pInputSignature->GetBufferSize(), &pLayout);

        // Repairing the missing uv set - ONLY for the _lmh family: the material declares a
        // lightmap so the blender picks the _lmh variant, but the geometry arrives in the
        // vertex-lit format (COLOR0 + a single uv set), and the -hq close-range variant
        // reads TEXCOORD1 which that format does not have. The shader choice matches the
        // original sources verbatim - the LEVEL DATA is inconsistent, so the binding is
        // repaired, not the choice. These calls drew nothing at all, so it cannot get
        // worse; the lightmap ends up sampled by the base uv, meaningless for vertex-lit
        // geometry, but the shape and placement come back.
        if ((FAILED(hr) || !pLayout) && vs_name && strstr(vs_name, "_lmh"))
        {
            xr_vector<D3D_INPUT_ELEMENT_DESC> patched;
            patched.reserve(decl->dx11_dcl_code.size() + 1);

            bool has_tc1 = false;
            const D3D_INPUT_ELEMENT_DESC* tc0 = nullptr;
            for (size_t i = 0; i + 1 < decl->dx11_dcl_code.size(); ++i)
            {
                const auto& e = decl->dx11_dcl_code[i];
                patched.push_back(e);
                if (!e.SemanticName || xr_strcmp(e.SemanticName, "TEXCOORD") != 0)
                    continue;
                if (e.SemanticIndex == 0)
                    tc0 = &decl->dx11_dcl_code[i];
                else if (e.SemanticIndex == 1)
                    has_tc1 = true;
            }

            if (!has_tc1 && tc0)
            {
                D3D_INPUT_ELEMENT_DESC extra = *tc0;
                extra.SemanticIndex = 1;
                patched.push_back(extra);

                ID3DInputLayout* patched_layout = nullptr;
                if (SUCCEEDED(HW.pDevice->CreateInputLayout(&patched[0], (UINT)patched.size(),
                        m_pInputSignature->GetBufferPointer(), m_pInputSignature->GetBufferSize(),
                        &patched_layout)) &&
                    patched_layout)
                {
                    static u32 fixed_count = 0;
                    if (++fixed_count <= 3)
                        Msg("* [layout] shader '%s': TEXCOORD1 backfilled from the base uv, "
                            "vertex-lit geometry draws again (case %u)",
                            vs_name, fixed_count);
                    pLayout = patched_layout;
                }
            }
        }

        if (FAILED(hr) && !pLayout)
        {
            // The refusal is cached too - retrying a hopeless call every frame costs more
            // than remembering it. But now it is NAMED, with the semantics the declaration
            // provides: the list shows what is MISSING, the shader name shows WHO demands it.
            static u32 fail_count = 0;
            ++fail_count;
            if (fail_count <= 10)
            {
                string512 semantics{};
                for (size_t i = 0; i + 1 < decl->dx11_dcl_code.size(); ++i)
                {
                    const auto& e = decl->dx11_dcl_code[i];
                    if (!e.SemanticName)
                        continue;
                    string64 one;
                    xr_sprintf(one, "%s%u ", e.SemanticName, e.SemanticIndex);
                    xr_strcat(semantics, one);
                }
                Msg("! [layout] input layout NOT created (0x%08x): draws of shader '%s' "
                    "produce nothing. Declaration provides: %s(case %u)",
                    (u32)hr, vs_name ? vs_name : "(unnamed)", semantics, fail_count);
            }
            pLayout = nullptr;
        }

        it = layouts.insert(std::pair<ID3DBlob*, ID3DInputLayout*>(m_pInputSignature, pLayout)).first;
    }

    m_pInputLayoutDecl = decl;
    m_pInputLayoutSignature = m_pInputSignature;
    if (m_pInputLayout != it->second)
    {
        m_pInputLayout = it->second;
        HW.get_context(context_id)->IASetInputLayout(m_pInputLayout);
    }
}

ICF void CBackend::set_VS(ref_vs& _vs)
{
    m_pInputSignature = _vs->signature->signature;
    set_VS(_vs->sh, _vs->cName.c_str());
}

ICF void CBackend::set_VS(SVS* _vs)
{
    m_pInputSignature = _vs->signature->signature;
    set_VS(_vs->sh, _vs->cName.c_str());
}

IC bool CBackend::UpdateConstantBuffers(ref_cbuffer current[MaxCBuffers],
    dx11ConstantBuffer* const desired[MaxCBuffers], u32& uiMin, u32& uiMax)
{
    bool changed = false;
    for (u32 i = 0; i < MaxCBuffers; ++i)
    {
        if (desired[i])
            constants.queue_for_flush(*desired[i]);

        if (current[i]._get() == desired[i])
            continue;

        if (!changed)
            uiMin = i;
        uiMax = i;
        current[i]._set(desired[i]);
        changed = true;
    }

    return changed;
}

IC void CBackend::set_Constants(R_constant_table* C)
{
    // caching
    if (ctable == C)
        return;
    ctable = C;
    lmaterial_base_constant = C ? C->get("s_base")._get() : nullptr;
    xforms.unmap();
    hemi.unmap();
    tree.unmap();
#ifdef USE_DX11
    LOD.unmap();
#endif
    StateManager.UnmapConstants();
    if (!C)
        return;

    PGO(Msg("PGO:c-table"));

    //  Setup constant tables
    {
        static_assert(MaxCBuffers == R_constant_table::ConstantBufferCount);
        const R_constant_table::cb_binding_layout& bindings = C->getConstantBufferBindings(context_id);

        ID3DBuffer* tempBuffer[MaxCBuffers];

        u32 uiMin;
        u32 uiMax;

        if (UpdateConstantBuffers(m_aPixelConstants, bindings.pixel, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aPixelConstants[i])
                    tempBuffer[i] = m_aPixelConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }

            HW.get_context(context_id)->PSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }

        if (UpdateConstantBuffers(m_aVertexConstants, bindings.vertex, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aVertexConstants[i])
                    tempBuffer[i] = m_aVertexConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }
            HW.get_context(context_id)->VSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }

        if (UpdateConstantBuffers(m_aGeometryConstants, bindings.geometry, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aGeometryConstants[i])
                    tempBuffer[i] = m_aGeometryConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }
            HW.get_context(context_id)->GSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }

        if (UpdateConstantBuffers(m_aHullConstants, bindings.hull, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aHullConstants[i])
                    tempBuffer[i] = m_aHullConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }
            HW.get_context(context_id)->HSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }

        if (UpdateConstantBuffers(m_aDomainConstants, bindings.domain, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aDomainConstants[i])
                    tempBuffer[i] = m_aDomainConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }
            HW.get_context(context_id)->DSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }

        if (UpdateConstantBuffers(m_aComputeConstants, bindings.compute, uiMin, uiMax))
        {
            ++uiMax;

            for (u32 i = uiMin; i < uiMax; ++i)
            {
                if (m_aComputeConstants[i])
                    tempBuffer[i] = m_aComputeConstants[i]->GetBuffer();
                else
                    tempBuffer[i] = 0;
            }
            HW.get_context(context_id)->CSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
        }
        /*
        for (int i=0; i<MaxCBuffers; ++i)
        {
            if (m_aPixelConstants[i])
                tempBuffer[i] = m_aPixelConstants[i]->GetBuffer();
            else
                tempBuffer[i] = 0;
        }
        HW.pDevice->PSSetConstantBuffers(0, MaxCBuffers, tempBuffer);

        for (int i=0; i<MaxCBuffers; ++i)
        {
            if (m_aVertexConstants[i])
                tempBuffer[i] = m_aVertexConstants[i]->GetBuffer();
            else
                tempBuffer[i] = 0;
        }
        HW.pDevice->VSSetConstantBuffers(0, MaxCBuffers, tempBuffer);

        for (int i=0; i<MaxCBuffers; ++i)
        {
            if (m_aGeometryConstants[i])
                tempBuffer[i] = m_aGeometryConstants[i]->GetBuffer();
            else
                tempBuffer[i] = 0;
        }
        HW.pDevice->GSSetConstantBuffers(0, MaxCBuffers, tempBuffer);
        */
    }

    // process constant-loaders
    R_constant_table::c_table::iterator it = C->table.begin();
    R_constant_table::c_table::iterator end = C->table.end();
    for (; it != end; ++it)
    {
        R_constant* Cs = &**it;
        VERIFY(Cs);
        if (Cs && Cs->handler)
            Cs->handler->setup(*this, Cs);
    }
}

ICF void CBackend::ApplyRTandZB()
{
    if (m_bChangedRTorZB)
    {
        m_bChangedRTorZB = false;
        HW.get_context(context_id)->OMSetRenderTargets(sizeof(pRT) / sizeof(pRT[0]), pRT, pZB);
    }
}

IC void CBackend::get_ConstantDirect(const shared_str& n, size_t DataSize, void** pVData, void** pGData, void** pPData)
{
    ref_constant C = get_c(n);

    if (C)
        constants.access_direct(&*C, DataSize, pVData, pGData, pPData);
    else
    {
        if (pVData)
            *pVData = 0;
        if (pGData)
            *pGData = 0;
        if (pPData)
            *pPData = 0;
    }
}

IC void CBackend::gpu_mark_begin(const wchar_t* name)
{
    pAnnotation->BeginEvent(name);
}

IC void CBackend::gpu_mark_end()
{
    pAnnotation->EndEvent();
}

IC void CBackend::set_pass_targets(const ref_rt& _1, const ref_rt& _2, const ref_rt& _3, const ref_rt& zb)
{
    if (_1)
    {
        curr_rt_width = _1->dwWidth;
        curr_rt_height = _1->dwHeight;
    }
    else
    {
        VERIFY(zb);
        curr_rt_width = zb->dwWidth;
        curr_rt_height = zb->dwHeight;
    }

    set_RT(_1 ? _1->pRT : nullptr, 0);
    set_RT(_2 ? _2->pRT : nullptr, 1);
    set_RT(_3 ? _3->pRT : nullptr, 2);
    set_ZB(zb ? zb->pZRT[context_id] : nullptr);

    const D3D_VIEWPORT viewport = { 0, 0, curr_rt_width, curr_rt_height, 0.f, 1.f };
    SetViewport(viewport);
}
} // namespace xray::render::RENDER_NAMESPACE
