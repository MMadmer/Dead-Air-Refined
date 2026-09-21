#include "stdafx.h"
#pragma hdrstop // ???

#include "xrCore/_fbox.h"
#include "xrCDB.h"

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K) || defined(XR_ARCHITECTURE_PPC64)
#include <xmmintrin.h>
#elif defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64)
#include "sse2neon/sse2neon.h"
#elif defined(XR_ARCHITECTURE_RISCV)
#include "sse2rvv/sse2rvv.h"
#else
#error Add your platform here
#endif

namespace CDB
{
using namespace Opcode;

struct alignas(16) vec_t : public Fvector3
{
    float pad;
};
// static vec_t	vec_c	( float _x, float _y, float _z)	{ vec_t v; v.x=_x;v.y=_y;v.z=_z;v.pad=0; return v; }

struct alignas(16) aabb_t
{
    vec_t min;
    vec_t max;
};

struct alignas(16) ray_t
{
    vec_t pos;
    vec_t inv_dir;
    vec_t fwd_dir;
};

ICF u32& uf(float& x) { return (u32&)x; }
ICF bool isect_fpu(const Fvector& min, const Fvector& max, const ray_t& ray, Fvector& coord)
{
    Fvector MaxT;
    MaxT.x = MaxT.y = MaxT.z = -1.0f;
    bool Inside = true;

    // Find candidate planes.
    if (ray.pos[0] < min[0])
    {
        coord[0] = min[0];
        Inside = FALSE;
        if (uf(ray.inv_dir[0]))
            MaxT[0] = (min[0] - ray.pos[0]) * ray.inv_dir[0]; // Calculate T distances to candidate planes
    }
    else if (ray.pos[0] > max[0])
    {
        coord[0] = max[0];
        Inside = FALSE;
        if (uf(ray.inv_dir[0]))
            MaxT[0] = (max[0] - ray.pos[0]) * ray.inv_dir[0]; // Calculate T distances to candidate planes
    }
    if (ray.pos[1] < min[1])
    {
        coord[1] = min[1];
        Inside = FALSE;
        if (uf(ray.inv_dir[1]))
            MaxT[1] = (min[1] - ray.pos[1]) * ray.inv_dir[1]; // Calculate T distances to candidate planes
    }
    else if (ray.pos[1] > max[1])
    {
        coord[1] = max[1];
        Inside = FALSE;
        if (uf(ray.inv_dir[1]))
            MaxT[1] = (max[1] - ray.pos[1]) * ray.inv_dir[1]; // Calculate T distances to candidate planes
    }
    if (ray.pos[2] < min[2])
    {
        coord[2] = min[2];
        Inside = FALSE;
        if (uf(ray.inv_dir[2]))
            MaxT[2] = (min[2] - ray.pos[2]) * ray.inv_dir[2]; // Calculate T distances to candidate planes
    }
    else if (ray.pos[2] > max[2])
    {
        coord[2] = max[2];
        Inside = FALSE;
        if (uf(ray.inv_dir[2]))
            MaxT[2] = (max[2] - ray.pos[2]) * ray.inv_dir[2]; // Calculate T distances to candidate planes
    }

    // Ray ray.pos inside bounding box
    if (Inside)
    {
        coord = ray.pos;
        return true;
    }

    // Get largest of the maxT's for final choice of intersection
    u32 WhichPlane = 0;
    if (MaxT[1] > MaxT[0])
        WhichPlane = 1;
    if (MaxT[2] > MaxT[WhichPlane])
        WhichPlane = 2;

    // Check final candidate actually inside box (if max < 0)
    if (uf(MaxT[WhichPlane]) & 0x80000000)
        return false;

    if (0 == WhichPlane)
    { // 1 & 2
        coord[1] = ray.pos[1] + MaxT[0] * ray.fwd_dir[1];
        if ((coord[1] < min[1]) || (coord[1] > max[1]))
            return false;
        coord[2] = ray.pos[2] + MaxT[0] * ray.fwd_dir[2];
        if ((coord[2] < min[2]) || (coord[2] > max[2]))
            return false;
        return true;
    }
    if (1 == WhichPlane)
    { // 0 & 2
        coord[0] = ray.pos[0] + MaxT[1] * ray.fwd_dir[0];
        if ((coord[0] < min[0]) || (coord[0] > max[0]))
            return false;
        coord[2] = ray.pos[2] + MaxT[1] * ray.fwd_dir[2];
        if ((coord[2] < min[2]) || (coord[2] > max[2]))
            return false;
        return true;
    }
    if (2 == WhichPlane)
    { // 0 & 1
        coord[0] = ray.pos[0] + MaxT[2] * ray.fwd_dir[0];
        if ((coord[0] < min[0]) || (coord[0] > max[0]))
            return false;
        coord[1] = ray.pos[1] + MaxT[2] * ray.fwd_dir[1];
        if ((coord[1] < min[1]) || (coord[1] > max[1]))
            return false;
        return true;
    }
    return false;
}

// turn those verbose intrinsics into something readable.
#define loadps(mem) _mm_load_ps((const float* const)(mem))
#define storess(ss, mem) _mm_store_ss((float* const)(mem), (ss))
#define minss _mm_min_ss
#define maxss _mm_max_ss
#define minps _mm_min_ps
#define maxps _mm_max_ps
#define mulps _mm_mul_ps
#define subps _mm_sub_ps
#define rotatelps(ps) _mm_shuffle_ps((ps), (ps), 0x39) // a,b,c,d -> b,c,d,a
#define muxhps(low, high) _mm_movehl_ps((low), (high)) // low{a,b,c,d}|high{e,f,g,h} = {c,d,g,h}

static constexpr float flt_plus_inf = std::numeric_limits<float>::infinity();
alignas(16) static constexpr float ps_cst_plus_inf[4] = { flt_plus_inf, flt_plus_inf, flt_plus_inf, flt_plus_inf },
                                   ps_cst_minus_inf[4] = { -flt_plus_inf, -flt_plus_inf, -flt_plus_inf, -flt_plus_inf };

ICF bool isect_sse(const aabb_t& box, const ray_t& ray, float& dist)
{
    // you may already have those values hanging around somewhere
    const __m128 plus_inf = loadps(ps_cst_plus_inf), minus_inf = loadps(ps_cst_minus_inf);

    // use whatever's appropriate to load.
    const __m128 box_min = loadps(&box.min), box_max = loadps(&box.max), pos = loadps(&ray.pos),
                 inv_dir = loadps(&ray.inv_dir);

    // use a div if inverted directions aren't available
    const __m128 l1 = mulps(subps(box_min, pos), inv_dir);
    const __m128 l2 = mulps(subps(box_max, pos), inv_dir);

    // the order we use for those min/max is vital to filter out
    // NaNs that happens when an inv_dir is +/- inf and
    // (box_min - pos) is 0. inf * 0 = NaN
    const __m128 filtered_l1a = minps(l1, plus_inf);
    const __m128 filtered_l2a = minps(l2, plus_inf);

    const __m128 filtered_l1b = maxps(l1, minus_inf);
    const __m128 filtered_l2b = maxps(l2, minus_inf);

    // now that we're back on our feet, test those slabs.
    __m128 lmax = maxps(filtered_l1a, filtered_l2a);
    __m128 lmin = minps(filtered_l1b, filtered_l2b);

    // unfold back. try to hide the latency of the shufps & co.
    const __m128 lmax0 = rotatelps(lmax);
    const __m128 lmin0 = rotatelps(lmin);
    lmax = minss(lmax, lmax0);
    lmin = maxss(lmin, lmin0);

    const __m128 lmax1 = muxhps(lmax, lmax);
    const __m128 lmin1 = muxhps(lmin, lmin);
    lmax = minss(lmax, lmax1);
    lmin = maxss(lmin, lmin1);

    const bool ret = _mm_comige_ss(lmax, _mm_setzero_ps()) & _mm_comige_ss(lmax, lmin);

    storess(lmin, &dist);
    // storess	(lmax, &rs.t_far);

    return ret;
}

#undef loadps
#undef storess
#undef minss
#undef maxss
#undef minps
#undef maxps
#undef mulps
#undef subps
#undef rotatelps
#undef muxhps

// SSE2 is part of the x64 ABI, so the FPU collider below can never run there. Compiling it
// anyway cost eight unreachable template instantiations in the instruction cache and a runtime
// branch on every single query.
#if defined(XR_ARCHITECTURE_X64)
static constexpr bool cdb_sse_always = true;
#else
static constexpr bool cdb_sse_always = false;
#endif

template <bool bUseSSE, bool bCull, bool bFirst, bool bNearest>
class alignas(16) ray_collider
{
public:
    COLLIDER* dest;
    TRI* tris;
    Fvector* verts;

