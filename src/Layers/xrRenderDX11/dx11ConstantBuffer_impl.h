#pragma once

namespace xray::render::RENDER_NAMESPACE
{
IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fmatrix& A)
{
    VERIFY(RC_float == C->type);
    Fvector4 data[4];
    u32 rows = 0;
    switch (L.cls)
    {
    case RC_2x4:
        VERIFY(u32((u32)L.index + 2 * lineSize) <= m_uiBufferSize);
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        rows = 2;
        break;
    case RC_3x4:
        VERIFY(u32((u32)L.index + 3 * lineSize) <= m_uiBufferSize);
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        data[2].set(A._13, A._23, A._33, A._43);
        rows = 3;
        break;
    case RC_4x4:
        VERIFY(u32((u32)L.index + 4 * lineSize) <= m_uiBufferSize);
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        data[2].set(A._13, A._23, A._33, A._43);
        data[3].set(A._14, A._24, A._34, A._44);
        rows = 4;
        break;
    default:
#ifdef DEBUG
        xrDebug::Fatal(DEBUG_INFO, "Invalid constant run-time-type for '%s'", C->name.c_str());
#else
        NODEFAULT;
#endif
    }
    Update(L.index, data, rows * lineSize);
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fvector4& A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);

    VERIFY(u32((u32)L.index + lineSize) <= m_uiBufferSize);

    size_t count = 4;
    switch (L.cls)
    {
    case RC_1x2: count = 2; break;
    case RC_1x3: count = 3; break;
    case RC_1x4: count = 4; break;
    default: break;
    }

    Update(L.index, &A[0], count * sizeof(float));
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, float A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x1 == L.cls);
    VERIFY(u32((u32)L.index + sizeof(float)) <= m_uiBufferSize);
    Update(L.index, &A, sizeof(A));
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, int A)
{
    VERIFY(RC_int == C->type);
    VERIFY(RC_1x1 == L.cls);
    VERIFY(u32((u32)L.index + sizeof(int)) <= m_uiBufferSize);
    Update(L.index, &A, sizeof(A));
}

IC void dx11ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A)
{
    VERIFY(RC_float == C->type);
    u32 base = 0;
    Fvector4 data[4];
    u32 rows = 0;
    switch (L.cls)
    {
    case RC_2x4:
        base = (u32)L.index + 2 * lineSize * e;
        if ((base + 2 * lineSize) > m_uiBufferSize)
            return;
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        rows = 2;
        break;
    case RC_3x4:
        base = (u32)L.index + 3 * lineSize * e;
        if ((base + 3 * lineSize) > m_uiBufferSize)
            return;
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        data[2].set(A._13, A._23, A._33, A._43);
        rows = 3;
        break;
    case RC_4x4:
        base = (u32)L.index + 4 * lineSize * e;
        if ((base + 4 * lineSize) > m_uiBufferSize)
            return;
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        data[2].set(A._13, A._23, A._33, A._43);
        data[3].set(A._14, A._24, A._34, A._44);
        rows = 4;
        break;
    default:
#ifdef DEBUG
        xrDebug::Fatal(DEBUG_INFO, "Invalid constant run-time-type for '%s'", C->name.c_str());
#else
        NODEFAULT;
#endif
    }
    Update(static_cast<u16>(base), data, rows * lineSize);
}

IC void dx11ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);

    static const u16 lineSize = 4 * sizeof(float);
    // e is a runtime array index (bone palette, light arrays); the bound has to hold in release,
    // because the offset is truncated to u16 on the way into the buffer.
    u32 base = (u32)L.index + lineSize * e;
    if ((base + lineSize) > m_uiBufferSize)
        return;
    Update(static_cast<u16>(base), &A, sizeof(A));
}

IC void* dx11ConstantBuffer::AccessDirect(R_constant_load& L, size_t DataSize)
{
    //	Check buffer size in client code: don't know if actual data will cross
    //	buffer boundaries.
    VERIFY(L.index < (int)m_uiBufferSize);
    if ((size_t)L.index + DataSize > m_uiBufferSize)
        return nullptr;

    // Straight into the mapped buffer when this buffer holds nothing else and nothing has
    // written its shadow since the last flush - see m_singleMember. Either condition
    // failing means something else's bytes are in there and the discard would lose them.
    if (m_singleMember && (m_pMapped || !m_bChanged))
    {
        u8* mapped = static_cast<u8*>(MapDirect());
        MarkDirty(L.index, DataSize);
        return mapped + L.index;
    }

    // The caller is handed the raw bytes and writes whatever it likes into them, so Flush
    // cannot know whether the range still matches what was committed. It used to find out
    // with a memcmp of the whole range on every flush - for the detail dump, a compare of
    // a batch of instances against the previous, unrelated batch.
    m_knownDifferent = true;
    MarkDirty(L.index, DataSize);
    return static_cast<u8*>(m_pBufferData) + L.index;
}
} // namespace xray::render::RENDER_NAMESPACE
