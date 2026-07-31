// HOM.cpp: implementation of the CHOM class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "xrCore/Threading/ParallelFor.hpp"

#include "HOM.h"
#include "occRasterizer.h"
#include "xrEngine/GameFont.h"
#include "xrEngine/PerformanceAlert.hpp"

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64)
#include <xmmintrin.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
IC u32 hom_skip_interval(const u32 triangleId, const u32 frame)
{
    u32 value = triangleId * 0x9e3779b9u ^ frame;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return 3u + value % 7u;
}
} // namespace

float psOSSR = .001f;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CHOM::CHOM() : xrc("HOM")
{
    bEnabled = FALSE;
    m_pModel = nullptr;
    m_pTris = nullptr;
#ifdef DEBUG
    Device.seqRender.Add(this, REG_PRIORITY_LOW - 1000);
#endif
}

CHOM::~CHOM()
{
#ifdef DEBUG
    Device.seqRender.Remove(this);
#endif
}

#pragma pack(push, 4)
struct HOM_poly
{
    Fvector v1, v2, v3;
    u32 flags;
};
#pragma pack(pop)

IC float Area(Fvector& v0, Fvector& v1, Fvector& v2)
{
    const float e1 = v0.distance_to(v1);
    const float e2 = v0.distance_to(v2);
    const float e3 = v1.distance_to(v2);

    const float p = (e1 + e2 + e3) / 2.f;
    return _sqrt(p * (p - e1) * (p - e2) * (p - e3));
}

void CHOM::Load()
{
    if (strstr(Core.Params, "-no_hom") )
        return;

    ZoneScoped;

    // Find and open file
    string_path fName;
    FS.update_path(fName, "$level$", "level.hom");
    if (!FS.exist(fName))
    {
        Msg(" WARNING: Occlusion map '%s' not found.", fName);
        return;
    }
    Msg("* Loading HOM: %s", fName);

    IReader* fs = FS.r_open(fName);

    // Prepare AABB-tree
    static const bool use_cache = !strstr(Core.Params, "-no_cdb_cache");

    m_pModel = xr_new<CDB::MODEL>();
    if (use_cache)
        m_pModel->set_model_crc32(crc32(fs->pointer(), fs->length()));

    // Load tris and merge them
    CDB::Collector CL;
    {
        IReader* S = fs->open_chunk(1);
        const auto begin = static_cast<HOM_poly*>(S->pointer());
        const auto end   = static_cast<HOM_poly*>(S->end());
        for (HOM_poly* poly = begin; poly != end; ++poly)
        {
            CL.add_face_packed_D(poly->v1, poly->v2, poly->v3, poly->flags, 0.01f);
        }
        S->close();
    }

    // Determine adjacency
    xr_vector<u32> adjacency;
    CL.calc_adjacency(adjacency);

    // Create RASTER-triangles
    m_pTris = xr_alloc<occTri>(CL.getTS());

    xr_parallel_for(TaskRange<size_t>(0, CL.getTS()), [&](const TaskRange<size_t>& range)
    {
        ZoneScopedN("Process triangles");
        for (size_t it = range.begin(); it != range.end(); ++it)
        {
            const CDB::TRI& clT = CL.getT()[it];
            occTri& rT = m_pTris[it];
            Fvector& v0 = CL.getV()[clT.verts[0]];
            Fvector& v1 = CL.getV()[clT.verts[1]];
            Fvector& v2 = CL.getV()[clT.verts[2]];
            rT.adjacent[0] = (0xffffffff == adjacency[3 * it + 0]) ? ((occTri*)(-1)) : (m_pTris + adjacency[3 * it + 0]);
            rT.adjacent[1] = (0xffffffff == adjacency[3 * it + 1]) ? ((occTri*)(-1)) : (m_pTris + adjacency[3 * it + 1]);
            rT.adjacent[2] = (0xffffffff == adjacency[3 * it + 2]) ? ((occTri*)(-1)) : (m_pTris + adjacency[3 * it + 2]);
            rT.flags = clT.dummy;
            rT.area = Area(v0, v1, v2);

            if (rT.area < EPS_L)
                Msg("! Invalid HOM triangle (%f,%f,%f)-(%f,%f,%f)-(%f,%f,%f)", VPUSH(v0), VPUSH(v1), VPUSH(v2));

            rT.plane.build(v0, v1, v2);
            rT.skip = 0;
            rT.center.add(v0, v1).add(v2).div(3.f);
        }
    });

    // Create AABB-tree
    static const bool skip_crc32_check = strstr(Core.Params, "-skip_cdb_cache_crc32_check");

    strconcat(fName, "cdb_cache" DELIMITER, FS.get_path("$level$")->m_Add, "hom.bin");
    FS.update_path(fName, "$app_data_root$", fName);

    if (use_cache && FS.exist(fName) && m_pModel->deserialize(fName, skip_crc32_check))
    {
#ifndef MASTER_GOLD
        Msg("* Loaded HOM cache (%s)...", fName);
#endif
    }
    else
    {
#ifndef MASTER_GOLD
        Msg("* HOM cache for '%s' was not loaded. Building the model from scratch..", fName);
#endif
        m_pModel->build(CL.getV(), CL.getVS(), CL.getT(), CL.getTS());

        if (use_cache)
            m_pModel->serialize(fName);
    }

    bEnabled = TRUE;
    FS.r_close(fs);
}