    ray_t ray;
    float rRange;
    float rRange2;

    // A nearest query keeps overwriting its single result as the range shrinks. Remember the
    // winner instead and build the RESULT once, at the end: the three vertex copies and the
    // material word are paid per surviving hit otherwise.
    int best_prim;
    float best_range, best_u, best_v;
    bool any_hit;

    IC void _init(COLLIDER* CL, Fvector* V, TRI* T, const Fvector& C, const Fvector& D, float R)
    {
        dest = CL;
        tris = T;
        verts = V;
        ray.pos.set(C);
        ray.inv_dir.set(1.f, 1.f, 1.f).div(D);
        ray.fwd_dir.set(D);
        rRange = R;
        rRange2 = R * R;
        best_prim = -1;
        best_range = best_u = best_v = 0.f;
        any_hit = false;
        if constexpr (!bUseSSE)
        {
            // for FPU - zero out inf
            if (_abs(D.x) > flt_eps)
            {
            }
            else
                ray.inv_dir.x = 0;
            if (_abs(D.y) > flt_eps)
            {
            }
            else
                ray.inv_dir.y = 0;
            if (_abs(D.z) > flt_eps)
            {
            }
            else
                ray.inv_dir.z = 0;
        }
    }

    // fpu
    ICF bool _box_fpu(const Fvector& bCenter, const Fvector& bExtents, Fvector& coord)
    {
        Fbox BB;
        BB.vMin.sub(bCenter, bExtents);
        BB.vMax.add(bCenter, bExtents);
        return isect_fpu(BB.vMin, BB.vMax, ray, coord);
    }

