// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Ray tracing scene (docs/dead-air/dx12-features/dxr.md 3.3-3.4, 3.8): the level walk, the
// acceleration structures and the geometry and instance tables the passes read.
//
// The static world (level meshes by 256 m cell and class, multiple-use models) is collected on the
// CPU (at the end of level_Load when an effect is on, else on the first frame that wants one) and
// built on the first frame that needs it: bottom-level structures in scratch batches of at most 64 MB,
// compacted after one wait, one top-level structure over them. With every effect off neither exists.
// While occlusion or reflections run, moving rigid objects and the nearest characters (pre-skinned
// by a compute pass, refitted every frame, rebuilt every 30) join a top-level structure rebuilt every
// frame. Shadow rays never see anything that moves or sways (instance masks). The same code runs on
// DirectX 12 and Vulkan through the lane's objects (da_rt_native.cpp).

#include "stdafx.h"

#include "da_rt.h"
#include "da_rt_device.h"

#if defined(DA_RT_LANE)
#include <unordered_map>
#include <unordered_set>

#include "da_gpu_trees.h"
#include "Layers/xrRender/FHierrarhyVisual.h"
#include "Layers/xrRender/FLOD.h"
#include "Layers/xrRender/FProgressive.h"
#include "Layers/xrRender/FTreeVisual.h"
#include "Layers/xrRender/LightTrack.h"
// The skeleton headers narrow size_t in inline code (C4267).
#pragma warning(push)
#pragma warning(disable : 4267)
#include "Layers/xrRender/FSkinned.h"
#include "Layers/xrRender/SkeletonCustom.h"
#pragma warning(pop)
#include "Layers/xrRender/r__sector.h"
#include "Layers/xrRender/da_veg.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "xrCore/Animation/Bone.hpp"
#include "xrCore/FMesh.hpp"
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(DA_RT_LANE)
// The protected parts of the engine's visuals the scene reads (pointers to members named through a
// derived class): the slide windows of a progressive mesh and the skinning data of a model part.
struct DaRtProgressiveAccess : FProgressive
{
    static const FSlideWindowItem& windows(const FProgressive& v) { return v.*(&DaRtProgressiveAccess::nSWI); }
};

struct DaRtSkeletonAccess : CSkeletonX
{
    static u16 render_mode(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::RenderMode); }
    static u32 bone(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::RMS_boneid); }
    static bool single(const CSkeletonX& s)
    {
        const u16 mode = render_mode(s);
        return mode == RM_SINGLE || mode == RM_SINGLE_HQ;
    }
    static const ref_smem<vertBoned1W>& v1(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::Vertices1W); }
    static const ref_smem<vertBoned2W>& v2(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::Vertices2W); }
    static const ref_smem<vertBoned3W>& v3(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::Vertices3W); }
    static const ref_smem<vertBoned4W>& v4(const CSkeletonX& s) { return s.*(&DaRtSkeletonAccess::Vertices4W); }
};
#endif