void CHOM::Unload()
{
    ZoneScoped;
    xr_delete(m_pModel);
    xr_free(m_pTris);
    bEnabled = FALSE;
}

void CHOM::Render_DB(CFrustum& base)
{
    ZoneScoped;

#if defined(USE_DX11)
    static const Fmatrix m_viewport = {occ_dim_0 / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, occ_dim_0 / 2.f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, occ_dim_0 / 2.f, occ_dim_0 / 2.f, 0.0f, 1.0f};
    static const Fmatrix m_viewport_01 = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.5f, 0.5f, 0.0f, 1.0f};
#elif defined(USE_OGL)
    static const Fmatrix m_viewport = {occ_dim_0 / 2.f, 0.0f, 0.0f, 0.0f, 0.0f, -occ_dim_0 / 2.f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, occ_dim_0 / 2.f, occ_dim_0 / 2.f, 0.0f, 1.0f};
    static const Fmatrix m_viewport_01 = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.5f, 0.5f, 0.0f, 1.0f};
#else
#   error No graphics API selected or enabled!
#endif
    m_xform.mul(m_viewport, Device.mFullTransform);
    m_xform_01.mul(m_viewport_01, Device.mFullTransform);

    // Query DB
    xrc.frustum_query(0, m_pModel, base);
    if (0 == xrc.r_count())
        return;

    const Fvector COP = Device.vCameraPosition;
    m_sortedTriangles.clear();
    m_sortedTriangles.reserve(xrc.r_count());
    for (const CDB::RESULT& result : *xrc.r_get())
    {
        const occTri& triangle = m_pTris[result.id];
        if (triangle.skip <= Device.dwFrame)
        {
            const float deltaX = COP.x - triangle.center.x;
            const float deltaY = COP.y - triangle.center.y;
            const float deltaZ = COP.z - triangle.center.z;
            m_sortedTriangles.emplace_back(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ, result.id);
        }
    }
    if (m_sortedTriangles.size() > 1)
        std::sort(m_sortedTriangles.begin(), m_sortedTriangles.end(), [](const auto& left, const auto& right)
    {
        return left.first < right.first;
    });

    // Build frustum with near plane only
    CFrustum clip;
    clip.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_NEAR);
    sPoly src, dst;
    u32 _frame = Device.dwFrame;
    stats.FrustumTriangleCount = static_cast<u32>(m_sortedTriangles.size());
    stats.VisibleTriangleCount = 0;
    const CDB::TRI* modelTriangles = m_pModel->get_tris();
    const Fvector* modelVertices = m_pModel->get_verts();

    // Perfrom selection, sorting, culling
    for (const auto& triangle : m_sortedTriangles)
    {
        // Control skipping
        occTri& T = m_pTris[triangle.second];

        // Test for good occluder - should be improved :)
        if (!(T.flags || (T.plane.classify(COP) > 0)))
        {
            T.skip = _frame + hom_skip_interval(triangle.second, _frame);
            continue;
        }

        // Access to triangle vertices
        const CDB::TRI& t = modelTriangles[triangle.second];
        src.clear();
        dst.clear();
        src.push_back(modelVertices[t.verts[0]]);
        src.push_back(modelVertices[t.verts[1]]);
        src.push_back(modelVertices[t.verts[2]]);
        sPoly* P = clip.ClipPoly(src, dst);
        if (!P)
        {
            T.skip = _frame + hom_skip_interval(triangle.second, _frame);
            continue;
        }

        // XForm and Rasterize
        stats.VisibleTriangleCount++;
        u32 pixels = 0;
        int limit = int(P->size()) - 1;
        m_xform.transform(T.raster[0], (*P)[0]);
        for (int v2 = 1; v2 < limit; v2++)
        {
            m_xform.transform(T.raster[1], (*P)[v2 + 0]);
            m_xform.transform(T.raster[2], (*P)[v2 + 1]);
            pixels += Raster.rasterize(&T);
        }
        if (0 == pixels)
        {
            T.skip = _frame + hom_skip_interval(triangle.second, _frame);
            continue;
        }
    }
}

