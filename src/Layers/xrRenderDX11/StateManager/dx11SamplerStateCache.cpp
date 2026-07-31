#include "stdafx.h"
#include "dx11SamplerStateCache.h"
#include "Layers/xrRenderDX11/dx11StateUtils.h"

namespace xray::render::RENDER_NAMESPACE
{
using dx11StateUtils::operator==;

dx11SamplerStateCache SSManager;

dx11SamplerStateCache::dx11SamplerStateCache() : m_uiMaxAnisotropy(1), m_uiMipLODBias(0.0f)
{
    static const int iMaxRSStates = 10;
    m_StateArray.reserve(iMaxRSStates);
}

dx11SamplerStateCache::~dx11SamplerStateCache() { ClearStateArray(); }
dx11SamplerStateCache::SHandle dx11SamplerStateCache::GetState(D3D_SAMPLER_DESC& desc)
{
    SHandle hResult;

    //	MaxAnisitropy is reset by ValidateState if not aplicable
    //	to the filter mode used.
    desc.MaxAnisotropy = m_uiMaxAnisotropy;
    // RZ
    desc.MipLODBias = m_uiMipLODBias;

    dx11StateUtils::ValidateState(desc);

    u32 crc = dx11StateUtils::GetHash(desc);

    hResult = FindState(desc, crc);

    if (hResult == hInvalidHandle)
    {
        StateRecord rec{};
        rec.m_crc = crc;
        rec.m_desc = desc;
        CreateState(desc, &rec.m_pState);
        hResult = m_StateArray.size();
        m_StateArray.push_back(rec);
    }

    return hResult;
}

void dx11SamplerStateCache::CreateState(StateDecs desc, IDeviceState** ppIState)
{
    CHK_DX(HW.pDevice->CreateSamplerState(&desc, ppIState));
}

dx11SamplerStateCache::SHandle dx11SamplerStateCache::FindState(const StateDecs& desc, u32 StateCRC)
{
    u32 res = 0xffffffff;
    u32 i = 0;
    while (i < m_StateArray.size())
    {
        if (m_StateArray[i].m_crc == StateCRC)
        {
            if (m_StateArray[i].m_desc == desc)
            // return i;
            //	TEST
            {
                // return i;
                res = i;
                break;
            }
            // else
            //{
            //	VERIFY(0);
            //}
        }
        i++;
    }

    return res != 0xffffffff ? i : (u32)hInvalidHandle;
}

void dx11SamplerStateCache::ClearStateArray()
{
    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        _RELEASE(m_StateArray[i].m_pState);
    }

    m_StateArray.clear();
    ZeroMemory(m_boundSamplerCounts, sizeof(m_boundSamplerCounts));
    InvalidateBoundArrays();
}

void dx11SamplerStateCache::ResetContext(u32 contextId)
{
    VERIFY(contextId < R__NUM_CONTEXTS);
    ZeroMemory(m_boundSamplerCounts[contextId], sizeof(m_boundSamplerCounts[contextId]));
    ZeroMemory(m_boundSamplerArrays[contextId], sizeof(m_boundSamplerArrays[contextId]));
}

bool dx11SamplerStateCache::IsAlreadyBound(
    u32 contextId, ShaderStage stage, const HArray& samplers) const
{
    return m_boundSamplerArrays[contextId][static_cast<u32>(stage)] == &samplers;
}

void dx11SamplerStateCache::MarkBound(u32 contextId, ShaderStage stage, const HArray& samplers)
{
    m_boundSamplerArrays[contextId][static_cast<u32>(stage)] = &samplers;
}

void dx11SamplerStateCache::InvalidateBoundArrays()
{
    ZeroMemory(m_boundSamplerArrays, sizeof(m_boundSamplerArrays));
}

