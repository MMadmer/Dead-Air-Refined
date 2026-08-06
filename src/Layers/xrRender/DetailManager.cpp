// DetailManager.cpp: implementation of the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "DetailManager.h"
#include "xrCDB/Intersect.hpp"

#ifdef _EDITOR
#include "ESceneClassList.h"
#include "Scene.h"
#include "SceneObject.h"
#include "IGame_Persistent.h"
#include "Environment.h"
#else
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

#include "xrCore/Threading/TaskManager.hpp"

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K) || defined(XR_ARCHITECTURE_PPC64)
#include <xmmintrin.h>
#elif defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64)
#include "sse2neon/sse2neon.h"
#elif defined(XR_ARCHITECTURE_RISCV)
#include "sse2rvv/sse2rvv.h"
#else
#error Add your platform here
#endif
#endif

namespace xray::render::RENDER_NAMESPACE
{
const float dbgOffset = 0.f;
const int dbgItems = 128;

//--------------------------------------------------- Decompression
static int magic4x4[4][4] = {{0, 14, 3, 13}, {11, 5, 8, 6}, {12, 2, 15, 1}, {7, 9, 4, 10}};

void bwdithermap(int levels, int magic[16][16])
{
    /* Get size of each step */
    float N = 255.0f / (levels - 1);

    /*
     * Expand 4x4 dither pattern to 16x16.  4x4 leaves obvious patterning,
     * and doesn't give us full intensity range (only 17 sublevels).
     *
     * magicfact is (N - 1)/16 so that we get numbers in the matrix from 0 to
     * N - 1: mod N gives numbers in 0 to N - 1, don't ever want all
     * pixels incremented to the next level (this is reserved for the
     * pixel value with mod N == 0 at the next level).
     */

    float magicfact = (N - 1) / 16;
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            for (int k = 0; k < 4; k++)
            {
                for (int l = 0; l < 4; l++)
                {
                    magic[4 * k + i][4 * l + j] =
                        (int)(0.5 + magic4x4[i][j] * magicfact + (magic4x4[k][l] / 16.) * magicfact);
                }
            }
        }
    }
}
//--------------------------------------------------- Decompression

void CDetailManager::SSwingValue::lerp(const SSwingValue& A, const SSwingValue& B, float f)
{
    float fi = 1.f - f;
    amp1 = fi * A.amp1 + f * B.amp1;
    amp2 = fi * A.amp2 + f * B.amp2;
    rot1 = fi * A.rot1 + f * B.rot1;
    rot2 = fi * A.rot2 + f * B.rot2;
    speed = fi * A.speed + f * B.speed;
}
//---------------------------------------------------

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
// XXX stats: add to statistics
CDetailManager::CDetailManager() : xrc("detail manager")
{
    ZoneScoped;

    dtFS = nullptr;
    dtSlots = nullptr;
    soft_Geom = nullptr;
    hw_Geom = nullptr;
    hw_BatchSize = 0;
    m_time_rot_1 = 0;
    m_time_rot_2 = 0;
    m_time_pos = 0;
    m_global_time_old = 0;

    // KD: variable detail radius
    dm_size = dm_current_size;
    dm_cache_line = dm_current_cache_line;
    dm_cache1_line = dm_current_cache1_line;
    dm_cache_size = dm_current_cache_size;
    dm_fade = dm_current_fade;
    ps_r__Detail_density = ps_current_detail_density;
    ps_current_detail_height = ps_r__Detail_height;
    cache_task.reserve(dm_cache_size);
    cache_level1 = (CacheSlot1**)xr_malloc(dm_cache1_line * sizeof(CacheSlot1*));
    for (u32 i = 0; i < dm_cache1_line; ++i)
    {
        cache_level1[i] = (CacheSlot1*)xr_malloc(dm_cache1_line * sizeof(CacheSlot1));
        for (u32 j = 0; j < dm_cache1_line; ++j)
            new(&cache_level1[i][j]) CacheSlot1();
    }
    cache = (Slot***)xr_malloc(dm_cache_line * sizeof(Slot**));
    for (u32 i = 0; i < dm_cache_line; ++i)
        cache[i] = (Slot**)xr_malloc(dm_cache_line * sizeof(Slot*));

    cache_pool = (Slot *)xr_malloc(dm_cache_size * sizeof(Slot));

    for (u32 i = 0; i < dm_cache_size; ++i)
        new(&cache_pool[i]) Slot();
    /*
    CacheSlot1 cache_level1[dm_cache1_line][dm_cache1_line];
    Slot* cache [dm_cache_line][dm_cache_line]; // grid-cache itself
    Slot cache_pool [dm_cache_size]; // just memory for slots
    */
}