void CHOM::Render(CFrustum& base)
{
    if (!bEnabled)
        return;

    ZoneScoped;
    stats.Total.Begin();
    Raster.clear();
    Render_DB(base);
    Raster.propagade();
    stats.Total.End();
}

Task& CHOM::DispatchMTRender()
{
    return TaskManager::AddTask([this]
    {
        ZoneScoped;
        CFrustum ViewBase;
        ViewBase.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
        Enable();
        Render(ViewBase);
    });
}

ICF BOOL xform_b0(Fvector2& min, Fvector2& max, float& minz, const Fmatrix& X, float _x, float _y, float _z)
{
    const float z = _x * X._13 + _y * X._23 + _z * X._33 + X._43;
    if (z < EPS)
        return TRUE;
    const float iw = 1.f / (_x * X._14 + _y * X._24 + _z * X._34 + X._44);
    min.x = max.x = (_x * X._11 + _y * X._21 + _z * X._31 + X._41) * iw;
    min.y = max.y = (_x * X._12 + _y * X._22 + _z * X._32 + X._42) * iw;
    minz = 0.f + z * iw;
    return FALSE;
}

ICF BOOL xform_b1(Fvector2& min, Fvector2& max, float& minz, const Fmatrix& X, float _x, float _y, float _z)
{
    const float z = _x * X._13 + _y * X._23 + _z * X._33 + X._43;
    if (z < EPS)
        return TRUE;
    const float iw = 1.f / (_x * X._14 + _y * X._24 + _z * X._34 + X._44);
    float t = (_x * X._11 + _y * X._21 + _z * X._31 + X._41) * iw;
    if (t < min.x)
        min.x = t;
    else if (t > max.x)
        max.x = t;
    t = (_x * X._12 + _y * X._22 + _z * X._32 + X._42) * iw;
    if (t < min.y)
        min.y = t;
    else if (t > max.y)
        max.y = t;
    t = 0.f + z * iw;
    if (t < minz)
        minz = t;
    return FALSE;
}

