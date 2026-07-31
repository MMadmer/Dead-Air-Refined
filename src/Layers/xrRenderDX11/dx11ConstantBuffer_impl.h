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
        VERIFY((base + 2 * lineSize) <= m_uiBufferSize);
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        rows = 2;
        break;
    case RC_3x4:
        base = (u32)L.index + 3 * lineSize * e;
        VERIFY((base + 3 * lineSize) <= m_uiBufferSize);
        data[0].set(A._11, A._21, A._31, A._41);
        data[1].set(A._12, A._22, A._32, A._42);
        data[2].set(A._13, A._23, A._33, A._43);
        rows = 3;
        break;
    case RC_4x4:
        base = (u32)L.index + 4 * lineSize * e;
        VERIFY((base + 4 * lineSize) <= m_uiBufferSize);
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
    u32 base = (u32)L.index + lineSize * e;
    VERIFY((base + lineSize) <= m_uiBufferSize);
    Update(static_cast<u16>(base), &A, sizeof(A));
}

IC void* dx11ConstantBuffer::AccessDirect(R_constant_load& L, size_t DataSize)
{
    //	Check buffer size in client code: don't know if actual data will cross
    //	buffer boundaries.
    VERIFY(L.index < (int)m_uiBufferSize);
    u8* res = ((u8*)m_pBufferData) + L.index;

    if ((size_t)L.index + DataSize <= m_uiBufferSize)
    {
        MarkDirty(L.index, DataSize);
        return res;
    }
    else
        return 0;
}
} // namespace xray::render::RENDER_NAMESPACE
