#include "stdafx.h"
#pragma hdrstop

#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/tss.h"
#include "Layers/xrRender/blender.h"
#include "Layers/xrRender/blender_recorder.h"
#include "Layers/xrRender/BufferUtils.h"
#include "Layers/xrRenderDX11/dx11ConstantBuffer.h"
#include "Layers/xrRender/ShaderResourceTraits.h"

namespace xray::render::RENDER_NAMESPACE
{
//--------------------------------------------------------------------------------------------------------------
SPass* CResourceManager::_CreatePass(const SPass& proto)
{
    for (SPass* pass : v_passes)
        if (pass->equal(proto))
            return pass;

    SPass* P = v_passes.emplace_back(xr_new<SPass>());
    P->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    P->state = proto.state;
    P->ps = proto.ps;
    P->vs = proto.vs;
    P->gs = proto.gs;
#ifdef USE_DX11
    P->hs = proto.hs;
    P->ds = proto.ds;
    P->cs = proto.cs;
#endif
    P->constants = proto.constants;
    P->T = proto.T;
#ifdef _EDITOR
    P->M = proto.M;
#endif
    P->C = proto.C;

    return P;
}

//--------------------------------------------------------------------------------------------------------------
SVS* CResourceManager::_CreateVS(cpcstr shader, u32 flags /*= 0*/)
{
    string_path name;
    xr_strcpy(name, shader);
    switch (RImplementation.m_skinning)
    {
    case 0:
        xr_strcat(name, "_0");
        break;
    case 1:
        xr_strcat(name, "_1");
        break;
    case 2:
        xr_strcat(name, "_2");
        break;
    case 3:
        xr_strcat(name, "_3");
        break;
    case 4:
        xr_strcat(name, "_4");
        break;
    }

    return CreateShader<SVS>(name, shader, flags);
}

void CResourceManager::_DeleteVS(const SVS* vs)
{
    // "null" and a mesh stage that failed to compile have no input signature.
    if (DestroyShader(vs) && vs->signature)
    {
        for (const auto& iDecl : v_declarations)
        {
            // The layout cache is split per context, purge the dying signature from each one.
            for (u32 ctx = 0; ctx < R__NUM_CONTEXTS; ++ctx)
            {
                auto& layouts = iDecl->vs_to_layout[ctx];

                const auto iLayout = layouts.find(vs->signature->signature);
                if (iLayout != layouts.end())
                {
                    //	Release vertex layout
                    _RELEASE(iLayout->second);
                    layouts.erase(iLayout);
                }
            }
        }
    }
}

//--------------------------------------------------------------------------------------------------------------
SPS* CResourceManager::_CreatePS(LPCSTR _name, u32 flags /*= 0*/)
{
    string_path name;
    xr_strcpy(name, _name);
    switch (RImplementation.m_MSAASample)
    {
    case 0:
        xr_strcat(name, "_0");
        break;
    case 1:
        xr_strcat(name, "_1");
        break;
    case 2:
        xr_strcat(name, "_2");
        break;
    case 3:
        xr_strcat(name, "_3");
        break;
    case 4:
        xr_strcat(name, "_4");
        break;
    case 5:
        xr_strcat(name, "_5");
        break;
    case 6:
        xr_strcat(name, "_6");
        break;
    case 7:
        xr_strcat(name, "_7");
        break;
    }

    return CreateShader<SPS>(name, _name, flags);
}

void CResourceManager::_DeletePS(const SPS* ps) { DestroyShader(ps); }

//--------------------------------------------------------------------------------------------------------------
SGS* CResourceManager::_CreateGS(LPCSTR Name, u32 flags /*= 0*/) { return CreateShader<SGS>(Name, nullptr, flags); }
void CResourceManager::_DeleteGS(const SGS* GS) { DestroyShader(GS); }

#ifdef USE_DX11
SHS* CResourceManager::_CreateHS(LPCSTR Name, u32 flags /*= 0*/) { return CreateShader<SHS>(Name, nullptr, flags); }
void CResourceManager::_DeleteHS(const SHS* HS) { DestroyShader(HS); }

SDS* CResourceManager::_CreateDS(LPCSTR Name, u32 flags /*= 0*/) { return CreateShader<SDS>(Name, nullptr, flags); }
void CResourceManager::_DeleteDS(const SDS* DS) { DestroyShader(DS); }

SCS* CResourceManager::_CreateCS(LPCSTR Name, u32 flags /*= 0*/) { return CreateShader<SCS>(Name, nullptr, flags); }
void CResourceManager::_DeleteCS(const SCS* CS) { DestroyShader(CS); }
#endif

//--------------------------------------------------------------------------------------------------------------

SDeclaration* CResourceManager::_CreateDecl(const D3DVERTEXELEMENT9* dcl)
{
    // Search equal code
    for (SDeclaration* D : v_declarations)
    {
        if (dcl_equal(dcl, &D->dcl_code.front()))
            return D;
    }

    // Create _new
    SDeclaration* D = v_declarations.emplace_back(xr_new<SDeclaration>());
    u32 dcl_size = GetDeclLength(dcl) + 1;
    //	Don't need it for DirectX 10 here
    // CHK_DX					(HW.pDevice->CreateVertexDeclaration(dcl,&D->dcl));
    D->dcl_code.assign(dcl, dcl + dcl_size);
    ConvertVertexDeclaration(D->dcl_code, D->dx11_dcl_code);
    D->dwFlags |= xr_resource_flagged::RF_REGISTERED;

    return D;
}

// [dx12:gd] begin decl
SDeclaration* CResourceManager::_CreateDeclGD(const SDeclaration* base)
{
    // The D3D9 code only tells declarations apart: the base's elements and a marker in stream 1 that
    // no D3D9 usage can produce.
    constexpr BYTE gdref_usage = 0xE7;
    xr_vector<VertexElement> code(base->dcl_code.begin(), base->dcl_code.end() - 1);
    code.push_back({1, 0, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT, gdref_usage, 0});
    code.push_back(D3DDECL_END());

    for (SDeclaration* D : v_declarations)
    {
        if (dcl_equal(code.data(), &D->dcl_code.front()))
            return D;
    }

    SDeclaration* D = v_declarations.emplace_back(xr_new<SDeclaration>());
    D->dcl_code = std::move(code);
    D->dx11_dcl_code.assign(base->dx11_dcl_code.begin(), base->dx11_dcl_code.end() - 1);
    D->dx11_dcl_code.push_back({"GDREF", 0, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1});
    D->dx11_dcl_code.push_back({});
    D->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    return D;
}

SGeometry* CResourceManager::CreateGeomGD(SDeclaration* dcl, VertexBufferHandle vb, IndexBufferHandle ib, u32 vb_stride)
{
    for (SGeometry* geom : v_geoms)
    {
        if (geom->dcl == dcl && geom->vb == vb && geom->ib == ib && geom->vb_stride == vb_stride)
            return geom;
    }
    SGeometry* geom = v_geoms.emplace_back(xr_new<SGeometry>());
    geom->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    geom->dcl = dcl;
    geom->vb = vb;
    geom->vb_stride = vb_stride;
    geom->ib = ib;
    return geom;
}
// [dx12:gd] end decl

//--------------------------------------------------------------------------------------------------------------
dx11ConstantBuffer* CResourceManager::_CreateConstantBuffer(u32 context_id, ID3DShaderReflectionConstantBuffer* pTable)
{
    VERIFY(pTable);
    dx11ConstantBuffer* pTempBuffer = xr_new<dx11ConstantBuffer>(context_id, pTable);

    for (dx11ConstantBuffer* buf : v_constant_buffer[context_id])
    {
        if (pTempBuffer->Similar(*buf))
        {
            xr_delete(pTempBuffer);
            return buf;
        }
    }

    pTempBuffer->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    v_constant_buffer[context_id].emplace_back(pTempBuffer);
    return pTempBuffer;
}

void CResourceManager::_DeleteConstantBuffer(u32 context_id, const dx11ConstantBuffer* pBuffer)
{
    if (0 == (pBuffer->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    if (reclaim(v_constant_buffer[context_id], pBuffer))
        return;
#ifndef MASTER_GOLD
    Msg("! ERROR: Failed to find compiled constant buffer");
#endif
}

//--------------------------------------------------------------------------------------------------------------
SInputSignature* CResourceManager::_CreateInputSignature(ID3DBlob* pBlob)
{
    VERIFY(pBlob);

    for (SInputSignature* sign : v_input_signature)
    {
        if ((pBlob->GetBufferSize() == sign->signature->GetBufferSize()) &&
            (!(memcmp(pBlob->GetBufferPointer(), sign->signature->GetBufferPointer(), pBlob->GetBufferSize()))))
        {
            return sign;
        }
    }

    SInputSignature* pSign = v_input_signature.emplace_back(xr_new<SInputSignature>(pBlob));
    pSign->dwFlags |= xr_resource_flagged::RF_REGISTERED;

    return pSign;
}

void CResourceManager::_DeleteInputSignature(const SInputSignature* pSignature)
{
    if (0 == (pSignature->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    if (reclaim(v_input_signature, pSignature))
        return;
#ifndef MASTER_GOLD
    Msg("! ERROR: Failed to find input signature");
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