CDetailManager::~CDetailManager()
{
    ZoneScoped;

    WaitForCalc();

    for (u32 i = 0; i < dm_cache_size; ++i)
        cache_pool[i].~Slot();
    xr_free(cache_pool);

    for (u32 i = 0; i < dm_cache_line; ++i)
        xr_free(cache[i]);
    xr_free(cache);

    for (u32 i = 0; i < dm_cache1_line; ++i)
    {
        for (u32 j = 0; j < dm_cache1_line; ++j)
            cache_level1[i][j].~CacheSlot1();
        xr_free(cache_level1[i]);
    }
    xr_free(cache_level1);
}

#ifndef _EDITOR

/*
void dump(CDetailManager::vis_list& lst)
{
    for (int i = 0; i<lst.size(); i++)
    {
        Msg("%8x / %8x / %8x",  lst[i]._M_start, lst[i]._M_finish, lst[i]._M_end_of_storage._M_data);
    }
}
*/
void CDetailManager::Load()
{
    ZoneScoped;

    // Open file stream
    if (!FS.exist("$level$", "level.details"))
    {
        dtFS = nullptr;
        return;
    }

    string_path fn;
    FS.update_path(fn, "$level$", "level.details");
    dtFS = FS.r_open(fn);

    // Header
    dtFS->r_chunk_safe(0, &dtH, sizeof(dtH));
    R_ASSERT(dtH.version() == DETAIL_VERSION);
    u32 m_count = dtH.object_count();
    objects.reserve(m_count);

    // Models
    IReader* m_fs = dtFS->open_chunk(1);
    for (u32 m_id = 0; m_id < m_count; m_id++)
    {
        CDetail* dt = xr_new<CDetail>();
        IReader* S = m_fs->open_chunk(m_id);
        dt->Load(S);
        objects.push_back(dt);
        m_objectRadiusSquared.push_back(dt->bv_sphere.R * dt->bv_sphere.R);
        S->close();
    }
    m_fs->close();

    // Get pointer to database (slots)
    IReader* m_slots = dtFS->open_chunk(2);
    dtSlots = (DetailSlot*)m_slots->pointer();
    m_slots->close();

    // Initialize 'vis' and 'cache'
    for (u32 i = 0; i < 3; ++i)
        m_visibles[i].resize(objects.size());
    cache_Initialize();

    // Make dither matrix
    bwdithermap(2, dither);

    // Hardware specific optimizations
    if (UseVS())
    {
        hw_Load();
        u32 vertexOffset = 0;
        u32 indexOffset = 0;
        for (const CDetail* object : objects)
        {
            m_objectVertexOffsets.push_back(vertexOffset);
            m_objectIndexOffsets.push_back(indexOffset);
            vertexOffset += hw_BatchSize * object->number_vertices;
            indexOffset += hw_BatchSize * object->number_indices;
        }
    }
    else
        soft_Load();

    // swing desc
    // normal
    swing_desc[0].amp1 = pSettings->r_float("details", "swing_normal_amp1");
    swing_desc[0].amp2 = pSettings->r_float("details", "swing_normal_amp2");
    swing_desc[0].rot1 = pSettings->r_float("details", "swing_normal_rot1");
    swing_desc[0].rot2 = pSettings->r_float("details", "swing_normal_rot2");
    swing_desc[0].speed = pSettings->r_float("details", "swing_normal_speed");
    // fast
    swing_desc[1].amp1 = pSettings->r_float("details", "swing_fast_amp1");
    swing_desc[1].amp2 = pSettings->r_float("details", "swing_fast_amp2");
    swing_desc[1].rot1 = pSettings->r_float("details", "swing_fast_rot1");
    swing_desc[1].rot2 = pSettings->r_float("details", "swing_fast_rot2");
    swing_desc[1].speed = pSettings->r_float("details", "swing_fast_speed");
}
#endif
void CDetailManager::Unload()
{
    ZoneScoped;
    WaitForCalc();

    if (UseVS())
        hw_Unload();
    else
        soft_Unload();

    for (CDetail* detailObject : objects)
    {
        detailObject->Unload();
        xr_delete(detailObject);
    }

    objects.clear();
    m_objectRadiusSquared.clear();
    m_objectVertexOffsets.clear();
    m_objectIndexOffsets.clear();
    for (int visibleGroup = 0; visibleGroup != 3; ++visibleGroup)
    {
        m_visibles[visibleGroup].clear();
        m_visibleObjectIds[visibleGroup].clear();
    }
    FS.r_close(dtFS);
}

extern ECORE_API float r_ssaDISCARD;