    // sse
    //
    // mCenter and mExtents are adjacent Points inside CollisionAABB, which is itself the first
    // member of the node, so a 4-float load from either stays inside the node. Lane 3 picks up
    // the neighbouring float and is garbage - isect_sse reduces over lanes 0..2 only and never
    // reads it, so two loads replace six load_ss plus four shuffles for bit-identical lanes.
    ICF bool _box_sse(const CollisionAABB& bb, float& dist)
    {
        aabb_t box;
        const __m128 CN = _mm_loadu_ps(&bb.mCenter.x);
        const __m128 EX = _mm_loadu_ps(&bb.mExtents.x);

        _mm_store_ps((float*)&box.min, _mm_sub_ps(CN, EX));
        _mm_store_ps((float*)&box.max, _mm_add_ps(CN, EX));

        return isect_sse(box, ray, dist);
    }

    IC bool _tri(u32* p, float& u, float& v, float& range)
    {
        Fvector edge1, edge2, tvec, pvec, qvec;
        float det, inv_det;

        // find vectors for two edges sharing vert0
        Fvector& p0 = verts[p[0]];
        Fvector& p1 = verts[p[1]];
        Fvector& p2 = verts[p[2]];
        edge1.sub(p1, p0);
        edge2.sub(p2, p0);
        // begin calculating determinant - also used to calculate U parameter
        // if determinant is near zero, ray lies in plane of triangle
        pvec.crossproduct(ray.fwd_dir, edge2);
        det = edge1.dotproduct(pvec);
        if constexpr (bCull)
        {
            if (det < EPS)
                return false;
            tvec.sub(ray.pos, p0); // calculate distance from vert0 to ray origin
            u = tvec.dotproduct(pvec); // calculate U parameter and test bounds
            if (u < 0.f || u > det)
                return false;
            qvec.crossproduct(tvec, edge1); // prepare to test V parameter
            v = ray.fwd_dir.dotproduct(qvec); // calculate V parameter and test bounds
            if (v < 0.f || u + v > det)
                return false;
            range = edge2.dotproduct(qvec); // calculate t, scale parameters, ray intersects triangle
            inv_det = 1.0f / det;
            range *= inv_det;
            u *= inv_det;
            v *= inv_det;
        }
        else
        {
            if (det > -EPS && det < EPS)
                return false;
            inv_det = 1.0f / det;
            tvec.sub(ray.pos, p0); // calculate distance from vert0 to ray origin
            u = tvec.dotproduct(pvec) * inv_det; // calculate U parameter and test bounds
            if (u < 0.0f || u > 1.0f)
                return false;
            qvec.crossproduct(tvec, edge1); // prepare to test V parameter
            v = ray.fwd_dir.dotproduct(qvec) * inv_det; // calculate V parameter and test bounds
            if (v < 0.0f || u + v > 1.0f)
                return false;
            range = edge2.dotproduct(qvec) * inv_det; // calculate t, ray intersects triangle
        }
        return true;
    }

    ICF void _store(u32 prim, float r, float u, float v)
    {
        const TRI& T = tris[prim];
        RESULT& R = dest->r_add();
        R.id = int(prim);
        R.range = r;
        R.u = u;
        R.v = v;
        R.verts[0] = verts[T.verts[0]];
        R.verts[1] = verts[T.verts[1]];
        R.verts[2] = verts[T.verts[2]];
        R.dummy = T.dummy;
    }

