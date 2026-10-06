// DetailManager.cpp: implementation of the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "DetailManager.h"
#include "xrCDB/Intersect.hpp"
#include "xrEngine/da_season.h"
#include "xrEngine/da_snow_field.h"
#include "da_grass_data.h"
#include "da_grass_blades.h"

#include <thread>

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
#if RENDER == R_R4 && !defined(_EDITOR)
#include "Layers/xrRender/da_temporal.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
const float dbgOffset = 0.f;
const int dbgItems = 128;

// A history reset (level load, teleport, back from the menu) starts the previous frame over.
static bool detail_history_reset()
{
#if RENDER == R_R4 && !defined(_EDITOR)
    return da_history_reset();
#else
    return true;
#endif
}

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

namespace
{
// The models' content keys (KSPR, docs/dead-air/SEASON_DATA.md): CDetail::Load's K1 folded with the mip-0
// DXT blocks of the level's BASE atlas ($level$\build_details.dds - season overlays are resolved per
// texture name, never here) that the model's uv rectangle touches, block rows top to bottom. Byte for byte
// the reference generator's rule, Python slicing and floor division included.
void detail_fold_sprites(CDetailManager::DetailVec& objects)
{
    if (!FS.exist("$level$", "build_details.dds"))
        return;
    string_path fn;
    FS.update_path(fn, "$level$", "build_details.dds");
    IReader* atlas = FS.r_open(fn);
    if (!atlas)
        return;
    const u8* bytes = static_cast<const u8*>(atlas->pointer());
    const s64 size = atlas->length();
    if (size >= 128)
    {
        u32 h = 0, w = 0;
        CopyMemory(&h, bytes + 12, 4);
        CopyMemory(&w, bytes + 16, 4);
        const bool dx10 = !memcmp(bytes + 84, "DX10", 4);
        const bool dxt1 = !memcmp(bytes + 84, "DXT1", 4);
        const s64 off = 128 + (dx10 ? 20 : 0);
        const s64 bs = dxt1 ? 8 : 16;
        const s64 bw = (s64(w) + 3) / 4;
        const s64 W = s64(w), H = s64(h);
        const auto floor4 = [](s64 a) { return a >= 0 ? a / 4 : -((-a + 3) / 4); };
        const auto clamp_index = [size](s64 i) {
            if (i < 0)
                i += size;
            return std::clamp<s64>(i, 0, size);
        };
        for (CDetail* object : objects)
        {
            const s64 x0 = std::max<s64>(0, s64(double(object->m_uv_min[0]) * double(W)));
            const s64 x1 = std::min<s64>(W - 1, s64(double(object->m_uv_max[0]) * double(W) - 1e-6));
            const s64 y0 = std::max<s64>(0, s64(double(object->m_uv_min[1]) * double(H)));
            const s64 y1 = std::min<s64>(H - 1, s64(double(object->m_uv_max[1]) * double(H) - 1e-6));
            for (s64 by = floor4(y0); by < floor4(y1) + 1; ++by)
            {
                const s64 row = off + (by * bw + floor4(x0)) * bs;
                const s64 start = clamp_index(row);
                const s64 stop = clamp_index(row + (floor4(x1) - floor4(x0) + 1) * bs);
                if (stop > start)
                    object->m_key = detail_key_fnv(bytes + start, size_t(stop - start), object->m_key);
            }
        }
    }
    FS.r_close(atlas);
}

// The class of every model of the level from its content key.
void detail_classify(CDetailManager::DetailVec& objects)
{
    detail_fold_sprites(objects);
    const DaSeasonData& data = da_season_data();
    u32 blades = 0, known = 0;
    for (CDetail* object : objects)
    {
        const auto it = data.detail_types.find(object->m_key);
        object->m_class = it != data.detail_types.end() ? it->second : dcUnknown;
        known += it != data.detail_types.end() ? 1 : 0;
        blades += da_grass_object_profile(*object) >= 0 ? 1 : 0;
    }
    Msg("* [details] %u models: %u known by content key, %u grown as blades", u32(objects.size()), known, blades);
}

// The per-item hash of the stock season (da_detail_common.h stock_u): drawn iff u < keep.
u32 detail_pcg(u32 v)
{
    v = v * 747796405u + 2891336453u;
    const u32 w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

float detail_stock_u(float x, float z)
{
    const s32 qx = s32(floorf(x * 64.f)), qz = s32(floorf(z * 64.f));
    return float(detail_pcg(u32(qx) ^ detail_pcg(u32(qz) + 0x9E3779B9u)) & 0xFFFFFFu) / 16777216.f;
}
} // namespace

u32 CDetailManager::SeasonClass(u32 object_id) const
{
    return object_id < objects.size() ? da_season_class(objects[object_id]->m_class) : 0u;
}

void CDetailManager::StockSeasonConstants(u32 object_id, Fvector4& cur, Fvector4& prev, Fvector4& obj) const
{
    const u32 season_class = SeasonClass(object_id);
    cur = m_stock_season[season_class];
    prev = m_stock_season_prev[season_class];
    const float top = object_id < objects.size() ? objects[object_id]->bv_bb.vMax.y : 0.f;
    obj.set(top, m_stock_hide_abs[season_class], m_stock_snow, 1.f);
}

// Main thread, once per frame (and at load, both with no calc task running): the season classes' keep,
// height and burial rule at D and at D_prev, and the classes whose keep stays zero over the next days
// (their parts are not even packed). A slot repacks every 15-30 frames, so the look-ahead covers 32
// frames of D's current motion (the QA year run, the glide after an r__season change), and a class that
// comes back after all (a pin jumping D) repacks every cached slot at once.
void CDetailManager::StockSeasonUpdate()
{
    const DaSeasonData& data = da_season_data();
    const float day = da_season_day(), day_prev = da_season_day_prev();
    const SSnowFrame& snow = da_snow_frame();
    const float visible = std::clamp(snow.grass_visible, 0.f, 1.f);
    m_stock_snow = snow.active ? 1.f : 0.f;
    // Days per frame along the shorter arc of the year; the look-ahead follows the direction D moves.
    const float rate = da_season_wrap(day - day_prev + 182.5f) - 182.5f;
    const float ahead_days = std::min(std::max(4.f, std::abs(rate) * 32.f), 365.f);
    const float dir = rate < 0.f ? -1.f : 1.f;
    bool returned = false;
    for (u32 c = 0; c < season_class_count; ++c)
    {
        const DaStockSeason& k = data.stock[c];
        const auto eval = [&k, visible](float d) {
            const float keep = k.keep.n ? std::clamp(k.keep.at(d), 0.f, 1.f) : 1.f;
            const float height = k.height.n ? std::clamp(k.height.at(d), 0.05f, 1.f) : 1.f;
            const float hide_rel = k.hide_rel.n ? std::max(k.hide_rel.at(d), 0.f) : 1.f;
            return Fvector4{keep * (k.grass_visible ? visible : 1.f), height, hide_rel, k.hide_margin};
        };
        m_stock_season[c] = eval(day);
        m_stock_season_prev[c] = eval(day_prev);
        m_stock_hide_abs[c] = k.hide_abs;
        float ahead = std::max(m_stock_season[c].x, m_stock_season_prev[c].x);
        for (float step = 1.f; step <= ahead_days && ahead <= 0.f; step += 1.f)
            ahead = std::max(ahead, k.keep.n ? k.keep.at(day + dir * step) : 1.f);
        if (ahead <= 0.f && k.keep.n)
            ahead = std::max(ahead, k.keep.at(day + dir * ahead_days));
        const bool skip = ahead <= 0.f;
        returned = returned || (m_season_skip[c] && !skip);
        m_season_skip[c] = skip;
    }
    if (returned && cache_pool)
        for (u32 i = 0; i < dm_cache_size; ++i)
            cache_pool[i].frame = 0;
}

// The blades take over the blade classes (or give them back): every cached slot decompresses again.
void CDetailManager::SetBladesActive(bool active)
{
    if (m_blades == active)
        return;
    WaitForCalc();
    m_blades = active;
    if (!dtFS || !cache)
        return;
    for (u32 z = 0; z < dm_cache_line; ++z)
        for (u32 x = 0; x < dm_cache_line; ++x)
            if (Slot* slot = cache[z][x])
                cache_Task(int(x), int(z), slot);
}

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
    for (u32& stamp : m_vis_stamp)
        stamp = 1;
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
#if RENDER == R_R4
    ms_Unload();
#endif

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