void CDetailManager::UpdateVisibleM()
{
    ZoneScoped;

    for (int visibleGroup = 0; visibleGroup != 3; ++visibleGroup)
    {
        for (const u8 objectId : m_visibleObjectIds[visibleGroup])
            m_visibles[visibleGroup][objectId].clear();
        m_visibleObjectIds[visibleGroup].clear();
    }

    CFrustum View;
    View.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);

    float fade_limit = dm_fade;
    fade_limit = fade_limit * fade_limit;
    float fade_start = 1.f;
    fade_start = fade_start * fade_start;
    float fade_range = fade_limit - fade_start;
    float r_ssaCHEAP = 16 * r_ssaDISCARD;
    constexpr u32 detailSlotsPerCache = dm_cache1_count * dm_cache1_count;

    // Initialize 'vis' and 'cache'
    // Collect objects for rendering
    RImplementation.BasicStats.DetailVisibility.Begin();
    for (u32 _mz = 0; _mz < dm_cache1_line; _mz++)
    {
        for (u32 _mx = 0; _mx < dm_cache1_line; _mx++)
        {
            CacheSlot1& MS = cache_level1[_mz][_mx];
            if (MS.empty)
            {
                continue;
            }
            u32 mask = View.getMask();
            u32 res = View.testSAABB(MS.vis.sphere.P, MS.vis.sphere.R, MS.vis.box.data(), mask);
            if (fcvNone == res)
            {
                continue; // invisible-view frustum
            }
            // test slots

            for (u32 _i = 0; _i < detailSlotsPerCache; _i++)
            {
                Slot* PS = *MS.slots[_i];
                Slot& S = *PS;

                //if (_i+1<dwCC);
                //    _mm_prefetch((char*)*MS.slots[_i+1], _MM_HINT_T1);

                // if slot empty - continue
                if (S.empty)
                {
                    continue;
                }

                // if upper test = fcvPartial - test inner slots
                if (fcvPartial == res)
                {
                    u32 _mask = mask;
                    u32 _res = View.testSAABB(S.vis.sphere.P, S.vis.sphere.R, S.vis.box.data(), _mask);
                    if (fcvNone == _res)
                    {
                        continue; // invisible-view frustum
                    }
                }
                const bool needsUpdate = Device.dwFrame > S.frame;
                float dist_sq = 0.0f;
                if (needsUpdate)
                {
                    const float deltaX = EYE.x - S.vis.sphere.P.x;
                    const float deltaY = EYE.y - S.vis.sphere.P.y;
                    const float deltaZ = EYE.z - S.vis.sphere.P.z;
                    dist_sq = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
                    if (dist_sq > fade_limit)
                        continue;
                }
#ifndef _EDITOR
                if (!RImplementation.HOM.visible(S.vis))
                {
                    continue; // invisible-occlusion
                }
#endif
                // Add to visibility structures
                if (needsUpdate)
                {
                    // Calc fade factor (per slot)
                    float alpha = (dist_sq < fade_start) ? 0.f : (dist_sq - fade_start) / fade_range;
                    float alpha_i = 1.f - alpha;
                    float dist_sq_rcp = 1.f / dist_sq;

                    S.frame = Device.dwFrame + Random.randI(15, 30);
                    for (int sp_id = 0; sp_id < dm_obj_in_slot; sp_id++)
                    {
                        SlotPart& sp = S.G[sp_id];
                        if (sp.id == DetailSlot::ID_Empty)
                            continue;

                        sp.r_items[0].clear();
                        sp.r_items[1].clear();
                        sp.r_items[2].clear();

                        const float Rq_drcp = m_objectRadiusSquared[sp.id] * dist_sq_rcp;

                        for (auto& siIT : sp.items)
                        {
                            SlotItem& Item = *siIT;
                            const float fade = ps_detail_scale_on_fade ? 1.f : alpha_i;
                            const float scale = Item.scale_calculated = Item.scale * fade;
                            const float ssa = scale * scale * Rq_drcp;
                            if (ssa < r_ssaDISCARD)
                            {
                                continue;
                            }
                            u32 vis_id = 0;
                            if (ssa > r_ssaCHEAP)
                                vis_id = Item.vis_ID;

                            sp.r_items[vis_id].push_back(siIT);

                            // 2 visible[vis_id][sp.id].push_back(&Item);
                        }
                    }
                }
                for (int sp_id = 0; sp_id < dm_obj_in_slot; sp_id++)
                {
                    SlotPart& sp = S.G[sp_id];
                    if (sp.id == DetailSlot::ID_Empty)
                        continue;
                    if (!sp.r_items[0].empty())
                    {
                        auto& visibleItems = m_visibles[0][sp.id];
                        if (visibleItems.empty())
                            m_visibleObjectIds[0].push_back(static_cast<u8>(sp.id));
                        visibleItems.push_back({&sp.r_items[0], &S.vis});
                    }
                    if (!sp.r_items[1].empty())
                    {
                        auto& visibleItems = m_visibles[1][sp.id];
                        if (visibleItems.empty())
                            m_visibleObjectIds[1].push_back(static_cast<u8>(sp.id));
                        visibleItems.push_back({&sp.r_items[1], &S.vis});
                    }
                    if (!sp.r_items[2].empty())
                    {
                        auto& visibleItems = m_visibles[2][sp.id];
                        if (visibleItems.empty())
                            m_visibleObjectIds[2].push_back(static_cast<u8>(sp.id));
                        visibleItems.push_back({&sp.r_items[2], &S.vis});
                    }
                }
            }
        }
    }
    for (auto& visibleObjectIds : m_visibleObjectIds)
        std::sort(visibleObjectIds.begin(), visibleObjectIds.end());
    RImplementation.BasicStats.DetailVisibility.End();
}