namespace
{
#if defined(DA_RT_LANE)
// ---- records shared with rt/rt_geometry.hlsli ---------------------------------------------------

struct DaRtGeometryRecord
{
    u32 vb, vb_offset, ib, ib_offset, layout, layout2, albedo, flags;
};
static_assert(sizeof(DaRtGeometryRecord) == 32);

struct DaRtInstanceRecord
{
    float root_y, hemi_scale, hemi_bias;
    u32 flags;
};
static_assert(sizeof(DaRtInstanceRecord) == 16);

constexpr u32 kNone = ~0u;
constexpr u32 kGeomAlpha = 0x1, kGeomEmissive = 0x2, kGeomAbsolute = 0x4;
// kInstSeason: a vegetation stand-in's crown following its species' day (bits 8..15 its tables row + 1).
constexpr u32 kInstTrunk = 0x1, kInstTree = 0x2, kInstSeason = 0x4;
constexpr u32 kInstanceDynamic = 0x800000u;
enum : u8
{
    kMaskStatic = 0x01,
    kMaskStaticAlpha = 0x02,
    kMaskFoliage = 0x04,
    kMaskTrunk = 0x08,
    kMaskDynamic = 0x10,
    kMaskSkinned = 0x20,
    kMaskNoShadow = 0x40,
};
enum : u32
{
    kTcNone,
    kTcShort2Frac,
    kTcShort4Tree,
    kTcFloat2,
    kTcShort2,
};
enum : u32
{
    kNormalNone,
    kNormalColor,
    kNormalFloat3,
    kNormalOct16,
};

// The scene's own vertices (rigid model parts, skinned output): float3 position, octahedral snorm16
// normal, float2 texcoord.
constexpr u32 kPackedStride = 24;
constexpr u32 kPackedLayout = kPackedStride | (12u << 8) | (16u << 16) | (kTcFloat2 << 24) | (kNormalOct16 << 28);
constexpr u32 kPackedLayout2 = 0xFFFFu;
// The pre-skinning source: position, normal, texcoord, four 16-bit bones, four weights (rt_skin.hlsl).
constexpr u32 kSkinSourceStride = 64;

constexpr float kCellSize = 256.f;
constexpr u64 kScratchBatch = 64ull << 20;
constexpr u64 kVramReserve = 512ull << 20;
constexpr u32 kMaxDynamicInstances = 512;
constexpr u32 kSkinRebuildFrames = 30;
constexpr u32 kDynamicEvictFrames = 300;
// Structures and scratch regions start at 256 bytes (D3D12's alignment; Vulkan asks 256 at most).
constexpr u64 kAsAlign = 256;

u64 da_align(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

// ---- one mesh as the walk found it ------------------------------------------------------------

struct DaRtMesh
{
    ID3D11Buffer* vb{};
    u32 vb_offset{}; // bytes to vertex 0
    u32 stride{};
    u32 vertex_count{};
    ID3D11Buffer* ib{};
    u32 ib_offset{}; // bytes to the first index
    u32 index_count{};
    u32 layout{}, layout2{0xFFFFu};
    ref_texture albedo;
    u32 flags{}; // kGeom*
};

struct DaRtMeshKey
{
    const void* vb;
    const void* ib;
    u32 vb_offset, stride, vertex_count, ib_offset, index_count, flags;
    u64 source; // the upload of a model part's vertices (DaRtModelBuffer::serial); 0 for engine buffers
    bool operator==(const DaRtMeshKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
};
static_assert(sizeof(DaRtMeshKey) == 48); // no padding: compared and hashed as bytes
struct DaRtMeshKeyHash
{
    size_t operator()(const DaRtMeshKey& k) const
    {
        const u8* bytes = reinterpret_cast<const u8*>(&k);
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k); ++i)
            h = (h ^ bytes[i]) * 1099511628211ull;
        return h;
    }
};

DaRtMeshKey da_rt_key(const DaRtMesh& m)
{
    DaRtMeshKey k{};
    k.vb = m.vb;
    k.ib = m.ib;
    k.vb_offset = m.vb_offset;
    k.stride = m.stride;
    k.vertex_count = m.vertex_count;
    k.ib_offset = m.ib_offset;
    k.index_count = m.index_count;
    k.flags = m.flags;
    return k;
}

// The decode of an engine vertex declaration (rt/rt_geometry.hlsli): a FLOAT3 position at 0 is
// required, the normal and the first texture coordinate are found by usage.
bool da_rt_layout(const SGeometry* geom, u32& layout, u32& layout2)
{
    if (!geom || !geom->dcl || !geom->vb_stride || geom->vb_stride > 255 || geom->vb_stride % 4)
        return false;
    u32 normal_off = 0xFF, normal_kind = kNormalNone, tc_off = 0xFF, tc_kind = kTcNone, t_off = 0xFF, b_off = 0xFF;
    bool position = false;
    u32 tc_type = kNone;
    for (const VertexElement& e : geom->dcl->dcl_code)
    {
        if (e.Stream == 0xFF)
            break;
        if (e.Stream != 0)
            continue;
        switch (e.Usage)
        {
        case D3DDECLUSAGE_POSITION:
            position = e.UsageIndex == 0 && e.Offset == 0 && e.Type == D3DDECLTYPE_FLOAT3;
            break;
        case D3DDECLUSAGE_NORMAL:
            normal_off = e.Offset;
            normal_kind = e.Type == D3DDECLTYPE_D3DCOLOR ? kNormalColor : e.Type == D3DDECLTYPE_FLOAT3 ? kNormalFloat3 : kNormalNone;
            break;
        case D3DDECLUSAGE_TANGENT:
            if (e.Type == D3DDECLTYPE_D3DCOLOR)
                t_off = e.Offset;
            break;
        case D3DDECLUSAGE_BINORMAL:
            if (e.Type == D3DDECLTYPE_D3DCOLOR)
                b_off = e.Offset;
            break;
        case D3DDECLUSAGE_TEXCOORD:
            if (e.UsageIndex == 0)
            {
                tc_off = e.Offset;
                tc_type = e.Type;
            }
            break;
        default: break;
        }
    }
    if (!position)
        return false;
    switch (tc_type)
    {
    case D3DDECLTYPE_SHORT2: tc_kind = t_off != 0xFF && b_off != 0xFF ? kTcShort2Frac : kTcShort2; break;
    case D3DDECLTYPE_SHORT4: tc_kind = kTcShort4Tree; break;
    case D3DDECLTYPE_FLOAT2: tc_kind = kTcFloat2; break;
    default: tc_off = 0xFF; break;
    }
    if (normal_kind == kNormalNone)
        normal_off = 0xFF;
    if (normal_off % 4 || (tc_off != 0xFF && tc_off % 4) || (t_off != 0xFF && t_off % 4) || (b_off != 0xFF && b_off % 4))
        return false;
    layout = geom->vb_stride | (normal_off << 8) | (tc_off << 16) | (tc_kind << 24) | (normal_kind << 28);
    layout2 = t_off | (b_off << 8);
    return true;
}

// The texture bound to s_base in pass 0 (the albedo the G-buffer samples); the first one otherwise.
ref_texture da_rt_albedo(ShaderElement* element)
{
    if (!element || element->passes.empty())
        return {};
    SPass* pass = element->passes[0]._get();
    if (!pass || !pass->T)
        return {};
    u32 stage = kNone;
    if (pass->constants)
    {
        if (const ref_constant c = pass->constants->get("s_base", RC_dx11texture))
            stage = c->samp.index;
    }
    for (const auto& [slot, texture] : *pass->T)
    {
        if (slot == stage && texture)
            return texture;
    }
    for (const auto& [slot, texture] : *pass->T)
    {
        if (texture && !texture->isUser())
            return texture;
    }
    return {};
}

bool da_rt_alpha_tested(ShaderElement* element)
{
    if (!element || element->passes.empty() || !element->passes[0]->ps)
        return false;
    pcstr name = element->passes[0]->ps->cName.c_str();
    return name && strstr(name, "aref");
}

// The element that shades the G-buffer; null for what no ray may see (glass, water, decals,
// wallmarks, distortion).
ShaderElement* da_rt_scene_element(dxRender_Visual* visual)
{
    Shader* shader = visual->shader._get();
    if (!shader)
        return nullptr;
    ShaderElement* e = shader->E[SE_R2_NORMAL_HQ]._get();
    if (!e)
        e = shader->E[SE_R2_NORMAL_LQ]._get();
    if (!e || e->passes.empty() || e->flags.iPriority / 2 != 0 || e->flags.bStrictB2F || e->flags.bWmark || e->flags.bDistort)
        return nullptr;
    return e;
}

bool da_rt_casts_sun_shadow(dxRender_Visual* visual)
{
    Shader* shader = visual->shader._get();
    return shader && shader->E[SE_R2_SHADOW]._get();
}

// A mesh of the engine's buffers: the full-detail window of a progressive one.
bool da_rt_engine_mesh(Fvisual* v, ShaderElement* element, DaRtMesh& mesh)
{
    if (!v->rm_geom || !v->p_rm_Vertices || !v->p_rm_Indices)
        return false;
    if (!da_rt_layout(&*v->rm_geom, mesh.layout, mesh.layout2))
        return false;
    mesh.stride = v->rm_geom->vb_stride;
    mesh.vb = *v->p_rm_Vertices;
    mesh.ib = *v->p_rm_Indices;
    mesh.vb_offset = v->vBase * mesh.stride;
    mesh.vertex_count = v->vCount;
    u32 first = v->iBase, count = v->dwPrimitives * 3;
    if (v->Type == MT_PROGRESSIVE)
    {
        if (const auto* pm = dynamic_cast<FProgressive*>(v))
        {
            const FSlideWindowItem& swi = DaRtProgressiveAccess::windows(*pm);
            if (swi.count && swi.sw)
            {
                first = v->iBase + swi.sw[0].offset;
                count = u32(swi.sw[0].num_tris) * 3;
            }
        }
    }
    mesh.ib_offset = first * 2;
    mesh.index_count = count;
    if (!mesh.vb || !mesh.ib || !mesh.vertex_count || count < 3)
        return false;
    mesh.albedo = da_rt_albedo(element);
    mesh.flags = (da_rt_alpha_tested(element) ? kGeomAlpha : 0u) | (element->flags.bEmissive ? kGeomEmissive : 0u);
    return true;
}

bool da_rt_tree_mesh(FTreeVisual* tree, ShaderElement* element, DaRtMesh& mesh)
{
    FTreeVisualInstancedDraw draw;
    if (!tree->window(0, draw) || !draw.geometry || !draw.geometry->vb || !draw.geometry->ib)
        return false;
    if (!da_rt_layout(draw.geometry, mesh.layout, mesh.layout2))
        return false;
    mesh.stride = draw.geometry->vb_stride;
    mesh.vb = draw.geometry->vb;
    mesh.ib = draw.geometry->ib;
    mesh.vb_offset = draw.base_vertex * mesh.stride;
    mesh.vertex_count = draw.vertex_count;
    mesh.ib_offset = draw.start_index * 2;
    mesh.index_count = draw.primitive_count * 3;
    if (!mesh.vertex_count || mesh.index_count < 3)
        return false;
    mesh.albedo = da_rt_albedo(element);
    mesh.flags = (da_rt_alpha_tested(element) ? kGeomAlpha : 0u) | (element->flags.bEmissive ? kGeomEmissive : 0u);
    return true;
}

// A cut-out whose mask texture is unknown is left to the raster, shadow included: the rays would find
// no surface on it (rt_alpha_solid).
bool da_rt_mask_known(const DaRtMesh& mesh) { return !(mesh.flags & kGeomAlpha) || mesh.albedo; }

DaRtTriangles da_rt_triangles(DaRtAddress vb, u32 stride, u32 vertex_count, DaRtAddress ib, u32 index_count, bool alpha)
{
    DaRtTriangles t;
    t.vb = vb;
    t.stride = stride;
    t.vertex_count = vertex_count;
    t.ib = ib;
    t.index_count = index_count;
    t.alpha = alpha;
    return t;
}

void da_rt_instance(DaRtInstanceDesc& d, const Fmatrix& m, u32 id, u8 mask, u32 flags, DaRtAddress blas)
{
    d.transform[0][0] = m._11;
    d.transform[0][1] = m._21;
    d.transform[0][2] = m._31;
    d.transform[0][3] = m._41;
    d.transform[1][0] = m._12;
    d.transform[1][1] = m._22;
    d.transform[1][2] = m._32;
    d.transform[1][3] = m._42;
    d.transform[2][0] = m._13;
    d.transform[2][1] = m._23;
    d.transform[2][2] = m._33;
    d.transform[2][3] = m._43;
    d.id = id;
    d.mask = mask;
    d.contribution = 0;
    d.flags = flags;
    d.accel = blas;
}

u32 da_rt_oct16(Fvector n)
{
    const float l = _abs(n.x) + _abs(n.y) + _abs(n.z);
    if (l < 1e-12f)
        n.set(0.f, 1.f, 0.f);
    else
        n.div(l);
    float x = n.x, y = n.y;
    if (n.z < 0.f)
    {
        x = (1.f - _abs(n.y)) * (n.x >= 0.f ? 1.f : -1.f);
        y = (1.f - _abs(n.x)) * (n.y >= 0.f ? 1.f : -1.f);
    }
    const s32 qx = iFloor(clampr(x, -1.f, 1.f) * 32767.f + 0.5f);
    const s32 qy = iFloor(clampr(y, -1.f, 1.f) * 32767.f + 0.5f);
    return (u32(qx) & 0xFFFFu) | (u32(qy) << 16);
}

// ---- the scene --------------------------------------------------------------------------------

struct DaRtStaticItem
{
    DaRtMesh mesh;
    u8 mask{};
    Fvector center{};
};

struct DaRtTreeItem
{
    FTreeVisual* visual{};
    DaRtMesh mesh;
    u8 mask{};
    bool trunk{};
    u16 season_row{}; // a stand-in crown's vegetation tables row + 1 (0: none)
};

struct DaRtBlas
{
    DaRtAccel* as{}; // the structure traced (compacted once the sizes are back)
    DaRtAccel* uncompacted{};
    u64 result_size{}, scratch_size{};
    u32 first_record{};
    xr_vector<u32> meshes; // DaRtScene::meshes
    u8 mask{};
    bool tree{};
};

// A rigid moving mesh's structure, built once and instanced with the object's world.
struct DaRtDynBlas
{
    DaRtAccel* as{};
    DaRtGeometryRecord record{};
    u32 last_frame{};
    bool built{};
};

// A model part's vertices uploaded once: the scene's own vertex format (rigid parts) or the
// pre-skinning source (skinned parts).
struct DaRtModelBuffer
{
    DaRtBuffer* buffer{};
    u32 vertices{};
    u32 bindless{kNone};
    u32 last_frame{};
    // The shared data's checksum and length: an address freed and reused by another model's data
    // is uploaded again, under a new serial.
    u32 crc{}, length{};
    u64 serial{};
    bool uploaded{};
};

// One character part on the GPU: its skinned vertices and its refitted structure.
struct DaRtSkinned
{
    DaRtBuffer* out{};
    u32 out_bindless{kNone};
    DaRtAccel* as{};
    u64 as_size{}, build_scratch{}, update_scratch{};
    u32 vertices{};
    u32 built_frame{};
    u32 last_frame{};
    bool built{};
    // What the structure was built from: a refit needs the same topology.
    u64 source{};
    ID3D11Buffer* index_buffer{};
    u32 first{}, count{};
    DaRtGeometryRecord record{};
};

// A bindless raw view of an engine buffer. Those the moving objects registered are referenced
// until evicted: their model may be freed meanwhile, and the address must not name another buffer.
struct DaRtBufferSlot
{
    u32 index{kNone};
    u32 last_frame{};
    bool held{};
};

// A moving object's texture: referenced until evicted, so the key cannot name a freed one.
struct DaRtDynTexture
{
    ref_texture texture;
    u32 index{kNone};
    ID3D11ShaderResourceView* view{};
    u32 last_frame{};
};

struct DaRtScene
{
    // A level is loaded: its visuals may be walked. Survives the release of everything below.
    bool level{};
    // Collected (CPU).
    bool collected{};
    xr_vector<DaRtStaticItem> statics;
    xr_vector<DaRtTreeItem> trees;

    // Built.
    bool ready{};
    bool refused{}; // over the memory budget or failed: effects stay off for this level
    xr_vector<DaRtMesh> meshes; // one geometry record each
    xr_vector<DaRtBlas> blases;
    xr_vector<DaRtInstanceDesc> instances; // static + multiple-use, CPU copy
    xr_vector<DaRtInstanceRecord> instance_records;
    xr_vector<DaRtGeometryRecord> records;
    DaRtBuffer* geometry_table{};
    DaRtBuffer* instance_table{};
    DaRtAccel* tlas{};
    DaRtBuffer* tlas_scratch{};
    ID3D11ShaderResourceView* tlas_view{};
    bool tlas_dynamic{}; // the top-level structure holds moving objects

    // Bindless: every static texture in one range (the records index it), every engine buffer its slot.
    xr_vector<ref_texture> textures;
    xr_vector<ID3D11ShaderResourceView*> texture_views;
    u32 texture_base{kNone};
    std::unordered_map<ID3D11Buffer*, DaRtBufferSlot> buffers;
    // Engine buffers the layer tracks (written by DirectStorage, dedicated): native reads require
    // their state; page ranges need nothing.
    xr_vector<ID3D11Resource*> tracked_buffers;
    u64 texture_generation{};

