// DetailManager.h: interface for the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "xrCore/xrPool.h"
#include "DetailFormat.h"
#include "DetailModel.h"

class CFrustum;

namespace xray::render::RENDER_NAMESPACE
{
#ifdef _EDITOR
//.	#include	"ESceneClassList.h"
const int dm_max_decompress = 14;
class CCustomObject;
typedef u32 ObjClassID;

typedef xr_list<CCustomObject*> ObjectList;
typedef ObjectList::iterator ObjectIt;
typedef xr_map<ObjClassID, ObjectList> ObjectMap;
typedef ObjectMap::iterator ObjectPairIt;

#else
const int dm_max_decompress = 7;
#endif
//const int dm_size = 24;
const int dm_cache1_count = 4;
//const int dm_cache1_line = dm_size * 2 / dm_cache1_count; //! dm_size*2 must be div dm_cache1_count
const int dm_max_objects = 64;
const int dm_obj_in_slot = 4;
//const int dm_cache_line = dm_size + 1 + dm_size;
//const int dm_cache_size = dm_cache_line * dm_cache_line;
//const float dm_fade = float(2 * dm_size) - .5f;
const float dm_slot_size = DETAIL_SLOT_SIZE;

extern u32 dm_size;
extern u32 dm_cache1_line;
extern u32 dm_cache_line;
extern u32 dm_cache_size;
extern float dm_fade;
extern u32 dm_current_size;// = iFloor((float)ps_r__detail_radius/4)*2; //!
extern u32 dm_current_cache1_line;// = dm_current_size*2/dm_cache1_count; //! dm_current_size*2 must be div dm_cache1_count
extern u32 dm_current_cache_line;// = dm_current_size+1+dm_current_size;
extern u32 dm_current_cache_size;// = dm_current_cache_line*dm_current_cache_line;
extern float dm_current_fade;// = float(2*dm_current_size)-.5f;
extern float ps_current_detail_density;
extern float ps_current_detail_height;

class ECORE_API CDetailManager
{
public:
    struct SlotItem
    { // один кустик
        float scale;
        float scale_calculated;
        // Separate HEIGHT multiplier (r__grass_fade_flat): spending the distance fade on
        // height alone lays the tuft flat while its ground footprint keeps covering the
        // soil, instead of shrinking below a pixel and baring the ground.
        float height_calculated;
        Fmatrix mRotY;
        u32 vis_ID; // индекс в visibility списке он же тип [не качается, качается1, качается2]
        float c_hemi;
        float c_sun;
#if RENDER == R_R1
        Fvector c_rgb;
#endif
    };

    using SlotItemVec = xr_vector<SlotItem*>;

    // The four constant-buffer rows one instance contributes, in upload order: three rows
    // of a 3x4 transform, then colour.
    struct InstanceRows
    {
        Fvector4 row[4];
    };

    using InstanceRowVec = xr_vector<InstanceRows>;

    struct Slot;
    struct SlotPart;

    // Where the rows of one wave group of a part live in the mesh path's GPU pool (DirectX 12,
    // DetailManager_MS.cpp): a block of the pool, current while `gen` equals the part's.
    struct GpuRows
    {
        u32 gen{};
        u32 last_frame{};
        u32 first_unit{};
        u32 order{u32(-1)}; // log2 of the block's units; -1 = not resident
        u32 live_index{};   // in the pool's list of live blocks (eviction)
    };

    struct VisiblePart
    {
        SlotItemVec* items;
        // The same instances with their upload bytes already packed. The hardware path
        // streams these; the software path still walks items.
        InstanceRowVec* rows;
        // The slot rather than just its bounds: the frustum answer depends only on the
        // slot, and one slot yields a part per object id per wave group.
        Slot* slot;
        // Mesh path: the part and, this frame, its entries in the row list (count 0 = the part is
        // not resident and the vertex path draws it).
        SlotPart* part;
        u32 index_first;
        u32 index_count;
    };

    struct SlotPart
    { //
        u32 id; // ID модельки
        SlotItemVec items; // список кустиков
        SlotItemVec r_items[3]; // список кустиков for render
        // Built beside r_items, once per slot refresh. The dump used to dereference a
        // SlotItem pointer per instance per pass and copy 64 bytes out of the middle of
        // it - a cache miss per blade, tens of thousands of them per pass. Packed, the
        // same upload is a memcpy the prefetcher can see coming.
        InstanceRowVec r_rows[3];
        // Mesh path: bumped whenever r_rows change (slot refresh, re-targeting); the GPU copy of
        // a wave group is current while its gen matches.
        u32 gen{};
        GpuRows gpu[3];
    };

    enum SlotType : u32
    {
        stReady = 0, // Ready to use
        stPending, // Pending for decompression
    };