    // Every model's class by its content key (dead_air_x64_seasons.ltx [detail_types]): blades, a stock
    // season class, or unknown (stock card with the generic grass season).
    da_season_data_reload();
    detail_classify(objects);
    {
        bool any_blade = false;
        for (const CDetail* object : objects)
            any_blade = any_blade || da_grass_object_profile(*object) >= 0;
        m_blades = any_blade && UseVS() && da_grass_blades_available();
    }
    StockSeasonUpdate();

    // The trample-rustle grass test classifies details by height (see k_veg_min_height).
    // Dump what this level actually ships so the threshold can be checked against real
    // models rather than guessed.
    if (strstr(Core.Params, "-wvdbg"))
        for (u32 i = 0; i < objects.size(); ++i)
            Msg("* [wind-veg] detail[%u] model height %.3f m, scale %.2f-%.2f -> world %.2f-%.2f", i,
                objects[i]->bv_bb.vMax.y - objects[i]->bv_bb.vMin.y, objects[i]->m_fMinScale,
                objects[i]->m_fMaxScale, (objects[i]->bv_bb.vMax.y - objects[i]->bv_bb.vMin.y) * objects[i]->m_fMinScale,
                (objects[i]->bv_bb.vMax.y - objects[i]->bv_bb.vMin.y) * objects[i]->m_fMaxScale);

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
            vertexOffset += u32(hw_GeometryCopies()) * object->number_vertices;
            indexOffset += u32(hw_GeometryCopies()) * object->number_indices;
        }