IC BOOL _visible(const Fbox& B, const Fmatrix& m_xform_01)
{
#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64)
    const __m128 x = _mm_set_ps(B.vMax.x, B.vMax.x, B.vMin.x, B.vMin.x);
    const __m128 z = _mm_set_ps(B.vMin.z, B.vMax.z, B.vMax.z, B.vMin.z);
    const __m128 yMin = _mm_set1_ps(B.vMin.y);
    const __m128 yMax = _mm_set1_ps(B.vMax.y);
    const auto transformComponent = [x, z](const __m128 y, const float cx, const float cy, const float cz, const float cw)
    {
        return _mm_add_ps(_mm_add_ps(_mm_add_ps(
            _mm_mul_ps(x, _mm_set1_ps(cx)),
            _mm_mul_ps(y, _mm_set1_ps(cy))),
            _mm_mul_ps(z, _mm_set1_ps(cz))),
            _mm_set1_ps(cw));
    };

    const __m128 clipZMin =
        transformComponent(yMin, m_xform_01._13, m_xform_01._23, m_xform_01._33, m_xform_01._43);
    const __m128 clipZMax =
        transformComponent(yMax, m_xform_01._13, m_xform_01._23, m_xform_01._33, m_xform_01._43);
    const __m128 epsilon = _mm_set1_ps(EPS);
    if (_mm_movemask_ps(_mm_cmplt_ps(clipZMin, epsilon)) ||
        _mm_movemask_ps(_mm_cmplt_ps(clipZMax, epsilon)))
    {
        return TRUE;
    }

    const __m128 one = _mm_set1_ps(1.f);
    const __m128 inverseWMin = _mm_div_ps(one,
        transformComponent(yMin, m_xform_01._14, m_xform_01._24, m_xform_01._34, m_xform_01._44));
    const __m128 inverseWMax = _mm_div_ps(one,
        transformComponent(yMax, m_xform_01._14, m_xform_01._24, m_xform_01._34, m_xform_01._44));

    alignas(16) float transformedX[8];
    alignas(16) float transformedY[8];
    alignas(16) float transformedZ[8];
    _mm_store_ps(transformedX, _mm_mul_ps(
        transformComponent(yMin, m_xform_01._11, m_xform_01._21, m_xform_01._31, m_xform_01._41),
        inverseWMin));
    _mm_store_ps(transformedX + 4, _mm_mul_ps(
        transformComponent(yMax, m_xform_01._11, m_xform_01._21, m_xform_01._31, m_xform_01._41),
        inverseWMax));
    _mm_store_ps(transformedY, _mm_mul_ps(
        transformComponent(yMin, m_xform_01._12, m_xform_01._22, m_xform_01._32, m_xform_01._42),
        inverseWMin));
    _mm_store_ps(transformedY + 4, _mm_mul_ps(
        transformComponent(yMax, m_xform_01._12, m_xform_01._22, m_xform_01._32, m_xform_01._42),
        inverseWMax));
    _mm_store_ps(transformedZ, _mm_mul_ps(clipZMin, inverseWMin));
    _mm_store_ps(transformedZ + 4, _mm_mul_ps(clipZMax, inverseWMax));

    Fvector2 min;
    Fvector2 max;
    min.x = max.x = transformedX[0];
    min.y = max.y = transformedY[0];
    float minz = transformedZ[0];
    for (int index = 1; index != 8; ++index)
    {
        const float xValue = transformedX[index];
        if (xValue < min.x)
            min.x = xValue;
        else if (xValue > max.x)
            max.x = xValue;

        const float yValue = transformedY[index];
        if (yValue < min.y)
            min.y = yValue;
        else if (yValue > max.y)
            max.y = yValue;

        if (transformedZ[index] < minz)
            minz = transformedZ[index];
    }
    return Raster.test(min.x, min.y, max.x, max.y, minz);
#else
    // Find min/max points of xformed-box
    Fvector2 min, max;
    float z;
    if (xform_b0(min, max, z, m_xform_01, B.vMin.x, B.vMin.y, B.vMin.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMin.x, B.vMin.y, B.vMax.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMax.x, B.vMin.y, B.vMax.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMax.x, B.vMin.y, B.vMin.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMin.x, B.vMax.y, B.vMin.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMin.x, B.vMax.y, B.vMax.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMax.x, B.vMax.y, B.vMax.z))
        return TRUE;
    if (xform_b1(min, max, z, m_xform_01, B.vMax.x, B.vMax.y, B.vMin.z))
        return TRUE;
    return Raster.test(min.x, min.y, max.x, max.y, z);
#endif
}

BOOL CHOM::visible(const Fbox3& B) const
{
    if (!bEnabled)
        return TRUE;
    if (B.contains(Device.vCameraPosition))
        return TRUE;
    return _visible(B, m_xform_01);
}

BOOL CHOM::visible(const Fbox2& B, float depth) const
{
    if (!bEnabled)
        return TRUE;
    return Raster.test(B.min.x, B.min.y, B.max.x, B.max.y, depth);
}

BOOL CHOM::visible(vis_data& vis) const
{
    if (Device.dwFrame < vis.hom_frame)
        return TRUE; // not at this time :)
    if (!bEnabled)
        return TRUE; // return - everything visible
    if (!vis.box.is_valid() || !_valid(vis.box))
        return TRUE; // invalid box

    ScopeStatTimer scopeStats(stats.Total, stats.TotalTimerLock);

    // Now, the test time comes
    // 0. The object was hidden, and we must prove that each frame	- test		| frame-old, tested-new, hom_res =
    // false;
    // 1. The object was visible, but we must to re-check it		- test		| frame-new, tested-???, hom_res = true;
    // 2. New object slides into view								- delay test| frame-old, tested-old, hom_res = ???;
    const u32 frame_current = Device.dwFrame;
    // u32	frame_prev		= frame_current-1;

    const BOOL result = _visible(vis.box, m_xform_01);
    u32 delay = 1;
    if (result)
    {
        // visible	- delay next test
        delay = ::Random.randI(5 * 2, 5 * 5);
    }
    else
    {
        // hidden	- shedule to next frame
    }
    vis.hom_frame = frame_current + delay;
    vis.hom_tested = frame_current;
    return result;
}