    struct Slot
    { // распакованый слот размером DETAIL_SLOT_SIZE
        struct
        {
            u32 empty : 1;
            u32 type : 1;
            u32 frame : 30;
        };
        int sx, sz; // координаты слота X x Y
        vis_data vis; //
        // Which visibility pass last asked about this slot, and what it answered. Every
        // part cut from this slot shares the answer, so only the first one pays.
        //
        // PER RENDER CONTEXT. The sun cascades run through xr_parallel_for, three of them
        // at once, each with its OWN frustum (render_phase_sun.cpp), and the local lamps
        // do the same on the aux contexts. One shared answer meant cascade 2 reading what
        // cascade 0 had just decided about a different frustum - and a slot of grass
        // appearing in one cascade's shadow map and not in the next, frame after frame.
        // That is the shimmer along the edges of grass shadows.
        u32 vis_stamp[R__NUM_CONTEXTS];
        u8 vis_result[R__NUM_CONTEXTS];
        SlotPart G[dm_obj_in_slot]; //

        Slot()
        {
            frame = 0;
            empty = 1;
            type = stReady;
            sx = sz = 0;
            ZeroMemory(vis_stamp, sizeof(vis_stamp));
            ZeroMemory(vis_result, sizeof(vis_result));
            vis.clear();
        }
    };

    struct CacheSlot1
    {
        u32 empty;
        vis_data vis;
        Slot** slots[dm_cache1_count * dm_cache1_count];
        CacheSlot1()
        {
            empty = 1;
            vis.clear();
        }
    };

    using VisiblePartVec = xr_vector<VisiblePart>;
    using vis_list = xr_vector<VisiblePartVec>;
    typedef svector<CDetail*, dm_max_objects> DetailVec;
    typedef DetailVec::iterator DetailIt;
    typedef poolSS<SlotItem, 4096> PSS;

    int dither[16][16];

    // swing values
    struct SSwingValue
    {
        float rot1;
        float rot2;
        float amp1;
        float amp2;
        float speed;
        void lerp(const SSwingValue& v1, const SSwingValue& v2, float factor);
    };
    SSwingValue swing_desc[2];
    SSwingValue swing_current;
    float m_time_rot_1;
    float m_time_rot_2;
    float m_time_pos;
    float m_global_time_old;
    Fvector4 m_wind_dir1{};
    Fvector4 m_wind_dir2{};
    // The same three as the previous frame drew with them: the motion vectors bend every blade
    // with both (UpdateRenderState keeps them, hw_Render_dump hands them over as *_prev).
    float m_time_pos_prev{};
    Fvector4 m_wind_dir1_prev{};
    Fvector4 m_wind_dir2_prev{};
    u32 m_render_state_frame{u32(-1)};
    // CPU time of every grass pass (all contexts) and the frames it covers: the [gpu] line's grass_cpu.
    std::atomic<u64> m_render_ticks{0};
    std::atomic<u32> m_render_frames{0};
    // Bumped once per render entry of ITS OWN context, where that context's frustum
    // changes. Slot stamps start at 0, so these start at 1 and no slot can look
    // pre-tested.
    u32 m_vis_stamp[R__NUM_CONTEXTS];

    IReader* dtFS;
    DetailHeader dtH;
    DetailSlot* dtSlots; // note: pointer into VFS
    DetailSlot DS_empty;

    DetailVec objects;
    svector<float, dm_max_objects> m_objectRadiusSquared;
    svector<u32, dm_max_objects> m_objectVertexOffsets;
    svector<u32, dm_max_objects> m_objectIndexOffsets;
    vis_list m_visibles[3]; // 0=still, 1=Wave1, 2=Wave2
    svector<u8, dm_max_objects> m_visibleObjectIds[3];

#ifndef _EDITOR
    xrXRC xrc;
#endif
    //AVO: detail draw radius
    CacheSlot1** cache_level1;
    Slot*** cache; // grid-cache itself
    xr_vector<Slot*> cache_task; // non-unpacked slots
    Slot* cache_pool; // just memory for slots

    int cache_cx;
    int cache_cz;

    PSS poolSI; // pool из которого выделяются SlotItem

    Fvector EYE;

    void UpdateVisibleM();
    void UpdateVisibleS();

#ifdef _EDITOR
    virtual ObjectList* GetSnapList() = 0;
#endif

    bool UseVS() const;

    // Software processor
    ref_geom soft_Geom;
    void soft_Load();
    void soft_Unload();
    void soft_Render(const CFrustum* frustum, u32 context_id);

    // Hardware processor
    ref_geom hw_Geom;
    size_t hw_BatchSize;

    // Copies of each detail mesh stored in the HW buffers. DX11 keeps a single copy and
    // replicates it with DrawIndexedInstanced (the shader reads SV_InstanceID); other
    // renderers keep hw_BatchSize copies with the copy index baked into every vertex.
    size_t hw_GeometryCopies() const
    {
#ifdef USE_DX11
        return 1;
#else
        return hw_BatchSize;
#endif
    }