    // Moving objects.
    std::unordered_map<DaRtMeshKey, DaRtDynBlas, DaRtMeshKeyHash> dyn_blas;
    std::unordered_map<const void*, DaRtModelBuffer> model_buffers; // by the model's shared vertex data
    std::unordered_map<const void*, DaRtModelBuffer> skin_sources;
    std::unordered_map<dxRender_Visual*, DaRtSkinned> skinned;
    std::unordered_map<CTexture*, DaRtDynTexture> dyn_textures;
    u64 upload_serial{}; // DaRtModelBuffer::serial
    bool dyn_textures_added{}; // a texture joined this frame: its read state is required
    DaRtBuffer* dyn_scratch{};
    u64 dyn_scratch_size{};

    DaRtSceneBinding binding;
    u64 memory{}; // bytes of structures and tables, for the log
};
DaRtScene g_scene;
#endif
} // namespace

#if defined(DA_RT_LANE)
namespace
{
// The bindless slot of an engine buffer; kNone when the range is full (not remembered: a later frame
// may find room). `moving`: a moving object's buffer, referenced until da_rt_evict_dynamic lets it go.
u32 da_rt_buffer_slot(ID3D11Buffer* buffer, bool moving = false)
{
    if (!buffer)
        return kNone;
    if (const auto it = g_scene.buffers.find(buffer); it != g_scene.buffers.end())
    {
        it->second.last_frame = Device.dwFrame;
        return it->second.index;
    }
    const u32 index = da_rt_bindless_alloc(1);
    if (index == da_rt_bindless_invalid)
        return kNone;
    da_rt_bindless_buffer(index, buffer);
    if (moving)
        buffer->AddRef();
    g_scene.buffers.emplace(buffer, DaRtBufferSlot{index, Device.dwFrame, moving});
    bool tracked = false;
    if (da_rt_engine_address(buffer, &tracked) && tracked)
        g_scene.tracked_buffers.push_back(buffer);
    return index;
}

u32 da_rt_native_slot(DaRtBuffer* buffer, u64 size)
{
    const u32 index = da_rt_bindless_raw(buffer, size);
    return index == da_rt_bindless_invalid ? kNone : index;
}

void da_rt_free_bindless(u32& index, u32 count = 1)
{
    if (index != kNone)
        da_rt_bindless_free(index, count);
    index = kNone;
}

ID3D11ShaderResourceView* da_rt_texture_view(CTexture* t)
{
    if (!t)
        return nullptr;
    if (t->ds_pending())
        t->ds_finish();
    return t->get_SRView();
}

// ---- collection ---------------------------------------------------------------------------------

// (stand_in: a replaced bush or tree standing in for its procedural plant - its swaying parts alone, never a shadow
// ray's; veg: what it stands for, its crown's cards then follow the species' day.)
void da_rt_collect_visual(dxRender_Visual* visual, std::unordered_set<dxRender_Visual*>& visited, bool stand_in = false,
    const DaVegStandIn* veg = nullptr)
{
    if (!visual || !visited.insert(visual).second)
        return;
    switch (visual->Type)
    {
    case MT_HIERRARHY:
    case MT_LOD: // the billboard facets are the FLOD itself; its children are the models
        for (dxRender_Visual* child : static_cast<FHierrarhyVisual*>(visual)->children)
            da_rt_collect_visual(child, visited, stand_in, veg);
        return;
    case MT_NORMAL:
    case MT_PROGRESSIVE:
    {
        if (stand_in)
            return;
        ShaderElement* element = da_rt_scene_element(visual);
        auto* mesh_visual = dynamic_cast<Fvisual*>(visual);
        if (!element || !mesh_visual)
            return;
        DaRtStaticItem item;
        if (!da_rt_engine_mesh(mesh_visual, element, item.mesh) || !da_rt_mask_known(item.mesh))
            return;
        if (!da_rt_casts_sun_shadow(visual))
            item.mask = kMaskNoShadow;
        else
        {
            item.mask = item.mesh.flags & kGeomAlpha ? kMaskStaticAlpha : kMaskStatic;
            // Its sun shadow comes from the rays: the near and middle cascades skip it.
            visual->rt_static = true;
        }
        item.center = visual->vis.sphere.P;
        g_scene.statics.push_back(std::move(item));
        return;
    }
    case MT_TREE_ST:
    case MT_TREE_PM:
    {
        auto* tree = static_cast<FTreeVisual*>(visual);
        ShaderElement* element = da_rt_scene_element(visual);
        if (!element)
            return;
        DaRtTreeItem item;
        item.visual = tree;
        if (!da_rt_tree_mesh(tree, element, item.mesh) || !da_rt_mask_known(item.mesh))
            return;
        if (tree->rigid() && !stand_in)
        {
            // Stumps, logs, props: the wind never moves them, so they are the static world.
            if (!da_rt_casts_sun_shadow(visual))
                item.mask = kMaskNoShadow;
            else
            {
                item.mask = item.mesh.flags & kGeomAlpha ? kMaskStaticAlpha : kMaskStatic;
                visual->rt_static = true;
            }
        }
        else if (item.mesh.flags & kGeomAlpha)
            item.mask = kMaskFoliage; // crowns sway: reflections only, their shadows stay in the cascades
        else
        {
            item.mask = kMaskTrunk;
            item.trunk = true;
        }
        // A living plant's crown cards follow its species' day; its trunk, a dead plant's cards and a Red Forest
        // pine's red ones keep their look.
        if (stand_in && veg && !veg->dead && (item.mesh.flags & kGeomAlpha))
            item.season_row = u16(veg->species + 1);
        g_scene.trees.push_back(std::move(item));
        return;
    }
    default: return; // particles, fluid volumes, skeletons (none in the level geometry)
    }
}

// An effect switched on whose passes and targets exist (an RT row restarts the renderer, which makes
// both; a console toggle without the restart has nothing to draw into).
bool da_rt_wanted()
{
    const auto wanted = [](RtEffect effect, int setting) {
        return setting && da_rt_pass_ready(effect) && da_rt_targets_ready(effect);
    };
    return wanted(RtEffect::Reflections, ps_r__rt_reflections) || wanted(RtEffect::Shadows, ps_r__rt_shadows) ||
        wanted(RtEffect::AO, ps_r__rtao);
}

void da_rt_collect()
{
    CTimer timer;
    timer.Start();
    auto& dsgraph = RImplementation.get_imm_context();
    std::unordered_set<dxRender_Visual*> visited;
    for (IRender_Sector* s : dsgraph.Sectors)
    {
        auto* sector = static_cast<CSector*>(s);
        if (!sector)
            continue;
        da_rt_collect_visual(sector->root(), visited);
        // The vegetation the GPU-driven path took out of the hierarchies (PLAN.md C16).
        da_gpu_trees_for_each_node(sector, [&](dxRender_Visual* node) { da_rt_collect_visual(node, visited); });
    }
    // The stock bushes the procedural shrubs replace stand in for them (docs/dead-air/SHRUBS.md): the shrubs live in
    // the frame's buffers, in no acceleration structure, and the water and puddles reflected no bush. Their crowns are
    // foliage, which the occlusion and shadow rays skip as they skip every swaying crown.
    xr_vector<dxRender_Visual*> stand_ins;
    da_veg_replaced_visuals(stand_ins);
    for (dxRender_Visual* v : stand_ins)
    {
        DaVegStandIn info;
        da_rt_collect_visual(v, visited, true, da_veg_stand_in(v, info) ? &info : nullptr);
    }
    g_scene.collected = true;
    // GD's shadow-map cull honours the flags of the static casters it draws.
    da_gpu_trees_refresh_flags();
    Msg("* [rt] level: %u static meshes, %u multiple-use models collected (%.1f ms)", u32(g_scene.statics.size()),
        u32(g_scene.trees.size()), timer.GetElapsed_sec() * 1000.f);
}
} // namespace
#endif

void da_rt_level_load()
{
#if defined(DA_RT_LANE)
    da_rt_scene_release();
    g_scene = {};
    g_scene.level = true;
    // An effect on at load walks the level inside the loading time.
    if (da_rt_available() && da_rt_wanted())
        da_rt_collect();
#endif
}

void da_rt_level_unload()
{
    da_rt_scene_release();
#if defined(DA_RT_LANE)
    // The level's visuals go with it: nothing collected may outlive them.
    g_scene = {};
#endif
}

bool da_rt_scene_ready()
{
#if defined(DA_RT_LANE)
    return g_scene.ready;
#else
    return false;
#endif
}

#if defined(DA_RT_LANE)
const DaRtSceneBinding& da_rt_scene_binding() { return g_scene.binding; }

void da_rt_require_geometry(DaRtSection& section)
{
    if (!g_scene.tracked_buffers.empty())
        section.require_read(g_scene.tracked_buffers.data(), u32(g_scene.tracked_buffers.size()));
}

