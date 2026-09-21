#include "stdafx.h"
#pragma hdrstop

#include "xrCDB.h"
#include "Frustum.h"

#include <xmmintrin.h>

using namespace CDB;
using namespace Opcode;

template <bool bClass3, bool bFirst>
class frustum_collider
{
public:
    COLLIDER* dest;
    TRI* tris;
    Fvector* verts;

    const CFrustum* F;

    IC void _init(COLLIDER* CL, Fvector* V, TRI* T, const CFrustum* _F)
    {
        dest = CL;
        tris = T;
        verts = V;
        F = _F;
    }
    IC EFC_Visible _box(Fvector& C, Fvector& E, u32& mask)
    {
        Fvector mM[2];
        mM[0].sub(C, E);
        mM[1].add(C, E);
        return F->testAABB(&mM[0].x, mask);
    }
    ICF void _store(u32 prim, const TRI& T)
    {
        RESULT& R = dest->r_add();
        R.id = prim;
        R.verts[0] = verts[T.verts[0]];
        R.verts[1] = verts[T.verts[1]];
        R.verts[2] = verts[T.verts[2]];
        R.dummy = T.dummy;
    }

    void _prim(u32 prim)
    {
        // tris[prim] was re-indexed five times per accepted triangle, and the clipped copy
        // re-read the vertices it had just been handed.
        const TRI& T = tris[prim];
        if constexpr (bClass3)
        {
            sPoly src, dst;
            src.resize(3);
            src[0] = verts[T.verts[0]];
            src[1] = verts[T.verts[1]];
            src[2] = verts[T.verts[2]];
            if (F->ClipPoly(src, dst))
                _store(prim, T);
        }
        else
        {
            _store(prim, T);
        }
    }

    void _stab(const AABBNoLeafNode* node, u32 mask)
    {
        // Actual frustum/aabb test
        EFC_Visible result = _box((Fvector&)node->mAABB.mCenter, (Fvector&)node->mAABB.mExtents, mask);
        if (fcvNone == result)
            return;

        // Keep the children in cache: a BVH node is walked by every query that reaches it.
        _mm_prefetch((const char*)node->GetPos(), _MM_HINT_T0);
        _mm_prefetch((const char*)node->GetNeg(), _MM_HINT_T0);

        // 1st chield
        if (node->HasLeaf())
            _prim(node->GetPrimitive());
        else
            _stab(node->GetPos(), mask);

        // Early exit for "only first"
        if constexpr (bFirst)
        {
            if (dest->r_count())
                return;
        }

        // 2nd chield
        if (node->HasLeaf2())
            _prim(node->GetPrimitive2());
        else
            _stab(node->GetNeg(), mask);
    }
};

template <bool bClass3, bool bFirst>
ICF void frustum_run(
    COLLIDER* dest, Fvector* V, TRI* T, const AABBNoLeafNode* N, const CFrustum& F, u32 mask)
{
    frustum_collider<bClass3, bFirst> BC;
    BC._init(dest, V, T, &F);
    BC._stab(N, mask);
}

void COLLIDER::frustum_query(u32 frustum_mode, const MODEL* m_def, const CFrustum& F)
{
    ZoneScoped;
    m_def->syncronize();

    // Get nodes
    const AABBNoLeafTree* T = (const AABBNoLeafTree*)m_def->tree->GetTree();
    const AABBNoLeafNode* N = T->GetNodes();
    const u32 mask = F.getMask();
    r_clear();

    // One switch instead of a two-level if tree: same instantiations, mode bits read once.
    const u32 sel = ((frustum_mode & OPT_FULL_TEST) ? 1u : 0u) | ((frustum_mode & OPT_ONLYFIRST) ? 2u : 0u);
    switch (sel)
    {
    case 0: frustum_run<false, false>(this, m_def->verts, m_def->tris, N, F, mask); break;
    case 1: frustum_run<true, false>(this, m_def->verts, m_def->tris, N, F, mask); break;
    case 2: frustum_run<false, true>(this, m_def->verts, m_def->tris, N, F, mask); break;
    default: frustum_run<true, true>(this, m_def->verts, m_def->tris, N, F, mask); break;
    }
}