    VertexStagingBuffer hw_VB;
    IndexStagingBuffer hw_IB;

    ref_constant hwc_consts;
    ref_constant hwc_wave;
    ref_constant hwc_wind;
    ref_constant hwc_array;
    ref_constant hwc_s_consts;
    ref_constant hwc_s_xform;
    ref_constant hwc_s_array;
    void hw_Load();
    void hw_Load_Geom();
    void hw_Load_Shaders();
    void hw_Unload();
    void hw_Render(CBackend& cmd_list, bool collectStats, const CFrustum* frustum, u32 dumps = all_dumps);
    // wave_prev/wind_prev: the same pair as the previous frame drew with it (motion vectors).
    // hybrid: only the parts the mesh path does not draw this frame (DetailManager_MS.cpp).
    void hw_Render_dump(CBackend& cmd_list, const Fvector4& consts, const Fvector4& wave,
        const Fvector4& wind, const Fvector4& wave_prev, const Fvector4& wind_prev, u32 var_id, u32 lod_id,
        bool collectStats, const CFrustum* frustum, bool hybrid = false);
    // The per-dump inputs of hw_Render: the constants, this and the previous frame's wave and wind
    // of dump d (0 wave 1, 1 wave 2, 2 still) and the group and element it draws.
    struct DumpInputs
    {
        Fvector4 consts, wave, wind, wave_prev, wind_prev;
        u32 var_id, lod_id;
    };
    DumpInputs hw_DumpInputs(u32 dump) const;

    // Mesh-shader grass (DirectX 12, DetailManager_MS.cpp; docs/dead-air/dx12-features/mesh.md
    // 3.2-3.8). m_mesh_frame is latched once per frame by UpdateRenderState: the calc task, the frame
    // prep and every pass of the frame use that value.
    struct MeshPath;
    MeshPath* m_mesh{};
    bool m_mesh_frame{};
    void ms_Load();
    void ms_Unload();
    bool ms_Latch();
    // Calc task, after UpdateVisibleM: pool blocks, uploads and the chunk list of the frame.
    void ms_Calc();
    // cache_Task: the part's pool blocks go back.
    void ms_Release(SlotPart& part);
    // Render thread, top of the frame: the frame's uploads into the pool and the row list.
    void ms_FramePrepare(CBackend& cmd_list);
    void ms_Render(CBackend& cmd_list, bool collectStats, const CFrustum* frustum, u32 dumps);
    // False: the part is drawn by the vertex path this frame.
    bool ms_Resident(u32 object_id, const VisiblePart& part) const;
    // r__dbg_grass_ab: this context draws its grass through the vertex path for the A/B's first half.
    bool ms_ForceVertex(u32 context_id) const;
    bool ms_HasHybrid(u32 var_id, u32 object_id) const;
    void ms_Stats();
    bool IsPartVisible(const VisiblePart& part, const CFrustum* frustum, u32 context_id);

    // get unpacked slot
    DetailSlot& QueryDB(int sx, int sz);

    void cache_Initialize();
    void cache_Update(int sx, int sz, Fvector& view);
    void cache_Task(int gx, int gz, Slot* D);
    Slot* cache_Query(int sx, int sz);
    void cache_Decompress(Slot* D);
    BOOL cache_Validate();
    // cache grid to world
    int cg2w_X(int x) { return cache_cx - dm_size + x; }
    int cg2w_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }
    // world to cache grid
    int w2cg_X(int x) { return x - cache_cx + dm_size; }
    int w2cg_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }
    void Load();
    void Unload();
    bool HasRenderableDetails() const;
    void UpdateRenderState();
    // The three batches hw_Render draws (two wind waves and the still objects); a parallel pass
    // hands each to a context of its own.
    static constexpr u32 dump_count = 3;
    static constexpr u32 all_dumps = (1u << dump_count) - 1;
    void Render(CBackend& cmd_list, bool collectStats, const CFrustum* frustum = nullptr, u32 dumps = all_dumps);

    /// MT stuff
    // The main pass and the parallel sun cascade passes all render details and all wait for
    // the calc first; the handle is claimed atomically by one of them, the rest spin on the
    // running flag (a shared plain pointer let two renderers race on the same task).
    std::atomic<Task*> m_calc_task{};
    std::atomic_bool m_calc_running{};
    u32 m_calc_scheduled_frame{u32(-1)};
    struct WindProbe
    {
        Fvector pos;
        bool active;
    };
    xr_vector<WindProbe> m_wind_probes;

    void WaitForCalc();
    void DispatchMTCalc();

    CDetailManager();
    virtual ~CDetailManager();
};
} // namespace xray::render::RENDER_NAMESPACE