namespace
{
// ---- the static build ------------------------------------------------------------------------------

constexpr u32 kStaticBlasFlags = DaRtBuildFastTrace | DaRtBuildCompaction;

bool da_rt_blas_inputs(const DaRtBlas& blas, xr_vector<DaRtTriangles>& out)
{
    out.clear();
    out.reserve(blas.meshes.size());
    for (const u32 m : blas.meshes)
    {
        const DaRtMesh& mesh = g_scene.meshes[m];
        const DaRtAddress vb = da_rt_engine_address(mesh.vb);
        const DaRtAddress ib = da_rt_engine_address(mesh.ib);
        if (!vb || !ib)
            return false;
        out.push_back(da_rt_triangles(vb + mesh.vb_offset, mesh.stride, mesh.vertex_count, ib + mesh.ib_offset, mesh.index_count,
            mesh.flags & kGeomAlpha));
    }
    return true;
}

DaRtGeometryRecord da_rt_record(const DaRtMesh& mesh, u32 texture_index, u32 extra_flags)
{
    DaRtGeometryRecord r{};
    r.vb = da_rt_buffer_slot(mesh.vb);
    r.vb_offset = mesh.vb_offset;
    r.ib = da_rt_buffer_slot(mesh.ib);
    r.ib_offset = mesh.ib_offset;
    r.layout = mesh.layout;
    r.layout2 = mesh.layout2;
    r.albedo = texture_index;
    r.flags = mesh.flags | extra_flags;
    return r;
}

// Every static texture the records name, in one bindless range; the requirement of the read state.
bool da_rt_write_textures()
{
    const u32 count = u32(g_scene.textures.size());
    g_scene.texture_views.assign(count, nullptr);
    if (!count)
    {
        g_scene.texture_base = kNone;
        return true;
    }
    const u32 base = da_rt_bindless_alloc(count);
    if (base == da_rt_bindless_invalid)
        return false;
    for (u32 i = 0; i < count; ++i)
    {
        g_scene.texture_views[i] = da_rt_texture_view(g_scene.textures[i]._get());
        da_rt_bindless_texture(base + i, g_scene.texture_views[i]);
    }
    // The previous range is read by frames still in flight: freed after this frame's fence.
    da_rt_free_bindless(g_scene.texture_base, count);
    g_scene.texture_base = base;
    return true;
}

void da_rt_require_textures(DaRtSection& section)
{
    xr_vector<ID3D11Resource*> resources;
    resources.reserve(g_scene.textures.size() + g_scene.dyn_textures.size());
    const auto add = [&](ID3D11ShaderResourceView* view) {
        if (!view)
            return;
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        if (resource)
        {
            resources.push_back(resource);
            resource->Release(); // the view keeps it alive
        }
    };
    for (ID3D11ShaderResourceView* view : g_scene.texture_views)
        add(view);
    for (const auto& [texture, slot] : g_scene.dyn_textures)
        add(slot.view);
    if (!resources.empty())
        section.require_read(resources.data(), u32(resources.size()));
}

bool da_rt_create_tlas(u32 capacity)
{
    u64 result = 0, scratch = 0;
    for (const u32 flags : {u32(DaRtBuildFastTrace), u32(DaRtBuildFastBuild)})
    {
        DaRtSizes sizes;
        if (!da_rt_sizes(nullptr, capacity, flags, sizes))
            return false;
        result = std::max(result, sizes.result);
        scratch = std::max(scratch, sizes.build);
    }
    if (!result || !(g_scene.tlas = da_rt_accel_create(da_align(result, kAsAlign), true, "rt tlas")) ||
        !(g_scene.tlas_scratch = da_rt_buffer_create(da_align(std::max<u64>(scratch, 256), kAsAlign), "rt tlas scratch")) ||
        !(g_scene.tlas_view = da_rt_accel_view(g_scene.tlas)))
        return false;
    g_scene.memory += result + scratch;
    return true;
}

// Records the top-level build over the static instances and `dynamic` more (moving objects).
bool da_rt_build_tlas(DaRtSection& section, const DaRtInstanceDesc* dynamic, u32 dynamic_count, bool fast_build)
{
    const u32 total = u32(g_scene.instances.size()) + dynamic_count;
    const u64 bytes = u64(std::max(total, 1u)) * sizeof(DaRtInstanceDesc);
    // Instance records start at 16 bytes (D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT, Vulkan the same).
    const DaRtUpload span = section.upload(bytes, 16);
    if (!span.cpu)
    {
        // The structure keeps its last build, whose moving objects name tables that are gone: the
        // passes trace the empty scene until a build succeeds.
        if (g_scene.tlas_dynamic)
            g_scene.binding.tlas = nullptr;
        return false;
    }
    auto* out = static_cast<DaRtInstanceDesc*>(span.cpu);
    if (!g_scene.instances.empty())
        std::memcpy(out, g_scene.instances.data(), g_scene.instances.size() * sizeof(DaRtInstanceDesc));
    if (dynamic_count)
        std::memcpy(out + g_scene.instances.size(), dynamic, dynamic_count * sizeof(DaRtInstanceDesc));

    DaRtBuild build;
    build.dst = g_scene.tlas;
    build.count = total;
    build.instances = span.address;
    build.flags = fast_build ? DaRtBuildFastBuild : DaRtBuildFastTrace;
    build.scratch = da_rt_address(g_scene.tlas_scratch);
    // The previous build's readers and this build's scratch.
    section.accel_barrier();
    section.build(build);
    section.accel_barrier(g_scene.tlas);
    g_scene.tlas_dynamic = dynamic_count != 0;
    g_scene.binding.tlas = g_scene.tlas_view;
    return true;
}

bool da_rt_scene_build()
{
    CTimer timer;
    timer.Start();

    // 1. Structures: static meshes by 256 m cell and class, multiple-use models by their geometry.
    g_scene.meshes.clear();
    g_scene.blases.clear();
    std::unordered_map<u64, u32> cells;
    for (const DaRtStaticItem& item : g_scene.statics)
    {
        const s64 cx = s64(std::floor(item.center.x / kCellSize)) + 32768;
        const s64 cz = s64(std::floor(item.center.z / kCellSize)) + 32768;
        const u64 key = (u64(item.mask) << 40) | (u64(cx & 0xFFFFF) << 20) | u64(cz & 0xFFFFF);
        auto [it, fresh] = cells.emplace(key, u32(g_scene.blases.size()));
        if (fresh)
        {
            DaRtBlas& blas = g_scene.blases.emplace_back();
            blas.mask = item.mask;
        }
        g_scene.blases[it->second].meshes.push_back(u32(g_scene.meshes.size()));
        g_scene.meshes.push_back(item.mesh);
    }
    std::unordered_map<DaRtMeshKey, u32, DaRtMeshKeyHash> models;
    xr_vector<u32> tree_blas(g_scene.trees.size());
    for (u32 i = 0; i < g_scene.trees.size(); ++i)
    {
        const DaRtTreeItem& item = g_scene.trees[i];
        auto [it, fresh] = models.emplace(da_rt_key(item.mesh), u32(g_scene.blases.size()));
        if (fresh)
        {
            DaRtBlas& blas = g_scene.blases.emplace_back();
            blas.tree = true;
            blas.meshes.push_back(u32(g_scene.meshes.size()));
            g_scene.meshes.push_back(item.mesh);
        }
        tree_blas[i] = it->second;
    }
    if (g_scene.blases.empty())
    {
        Msg("~ [rt] scene: the level has no geometry the rays can use");
        return false;
    }

    // 2. Sizes, and the memory budget: crowns and trunks go first, then the scene.
    xr_vector<DaRtTriangles> inputs;
    u64 result_total = 0, scratch_max = 0, foliage_total = 0;
    for (DaRtBlas& blas : g_scene.blases)
    {
        DaRtSizes sizes;
        if (!da_rt_blas_inputs(blas, inputs) || !da_rt_sizes(inputs.data(), u32(inputs.size()), kStaticBlasFlags, sizes))
        {
            Msg("! [rt] scene: a level buffer has no native range");
            return false;
        }
        blas.result_size = da_align(sizes.result, kAsAlign);
        blas.scratch_size = da_align(sizes.build, kAsAlign);
        result_total += blas.result_size;
        scratch_max = std::max(scratch_max, blas.scratch_size);
    }
    // What leaving the trees out saves: each structure once, and only one no other instance names
    // (bit 0: a crown or trunk uses it, bit 1: something else does).
    xr_vector<u8> tree_use(g_scene.blases.size(), 0);
    for (u32 i = 0; i < g_scene.trees.size(); ++i)
        tree_use[tree_blas[i]] |= (g_scene.trees[i].mask & (kMaskFoliage | kMaskTrunk)) ? 1 : 2;
    for (u32 b = 0; b < g_scene.blases.size(); ++b)
    {
        if (tree_use[b] == 1)
            foliage_total += g_scene.blases[b].result_size;
    }
    const u64 scratch_size = std::max(kScratchBatch, scratch_max);
    const u64 tables = (g_scene.meshes.size() * sizeof(DaRtGeometryRecord)) +
        (g_scene.statics.size() + g_scene.trees.size()) * sizeof(DaRtInstanceRecord);
    const u64 estimate = result_total + scratch_size + tables;
    bool drop_foliage = false;
    u64 budget = 0, usage = 0;
    if (da_rt_video_memory(budget, usage) && budget)
    {
        const u64 free = budget > usage ? budget - usage : 0;
        if (free < estimate + kVramReserve)
        {
            const u64 kept = estimate > foliage_total ? estimate - foliage_total : 0;
            drop_foliage = free >= kept + kVramReserve;
            if (!drop_foliage)
            {
                Msg("! [rt] scene refused: %.0f MB would leave less than %.0f MB of the %.0f MB video memory budget free",
                    estimate / 1048576.0, kVramReserve / 1048576.0, budget / 1048576.0);
                return false;
            }
            Msg("~ [rt] scene: trees left out of the reflections to stay within the video memory budget");
        }
    }

    // 3. Bindless slots, records and instances.
    std::unordered_map<CTexture*, u32> texture_index;
    const auto texture_of = [&](const ref_texture& t) -> u32 {
        CTexture* texture = t._get();
        if (!texture)
            return kNone;
        auto [it, fresh] = texture_index.emplace(texture, u32(g_scene.textures.size()));
        if (fresh)
            g_scene.textures.push_back(t);
        return it->second;
    };
    g_scene.records.clear();
    for (DaRtBlas& blas : g_scene.blases)
    {
        blas.first_record = u32(g_scene.records.size());
        for (const u32 m : blas.meshes)
        {
            const DaRtMesh& mesh = g_scene.meshes[m];
            g_scene.records.push_back(da_rt_record(mesh, texture_of(mesh.albedo), 0));
        }
    }
    for (const DaRtGeometryRecord& r : g_scene.records)
    {
        if (r.vb == kNone || r.ib == kNone)
        {
            Msg("! [rt] scene: the bindless range is full");
            return false;
        }
    }
    if (!da_rt_write_textures())
    {
        Msg("! [rt] scene: no bindless range for %u textures", u32(g_scene.textures.size()));
        return false;
    }
    g_scene.texture_generation = da_rt_texture_generation();

    g_scene.instances.clear();
    g_scene.instance_records.clear();
    xr_vector<u32> instance_blas; // the structure each instance points at, once it exists
    constexpr u32 cull_none = da_rt_instance_cull_disable;
    for (u32 b = 0; b < g_scene.blases.size(); ++b)
    {
        const DaRtBlas& blas = g_scene.blases[b];
        if (blas.tree)
            continue;
        DaRtInstanceDesc& d = g_scene.instances.emplace_back();
        da_rt_instance(d, Fidentity, blas.first_record, blas.mask, cull_none, 0);
        g_scene.instance_records.push_back({0.f, 1.f, 0.f, 0u});
        instance_blas.push_back(b);
    }
    for (u32 i = 0; i < g_scene.trees.size(); ++i)
    {
        const DaRtTreeItem& item = g_scene.trees[i];
        if (drop_foliage && (item.mask & (kMaskFoliage | kMaskTrunk)))
            continue;
        const DaRtBlas& blas = g_scene.blases[tree_blas[i]];
        const FTreeVisualColors colors = item.visual->colors();
        DaRtInstanceDesc& d = g_scene.instances.emplace_back();
        // A trunk reaches the occlusion rays' candidate loop, which accepts it near the root only.
        da_rt_instance(d, item.visual->world(), blas.first_record, item.mask,
            cull_none | (item.trunk ? da_rt_instance_force_non_opaque : 0u), 0);
        g_scene.instance_records.push_back({item.visual->world().c.y, colors.scale.w, colors.bias.w,
            kInstTree | (item.trunk ? kInstTrunk : 0u) | (item.season_row ? kInstSeason | (u32(item.season_row) << 8) : 0u)});
        instance_blas.push_back(tree_blas[i]);
    }
    // Only the structures an instance names are built (the budget may have left trees out).
    xr_vector<u8> used(g_scene.blases.size(), 0);
    for (const u32 b : instance_blas)
        used[b] = 1;

    // 4. Resources: the structures as built, one scratch, the compacted sizes' way back.
    const u32 count = u32(g_scene.blases.size());
    DaRtBuffer* scratch = nullptr;
    DaRtSizeQuery* sizes = nullptr;
    const auto cleanup = [&] {
        da_rt_release(scratch);
        da_rt_release(sizes);
    };
    scratch = da_rt_buffer_create(scratch_size, "rt build scratch");
    sizes = da_rt_size_query(count);
    bool created = scratch && sizes;
    for (u32 i = 0; created && i < count; ++i)
    {
        if (used[i])
            created = (g_scene.blases[i].uncompacted = da_rt_accel_create(g_scene.blases[i].result_size, false, "rt blas")) != nullptr;
    }
    created = created && da_rt_create_tlas(u32(g_scene.instances.size()) + kMaxDynamicInstances);
    if (!created)
    {
        cleanup();
        Msg("! [rt] scene: the acceleration structure memory could not be allocated");
        return false;
    }

    // 5. The tables and every bottom-level build, the sizes copied back.
    {
        dx11GpuTimerScope gpu(dx11GpuTimers::RtScene);
        DaRtSection section;
        if (!section)
        {
            cleanup();
            return false;
        }
        g_scene.geometry_table = section.upload_buffer(g_scene.records.data(),
            g_scene.records.size() * sizeof(DaRtGeometryRecord), "rt geometry table");
        g_scene.instance_table = section.upload_buffer(g_scene.instance_records.data(),
            std::max<size_t>(g_scene.instance_records.size(), 1) * sizeof(DaRtInstanceRecord), "rt instance table");
        da_rt_require_textures(section);
        da_rt_require_geometry(section);

        u64 offset = 0;
        for (u32 i = 0; i < count; ++i)
        {
            DaRtBlas& blas = g_scene.blases[i];
            if (!used[i])
                continue;
            if (offset + blas.scratch_size > scratch_size)
            {
                // The next batch reuses the scratch.
                section.scratch_barrier(scratch);
                offset = 0;
            }
            da_rt_blas_inputs(blas, inputs);
            DaRtBuild build;
            build.dst = blas.uncompacted;
            build.geometry = inputs.data();
            build.count = u32(inputs.size());
            build.flags = kStaticBlasFlags;
            build.scratch = da_rt_address(scratch) + offset;
            build.sizes = sizes;
            build.size_index = i;
            section.build(build);
            offset += blas.scratch_size;
        }
        section.accel_barrier();
        section.copy_sizes(sizes, count);
    }
    if (!g_scene.geometry_table || !g_scene.instance_table)
    {
        cleanup();
        Msg("! [rt] scene: the tables could not be uploaded");
        return false;
    }
    CTimer wait;
    wait.Start();
    da_rt_flush_wait();
    const float wait_ms = wait.GetElapsed_sec() * 1000.f;

    // 6. Compaction into exact-size buffers, then the top-level structure.
    u64 before = 0, after = 0;
    bool tlas_built = false;
    {
        xr_vector<u64> compacted(count, 0);
        if (!da_rt_read_sizes(sizes, compacted.data(), count))
            std::fill(compacted.begin(), compacted.end(), 0ull);
        dx11GpuTimerScope gpu(dx11GpuTimers::RtScene);
        DaRtSection section;
        if (!section)
        {
            cleanup();
            return false;
        }
        for (u32 i = 0; i < count; ++i)
        {
            DaRtBlas& blas = g_scene.blases[i];
            if (!used[i])
                continue;
            before += blas.result_size;
            const u64 size = da_align(compacted[i], kAsAlign);
            DaRtAccel* target = nullptr;
            if (compacted[i] && size < blas.result_size && (target = da_rt_accel_create(size, false, "rt blas compacted")))
            {
                section.compact(target, blas.uncompacted);
                blas.as = target;
                da_rt_release(blas.uncompacted);
                after += size;
            }
            else
            {
                blas.as = blas.uncompacted;
                blas.uncompacted = nullptr;
                after += blas.result_size;
            }
        }
        section.accel_barrier();
        for (u32 i = 0; i < g_scene.instances.size(); ++i)
            g_scene.instances[i].accel = da_rt_address(g_scene.blases[instance_blas[i]].as);
        tlas_built = da_rt_build_tlas(section, nullptr, 0, false);
    }
    cleanup();
    if (!tlas_built)
    {
        Msg("! [rt] scene: the top-level structure could not be built");
        return false;
    }

    g_scene.memory += after + tables;
    g_scene.binding.tlas = g_scene.tlas_view;
    g_scene.binding.tables[0] = da_rt_address(g_scene.geometry_table);
    g_scene.binding.tables[1] = da_rt_address(g_scene.instance_table);
    g_scene.binding.tables[2] = 0;
    g_scene.binding.tables[3] = 0;
    g_scene.binding.static_instances = u32(g_scene.instances.size());
    g_scene.binding.static_texture_base = g_scene.texture_base;
    // The vegetation's species and season tables, read by the stand-ins' crowns (rt_geometry.hlsli rt_veg_*): its
    // slot keeps the buffer in the tracked set, so every native pass requires its read state after the day's copy.
    {
        ID3D11Buffer* veg = nullptr;
        u32 prof = 0, seas_first = 0, seas = 0;
        float snow = 0.f;
        const bool on = da_veg_rt_tables(veg, prof, seas_first, seas, snow);
        g_scene.binding.veg_tables = on ? da_rt_buffer_slot(veg) : kNone;
        g_scene.binding.veg_prof = prof;
        g_scene.binding.veg_seas = seas_first;
        g_scene.binding.veg_rows = seas;
    }
    u64 triangles = 0;
    for (const DaRtMesh& m : g_scene.meshes)
        triangles += m.index_count / 3;
    Msg("* [rt] scene: %u BLAS, %llu tris, %.1f MB compacted (%.1f before), TLAS %u instances, %u textures, "
        "%.1f ms GPU wait, %.1f ms total",
        count, triangles, after / 1048576.0, before / 1048576.0, u32(g_scene.instances.size()), u32(g_scene.textures.size()),
        wait_ms, timer.GetElapsed_sec() * 1000.f);
    return true;
}

// The scene's textures whose views changed (a reload, a recreation) move to fresh slots holding the new
// views; the old slots go back after the frames in flight are done with them.
void da_rt_repoint_textures()
{
    bool changed = false;
    for (u32 i = 0; i < g_scene.textures.size() && !changed; ++i)
        changed = da_rt_texture_view(g_scene.textures[i]._get()) != g_scene.texture_views[i];
    if (changed && da_rt_write_textures())
        g_scene.binding.static_texture_base = g_scene.texture_base;
    for (auto& [texture, slot] : g_scene.dyn_textures)
    {
        ID3D11ShaderResourceView* view = da_rt_texture_view(texture);
        if (view == slot.view)
            continue;
        const u32 index = da_rt_bindless_alloc(1);
        if (index == da_rt_bindless_invalid)
            continue;
        da_rt_bindless_texture(index, view);
        da_rt_free_bindless(slot.index);
        slot.index = index;
        slot.view = view;
    }
}

// A texture reloaded, recreated or written in place: new views into a fresh range, and the read state.
void da_rt_refresh_textures(DaRtSection& section)
{
    const u64 generation = da_rt_texture_generation();
    if (generation == g_scene.texture_generation)
        return;
    g_scene.texture_generation = generation;
    da_rt_repoint_textures();
    da_rt_require_textures(section);
}

// ---- moving objects -----------------------------------------------------------------------------

struct DaRtFrameDynamic
{
    xr_vector<DaRtInstanceDesc> instances;
    xr_vector<DaRtInstanceRecord> instance_records;
    xr_vector<DaRtGeometryRecord> records;
};

struct DaRtSkinCandidate
{
    dxRender_Visual* child;
    CKinematics* parent;
    CSkeletonX* skeleton;
    Fvisual* mesh;
    Fmatrix world;
    float distance;
    float hemi;
};

struct DaRtRigidCandidate
{
    DaRtMesh mesh;
    const void* model_data; // a model part's shared vertices (null: the engine buffers carry it)
    CSkeletonX* skeleton;
    Fvisual* visual;
    Fmatrix world;
    float hemi;
};

u32 da_rt_dynamic_texture(const ref_texture& t)
{
    CTexture* texture = t._get();
    if (!texture)
        return kNone;
    if (const auto it = g_scene.dyn_textures.find(texture); it != g_scene.dyn_textures.end())
    {
        it->second.last_frame = Device.dwFrame;
        return it->second.index;
    }
    ID3D11ShaderResourceView* view = da_rt_texture_view(texture);
    const u32 index = da_rt_bindless_alloc(1);
    if (index == da_rt_bindless_invalid)
        return kNone;
    da_rt_bindless_texture(index, view);
    g_scene.dyn_textures.emplace(texture, DaRtDynTexture{t, index, view, Device.dwFrame});
    g_scene.dyn_textures_added = true;
    return index;
}

// The index window of a model part (a skeleton child draws from index 0, or from its slide window).
bool da_rt_part_window(Fvisual* v, u32& first, u32& count)
{
    first = 0;
    count = v->dwPrimitives * 3;
    if (auto* pm = dynamic_cast<FProgressive*>(v))
    {
        const FSlideWindowItem& swi = DaRtProgressiveAccess::windows(*pm);
        if (swi.count && swi.sw)
        {
            first = swi.sw[0].offset;
            count = u32(swi.sw[0].num_tris) * 3;
        }
    }
    return count >= 3 && v->p_rm_Indices;
}

// A model part's shared vertex data: the address keys the caches, the checksum and length tell a
// reused address from the data uploaded earlier.
struct DaRtPartData
{
    const void* data{};
    u32 vertices{}, crc{}, length{};
};

DaRtPartData da_rt_part_data(const CSkeletonX& s)
{
    const auto of = [](const auto& v) {
        const smem_value* m = v._get();
        return DaRtPartData{*v, v.size(), m->dwCRC, m->dwLength};
    };
    if (const auto& v = DaRtSkeletonAccess::v1(s); *v)
        return of(v);
    if (const auto& v = DaRtSkeletonAccess::v2(s); *v)
        return of(v);
    if (const auto& v = DaRtSkeletonAccess::v3(s); *v)
        return of(v);
    if (const auto& v = DaRtSkeletonAccess::v4(s); *v)
        return of(v);
    return {};
}

// Bind-pose vertices of a model part in the scene's vertex format (rigid parts) or as the
// pre-skinning source (skinned parts).
xr_vector<u8> da_rt_part_vertices(const CSkeletonX& s, bool skin_source)
{
    const u32 stride = skin_source ? kSkinSourceStride : kPackedStride;
    xr_vector<u8> out;
    const auto emit = [&](u32 index, const Fvector& p, const Fvector& n, float u, float v, const u16 (&bones)[4], const float (&w)[4]) {
        u8* dst = out.data() + size_t(index) * stride;
        if (skin_source)
        {
            float* f = reinterpret_cast<float*>(dst);
            f[0] = p.x, f[1] = p.y, f[2] = p.z, f[3] = n.x, f[4] = n.y, f[5] = n.z, f[6] = u, f[7] = v;
            u32* b = reinterpret_cast<u32*>(dst + 32);
            b[0] = u32(bones[0]) | (u32(bones[1]) << 16);
            b[1] = u32(bones[2]) | (u32(bones[3]) << 16);
            float* weights = reinterpret_cast<float*>(dst + 40);
            for (u32 i = 0; i < 4; ++i)
                weights[i] = w[i];
        }
        else
        {
            float* f = reinterpret_cast<float*>(dst);
            f[0] = p.x, f[1] = p.y, f[2] = p.z;
            *reinterpret_cast<u32*>(dst + 12) = da_rt_oct16(n);
            f[4] = u, f[5] = v;
        }
    };
    if (const auto& v = DaRtSkeletonAccess::v1(s); *v)
    {
        out.resize(size_t(v.size()) * stride);
        for (u32 i = 0; i < v.size(); ++i)
        {
            const vertBoned1W& x = v[i];
            emit(i, x.P, x.N, x.u, x.v, {u16(x.matrix), 0, 0, 0}, {1.f, 0.f, 0.f, 0.f});
        }
    }
    else if (const auto& v2 = DaRtSkeletonAccess::v2(s); *v2)
    {
        out.resize(size_t(v2.size()) * stride);
        for (u32 i = 0; i < v2.size(); ++i)
        {
            const vertBoned2W& x = v2[i];
            emit(i, x.P, x.N, x.u, x.v, {x.matrix0, x.matrix1, 0, 0}, {1.f - x.w, x.w, 0.f, 0.f});
        }
    }
    else if (const auto& v3 = DaRtSkeletonAccess::v3(s); *v3)
    {
        out.resize(size_t(v3.size()) * stride);
        for (u32 i = 0; i < v3.size(); ++i)
        {
            const vertBoned3W& x = v3[i];
            emit(i, x.P, x.N, x.u, x.v, {x.m[0], x.m[1], x.m[2], 0}, {x.w[0], x.w[1], 1.f - x.w[0] - x.w[1], 0.f});
        }
    }
    else if (const auto& v4 = DaRtSkeletonAccess::v4(s); *v4)
    {
        out.resize(size_t(v4.size()) * stride);
        for (u32 i = 0; i < v4.size(); ++i)
        {
            const vertBoned4W& x = v4[i];
            emit(i, x.P, x.N, x.u, x.v, {x.m[0], x.m[1], x.m[2], x.m[3]},
                {x.w[0], x.w[1], x.w[2], 1.f - x.w[0] - x.w[1] - x.w[2]});
        }
    }
    return out;
}

DaRtModelBuffer* da_rt_model_buffer(DaRtSection& section, const CSkeletonX& s, bool skin_source)
{
    const DaRtPartData part = da_rt_part_data(s);
    if (!part.data || !part.vertices)
        return nullptr;
    auto& map = skin_source ? g_scene.skin_sources : g_scene.model_buffers;
    DaRtModelBuffer& entry = map[part.data];
    entry.last_frame = Device.dwFrame;
    if (entry.uploaded && (entry.crc != part.crc || entry.length != part.length))
    {
        da_rt_release(entry.buffer);
        da_rt_free_bindless(entry.bindless);
        entry.uploaded = false;
    }
    if (entry.uploaded)
    {
        // The bindless range was full at the upload.
        if (entry.buffer && !skin_source && entry.bindless == kNone)
            entry.bindless = da_rt_native_slot(entry.buffer, da_align(u64(entry.vertices) * kPackedStride, 256));
        return entry.buffer ? &entry : nullptr;
    }
    entry.uploaded = true;
    entry.crc = part.crc;
    entry.length = part.length;
    entry.serial = ++g_scene.upload_serial;
    const xr_vector<u8> data = da_rt_part_vertices(s, skin_source);
    entry.buffer = section.upload_buffer(data.data(), data.size(), skin_source ? "rt skin source" : "rt model vertices");
    if (!entry.buffer)
        return nullptr;
    entry.vertices = part.vertices;
    if (!skin_source)
        entry.bindless = da_rt_native_slot(entry.buffer, da_align(data.size(), 256));
    g_scene.memory += data.size();
    return &entry;
}

bool da_rt_dyn_scratch(u64 size)
{
    if (size <= g_scene.dyn_scratch_size)
        return true;
    da_rt_release(g_scene.dyn_scratch);
    g_scene.dyn_scratch_size = 0;
    const u64 grown = da_align(std::max<u64>(size, 8ull << 20), kAsAlign);
    if (!(g_scene.dyn_scratch = da_rt_buffer_create(grown, "rt dynamic scratch")))
        return false;
    g_scene.dyn_scratch_size = grown;
    return true;
}

// Builds one rigid structure over a scratch region; false when the frame's scratch is used up.
bool da_rt_build_rigid(DaRtSection& section, DaRtDynBlas& entry, const DaRtTriangles& geometry, u64& scratch_used)
{
    DaRtSizes sizes;
    if (!da_rt_sizes(&geometry, 1, DaRtBuildFastTrace, sizes))
        return false;
    const u64 scratch = da_align(sizes.build, kAsAlign);
    if (scratch_used + scratch > g_scene.dyn_scratch_size)
        return false;
    if (!(entry.as = da_rt_accel_create(da_align(sizes.result, kAsAlign), false, "rt dynamic blas")))
        return false;
    DaRtBuild build;
    build.dst = entry.as;
    build.geometry = &geometry;
    build.count = 1;
    build.flags = DaRtBuildFastTrace;
    build.scratch = da_rt_address(g_scene.dyn_scratch) + scratch_used;
    section.build(build);
    scratch_used += scratch;
    entry.built = true;
    g_scene.memory += sizes.result;
    return true;
}

void da_rt_collect_dynamic(xr_vector<DaRtRigidCandidate>& rigid, xr_vector<DaRtSkinCandidate>& skinned, const DaRtTier& tier)
{
    const Fvector& camera = Device.vCameraPosition;
    xr_vector<ISpatial*> found;
    g_pGamePersistent->SpatialSpace.q_sphere(found, 0, STYPE_RENDERABLE, camera, tier.rigid_radius);
    IGameObject* viewer = g_pGameLevel ? g_pGameLevel->CurrentViewEntity() : nullptr;

    struct Walk
    {
        xr_vector<DaRtRigidCandidate>& rigid;
        xr_vector<DaRtSkinCandidate>& skinned;
        const DaRtTier& tier;
        float distance, hemi;

        void mesh(dxRender_Visual* v, const Fmatrix& world)
        {
            auto* f = dynamic_cast<Fvisual*>(v);
            ShaderElement* element = da_rt_scene_element(v);
            if (!f || !element || dynamic_cast<CSkeletonX*>(v))
                return;
            DaRtRigidCandidate c{};
            if (!da_rt_engine_mesh(f, element, c.mesh))
                return;
            c.visual = f;
            c.world = world;
            c.hemi = hemi;
            rigid.push_back(std::move(c));
        }

        void part(dxRender_Visual* child, CKinematics* parent, const Fmatrix& world)
        {
            auto* skeleton = dynamic_cast<CSkeletonX*>(child);
            auto* f = dynamic_cast<Fvisual*>(child);
            ShaderElement* element = da_rt_scene_element(child);
            if (!skeleton || !f || !element)
                return;
            if (DaRtSkeletonAccess::single(*skeleton))
            {
                // A part riding one bone: its bind-pose vertices, instanced with the bone's world.
                const u16 bone = u16(DaRtSkeletonAccess::bone(*skeleton));
                if (bone >= parent->LL_BoneCount())
                    return;
                DaRtRigidCandidate c{};
                c.world.mul_43(world, parent->LL_GetTransform_R(bone));
                c.skeleton = skeleton;
                c.visual = f;
                c.hemi = hemi;
                c.model_data = da_rt_part_data(*skeleton).data;
                if (!c.model_data)
                    return;
                c.mesh.albedo = da_rt_albedo(element);
                c.mesh.flags = (da_rt_alpha_tested(element) ? kGeomAlpha : 0u) | (element->flags.bEmissive ? kGeomEmissive : 0u);
                rigid.push_back(std::move(c));
            }
            else if (distance <= tier.skinned_radius)
                skinned.push_back({child, parent, skeleton, f, world, distance, hemi});
        }

        void visual(dxRender_Visual* v, const Fmatrix& world)
        {
            switch (v->Type)
            {
            case MT_HIERRARHY:
                for (dxRender_Visual* child : static_cast<FHierrarhyVisual*>(v)->children)
                    visual(child, world);
                return;
            case MT_SKELETON_ANIM:
            case MT_SKELETON_RIGID:
            {
                CKinematics* k = PCKinematics(v);
                if (!k)
                    return;
                for (dxRender_Visual* child : k->children)
                    part(child, k, world);
                return;
            }
            case MT_NORMAL:
            case MT_PROGRESSIVE: mesh(v, world); return;
            default: return;
            }
        }
    };

    for (ISpatial* spatial : found)
    {
        IRenderable* renderable = spatial->dcast_Renderable();
        if (!renderable || (viewer && spatial->dcast_GameObject() == viewer))
            continue;
        const RenderData& data = renderable->GetRenderData();
        if (!data.visual || data.hud || data.invisible)
            continue;
        auto* v = static_cast<dxRender_Visual*>(data.visual);
        const float distance = camera.distance_to(data.xform.c);
        float hemi = 0.5f;
        if (data.pROS)
            hemi = 0.75f * static_cast<CROS_impl*>(data.pROS)->get_hemi();
        Walk walk{rigid, skinned, tier, distance, hemi};
        walk.visual(v, data.xform);
    }
    // The nearest skinned_max characters, each with every part: a model's parts sort together (one
    // distance, then the model) and the cut falls between two models.
    std::sort(skinned.begin(), skinned.end(), [](const DaRtSkinCandidate& a, const DaRtSkinCandidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : std::less<CKinematics*>()(a.parent, b.parent);
    });
    u32 characters = 0;
    size_t keep = 0;
    for (; keep < skinned.size(); ++keep)
    {
        if ((!keep || skinned[keep].parent != skinned[keep - 1].parent) && ++characters > tier.skinned_max)
            break;
    }
    skinned.resize(keep);
}

// The moving objects of this frame into the structures, the tables and the top-level build.
void da_rt_update_dynamic(DaRtSection& section)
{
    const DaRtTier& tier = da_rt_tier();
    xr_vector<DaRtRigidCandidate> rigid;
    xr_vector<DaRtSkinCandidate> skin;
    da_rt_collect_dynamic(rigid, skin, tier);

    DaRtFrameDynamic frame;
    const u32 frame_index = Device.dwFrame;
    u64 scratch_used = 0;
    u32 builds = 0;
    // Every new structure this frame shares one scratch: sized for the frame's worst case.
    da_rt_dyn_scratch(32ull << 20);

    const auto add_instance = [&](const Fmatrix& world, const DaRtGeometryRecord& record, u8 mask, float hemi, DaRtAddress as) {
        // A record without its buffers' slots (the bindless range was full) is left out, not traced.
        if (frame.instances.size() >= kMaxDynamicInstances || record.vb == kNone || record.ib == kNone || !as)
            return;
        const u32 id = kInstanceDynamic | u32(frame.records.size());
        frame.records.push_back(record);
        DaRtInstanceDesc& d = frame.instances.emplace_back();
        da_rt_instance(d, world, id, mask, da_rt_instance_cull_disable, as);
        frame.instance_records.push_back({world.c.y, 0.f, hemi, 0u});
    };

    // Rigid meshes: built once, instanced with this frame's world. Their buffers are looked up every
    // frame the entry is used, so they live at least as long as it does.
    for (DaRtRigidCandidate& c : rigid)
    {
        DaRtMesh mesh = c.mesh;
        u32 vb_bindless = kNone;
        DaRtAddress vb = 0;
        u64 source = 0;
        if (c.model_data)
        {
            DaRtModelBuffer* buffer = da_rt_model_buffer(section, *c.skeleton, false);
            u32 first = 0, count = 0;
            if (!buffer || !da_rt_part_window(c.visual, first, count))
                continue;
            mesh.vb = nullptr;
            mesh.ib = *c.visual->p_rm_Indices;
            mesh.stride = kPackedStride;
            mesh.vb_offset = 0;
            mesh.vertex_count = buffer->vertices;
            mesh.ib_offset = first * 2;
            mesh.index_count = count;
            mesh.layout = kPackedLayout;
            mesh.layout2 = kPackedLayout2;
            vb_bindless = buffer->bindless;
            vb = da_rt_address(buffer->buffer);
            source = buffer->serial;
        }
        else
        {
            vb_bindless = da_rt_buffer_slot(mesh.vb, true);
            vb = da_rt_engine_address(mesh.vb);
        }
        DaRtMeshKey key = da_rt_key(mesh);
        if (c.model_data)
            key.vb = c.model_data;
        key.source = source;
        const DaRtAddress ib = da_rt_engine_address(mesh.ib);
        const u32 ib_bindless = da_rt_buffer_slot(mesh.ib, true);
        if (!vb || !ib || vb_bindless == kNone || ib_bindless == kNone)
            continue;
        DaRtDynBlas& entry = g_scene.dyn_blas[key];
        entry.last_frame = frame_index;
        if (!entry.built)
        {
            if (builds >= tier.blas_per_frame)
                continue;
            // A buffer registered just now may be a tracked one.
            da_rt_require_geometry(section);
            const DaRtTriangles g = da_rt_triangles(vb + mesh.vb_offset, mesh.stride, mesh.vertex_count, ib + mesh.ib_offset,
                mesh.index_count, mesh.flags & kGeomAlpha);
            if (!da_rt_build_rigid(section, entry, g, scratch_used))
                continue;
            ++builds;
            entry.record.vb_offset = mesh.vb_offset;
            entry.record.ib_offset = mesh.ib_offset;
            entry.record.layout = mesh.layout;
            entry.record.layout2 = mesh.layout2;
        }
        if (!entry.as)
            continue;
        DaRtGeometryRecord record = entry.record;
        record.vb = vb_bindless;
        record.ib = ib_bindless;
        record.albedo = da_rt_dynamic_texture(mesh.albedo);
        record.flags = mesh.flags | kGeomAbsolute;
        add_instance(c.world, record, kMaskDynamic, c.hemi, da_rt_address(entry.as));
    }

    // Characters: pre-skinned into their own vertices, the structure refitted (rebuilt every 30 frames).
    const DaRtPipeline skin_pipeline = da_rt_skin_pipeline();
    struct Refit
    {
        DaRtSkinned* entry;
        DaRtTriangles geometry;
        bool update;
    };
    xr_vector<Refit> refits;
    for (const DaRtSkinCandidate& c : skin)
    {
        if (!skin_pipeline)
            break;
        DaRtModelBuffer* source = da_rt_model_buffer(section, *c.skeleton, true);
        u32 first = 0, count = 0;
        if (!source || !da_rt_part_window(c.mesh, first, count))
            continue;
        ID3D11Buffer* index_buffer = *c.mesh->p_rm_Indices;
        const DaRtAddress ib = da_rt_engine_address(index_buffer);
        // Looked up every frame the entry is used, so the slots live at least as long as it does.
        const u32 ib_bindless = da_rt_buffer_slot(index_buffer, true);
        ShaderElement* element = da_rt_scene_element(c.child);
        const u32 albedo = da_rt_dynamic_texture(da_rt_albedo(element));
        if (!ib || ib_bindless == kNone)
            continue;
        DaRtSkinned& entry = g_scene.skinned[c.child];
        entry.last_frame = frame_index;
        // Another part at a reused visual address: a structure sized and built anew, never refitted.
        if (entry.source != source->serial || entry.index_buffer != index_buffer || entry.first != first || entry.count != count)
        {
            da_rt_release(entry.as);
            entry.built = false;
            entry.source = source->serial;
            entry.index_buffer = index_buffer;
            entry.first = first;
            entry.count = count;
        }
        if (!entry.out || entry.vertices != source->vertices)
        {
            da_rt_release(entry.out);
            da_rt_release(entry.as);
            da_rt_free_bindless(entry.out_bindless);
            entry.built = false;
            const u64 bytes = da_align(u64(source->vertices) * kPackedStride, 256);
            if (!(entry.out = da_rt_buffer_create(bytes, "rt skinned vertices")))
                continue;
            entry.vertices = source->vertices;
            entry.out_bindless = da_rt_native_slot(entry.out, bytes);
            g_scene.memory += bytes;
        }
        // The bindless range was full when the vertices were made (add_instance skips kNone).
        if (entry.out_bindless == kNone)
            entry.out_bindless = da_rt_native_slot(entry.out, da_align(u64(entry.vertices) * kPackedStride, 256));
        entry.record.vb = entry.out_bindless;
        entry.record.vb_offset = 0;
        entry.record.ib = ib_bindless;
        entry.record.ib_offset = first * 2;
        entry.record.layout = kPackedLayout;
        entry.record.layout2 = kPackedLayout2;
        entry.record.albedo = albedo;
        entry.record.flags = kGeomAbsolute | (da_rt_alpha_tested(element) ? kGeomAlpha : 0u);
        // This frame's palette, as SkeletonX uploads sbones_array.
        const u32 bones = c.parent->LL_BoneCount();
        const DaRtUpload palette = section.upload(u64(bones) * 48, 16);
        if (!palette.cpu || !bones)
            continue;
        auto* rows = static_cast<Fvector4*>(palette.cpu);
        for (u32 b = 0; b < bones; ++b)
        {
            const Fmatrix& m = c.parent->LL_GetTransform_R(u16(b));
            rows[b * 3 + 0].set(m._11, m._21, m._31, m._41);
            rows[b * 3 + 1].set(m._12, m._22, m._32, m._42);
            rows[b * 3 + 2].set(m._13, m._23, m._33, m._43);
        }
        const u32 constants[4] = {entry.vertices, bones, 0, 0};
        DaRtDispatch d;
        d.pipeline = skin_pipeline;
        d.constants = constants;
        d.constants_size = sizeof(constants);
        d.tables[0] = da_rt_address(source->buffer);
        d.tables[1] = palette.address;
        d.raw_output = da_rt_address(entry.out);
        d.groups_x = (entry.vertices + 63) / 64;
        if (!section.dispatch(d))
            continue;
        Refit& r = refits.emplace_back();
        r.entry = &entry;
        r.geometry = da_rt_triangles(da_rt_address(entry.out), kPackedStride, entry.vertices, ib + first * 2, count, false);
        r.update = entry.built && frame_index - entry.built_frame < kSkinRebuildFrames;
    }
    if (!refits.empty())
    {
        da_rt_require_geometry(section);
        for (const Refit& r : refits)
            section.written(r.entry->out);
        constexpr u32 skinned_flags = DaRtBuildFastBuild | DaRtBuildAllowUpdate;
        for (Refit& r : refits)
        {
            DaRtSkinned& entry = *r.entry;
            if (!entry.as)
            {
                DaRtSizes sizes;
                if (!da_rt_sizes(&r.geometry, 1, skinned_flags, sizes))
                    continue;
                entry.as_size = da_align(sizes.result, kAsAlign);
                entry.build_scratch = da_align(sizes.build, kAsAlign);
                entry.update_scratch = da_align(std::max<u64>(sizes.update, 256), kAsAlign);
                if (!(entry.as = da_rt_accel_create(entry.as_size, false, "rt skinned blas")))
                    continue;
                g_scene.memory += entry.as_size;
                r.update = false;
            }
            const u64 scratch = r.update ? entry.update_scratch : entry.build_scratch;
            if (scratch_used + scratch > g_scene.dyn_scratch_size)
                continue;
            DaRtBuild build;
            build.dst = entry.as;
            build.src = entry.as;
            build.geometry = &r.geometry;
            build.count = 1;
            build.flags = skinned_flags;
            build.update = r.update;
            build.scratch = da_rt_address(g_scene.dyn_scratch) + scratch_used;
            section.build(build);
            scratch_used += scratch;
            if (!r.update)
            {
                entry.built = true;
                entry.built_frame = frame_index;
            }
        }
    }
    for (const DaRtSkinCandidate& c : skin)
    {
        const auto it = g_scene.skinned.find(c.child);
        if (it == g_scene.skinned.end() || !it->second.built || it->second.last_frame != frame_index)
            continue;
        add_instance(c.world, it->second.record, kMaskSkinned, c.hemi, da_rt_address(it->second.as));
    }

    // Engine buffers first seen this frame, and every tracked one again (they decay at each submission);
    // the textures when one joined.
    da_rt_require_geometry(section);
    if (g_scene.dyn_textures_added)
    {
        da_rt_require_textures(section);
        g_scene.dyn_textures_added = false;
    }
    // The per-frame tables live in this frame's upload memory. Written before the top level names
    // their records: without them it takes the static instances alone.
    g_scene.binding.tables[2] = 0;
    g_scene.binding.tables[3] = 0;
    u32 dynamic_count = 0;
    if (!frame.records.empty())
    {
        const u64 rb = frame.records.size() * sizeof(DaRtGeometryRecord);
        const u64 ib = frame.instance_records.size() * sizeof(DaRtInstanceRecord);
        const DaRtUpload records = section.upload(rb, 16);
        const DaRtUpload instances = section.upload(ib, 16);
        if (records.cpu && instances.cpu)
        {
            std::memcpy(records.cpu, frame.records.data(), rb);
            std::memcpy(instances.cpu, frame.instance_records.data(), ib);
            g_scene.binding.tables[2] = records.address;
            g_scene.binding.tables[3] = instances.address;
            dynamic_count = u32(frame.instances.size());
        }
    }
    // Every structure written above is complete before the top level reads it.
    section.accel_barrier();
    da_rt_build_tlas(section, frame.instances.data(), dynamic_count, true);
}

// Structures nobody used for a while go (after the GPU is done with them).
void da_rt_evict_dynamic()
{
    const u32 now = Device.dwFrame;
    for (auto it = g_scene.dyn_blas.begin(); it != g_scene.dyn_blas.end();)
    {
        if (now - it->second.last_frame > kDynamicEvictFrames)
        {
            da_rt_release(it->second.as);
            it = g_scene.dyn_blas.erase(it);
        }
        else
            ++it;
    }
    for (auto it = g_scene.skinned.begin(); it != g_scene.skinned.end();)
    {
        if (now - it->second.last_frame > kDynamicEvictFrames)
        {
            da_rt_release(it->second.out);
            da_rt_release(it->second.as);
            da_rt_free_bindless(it->second.out_bindless);
            it = g_scene.skinned.erase(it);
        }
        else
            ++it;
    }
    for (auto* map : {&g_scene.model_buffers, &g_scene.skin_sources})
    {
        for (auto it = map->begin(); it != map->end();)
        {
            if (now - it->second.last_frame > kDynamicEvictFrames)
            {
                da_rt_release(it->second.buffer);
                da_rt_free_bindless(it->second.bindless);
                it = map->erase(it);
            }
            else
                ++it;
        }
    }
    // The records naming these slots were evicted with them (looked up every frame they are used).
    for (auto it = g_scene.dyn_textures.begin(); it != g_scene.dyn_textures.end();)
    {
        if (now - it->second.last_frame > kDynamicEvictFrames)
        {
            da_rt_free_bindless(it->second.index);
            it = g_scene.dyn_textures.erase(it);
        }
        else
            ++it;
    }
    for (auto it = g_scene.buffers.begin(); it != g_scene.buffers.end();)
    {
        if (it->second.held && now - it->second.last_frame > kDynamicEvictFrames)
        {
            da_rt_free_bindless(it->second.index);
            std::erase(g_scene.tracked_buffers, static_cast<ID3D11Resource*>(it->first));
            it->first->Release();
            it = g_scene.buffers.erase(it);
        }
        else
            ++it;
    }
}
} // namespace
#endif