#if RENDER == R_R4
        ms_Load();
#endif
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

    // Optional x64-pack override: system.ltx is sealed inside the base archives, and the mod's
    // authored fast-set was tuned against an engine that ignored amplitudes entirely. Retuned
    // values travel in their own data file instead of a magic constant here.
    if (FS.exist("$game_config$", "dead_air_x64_details.ltx"))
    {
        string_path fn;
        FS.update_path(fn, "$game_config$", "dead_air_x64_details.ltx");
        CInifile ini(fn, TRUE);
        const auto rd = [&ini](pcstr key, float def) {
            return ini.line_exist("details", key) ? ini.r_float("details", key) : def;
        };
        swing_desc[0].amp1 = rd("swing_normal_amp1", swing_desc[0].amp1);
        swing_desc[0].amp2 = rd("swing_normal_amp2", swing_desc[0].amp2);
        swing_desc[0].rot1 = rd("swing_normal_rot1", swing_desc[0].rot1);
        swing_desc[0].rot2 = rd("swing_normal_rot2", swing_desc[0].rot2);
        swing_desc[0].speed = rd("swing_normal_speed", swing_desc[0].speed);
        swing_desc[1].amp1 = rd("swing_fast_amp1", swing_desc[1].amp1);
        swing_desc[1].amp2 = rd("swing_fast_amp2", swing_desc[1].amp2);
        swing_desc[1].rot1 = rd("swing_fast_rot1", swing_desc[1].rot1);
        swing_desc[1].rot2 = rd("swing_fast_rot2", swing_desc[1].rot2);
        swing_desc[1].speed = rd("swing_fast_speed", swing_desc[1].speed);
        Msg("* [details] swing overridden from dead_air_x64_details.ltx");
    }
}
#endif
void CDetailManager::Unload()
{
    ZoneScoped;
    WaitForCalc();

#if RENDER == R_R4
    ms_Unload();
#endif
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
    // Fade onset as a share of the draw radius (r__grass_fade_start); 0 keeps the stock
    // shrink-from-one-metre. Whatever the share, the band keeps at least twelve metres: the
    // 0.95 of the high presets left four to five metres of an 80-100 m radius, and with the
    // slot cadence of 15-30 frames a bush or a sapling shrank away in three or four visible
    // steps - the "pop" players reported. Grass at twelve metres from the edge is a few
    // pixels tall, so the longer band costs it nothing.
    constexpr float fade_band_min = 12.f;
    const float fade_share_cap = dm_fade > fade_band_min + 1.f ? 1.f - fade_band_min / (dm_fade - 1.f) : 0.f;
    const float fade_share = std::min(ps_r__grass_fade_start, fade_share_cap);
    float fade_start = 1.f + (dm_fade - 1.f) * fade_share;
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

                        ++sp.gen;
                        sp.r_items[0].clear();
                        sp.r_items[1].clear();
                        sp.r_items[2].clear();
                        sp.r_rows[0].clear();
                        sp.r_rows[1].clear();
                        sp.r_rows[2].clear();
                        // A class the season keeps out for the next days draws nothing: nothing to pack.
                        if (m_season_skip[SeasonClass(sp.id)])
                            continue;

                        const float Rq_drcp = m_objectRadiusSquared[sp.id] * dist_sq_rcp;

                        for (auto& siIT : sp.items)
                        {
                            SlotItem& Item = *siIT;
                            const float fade = ps_detail_scale_on_fade ? 1.f : alpha_i;
                            // r__grass_fade_flat splits the fade between uniform shrink and
                            // height-only: uni is what the footprint loses, and the height
                            // multiplier restores fade/uni so total vertical shrink stays fade.
                            const float uni = 1.f - (1.f - fade) * (1.f - ps_r__grass_fade_flat);
                            Item.height_calculated = (uni > 1e-4f) ? (fade / uni) : 1.f;
                            const float scale = Item.scale_calculated = Item.scale * uni;
                            const float ssa = scale * scale * Rq_drcp;
                            if (ssa < r_ssaDISCARD)
                            {
                                continue;
                            }
                            u32 vis_id = 0;
                            if (ssa > r_ssaCHEAP)
                                vis_id = Item.vis_ID;

                            sp.r_items[vis_id].push_back(siIT);

                            // The bytes the hardware dump uploads, built where their inputs
                            // change instead of in the draw loop. Height is scaled SEPARATELY:
                            // the local-height contribution is the second element of each row,
                            // so scaling just those lays the blade flat without touching its
                            // ground footprint (r__grass_fade_flat).
                            const float hs = scale * Item.height_calculated;
                            const Fmatrix& M = Item.mRotY;
                            InstanceRows& rows = sp.r_rows[vis_id].emplace_back();
                            rows.row[0].set(M._11 * scale, M._21 * hs, M._31 * scale, M._41);
                            rows.row[1].set(M._12 * scale, M._22 * hs, M._32 * scale, M._42);
                            rows.row[2].set(M._13 * scale, M._23 * hs, M._33 * scale, M._43);
                            // R2 only needs hemisphere
                            rows.row[3].set(Item.c_sun, Item.c_sun, Item.c_sun, Item.c_hemi);

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
                        visibleItems.push_back({&sp.r_items[0], &sp.r_rows[0], &S, &sp, 0, 0});
                    }
                    if (!sp.r_items[1].empty())
                    {
                        auto& visibleItems = m_visibles[1][sp.id];
                        if (visibleItems.empty())
                            m_visibleObjectIds[1].push_back(static_cast<u8>(sp.id));
                        visibleItems.push_back({&sp.r_items[1], &sp.r_rows[1], &S, &sp, 0, 0});
                    }
                    if (!sp.r_items[2].empty())
                    {
                        auto& visibleItems = m_visibles[2][sp.id];
                        if (visibleItems.empty())
                            m_visibleObjectIds[2].push_back(static_cast<u8>(sp.id));
                        visibleItems.push_back({&sp.r_items[2], &sp.r_rows[2], &S, &sp, 0, 0});
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

bool CDetailManager::IsPartVisible(const VisiblePart& part, const CFrustum* frustum, u32 context_id)
{
    if (!frustum)
        return true;

    Slot& slot = *part.slot;
    if (slot.vis_stamp[context_id] == m_vis_stamp[context_id])
        return slot.vis_result[context_id] != 0;

    u32 mask = frustum->getMask();
    const bool visible =
        frustum->testSAABB(slot.vis.sphere.P, slot.vis.sphere.R, slot.vis.box.data(), mask) != fcvNone;

    slot.vis_stamp[context_id] = m_vis_stamp[context_id];
    slot.vis_result[context_id] = visible ? 1u : 0u;
    return visible;
}

void CDetailManager::UpdateRenderState()
{
    if (m_render_state_frame == Device.dwFrame)
        return;
    WaitForCalc();
    // What the previous frame drew with, for the motion vectors. After a frame without an
    // update (the menu, a load) or on a history reset there is nothing to reproject from: the
    // new values below stand for both.
    const bool continuous = m_render_state_frame + 1 == Device.dwFrame && !detail_history_reset();
    m_render_state_frame = Device.dwFrame;
    m_render_frames.fetch_add(1, std::memory_order_relaxed);
    m_time_pos_prev = m_time_pos;
    m_wind_dir1_prev = m_wind_dir1;
    m_wind_dir2_prev = m_wind_dir2;

#ifndef _EDITOR
    const auto& env = g_pGamePersistent->Environment();
    const float gust_smooth = env.eff_wind_gust_smooth;
    const float wind_dir = env.eff_wind_dir;
    // The effective-wind service supplies the real-life variability (minute trends, waves,
    // discrete gusts) inside the weather envelope; the slope here is deliberately steep,
    // because a storm read as near-calm through three multiplied moderating layers.
    //
    // The floors are the part that had to go. At 0.28 and 0.70 a meadow in a literal dead calm
    // still ran at a third of its storm amplitude with the oscillator turning at three quarters
    // speed - the whole range from nothing to a gale bought a 35 per cent change, and tall marsh
    // reed, whose tip travels with its real height, looked stormy in every weather.
    const float wind_norm = 0.03f + 1.50f * env.eff_wind_norm;
    const float whip = 0.30f + 1.20f * env.eff_wind_norm;
#else
    constexpr float gust_smooth = 0.3f;
    constexpr float wind_dir = 0.f;
    constexpr float wind_norm = 0.5f;
    constexpr float whip = 1.f;
#endif

    float delta = Device.fTimeGlobal - m_global_time_old;
    if (delta < 0.f || delta > 1.f)
        delta = 0.03f;
    m_global_time_old = Device.fTimeGlobal;
#ifndef _EDITOR
    // Frozen wind holds the grass as well; a wind reseed (its clock jumps back) restarts the
    // phase, so reseed + freeze gives the same meadow in every run (QA frame parity).
    static float wind_time_seen = 0.f;
    if (env.eff_wind_time < wind_time_seen)
        m_time_rot_1 = m_time_rot_2 = m_time_pos = m_time_pos_prev = 0.f;
    wind_time_seen = env.eff_wind_time;
    if (env.eff_wind_freeze)
        delta = 0.f;
#endif

    // The swing preset (calm vs storm set) follows the smoothed gustiness from the wind service.
    swing_current.lerp(swing_desc[0], swing_desc[1], gust_smooth);

    // Phase advances FASTER in strong wind (grass whips, not just leans): the factor scales the
    // accumulator increments, so a changing wind never jumps the phase.
    m_time_rot_1 += PI_MUL_2 * delta * whip / swing_current.rot1;
    m_time_rot_2 += PI_MUL_2 * delta * whip / swing_current.rot2;
    m_time_pos += delta * swing_current.speed * whip;

    // Direction comes from the service (weather heading + its own bounded wander); the two wave
    // groups get a small fixed split so the field does not move as one sheet.
    constexpr float group_split = deg2rad(4.f);
    const float dir1 = wind_dir + group_split;
    const float dir2 = wind_dir - group_split;

    m_wind_dir1.set(_sin(dir1), 0.f, _cos(dir1), 0.f).normalize().mul(swing_current.amp1 * wind_norm);
    m_wind_dir2.set(_sin(dir2), 0.f, _cos(dir2), 0.f).normalize().mul(swing_current.amp2 * wind_norm);
    if (!continuous)
    {
        m_time_pos_prev = m_time_pos;
        m_wind_dir1_prev = m_wind_dir1;
        m_wind_dir2_prev = m_wind_dir2;
    }
#if RENDER == R_R4
    // Before DispatchMTCalc: the calc task reads it.
    m_mesh_frame = ms_Latch();
#endif
    // The season of the stock classes and the blades' frame state, before the calc task and the
    // passes of this frame read them.
    StockSeasonUpdate();
    if (m_blades)
        da_grass_frame_latch();

#ifndef _EDITOR
    // Publish "how much green is around" for the vegetation-audio layer: the visible stock parts
    // (~600 read as a full meadow) weighted by what the season keeps of their class, plus the share of
    // blade grass around the camera the calc task measured.
    float visible_count = 0.f;
    for (const auto& list : m_visibles)
        for (u32 object = 0; object < list.size(); ++object)
            visible_count += float(list[object].size()) * m_stock_season[SeasonClass(object)].x;
    const float blades = m_blades ? da_grass_green() : 0.f;
    g_pGamePersistent->Environment().wind_veg_green = clampr(visible_count / 600.f + blades, 0.f, 1.f);
#endif
}

void CDetailManager::Render(CBackend& cmd_list, const bool collectStats, const CFrustum* frustum, const u32 dumps)
{
    if (!HasRenderableDetails())
        return;
    const u64 started = CPU::QPC();

    // A new frustum invalidates every cached slot answer at once, without touching them -
    // this context's answers, and only this context's.
    ++m_vis_stamp[cmd_list.context_id];

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
#if RENDER == R_R4
    if (UseVS() && m_mesh_frame && !ms_ForceVertex(cmd_list.context_id))
        ms_Render(cmd_list, collectStats, frustum, dumps);
    else
#endif
    if (UseVS())
        hw_Render(cmd_list, collectStats, frustum, dumps);
    else
        soft_Render(frustum, cmd_list.context_id);
    cmd_list.detailRendering = previousDetailRendering;
    cmd_list.set_CullMode(CULL_CCW);

    if (collectStats)
        RImplementation.BasicStats.DetailRender.End();
    m_render_ticks.fetch_add(CPU::QPC() - started, std::memory_order_relaxed);
}

void CDetailManager::WaitForCalc()
{
    if (Task* task = m_calc_task.exchange(nullptr, std::memory_order_acq_rel))
    {
        TaskScheduler->Wait(*task);
        m_calc_running.store(false, std::memory_order_release);
        return;
    }

    // Somebody else owns the handle: the task clears the flag as its very last step.
    while (m_calc_running.load(std::memory_order_acquire))
        std::this_thread::yield();
}

namespace
{
// How far around a presser to look for a tuft: a boot's reach, matching the press motor's
// own footprint (Actor.cpp).
constexpr float k_veg_probe_r = 0.55f;
// Minimum WORLD height of a detail instance for it to count as something you brush through.
// Below this it is ground clutter - gravel, twigs, moss patches - which a road is full of
// and which must not rustle. Picked off the level's own model heights (-wvdbg dumps them).
constexpr float k_veg_min_height = 0.25f;
// How far below the feet a tuft's top may be and still be brushed. The shader fades a press
// out over the metre below the presser (da_wind_motors.h); the sound is one bit, so it stops
// at the half-way point of that fade - which is what tells a boot in the grass from a boot on
// a crate standing in it.
constexpr float k_veg_below_feet = 0.5f;
// And how far ABOVE the feet a tuft's ROOT may sit and still be brushed. A boot brushes what
// grows at the height it walks on; grass rooted well over your head grows on whatever you are
// walking UNDER - a ledge, a roof, the terrain above an interior - and you are not touching it.
// Reported from the field as a rustle at every step inside a windowless concrete room with the
// map's grass overhead: only the tuft's TOP was tested, so anything high enough passed.
// A step's worth of slack, so a verge above a kerb still counts.
constexpr float k_veg_above_feet = 0.5f;
} // namespace

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
    auto* environment = g_pGamePersistent ? &g_pGamePersistent->Environment() : nullptr;
    if (environment)
    {
        m_wind_probes.resize(CEnvironment::wind_motor_count);
        for (u32 i = 0; i < CEnvironment::wind_motor_count; ++i)
        {
            const auto& motor = environment->wind_motors[i];
            m_wind_probes[i] = {motor.pos, motor.used && motor.type == CEnvironment::EWindMotor::press};
        }
    }

    m_calc_task.store(&TaskScheduler->AddTask([this, eye, environment]
    {
        ZoneScoped;

        EYE = eye;

        const int s_x = iFloor(EYE.x / dm_slot_size + .5f);
        const int s_z = iFloor(EYE.z / dm_slot_size + .5f);

        RImplementation.BasicStats.DetailCache.Begin();
        cache_Update(s_x, s_z, EYE);
        RImplementation.BasicStats.DetailCache.End();

        UpdateVisibleM();
#if RENDER == R_R4
        if (m_mesh_frame)
            ms_Calc();
#endif
        // The blades' tiles of this frame (da_grass_blades.cpp).
        if (m_blades)
            da_grass_calc(EYE);

        // Feed the press wind-motors the one fact only this side knows: is there actual
        // grass in the detail cache under the pressing entity. The sound layer
        // (WindVegSound) reads it for the walking-through-grass rustle. Positions are a
        // main-thread snapshot; only the atomic result is shared with the sound reader.
        //
        // The first cut asked the SLOT ("does this 2x2 m cell hold any details at all") and
        // was wrong twice over, reported from the field as rustle while walking a bare road:
        // a cell that big straddles the verge, and details are not only grass - gravel and
        // dry litter are details too. So walk the actual tufts near the feet and judge each
        // by its own height: what a boot brushes through is TALL, ground clutter is flat.
        if (environment)
        {
            static const bool wvdbg = !!strstr(Core.Params, "-wvdbg");
            for (u32 i = 0; i < m_wind_probes.size(); ++i)
            {
                const auto& m = m_wind_probes[i];
                if (!m.active)
                    continue;

                float vegetation = 0.f;
                // -wvdbg: the nearest tall tuft the disc saw, whether or not it was accepted -
                // the answer to "why does this room rustle" is its height against the feet.
                float dbg_root = 0.f, dbg_top = 0.f, dbg_d2 = flt_max;
                // Only the slots the search disc actually touches (it is smaller than a cell,
                // so at most four), each clamped into the cache the same way as before.
                const int lo_x = iFloor((m.pos.x - k_veg_probe_r) / dm_slot_size + .5f) - s_x;
                const int hi_x = iFloor((m.pos.x + k_veg_probe_r) / dm_slot_size + .5f) - s_x;
                const int lo_z = iFloor((m.pos.z - k_veg_probe_r) / dm_slot_size + .5f) - s_z;
                const int hi_z = iFloor((m.pos.z + k_veg_probe_r) / dm_slot_size + .5f) - s_z;
                for (int cz = lo_z; cz <= hi_z && vegetation == 0.f; ++cz)
                {
                    for (int cx = lo_x; cx <= hi_x && vegetation == 0.f; ++cx)
                    {
                        if (abs(cx) >= int(dm_size) || abs(cz) >= int(dm_size))
                            continue;
                        Slot* s = cache_Query(cx, cz);
                        // A slot still queued for decompression carries no items yet.
                        if (!s || s->empty || s->type != stReady)
                            continue;
                        for (const auto& part : s->G)
                        {
                            if (part.items.empty() || part.id >= objects.size())
                                continue;
                            // Model height scales with the instance, so this is the tuft's
                            // real height in the world.
                            const CDetail* obj = objects[part.id];
                            if (!obj)
                                continue;
                            // What the season leaves of the class: the shaders' keep, height and
                            // burial, on the CPU (a buried or wintered-away tuft does not rustle).
                            const u32 season_class = SeasonClass(part.id);
                            const Fvector4& season = m_stock_season[season_class];
                            if (season.x <= 0.f)
                                continue;
                            const float model_h = (obj->bv_bb.vMax.y - obj->bv_bb.vMin.y) * season.y;
                            for (const SlotItem* it : part.items)
                            {
                                if (!it || model_h * it->scale < k_veg_min_height)
                                    continue;
                                if (detail_stock_u(it->mRotY.c.x, it->mRotY.c.z) >= season.x)
                                    continue;
                                // The tuft has to straddle the height the feet are at: its top
                                // above them (grass a storey down is not brushed) AND its root
                                // not above them (grass a storey up is not brushed either).
                                const float root = it->mRotY.c.y;
                                const float top = root + model_h * it->scale;
                                if (m_stock_snow > 0.f)
                                {
                                    const float h_top = obj->bv_bb.vMax.y * it->scale * season.y;
                                    const float hide = std::max(m_stock_hide_abs[season_class], season.z * h_top - season.w);
                                    if (da_snow_depth_at(it->mRotY.c.x, it->mRotY.c.z) >= hide)
                                        continue;
                                }
                                if (wvdbg)
                                {
                                    const float sx = it->mRotY.c.x - m.pos.x;
                                    const float sz = it->mRotY.c.z - m.pos.z;
                                    const float s_d2 = sx * sx + sz * sz;
                                    if (s_d2 <= k_veg_probe_r * k_veg_probe_r && s_d2 < dbg_d2)
                                    {
                                        dbg_d2 = s_d2;
                                        dbg_root = root;
                                        dbg_top = top;
                                    }
                                }
                                if (top < m.pos.y - k_veg_below_feet)
                                    continue;
                                if (root > m.pos.y + k_veg_above_feet)
                                    continue;
                                const float dx = it->mRotY.c.x - m.pos.x;
                                const float dz = it->mRotY.c.z - m.pos.z;
                                if (dx * dx + dz * dz <= k_veg_probe_r * k_veg_probe_r)
                                {
                                    vegetation = 1.f;
                                    break;
                                }
                            }
                            if (vegetation > 0.f)
                                break;
                        }
                    }
                }
                // The blades: no tufts to walk, the slot's grass, the season and the snow decide.
                if (vegetation == 0.f && m_blades && da_grass_brushes(m.pos, k_veg_probe_r, k_veg_min_height,
                                                         k_veg_below_feet, k_veg_above_feet))
                    vegetation = 1.f;
                if (wvdbg && dbg_d2 < flt_max)
                    Msg("* [wind-veg] probe[%u]: feet=%.2f nearest tuft root=%.2f top=%.2f -> veg=%.0f", i,
                        m.pos.y, dbg_root, dbg_top, vegetation);
                std::atomic_ref<float>(environment->wind_motors[i].veg).store(vegetation, std::memory_order_relaxed);
            }
        }

        m_calc_running.store(false, std::memory_order_release);
    }), std::memory_order_release);
}
} // namespace xray::render::RENDER_NAMESPACE
