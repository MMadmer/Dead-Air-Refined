#include "stdafx.h"
#pragma hdrstop
#include "DetailModel.h"

#if !defined(_EDITOR) && defined(USE_DX11)
#include "xrstripify.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
CDetail::~CDetail() {}
void CDetail::Unload()
{
    if (vertices)
    {
        xr_free(vertices);
        vertices = nullptr;
    }
    if (indices)
    {
        xr_free(indices);
        indices = nullptr;
    }
    shader.destroy();
}

/** Dirty hack - transfer indices (in 32bit lines) (2 indices by pass)
 * @brief CDetail::transfer_indices
 * @param iDest
 * @param iOffset
 */
void CDetail::transfer_indices(u16* iDest, u32 iOffset)
{
    VERIFY(iOffset < 65535);
    {
        u32 item = (iOffset << 16) | iOffset;
        u32 count = number_indices / 2;
        u32* sit = (u32*)(indices);
        u32* send = sit + count;
        u32* dit = (u32*)(iDest);
        for (; sit != send; dit++, sit++)
            *dit = *sit + item;
        if (number_indices & 1)
            iDest[number_indices - 1] = u16(indices[number_indices - 1] + u16(iOffset));
    }
}

void CDetail::transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u16* iDest, u32 iOffset)
{
    // Transfer vertices
    {
        CDetail::fvfVertexIn *srcIt = vertices, *srcEnd = vertices + number_vertices;
        CDetail::fvfVertexOut* dstIt = vDest;
        for (; srcIt != srcEnd; srcIt++, dstIt++)
        {
            mXform.transform_tiny(dstIt->P, srcIt->P);
            dstIt->C = C;
            dstIt->u = srcIt->u;
            dstIt->v = srcIt->v;
        }
    }

    transfer_indices(iDest, iOffset);
}

void CDetail::transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u16* iDest, u32 iOffset, float du, float dv)
{
    // Transfer vertices
    {
        CDetail::fvfVertexIn *srcIt = vertices, *srcEnd = vertices + number_vertices;
        CDetail::fvfVertexOut* dstIt = vDest;
        for (; srcIt != srcEnd; srcIt++, dstIt++)
        {
            mXform.transform_tiny(dstIt->P, srcIt->P);
            dstIt->C = C;
            dstIt->u = srcIt->u + du;
            dstIt->v = srcIt->v + dv;
        }
    }

    transfer_indices(iDest, iOffset);
}

u64 detail_key_fnv(const void* data, size_t size, u64 hash)
{
    for (const u8 *it = static_cast<const u8*>(data), *end = it + size; it != end; ++it)
        hash = (hash ^ *it) * 0x100000001b3ull;
    return hash;
}

namespace
{
// Round half away from zero in double, as the reference generator does.
s32 key_quant(float x, double k)
{
    const double v = double(x) * k;
    return v >= 0.0 ? s32(v + 0.5) : -s32(-v + 0.5);
}

u64 key_name(pcstr name, u64 hash)
{
    for (pcstr c = name; *c; ++c)
    {
        // ASCII-only lower case: the reference lowers bytes, not code-page letters.
        const u8 b = u8(*c);
        const u8 l = (b >= 'A' && b <= 'Z') ? u8(b + 32) : b;
        hash = detail_key_fnv(&l, 1, hash);
    }
    const u8 zero = 0;
    return detail_key_fnv(&zero, 1, hash);
}
} // namespace

void CDetail::Load(IReader* S)
{
    // Shader
    string256 fnT, fnS;
    S->r_stringZ(fnS, sizeof(fnS));
    S->r_stringZ(fnT, sizeof(fnT));
    shader.create(fnS, fnT);
    m_key = key_name(fnT, key_name(fnS, 0xcbf29ce484222325ull));

    // Params
    m_Flags.assign(S->r_u32());
    m_fMinScale = S->r_float();
    m_fMaxScale = S->r_float();
    number_vertices = S->r_u32();
    number_indices = S->r_u32();
    R_ASSERT(0 == (number_indices % 3));

    // Vertices
    u32 size_vertices = number_vertices * sizeof(fvfVertexIn);
    vertices = xr_alloc<CDetail::fvfVertexIn>(number_vertices);
    S->r(vertices, size_vertices);

    // Indices
    u32 size_indices = number_indices * sizeof(u16);
    indices = xr_alloc<u16>(number_indices);
    S->r(indices, size_indices);

// Validate indices
#ifdef DEBUG
    for (u32 idx = 0; idx < number_indices; idx++)
        R_ASSERT(indices[idx] < (u16)number_vertices);
#endif

    // Calc BB & SphereRadius
    bv_bb.invalidate();
    for (u32 i = 0; i < number_vertices; i++)
        bv_bb.modify(vertices[i].P);
    bv_bb.getsphere(bv_sphere.P, bv_sphere.R);

    // K1, from the data in file order (Optimize below permutes it).
    {
        const u32 flags = m_Flags.get();
        const s32 scale[2] = {key_quant(m_fMinScale, 1000.0), key_quant(m_fMaxScale, 1000.0)};
        const u32 counts[2] = {number_vertices, number_indices};
        m_key = detail_key_fnv(&flags, sizeof(flags), m_key);
        m_key = detail_key_fnv(scale, sizeof(scale), m_key);
        m_key = detail_key_fnv(counts, sizeof(counts), m_key);
        m_uv_min[0] = m_uv_min[1] = flt_max;
        m_uv_max[0] = m_uv_max[1] = -flt_max;
        for (u32 i = 0; i < number_vertices; ++i)
        {
            const fvfVertexIn& v = vertices[i];
            const s32 q[5] = {key_quant(v.P.x, 1000.0), key_quant(v.P.y, 1000.0), key_quant(v.P.z, 1000.0),
                key_quant(v.u, 16384.0), key_quant(v.v, 16384.0)};
            m_key = detail_key_fnv(q, sizeof(q), m_key);
            m_uv_min[0] = std::min(m_uv_min[0], v.u);
            m_uv_min[1] = std::min(m_uv_min[1], v.v);
            m_uv_max[0] = std::max(m_uv_max[0], v.u);
            m_uv_max[1] = std::max(m_uv_max[1], v.v);
        }
        m_key = detail_key_fnv(indices, number_indices * sizeof(u16), m_key);
        if (!number_vertices)
            m_uv_min[0] = m_uv_min[1] = m_uv_max[0] = m_uv_max[1] = 0.f;
    }

#if !defined(_EDITOR) && defined(USE_DX11)
    Optimize();
#endif
}

#if !defined(_EDITOR) && defined(USE_DX11)
void CDetail::Optimize()
{
    xr_vector<u16> vec_indices, vec_permute;
    const int cache = HW.Caps.geometry.dwVertexCache;

    // Stripify
    vec_indices.assign(indices, indices + number_indices);
    vec_permute.resize(number_vertices);
    int vt_old = xrSimulate(vec_indices, cache);
    xrStripify(vec_indices, vec_permute, cache, 0);
    int vt_new = xrSimulate(vec_indices, cache);
    if (vt_new < vt_old)
    {
        // Msg					("* DM: %d verts, %d indices, VT: %d/%d",number_vertices,number_indices,vt_old,vt_new);

        // Copy faces
        CopyMemory(indices, &*vec_indices.begin(), vec_indices.size() * sizeof(u16));

        // Permute vertices
        xr_vector<fvfVertexIn> verts;
        verts.assign(vertices, vertices + number_vertices);
        for (u32 i = 0; i < verts.size(); i++)
            vertices[i] = verts[vec_permute[i]];
    }
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