void da_rt_scene_release()
{
#if defined(DA_RT_LANE)
    // The level stays: its walk is made again when an effect comes back on.
    const bool level = g_scene.level;
    if (!HW.pDevice)
    {
        g_scene = {};
        g_scene.level = level;
        return;
    }
    for (DaRtBlas& blas : g_scene.blases)
    {
        da_rt_release(blas.as);
        da_rt_release(blas.uncompacted);
    }
    for (auto& [key, entry] : g_scene.dyn_blas)
        da_rt_release(entry.as);
    for (auto& [child, entry] : g_scene.skinned)
    {
        da_rt_release(entry.out);
        da_rt_release(entry.as);
        da_rt_free_bindless(entry.out_bindless);
    }
    for (auto* map : {&g_scene.model_buffers, &g_scene.skin_sources})
    {
        for (auto& [key, entry] : *map)
        {
            da_rt_release(entry.buffer);
            da_rt_free_bindless(entry.bindless);
        }
    }
    for (auto& [texture, slot] : g_scene.dyn_textures)
        da_rt_free_bindless(slot.index);
    for (auto& [buffer, slot] : g_scene.buffers)
    {
        da_rt_free_bindless(slot.index);
        if (slot.held)
            buffer->Release();
    }
    da_rt_free_bindless(g_scene.texture_base, u32(g_scene.textures.size()));
    da_rt_release(g_scene.geometry_table);
    da_rt_release(g_scene.instance_table);
    // The view holds the structure's storage on Vulkan: it goes first.
    if (g_scene.tlas_view)
        g_scene.tlas_view->Release();
    da_rt_release(g_scene.tlas);
    da_rt_release(g_scene.tlas_scratch);
    da_rt_release(g_scene.dyn_scratch);
    g_scene = {};
    g_scene.level = level;
#endif
}

