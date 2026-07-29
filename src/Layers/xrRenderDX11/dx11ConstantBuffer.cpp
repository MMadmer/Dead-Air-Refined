#include "stdafx.h"
#include "dx11ConstantBuffer.h"

#include "Layers/xrRender/BufferUtils.h"

namespace xray::render::RENDER_NAMESPACE
{
dx11ConstantBuffer::~dx11ConstantBuffer()
{
    RImplementation.Resources->_DeleteConstantBuffer(m_contextId, this);
    //	Flush();
    _RELEASE(m_pBuffer);
    xr_free(m_pBufferData);
    xr_free(m_pCommittedData);
}

dx11ConstantBuffer::dx11ConstantBuffer(u32 contextId, ID3DShaderReflectionConstantBuffer* pTable)
    : m_contextId(contextId), m_pBuffer(nullptr), m_uiBufferSize(0), m_pBufferData(nullptr), m_pCommittedData(nullptr),
      m_bChanged(true), m_hasCommittedData(false), m_queuedForFlush(false), m_knownDifferent(false),
      m_dirtyBegin(0), m_dirtyEnd(0)
{
    D3D_SHADER_BUFFER_DESC Desc;

    CHK_DX(pTable->GetDesc(&Desc));

    m_strBufferName._set(Desc.Name);
    m_eBufferType = Desc.Type;
    m_uiBufferSize = Desc.Size;

    //	Fill member list with variable descriptions
    m_MembersList.resize(Desc.Variables);
    m_MembersNames.resize(Desc.Variables);
    for (u32 i = 0; i < Desc.Variables; ++i)
    {
        ID3DShaderReflectionVariable* pVar;
        ID3DShaderReflectionType* pType;

        D3D_SHADER_VARIABLE_DESC var_desc;

        pVar = pTable->GetVariableByIndex(i);
        VERIFY(pVar);
        pType = pVar->GetType();
        VERIFY(pType);
        pType->GetDesc(&m_MembersList[i]);
        //	Buffers with the same layout can contain totally different members
        CHK_DX(pVar->GetDesc(&var_desc));
        m_MembersNames[i] = var_desc.Name;
    }

    m_uiMembersCRC = crc32(&m_MembersList[0], Desc.Variables * sizeof(m_MembersList[0]));

    R_CHK(BufferUtils::CreateConstantBuffer(&m_pBuffer, Desc.Size));
    VERIFY(m_pBuffer);
    m_pBufferData = xr_malloc(Desc.Size);
    VERIFY(m_pBufferData);
    m_pCommittedData = xr_malloc(Desc.Size);
    VERIFY(m_pCommittedData);
    ZeroMemory(m_pBufferData, Desc.Size);
    ZeroMemory(m_pCommittedData, Desc.Size);
    m_dirtyEnd = Desc.Size;

#ifdef DEBUG
    if (m_pBuffer)
    {
        m_pBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, xr_strlen(Desc.Name), Desc.Name);
    }
#endif
}

bool dx11ConstantBuffer::Similar(dx11ConstantBuffer& _in)
{
    if (m_strBufferName._get() != _in.m_strBufferName._get())
        return false;

    if (m_eBufferType != _in.m_eBufferType)
        return false;

    if (m_uiMembersCRC != _in.m_uiMembersCRC)
        return false;

    if (m_MembersList.size() != _in.m_MembersList.size())
        return false;

    if (memcmp(&m_MembersList[0], &_in.m_MembersList[0], m_MembersList.size() * sizeof(m_MembersList[0])))
        return false;

    VERIFY(m_MembersNames.size() == _in.m_MembersNames.size());

    int iMemberNum = m_MembersNames.size();
    for (int i = 0; i < iMemberNum; ++i)
    {
        if (m_MembersNames[i].c_str() != _in.m_MembersNames[i].c_str())
            return false;
    }

    return true;
}

bool dx11ConstantBuffer::QueueForFlush()
{
    if (m_queuedForFlush)
        return false;

    m_queuedForFlush = true;
    return true;
}

void dx11ConstantBuffer::MarkDirty(u16 offset, size_t size)
{
    const u32 end = offset + static_cast<u32>(size);
    if (!m_bChanged)
    {
        m_dirtyBegin = offset;
        m_dirtyEnd = end;
        m_bChanged = true;
        return;
    }

    m_dirtyBegin = std::min(m_dirtyBegin, static_cast<u32>(offset));
    m_dirtyEnd = std::max(m_dirtyEnd, end);
}

void dx11ConstantBuffer::ResetDirtyRange()
{
    m_dirtyBegin = m_uiBufferSize;
    m_dirtyEnd = 0;
}

void dx11ConstantBuffer::Update(u16 offset, const void* data, size_t size)
{
    VERIFY(static_cast<size_t>(offset) + size <= m_uiBufferSize);
    auto* destination = static_cast<u8*>(m_pBufferData) + offset;
    if (!m_bChanged)
    {
        if (!memcmp(destination, data, size))
            return;
        m_knownDifferent = true;
    }

    CopyMemory(destination, data, size);
    MarkDirty(offset, size);
}

void dx11ConstantBuffer::Flush(u32 context_id)
{
    if (!m_bChanged)
    {
        m_queuedForFlush = false;
        return;
    }

    const size_t dirtySize = m_dirtyEnd - m_dirtyBegin;
    const auto* currentDirtyData = static_cast<const u8*>(m_pBufferData) + m_dirtyBegin;
    const auto* committedDirtyData = static_cast<const u8*>(m_pCommittedData) + m_dirtyBegin;
    if (m_hasCommittedData && !m_knownDifferent && !memcmp(currentDirtyData, committedDirtyData, dirtySize))
    {
        m_bChanged = false;
        m_queuedForFlush = false;
        m_knownDifferent = false;
        ResetDirtyRange();
        return;
    }

    void* pData;
#ifdef USE_DX11
    D3D11_MAPPED_SUBRESOURCE pSubRes;
    const auto context = HW.get_context(context_id);
    CHK_DX(context->Map(m_pBuffer, 0, D3D_MAP_WRITE_DISCARD, 0, &pSubRes));
    pData = pSubRes.pData;
#else
    CHK_DX(m_pBuffer->Map(D3D_MAP_WRITE_DISCARD, 0, &pData));
#endif
    VERIFY(pData);
    VERIFY(m_pBufferData);
    CopyMemory(pData, m_pBufferData, m_uiBufferSize);
#ifdef USE_DX11
    context->Unmap(m_pBuffer, 0);
#else
    m_pBuffer->Unmap();
#endif
    if (m_hasCommittedData)
        CopyMemory(static_cast<u8*>(m_pCommittedData) + m_dirtyBegin, currentDirtyData, dirtySize);
    else
        CopyMemory(m_pCommittedData, m_pBufferData, m_uiBufferSize);
    m_hasCommittedData = true;
    m_bChanged = false;
    m_queuedForFlush = false;
    m_knownDifferent = false;
    ResetDirtyRange();
}
} // namespace xray::render::RENDER_NAMESPACE