    void _prim(u32 prim)
    {
        float u, v, r;
        if (!_tri(tris[prim].verts, u, v, r))
            return;
        if (r <= 0 || r > rRange)
            return;

        if constexpr (bNearest)
        {
            // An exact tie keeps the hit found first, which is what the immediate-write version
            // did through its own `r < R.range`.
            if (any_hit && !(r < best_range))
                return;
            best_prim = int(prim);
            best_range = r;
            best_u = u;
            best_v = v;
            rRange = r;
            rRange2 = r * r;
            any_hit = true;
        }
        else
        {
            _store(prim, r, u, v);
            any_hit = true;
        }
    }

    // Writes out whatever a nearest query decided on. Nothing to do for the other modes, which
    // append as they go.
    void _flush()
    {
        if constexpr (bNearest)
        {
            if (best_prim >= 0)
                _store(u32(best_prim), best_range, best_u, best_v);
        }
    }

    void _stab(const AABBNoLeafNode* node)
    {
        // Actual ray/aabb test
        if constexpr (bUseSSE)
        {
            // use SSE
            float d;
            if (!_box_sse(node->mAABB, d))
                return;
            if (d > rRange)
                return;
        }
        else
        {
            // use FPU
            Fvector P;
            if (!_box_fpu((Fvector&)node->mAABB.mCenter, (Fvector&)node->mAABB.mExtents, P))
                return;
            if (P.distance_to_sqr(ray.pos) > rRange2)
                return;
        }

        // Both children are about to be needed, and BVH nodes are re-read by every ray that
        // passes through them - T0 keeps them, NTA (what this was) asks the cache to throw
        // away the hottest data in the structure.
        _mm_prefetch((const char*)node->GetPos(), _MM_HINT_T0);
        _mm_prefetch((const char*)node->GetNeg(), _MM_HINT_T0);

        // 1st chield
        if (node->HasLeaf())
            _prim(node->GetPrimitive());
        else
            _stab(node->GetPos());

        // Early exit for "only first"
        if constexpr (bFirst)
        {
            if (any_hit)
                return;
        }

        // 2nd chield
        if (node->HasLeaf2())
            _prim(node->GetPrimitive2());
        else
            _stab(node->GetNeg());
    }
};

template <bool bUseSSE, bool bCull, bool bFirst, bool bNearest>
ICF void ray_run(COLLIDER* dest, Fvector* V, TRI* T, const AABBNoLeafNode* N, const Fvector& start,
    const Fvector& dir, float range)
{
    ray_collider<bUseSSE, bCull, bFirst, bNearest> RC;
    RC._init(dest, V, T, start, dir, range);
    RC._stab(N);
    RC._flush();
}

// One switch instead of a four-level if tree: same instantiations, same codegen, and the mode
// bits are read once.
template <bool bUseSSE>
ICF void ray_dispatch(u32 mode, COLLIDER* dest, Fvector* V, TRI* T, const AABBNoLeafNode* N, const Fvector& start,
    const Fvector& dir, float range)
{
    const u32 sel = ((mode & OPT_CULL) ? 1u : 0u) | ((mode & OPT_ONLYFIRST) ? 2u : 0u) |
        ((mode & OPT_ONLYNEAREST) ? 4u : 0u);
    switch (sel)
    {
    case 0: ray_run<bUseSSE, false, false, false>(dest, V, T, N, start, dir, range); break;
    case 1: ray_run<bUseSSE, true, false, false>(dest, V, T, N, start, dir, range); break;
    case 2: ray_run<bUseSSE, false, true, false>(dest, V, T, N, start, dir, range); break;
    case 3: ray_run<bUseSSE, true, true, false>(dest, V, T, N, start, dir, range); break;
    case 4: ray_run<bUseSSE, false, false, true>(dest, V, T, N, start, dir, range); break;
    case 5: ray_run<bUseSSE, true, false, true>(dest, V, T, N, start, dir, range); break;
    case 6: ray_run<bUseSSE, false, true, true>(dest, V, T, N, start, dir, range); break;
    default: ray_run<bUseSSE, true, true, true>(dest, V, T, N, start, dir, range); break;
    }
}

void COLLIDER::ray_query(u32 ray_mode, const MODEL* m_def, const Fvector& r_start, const Fvector& r_dir, float r_range)
{
    ZoneScoped;
    m_def->syncronize();

    // Get nodes
    const AABBNoLeafTree* T = (const AABBNoLeafTree*)m_def->tree->GetTree();
    const AABBNoLeafNode* N = T->GetNodes();
    r_clear();

    if constexpr (cdb_sse_always)
        ray_dispatch<true>(ray_mode, this, m_def->verts, m_def->tris, N, r_start, r_dir, r_range);
    else if (CPU::HasSSE)
        ray_dispatch<true>(ray_mode, this, m_def->verts, m_def->tris, N, r_start, r_dir, r_range);
    else
        ray_dispatch<false>(ray_mode, this, m_def->verts, m_def->tris, N, r_start, r_dir, r_range);
}
} // namespace CDB