void da_rt_textures_replaced()
{
#if defined(DA_RT_LANE)
    // The read state of the new textures waits for the next frame's refresh: their writes moved the
    // generation it compares.
    if (g_scene.ready)
        da_rt_repoint_textures();
#endif
}

void da_rt_frame_scene(CBackend& cmd_list)
{
    UNUSED(cmd_list);
#if defined(DA_RT_LANE)
    if (!da_rt_available() || !g_scene.level)
        return;
    if (!da_rt_wanted())
    {
        // Every effect went off: the memory goes back, the level's walk with it.
        if (g_scene.ready)
            Msg("* [rt] scene freed: every ray traced effect is off");
        if (g_scene.ready || g_scene.collected)
            da_rt_scene_release();
        return;
    }
    if (!g_scene.ready)
    {
        if (g_scene.refused)
            return;
        if (!g_scene.collected)
            da_rt_collect();
        g_scene.ready = da_rt_scene_build();
        if (!g_scene.ready)
        {
            da_rt_scene_release();
            g_scene.refused = true;
            Msg("! [rt] no ray traced effects on this level");
        }
        return;
    }

    // Moving objects only matter to the occlusion and the reflections; the shadow rays mask them out.
    const bool track = da_rt_live(RtEffect::AO) || da_rt_live(RtEffect::Reflections);
    dx11GpuTimerScope gpu(dx11GpuTimers::RtScene);
    DaRtSection section;
    if (!section)
        return;
    da_rt_refresh_textures(section);
    if (track)
        da_rt_update_dynamic(section);
    else
    {
        g_scene.binding.tables[2] = 0;
        g_scene.binding.tables[3] = 0;
        if (g_scene.tlas_dynamic)
            da_rt_build_tlas(section, nullptr, 0, false);
    }
    // Eviction releases after the GPU is done; a structure the top level still names was just
    // replaced by this frame's build.
    da_rt_evict_dynamic();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