bool CDetailManager::UseVS() const
{
    return HW.Caps.geometry_major >= 1 && !RImplementation.o.ffp;
}

bool CDetailManager::HasRenderableDetails() const
{
#ifndef _EDITOR
    return dtFS && psDeviceFlags.is(rsDrawDetails);
#else
    return true;
#endif
}

bool CDetailManager::IsPartVisible(const VisiblePart& part, const CFrustum* frustum) const
{
    if (!frustum)
        return true;

    u32 mask = frustum->getMask();
    return frustum->testSAABB(part.bounds->sphere.P, part.bounds->sphere.R, part.bounds->box.data(), mask) != fcvNone;
}

void CDetailManager::UpdateRenderState()
{
    if (m_render_state_frame == Device.dwFrame)
        return;
    m_render_state_frame = Device.dwFrame;

#ifndef _EDITOR
    const float factor = g_pGamePersistent->Environment().wind_strength_factor;
#else
    constexpr float factor = 0.3f;
#endif
    swing_current.lerp(swing_desc[0], swing_desc[1], factor);

    float delta = Device.fTimeGlobal - m_global_time_old;
    if (delta < 0.f || delta > 1.f)
        delta = 0.03f;
    m_global_time_old = Device.fTimeGlobal;

    m_time_rot_1 += PI_MUL_2 * delta / swing_current.rot1;
    m_time_rot_2 += PI_MUL_2 * delta / swing_current.rot2;
    m_time_pos += delta * swing_current.speed;

    const auto& environment = g_pGamePersistent->Environment().CurrentEnv;
    m_wind_dir1.set(_sin(m_time_rot_1), 0.f, _cos(m_time_rot_1), 0.f)
        .normalize()
        .mul(0.1f + environment.wind_velocity * 0.0016f);
    m_wind_dir2.set(_sin(m_time_rot_2), 0.f, _cos(m_time_rot_2), 0.f)
        .normalize()
        .mul(0.05f + environment.wind_velocity * 0.0008f);
}

void CDetailManager::Render(CBackend& cmd_list, const bool collectStats, const CFrustum* frustum)
{
    if (!HasRenderableDetails())
        return;

    ZoneScoped;

#ifdef _EDITOR
    UpdateRenderState();
#endif
    WaitForCalc();

    if (collectStats)
    {
        RImplementation.BasicStats.DetailRender.Begin();
        RImplementation.BasicStats.DetailCount = 0;
    }

    cmd_list.set_CullMode(CULL_NONE);
    cmd_list.set_xform_world(Fidentity);
    const bool previousDetailRendering = cmd_list.detailRendering;
    cmd_list.detailRendering = true;
    if (UseVS())
        hw_Render(cmd_list, collectStats, frustum);
    else
        soft_Render(frustum);
    cmd_list.detailRendering = previousDetailRendering;
    cmd_list.set_CullMode(CULL_CCW);

    if (collectStats)
        RImplementation.BasicStats.DetailRender.End();
}

void CDetailManager::WaitForCalc()
{
    if (!m_calc_task)
        return;

    TaskScheduler->Wait(*m_calc_task);
    m_calc_task = nullptr;
    m_calc_running.store(false, std::memory_order_release);
}

void CDetailManager::DispatchMTCalc()
{
#ifndef _EDITOR
    if (!dtFS || !psDeviceFlags.is(rsDrawDetails))
        return;
#endif

    if (m_calc_scheduled_frame == Device.dwFrame || m_calc_running.exchange(true, std::memory_order_acq_rel))
        return;

    m_calc_scheduled_frame = Device.dwFrame;
    const Fvector eye = Device.vCameraPosition;

    m_calc_task = &TaskScheduler->AddTask([this, eye]
    {
        ZoneScoped;

        EYE = eye;

        const int s_x = iFloor(EYE.x / dm_slot_size + .5f);
        const int s_z = iFloor(EYE.z / dm_slot_size + .5f);

        RImplementation.BasicStats.DetailCache.Begin();
        cache_Update(s_x, s_z, EYE);
        RImplementation.BasicStats.DetailCache.End();

        UpdateVisibleM();
        m_calc_running.store(false, std::memory_order_release);
    });
}
} // namespace xray::render::RENDER_NAMESPACE