u32 dx11SamplerStateCache::PrepareSamplerStates(
    u32 contextId, ShaderStage stage, const HArray& samplers,
    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT])
{
    VERIFY(contextId < R__NUM_CONTEXTS);
    VERIFY(samplers.size() <= D3D_COMMONSHADER_SAMPLER_SLOT_COUNT);
    for (u32 i = 0; i < samplers.size(); ++i)
    {
        if (samplers[i] != hInvalidHandle)
        {
            VERIFY(samplers[i] < m_StateArray.size());
            pSS[i] = m_StateArray[samplers[i]].m_pState;
        }
    }

    auto& previousCount = m_boundSamplerCounts[contextId][static_cast<u32>(stage)];
    const auto applyCount = std::max<u32>(static_cast<u32>(samplers.size()), previousCount);
    previousCount = static_cast<u8>(samplers.size());
    return applyCount;
}

void dx11SamplerStateCache::VSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Vertex, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Vertex)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Vertex, samplers, pSS);
    if (count)
        HW.get_context(context_id)->VSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Vertex, samplers);
}

void dx11SamplerStateCache::PSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Pixel, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Pixel)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Pixel, samplers, pSS);
    if (count)
        HW.get_context(context_id)->PSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Pixel, samplers);
}

void dx11SamplerStateCache::GSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Geometry, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Geometry)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Geometry, samplers, pSS);
    if (count)
        HW.get_context(context_id)->GSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Geometry, samplers);
}

void dx11SamplerStateCache::HSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Hull, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Hull)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Hull, samplers, pSS);
    if (count)
        HW.get_context(context_id)->HSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Hull, samplers);
}

void dx11SamplerStateCache::DSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Domain, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Domain)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Domain, samplers, pSS);
    if (count)
        HW.get_context(context_id)->DSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Domain, samplers);
}

void dx11SamplerStateCache::CSApplySamplers(u32 context_id, HArray& samplers)
{
    if (IsAlreadyBound(context_id, ShaderStage::Compute, samplers))
        return;
    if (samplers.empty() && !m_boundSamplerCounts[context_id][static_cast<u32>(ShaderStage::Compute)])
        return;

    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    const auto count = PrepareSamplerStates(context_id, ShaderStage::Compute, samplers, pSS);
    if (count)
        HW.get_context(context_id)->CSSetSamplers(0, count, pSS);
    MarkBound(context_id, ShaderStage::Compute, samplers);
}

void dx11SamplerStateCache::SetMaxAnisotropy(u32 uiMaxAniso)
{
    clamp(uiMaxAniso, (u32)1, (u32)16);

    if (m_uiMaxAnisotropy == uiMaxAniso)
        return;

    m_uiMaxAnisotropy = uiMaxAniso;

    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        StateRecord& rec = m_StateArray[i];
        StateDecs desc = rec.m_desc;

        //	MaxAnisitropy is reset by ValidateState if not aplicable
        //	to the filter mode used.
        //	Reason: all checks for aniso applicability are done
        //	in ValidateState.
        desc.MaxAnisotropy = m_uiMaxAnisotropy;
        dx11StateUtils::ValidateState(desc);

        //	This can cause fragmentation if called too often
        rec.m_pState->Release();
        CreateState(desc, &rec.m_pState);
        rec.m_desc = desc;
    }
    InvalidateBoundArrays();
}

void dx11SamplerStateCache::SetMipLODBias(float uiMipLODBias)
{
    if (m_uiMipLODBias == uiMipLODBias)
        return;

    m_uiMipLODBias = uiMipLODBias;

    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        StateRecord& rec = m_StateArray[i];
        StateDecs desc = rec.m_desc;

        desc.MipLODBias = m_uiMipLODBias;
        dx11StateUtils::ValidateState(desc);

        // This can cause fragmentation if called too often
        rec.m_pState->Release();
        CreateState(desc, &rec.m_pState);
        rec.m_desc = desc;
    }
    InvalidateBoundArrays();
}
} // namespace xray::render::RENDER_NAMESPACE