BOOL CHOM::visible(const sPoly& P) const
{
    if (!bEnabled)
        return TRUE;

    // Find min/max points of xformed-box
    Fvector2 min, max;
    float z;

    if (xform_b0(min, max, z, m_xform_01, P.front().x, P.front().y, P.front().z))
        return TRUE;
    for (u32 it = 1; it < P.size(); it++)
        if (xform_b1(min, max, z, m_xform_01, P[it].x, P[it].y, P[it].z))
            return TRUE;
    return Raster.test(min.x, min.y, max.x, max.y, z);
}

void CHOM::Disable() { bEnabled = FALSE; }
void CHOM::Enable() { bEnabled = m_pModel ? TRUE : FALSE; }
void CHOM::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    stats.FrameEnd();
    font.OutNext("HOM:          %2.2fms, %u", stats.Total.result, stats.Total.count);
    font.OutNext("- visible:    %u", stats.VisibleTriangleCount);
    font.OutNext("- frustum:    %u", stats.FrustumTriangleCount);
    font.OutNext("- total:      %d", m_pModel ? m_pModel->get_tris_count() : 0);
    stats.FrameStart();
    xrc.DumpStatistics(font, alert);
}

#ifdef DEBUG
void CHOM::OnRender()
{
    Raster.on_dbg_render();

    if (psDeviceFlags.is(rsOcclusionDraw))
    {
        if (m_pModel)
        {
            DEFINE_VECTOR(FVF::L, LVec, LVecIt);
            static LVec poly;
            poly.resize(m_pModel->get_tris_count() * 3);
            static LVec line;
            line.resize(m_pModel->get_tris_count() * 6);
            for (int it = 0; it < m_pModel->get_tris_count(); it++)
            {
                CDB::TRI* T = m_pModel->get_tris() + it;
                Fvector* verts = m_pModel->get_verts();
                poly[it * 3 + 0].set(*(verts + T->verts[0]), 0x80FFFFFF);
                poly[it * 3 + 1].set(*(verts + T->verts[1]), 0x80FFFFFF);
                poly[it * 3 + 2].set(*(verts + T->verts[2]), 0x80FFFFFF);
                line[it * 6 + 0].set(*(verts + T->verts[0]), 0xFFFFFFFF);
                line[it * 6 + 1].set(*(verts + T->verts[1]), 0xFFFFFFFF);
                line[it * 6 + 2].set(*(verts + T->verts[1]), 0xFFFFFFFF);
                line[it * 6 + 3].set(*(verts + T->verts[2]), 0xFFFFFFFF);
                line[it * 6 + 4].set(*(verts + T->verts[2]), 0xFFFFFFFF);
                line[it * 6 + 5].set(*(verts + T->verts[0]), 0xFFFFFFFF);
            }
            RCache.set_xform_world(Fidentity);
            // draw solid
            Device.SetNearer(TRUE);
            RCache.set_Shader(RImplementation.m_SelectionShader);
#ifndef USE_DX9 // when we don't have FFP support
            RCache.set_c("tfactor", float(color_get_R(0x80FFFFFF)) / 255.f, float(color_get_G(0x80FFFFFF)) / 255.f, \
                float(color_get_B(0x80FFFFFF)) / 255.f, float(color_get_A(0x80FFFFFF)) / 255.f);
#endif
            RCache.dbg_Draw(D3DPT_TRIANGLELIST, &*poly.begin(), poly.size() / 3);
            Device.SetNearer(FALSE);
            // draw wire
            if (bDebug)
            {
                RImplementation.rmNear(RCache);
            }
            else
            {
                Device.SetNearer(TRUE);
            }
            RCache.set_Shader(RImplementation.m_SelectionShader);
#ifndef USE_DX9 // when we don't have FFP support
            RCache.set_c("tfactor", 1.f, 1.f, 1.f, 1.f);
#endif
            RCache.dbg_Draw(D3DPT_LINELIST, &*line.begin(), line.size() / 2);
            if (bDebug)
            {
                RImplementation.rmNormal(RCache);
            }
            else
            {
                Device.SetNearer(FALSE);
            }
        }
    }
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
