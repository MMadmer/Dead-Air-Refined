// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The vegetation core (docs/dead-air/TREES.md, SHRUBS.md).
//
// Level load: one census of the stock models (every FLOD of tree parts under the sector hierarchies, keyed from
// level.geom), then each population - the shrubs, the trees - takes the models its data names, grows its templates
// and detaches the replaced FLODs. Per frame (render thread, before the G-buffer): the season tables when the day
// moves, each population's instances by a grid walk with frustum and occlusion tests (the view near to far, the sun
// cascades by their reach, the rain map), each population choosing its tiers and emitting chunks of up to 64 elements
// of one kind of one instance; one bend pass over the frame's chunks; the G-buffer and each shadow map draw each
// element kind once per population, instanced over its chunks, the vertex shaders reading the records.

#include "stdafx.h"
#include "da_veg.h"
#include "da_hzb.h"
#include "da_veg_internal.h"
#include "da_reeds.h"
#include "da_grass_data.h"
#include "da_temporal.h"
#include "da_wind_history.h"
#include "FTreeVisual.h"
#include "FHierrarhyVisual.h"
#include "FLOD.h"
#include "BufferUtils.h"
#include "ResourceManager.h"
#include "xrRender_console.h"
#include "r__sector.h"
#include "xrEngine/da_season.h"
#include "xrEngine/da_snow_field.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "Common/LevelStructure.hpp"
#include "xrCore/stream_reader.h"
#include "xrCommon/xr_unordered_map.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderPC_R4/da_gpu_trees.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "Layers/xrRenderPC_R4/da_rt.h"

// The wind field's maths, shared with the vertex shaders (one file, two compilers).
#include "../../../packaging/dead-air-x64/compatibility/gamedata/shaders/r3/da_wind_core.h"

#include <DirectXPackedVector.h>
#include <bit>
#include <unordered_map>

namespace xray::render::RENDER_NAMESPACE
{
// The populations (da_shrubs.cpp, da_trees.cpp).
namespace da_veg
{
Population* shrubs_population();
Population* trees_population();
} // namespace da_veg

namespace
{
using namespace da_veg;
using namespace DirectX;

// The frame's bends (r3\da_shrub_bend.cs): a compute pass, one group a chunk.
class CBlender_da_shrub_compute : public IBlender
{
    pcstr m_cs;

public:
    CBlender_da_shrub_compute(pcstr cs) : m_cs(cs) {}
    LPCSTR getComment() override { return "INTERNAL: vegetation, compute"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass(m_cs);
        C.r_End();
    }
};

// The draws: the G-buffer pass (stencil like every deferred surface, two-sided) and the shadow maps (depth, with a
// pixel shader where the element is cut out of its quad).
class CBlender_da_shrub : public IBlender
{
    pcstr m_vs, m_ps;
    bool m_shadow;

public:
    CBlender_da_shrub(pcstr vs, pcstr ps, bool shadow) : m_vs(vs), m_ps(ps), m_shadow(shadow) {}
    LPCSTR getComment() override { return "INTERNAL: vegetation"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        if (m_shadow)
            C.r_Pass(m_vs, m_ps, FALSE, TRUE, TRUE, FALSE);
        else
        {
            C.r_Pass(m_vs, m_ps, FALSE, TRUE, TRUE);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
        }
        // The leaf and bark texture arrays (tools/shrubs/textures); a shader binds the ones it declares.
        C.r_dx11Texture("s_shrub_leaf_d", "da\\shrub_leaf_d");
        C.r_dx11Texture("s_shrub_leaf_n", "da\\shrub_leaf_n");
        C.r_dx11Texture("s_shrub_bark_d", "da\\shrub_bark_d");
        C.r_dx11Texture("s_shrub_bark_n", "da\\shrub_bark_n");
        C.r_dx11Sampler("smp_base");
        // (The leaves' cut-out asks the isotropic level of detail through it: r3\da_shrub_leaf_ps.h.)
        C.r_dx11Sampler("smp_linear");
        C.r_CullMode(D3DCULL_NONE);
        C.r_End();
    }
};

// ======================================================================================================
// State
// ======================================================================================================
// A shadowed local light of the frame: each population's chunks after the frame's, its side for the quads.
struct LightDraw
{
    const void* key{};
    Fvector to_light{};
    float texel{};
    u32 first[kPops][drCount]{}, count[kPops][drCount]{};
};

// A run of the frame's chunks of one population (the bend pass binds that population's buffers).
struct BendBlock
{
    u32 pop, first, count;
};

struct State
{
    bool gpu{}, failed{};
    u32 preset{2};
    Census census;
    ref_shader s_draw[drCount], s_shadow[drCount], s_bend;
    GpuBuffer inst_b, chunks_b, tables_b, dummy_b;
    // The frame's bends (kBendRec bytes a chunk slot): bend_chunks chunks' slots of this frame, then bend_view_chunks of
    // the view a frame back; grown in Calculate only (the cascades' tasks bind it from then on), never to fail twice on
    // a level. bend_view_end: the view's chunks this frame (they come first); bend_ready: the frame's records are to be
    // written (da_veg_bend).
    GpuBuffer bend_b;
    u32 bend_chunks{}, bend_view_chunks{}, bend_view_end{};
    // The bend slots the frame's chunks took (a chunk that bends takes the next one, its keep word's top half: the rigid
    // ones - stock tubes, patches, blobs, hundreds a near tree - none), the view's of them.
    u32 bend_n{}, bend_view_n{};
    bool bend_failed{}, bend_ready{};
    xr_vector<BendBlock> bend_blocks;
    // The trees' GPU cut (r3\da_tree_cut.cs, da_tree_cut_args.cs): its candidate chunks in the frame's chunk buffer, their
    // routes, the words each draw took and its draws' arguments; each draw's list (its first word past the chunk records,
    // its room in whole instances, the vertex class's indices an instance and first index) and the room its candidates
    // asked so far, by phase (the near trees, the far ones: kCutPhases); the frustum of the view (world planes, outward);
    // QA: a readback of the counts (1 asked, 2 copied).
    ref_shader s_cut, s_cut_args;
    GpuBuffer cut_routes_b, cut_counts_b, cut_args_b;
    ID3D11Buffer* cut_staging{};
    u32 cut_first{}, cut_count{}, cut_readback{};
    u32 cut_n[kCutPhases]{};
    u32 cut_reg[kCutPhases][drCount][4]{}, cut_room[kCutPhases][drCount]{};
    Fvector4 cut_planes[6]{};
    // The candidate chunks the cut took this frame (every population's: their routes share one buffer).
    u32 cut_chunk_total{};
    VertexStagingBuffer vb;
    IndexStagingBuffer ib;
    ref_geom geom;
    VertexClassInfo vclass[vcCount]{};
    Latch latch;
    Fvector4 tables[kTabTotal]{};
    float tab_day{-1000.f};
    bool tab_upload{};
    // The frame: the records of its instances, the chunks of every pass, draw and population.
    u32 records{};
    xr_vector<u8> inst_records;
    xr_vector<ChunkRec> chunks;
    xr_vector<LightDraw> lights;
    // Both light packages of a frame (the normal and the occlusion-pending one) add to the list; the stats count the
    // shadowed lights offered and those near enough.
    u32 lights_frame = u32(-1), lights_offered{}, lights_near{};
    double clock{}; // the shaders' wrapped clock, s
    // The sun cascades that asked for the vegetation before this frame's build (the frame is built before the sun task
    // starts: anything but 0 is a broken order).
    std::atomic<u32> stale_passes{};
};
State g;
// The time the render thread waits for the frame's pick (da_veg_frame_prepare; r__gpu_log veg_wait: the share of the
// pick the render thread pays).
std::atomic<u64> g_wait_ticks{};
std::atomic<u32> g_wait_frames{};

Population* pop(u32 i) { return i == kPopShrubs ? shrubs_population() : i == kPopTrees ? trees_population() : nullptr; }

bool pop_on(const Population* P) { return P && P->active && P->detached; }

// One stretch of a population's frame work on the CPU, into its r__gpu_log field.
struct CpuSpan
{
    Population& P;
    const u64 t0 = CPU::QPC();
    explicit CpuSpan(Population& p) : P(p) {}
    ~CpuSpan() { P.cpu_ticks.fetch_add(CPU::QPC() - t0, std::memory_order_relaxed); }
};
} // namespace

// ======================================================================================================
// Services
// ======================================================================================================
namespace da_veg
{
Population* population(u32 index) { return pop(index); }

bool make_buffer(GpuBuffer& b, u32 bytes, const void* init, bool uav, bool srv, u32 misc)
{
    b.release();
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = std::max((bytes + 15) & ~15u, 16u);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = (srv ? D3D11_BIND_SHADER_RESOURCE : 0u) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | misc;
    xr_vector<u8> padded;
    D3D11_SUBRESOURCE_DATA data{};
    if (init)
    {
        padded.assign(desc.ByteWidth, 0);
        CopyMemory(padded.data(), init, bytes);
        data.pSysMem = padded.data();
    }
    if (FAILED(HW.pDevice->CreateBuffer(&desc, init ? &data : nullptr, &b.buf)))
        return false;
    b.bytes = desc.ByteWidth;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    if (srv && FAILED(HW.pDevice->CreateShaderResourceView(b.buf, &view, &b.srv)))
        return false;
    if (!uav)
        return true;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uview{};
    uview.Format = DXGI_FORMAT_R32_TYPELESS;
    uview.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uview.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    uview.Buffer.NumElements = desc.ByteWidth / 4;
    return SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(b.buf, &uview, &b.uav));
}

const Latch& latch() { return g.latch; }

float smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float box_distance(const Fbox& b, const Fvector& p)
{
    const float dx = std::max({b.vMin.x - p.x, 0.f, p.x - b.vMax.x}), dy = std::max({b.vMin.y - p.y, 0.f, p.y - b.vMax.y});
    const float dz = std::max({b.vMin.z - p.z, 0.f, p.z - b.vMax.z});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

u32 f16(float v) { return PackedVector::XMConvertFloatToHalf(v); }

u32 oct(const da_shrub::V3& n)
{
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    float px = l1 > 0.f ? n.x / l1 : 0.f, pz = l1 > 0.f ? n.z / l1 : 0.f;
    if (n.y < 0.f)
    {
        const float ox = (1.f - std::abs(pz)) * (px >= 0.f ? 1.f : -1.f);
        const float oz = (1.f - std::abs(px)) * (pz >= 0.f ? 1.f : -1.f);
        px = ox;
        pz = oz;
    }
    return sn16(px) | (sn16(pz) << 16);
}

u32 oct12(const da_shrub::V3& n)
{
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    float px = l1 > 0.f ? n.x / l1 : 0.f, pz = l1 > 0.f ? n.z / l1 : 0.f;
    if (n.y < 0.f)
    {
        const float ox = (1.f - std::abs(pz)) * (px >= 0.f ? 1.f : -1.f);
        const float oz = (1.f - std::abs(px)) * (pz >= 0.f ? 1.f : -1.f);
        px = ox;
        pz = oz;
    }
    const auto q = [](float v) { return u32(std::lround((std::clamp(v, -1.f, 1.f) * 0.5f + 0.5f) * 4095.f)); };
    return q(px) | (q(pz) << 12);
}

u32 oct8(const da_shrub::V3& n)
{
    const float l1 = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    float px = l1 > 0.f ? n.x / l1 : 0.f, pz = l1 > 0.f ? n.z / l1 : 0.f;
    if (n.y < 0.f)
    {
        const float ox = (1.f - std::abs(pz)) * (px >= 0.f ? 1.f : -1.f);
        const float oz = (1.f - std::abs(px)) * (pz >= 0.f ? 1.f : -1.f);
        px = ox;
        pz = oz;
    }
    const auto q = [](float v) { return u32(std::lround((std::clamp(v, -1.f, 1.f) * 0.5f + 0.5f) * 255.f)); };
    return q(px) | (q(pz) << 8);
}

u32 leaf_roll8(const da_shrub::V3& dir, const da_shrub::V3& side)
{
    using namespace da_shrub;
    const V3 c = std::abs(dir.y) < 0.99f ? normalize(cross(v3(0.f, 1.f, 0.f), dir)) : normalize(cross(dir, v3(1.f, 0.f, 0.f)));
    const float roll = std::atan2(dot(side, cross(dir, c)), dot(side, c));
    return u32(std::lround((roll / 3.14159265f * 0.5f + 0.5f) * 255.f)) & 0xFFu;
}

u32 hash32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

u32 pos_hash(const Fvector& p)
{
    return hash32(u32(iFloor(p.x * 10.f)) * 73856093u ^ u32(iFloor(p.z * 10.f)) * 19349663u ^ u32(iFloor(p.y * 10.f)) * 83492791u);
}

// ---- Species ----
pcstr skip_space(pcstr s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

void read_float(const CInifile::Sect& s, pcstr key, float& out)
{
    if (pcstr v = da_ini_value(s, key))
        out = float(atof(v));
}

void read_floats(const CInifile::Sect& s, pcstr key, float* out, u32 n)
{
    pcstr v = da_ini_value(s, key);
    if (!v)
        return;
    string64 item;
    const u32 count = _GetItemCount(v);
    for (u32 i = 0; i < n && count; ++i)
        out[i] = float(atof(_GetItem(v, std::min(i, count - 1), item)));
}

namespace
{
constexpr pcstr kKeyNames[skCount] = {"leaf_out", "leaf_grow", "autumn", "fallen", "bloom", "bloom_stage", "fruit", "ripe",
    "catkin", "catkin_grow", "autumn_spread", "fruit_keep"};
constexpr pcstr kColourNames[scCount] = {"c_young", "c_summer", "c_autumn_a", "c_autumn_b", "c_dead", "c_under", "c_bark",
    "c_twig", "c_petal", "c_centre", "c_fruit_unripe", "c_fruit_ripe", "c_catkin"};

// A named value of a drawing key (its index in names), or the number written.
float named(pcstr v, std::initializer_list<pcstr> names)
{
    v = skip_space(v);
    u32 i = 0;
    for (pcstr n : names)
    {
        if (!_strnicmp(v, n, xr_strlen(n)))
            return float(i);
        ++i;
    }
    return float(atof(v));
}

void read_named(const CInifile::Sect& s, pcstr key, float& out, std::initializer_list<pcstr> names)
{
    if (pcstr v = da_ini_value(s, key))
        out = named(v, names);
}

float curve_or(const DaSeasonCurve& c, float day, float fallback) { return c.n ? c.at(day) : fallback; }

Fvector3 colour_or(const DaColourCurve& c, float day, const Fvector3& fallback)
{
    if (c.empty())
        return fallback;
    Fvector3 v = c.at(day);
    v.set(std::max(v.x, 0.f), std::max(v.y, 0.f), std::max(v.z, 0.f));
    return v;
}
} // namespace

void parse_species(const CInifile::Sect& s, Species& sp, u32 (*class_bits)(pcstr list))
{
    sp.used = true;
    for (const CInifile::Item& item : s.Data)
        if (item.first.c_str() && item.second.c_str())
            da_shrub::arch_set(sp.arch, item.first.c_str(), item.second.c_str());
    read_floats(s, "height", sp.height, 2);
    read_floats(s, "width", sp.width, 2);
    read_float(s, "dead", sp.dead);
    read_float(s, "spread_days", sp.spread_days);
    if (pcstr v = da_ini_value(s, "classes"); v && class_bits)
        sp.classes = class_bits(v);
    read_named(s, "leaf_outline", sp.outline, {"ovate", "elliptic", "lanceolate", "round", "lobed", "narrow"});
    // A spray draws its leaves as a compound leaf's leaflets, splayed by the leaf angle.
    const bool spray = sp.arch.leaf_kind == da_shrub::lkSpray;
    sp.leaflets = sp.arch.leaf_kind == da_shrub::lkCompound || spray ? sp.arch.leaflets : 0.f;
    sp.leaflet_angle = spray ? 0.5f * (sp.arch.leaf_angle[0] + sp.arch.leaf_angle[1]) : sp.arch.leaflet_angle;
    read_float(s, "leaflet_ratio", sp.leaflet_ratio);
    read_float(s, "translucency", sp.translucency);
    read_float(s, "gloss", sp.gloss);
    read_float(s, "flutter", sp.flutter);
    read_float(s, "fold", sp.fold);
    read_floats(s, "underside", &sp.under_light, 2);
    read_floats(s, "jitter", &sp.jitter_value, 2);
    read_named(s, "bloom_shape", sp.bloom_shape,
        {"none", "single", "corymb", "umbel", "raceme", "panicle", "pea", "cone", "lacecap"});
    read_float(s, "petals", sp.petals);
    read_named(s, "fruit_shape", sp.fruit_shape,
        {"none", "berry", "hip", "drupe", "pome", "nut", "capsule", "cone", "pod", "cotton", "aggregate"});
    read_float(s, "fruits", sp.fruits);
    read_named(s, "catkin_shape", sp.catkin_shape, {"none", "hanging", "upright"});
    read_floats(s, "bark", &sp.bark_rough, 3);
    read_float(s, "needle_density", sp.needle_density);
    read_float(s, "evergreen", sp.evergreen);
    read_float(s, "lobes", sp.lobes);
    read_floats(s, "autumn_mix", sp.autumn_mix, 4);
    read_floats(s, "leaf_tex", sp.leaf_tex, 4);
    read_floats(s, "leaf_hull", sp.leaf_hull, 6);
    read_float(s, "bark_tex", sp.bark_tex);
    read_float(s, "lichen", sp.lichen);
    read_floats(s, "leaf_fit", sp.leaf_fit, 4);
    read_floats(s, "card_fit", sp.card_fit, 3);
    read_float(s, "young_relief", sp.young_relief);
    read_floats(s, "bark_tint", sp.bark_tint, 3);
    read_float(s, "bark_contrast", sp.bark_contrast);
    read_floats(s, "prickle_colour", sp.prickle_colour, 3);
    read_float(s, "bark_scale", sp.bark_scale);
    sp.bark_scale = std::clamp(sp.bark_scale, 0.25f, 8.f);
    read_floats(s, "twig_blush", sp.twig_blush, 4);
    read_float(s, "far_value", sp.far_value);
    sp.far_value = std::clamp(sp.far_value, 0.25f, 4.f);
    read_float(s, "far_chroma", sp.far_chroma);
    sp.far_chroma = std::clamp(sp.far_chroma, 0.f, 2.f);
    // The trees' bark zones: the upper bark's layer and where it takes over.
    read_float(s, "bark_tex_upper", sp.bark_tex_upper);
    read_floats(s, "bark_zone", sp.bark_zone, 2);
    if (pcstr v = da_ini_value(s, "bark_zone_mode"))
        sp.bark_zone_age = strstr(v, "age") ? 1.f : (strstr(v, "share") ? 2.f : 0.f);
    if (pcstr v = da_ini_value(s, "bark_zone_order"); v && strstr(v, "ridges"))
        sp.bark_zone_age += 4.f;
    read_float(s, "marcescent", sp.marcescent);
    sp.marcescent = std::clamp(sp.marcescent, 0.f, 1.f);
    read_float(s, "bark_scale_upper", sp.bark_scale_upper);
    read_float(s, "bark_contrast_upper", sp.bark_contrast_upper);
    read_float(s, "twig_r", sp.twig_r);
    sp.twig_r = std::clamp(sp.twig_r, 0.f, 0.1f);
    read_floats(s, "wood_sap", sp.wood_sap, 3);
    read_floats(s, "wood_heart", sp.wood_heart, 4);
    read_float(s, "ring_width", sp.ring_width);
    sp.ring_width = std::clamp(sp.ring_width, 0.0005f, 0.02f);
    read_float(s, "bark_stretch", sp.bark_stretch);
    sp.bark_stretch = std::clamp(sp.bark_stretch, 0.25f, 4.f);
    read_float(s, "bark_value_upper", sp.bark_value_upper);
    sp.bark_value_upper = std::clamp(sp.bark_value_upper, 0.25f, 4.f);
    read_float(s, "bark_chroma_upper", sp.bark_chroma_upper);
    sp.bark_chroma_upper = std::clamp(sp.bark_chroma_upper, 0.f, 2.f);
    read_float(s, "crop_spread", sp.crop_spread);
    sp.crop_spread = std::clamp(sp.crop_spread, 0.f, 0.95f);
    sp.droop = sp.arch.leaf_droop;
    for (u32 k = 0; k < skCount; ++k)
        if (pcstr v = da_ini_value(s, kKeyNames[k]))
            da_parse_curve(v, sp.keys[k]);
    for (u32 c = 0; c < scCount; ++c)
        if (pcstr v = da_ini_value(s, kColourNames[c]))
            da_parse_colour_curve(v, sp.colours[c]);
}

void parse_community(const CInifile::Sect& s, Community& c, u32 max_species, s32 (*by_name)(pcstr name))
{
    pcstr list = da_ini_value(s, "species");
    if (!list)
        return;
    const u32 cap = std::min<u32>(max_species, u32(std::size(c.species)));
    string256 item;
    float total = 0.f;
    for (u32 i = 0, n = _GetItemCount(list); i < n && c.count < cap; ++i)
    {
        _GetItem(list, i, item);
        pstr colon = strchr(item, ':');
        if (!colon)
            continue;
        *colon = 0;
        const s32 sp = by_name(skip_space(item));
        const float share = std::max(float(atof(colon + 1)), 0.f);
        if (sp < 0 || share <= 0.f)
            continue;
        c.species[c.count] = u8(sp);
        c.share[c.count] = share;
        total += share;
        ++c.count;
    }
    for (u32 i = 0; i < c.count; ++i)
        c.share[i] /= total;
    c.used = c.count > 0;
}

void fill_profile(const Species& S, float elem_area, Fvector4* r)
{
    r[0].set(float(S.arch.leaf_kind), S.outline, S.leaflets, deg2rad(S.leaflet_angle));
    r[1].set(S.translucency, S.gloss, S.flutter, S.fold);
    r[2].set(S.under_light, S.under_blue, S.jitter_value, S.jitter_hue);
    r[3].set(S.bloom_shape, S.petals, S.fruit_shape, S.fruits);
    r[4].set(S.catkin_shape, S.bark_rough, S.twig_gloss, S.lenticels);
    r[5].set(S.droop, S.needle_density, S.evergreen, S.lobes);
    r[6].set(S.autumn_mix[0], S.autumn_mix[1], S.autumn_mix[2], S.autumn_mix[3]);
    r[7].set(S.spread_days, S.leaflet_ratio, deg2rad(S.arch.leaf_phyllo), S.arch.leaf_per_node);
    r[8].set(S.leaf_tex[0], S.leaf_tex[1], S.leaf_tex[2], S.leaf_tex[3]);
    r[9].set(S.leaf_hull[0], S.leaf_hull[1], S.leaf_hull[2], S.leaf_hull[3]);
    r[10].set(S.leaf_hull[4], S.leaf_hull[5], S.bark_tex, S.lichen);
    r[11].set(S.leaf_fit[0], S.leaf_fit[1], S.leaf_fit[2], S.leaf_fit[3]);
    r[12].set(S.card_fit[0], S.card_fit[1], S.card_fit[2], S.young_relief);
    r[13].set(S.bark_tint[0], S.bark_tint[1], S.bark_tint[2], S.bark_contrast);
    r[14].set(S.prickle_colour[0], S.prickle_colour[1], S.prickle_colour[2], S.bark_scale);
    r[15].set(S.twig_blush[0], S.twig_blush[1], S.twig_blush[2], S.twig_blush[3]);
    r[16].set(elem_area, S.far_value, S.far_chroma, S.twig_r);
    r[17].set(S.bark_tex_upper, S.bark_zone[0], S.bark_zone[1], S.bark_zone_age);
    r[18].set(S.marcescent, S.bark_stretch, S.bark_scale_upper > 0.f ? std::clamp(S.bark_scale_upper, 0.25f, 8.f) : S.bark_scale,
        S.bark_contrast_upper > 0.f ? S.bark_contrast_upper : S.bark_contrast);
    const bool heart = S.wood_heart[0] >= 0.f && S.wood_heart[1] >= 0.f && S.wood_heart[2] >= 0.f;
    r[19].set(S.wood_sap[0], S.wood_sap[1], S.wood_sap[2], heart ? std::clamp(S.wood_heart[3], 0.f, 0.95f) : 0.f);
    r[20].set(heart ? S.wood_heart[0] : S.wood_sap[0], heart ? S.wood_heart[1] : S.wood_sap[1], heart ? S.wood_heart[2] : S.wood_sap[2],
        S.ring_width);
    r[21].set(S.crop_spread, 0.f, S.bark_value_upper, S.bark_chroma_upper);
}

void fill_season(const Species& S, float day, Fvector4* r, float lead)
{
    const auto k = [&S, day](SeasonKey key, float lo, float hi, float fallback) {
        return std::clamp(curve_or(S.keys[key], day, fallback), lo, hi);
    };
    r[0].set(k(skLeafOut, 0.f, 1.f, 1.f), k(skLeafGrow, 0.f, 1.f, 1.f), k(skAutumn, 0.f, 1.f, 0.f), k(skFallen, 0.f, 1.f, 0.f));
    r[1].set(k(skBloom, 0.f, 1.f, 0.f), k(skBloomStage, 0.f, 2.f, 0.f), k(skFruit, 0.f, 1.f, 0.f), k(skRipe, 0.f, 2.f, 0.f));
    r[2].set(k(skCatkin, 0.f, 3.f, 0.f), k(skCatkinLen, 0.f, 1.f, 1.f), k(skAutumnSpread, 0.02f, 1.f, 0.25f),
        k(skFruitKeep, 0.f, 1.f, 1.f));
    static const Fvector3 fallback[scCount] = {{0.16f, 0.3f, 0.06f}, {0.07f, 0.16f, 0.04f}, {0.45f, 0.36f, 0.06f},
        {0.35f, 0.08f, 0.04f}, {0.2f, 0.12f, 0.06f}, {0.12f, 0.2f, 0.08f}, {0.12f, 0.1f, 0.08f}, {0.16f, 0.12f, 0.08f},
        {0.8f, 0.8f, 0.75f}, {0.6f, 0.5f, 0.15f}, {0.15f, 0.3f, 0.08f}, {0.4f, 0.03f, 0.03f}, {0.35f, 0.32f, 0.18f}};
    for (u32 c = 0; c < scCount; ++c)
    {
        const Fvector3 v = colour_or(S.colours[c], day, fallback[c]);
        r[3 + c].set(v.x, v.y, v.z, 0.f);
    }
    // A bloom element is the larger of the flower and the fruit (da_shrub_gen.cpp add_bloom); each draws at its own
    // size (m), kept beside its colour: the petals' w the flower's, the unripe fruit's w the fruit group's.
    r[3 + scPetal].w = S.arch.bloom_size;
    r[3 + scFruitUnripe].w = S.arch.fruit_size;
    // A young tree's spring: the leaf out, the leaves' growth and the fallen share `lead` days on (the curves wrap round
    // the year) while that day has more leaves on than today - in spring; in autumn today's (the old trees' dates).
    const auto on = [&S](float d) {
        return std::clamp(curve_or(S.keys[skLeafOut], d, 1.f), 0.f, 1.f) * (1.f - std::clamp(curve_or(S.keys[skFallen], d, 0.f), 0.f, 1.f));
    };
    const float d = lead > 0.f && on(day + lead) > on(day) + 1e-4f ? day + lead : day;
    r[3 + scYoung].w = std::clamp(curve_or(S.keys[skLeafOut], d, 1.f), 0.f, 1.f);
    r[3 + scSummer].w = std::clamp(curve_or(S.keys[skLeafGrow], d, 1.f), 0.f, 1.f);
    r[3 + scAutumnA].w = std::clamp(curve_or(S.keys[skFallen], d, 0.f), 0.f, 1.f);
}

bool leafy(u32 s)
{
    const Fvector4& r0 = g.tables[kTabSeas + s * kSeasF4];
    return r0.x > 0.01f && r0.w < 0.99f;
}

bool blooming(u32 s)
{
    const Fvector4& r1 = g.tables[kTabSeas + s * kSeasF4 + 1];
    const Fvector4& r2 = g.tables[kTabSeas + s * kSeasF4 + 2];
    return r1.x > 0.01f || r1.z > 0.01f || (r2.x > 0.25f && r2.x < 2.99f);
}

void tables_dirty() { g.tab_day = -1000.f; }

float cascade_size(u32 c)
{
    return c == 0 ? 20.f : (c == 1 ? ps_r__sun_cascade_mid : std::max(ps_r__sun_cascade_far, ps_r__sun_cascade_mid));
}

// The rain's occlusion map covers the wet-surface radius round the camera, shifted along the view (r3_R_rain.cpp): the
// crowns within twice it.
float rain_reach() { return 2.f * std::max(ps_r3_dyn_wet_surf_far, 10.f) + 10.f; }

void build_grid(Population& P)
{
    P.cells.clear();
    P.cell_items.clear();
    if (P.instances.empty())
        return;
    Fbox all;
    all.invalidate();
    for (const Instance& I : P.instances)
        all.merge(I.box);
    P.grid_origin.set(all.vMin.x, all.vMin.z);
    P.grid_x = std::max(u32(std::ceil((all.vMax.x - all.vMin.x) / kCell)), 1u);
    P.grid_z = std::max(u32(std::ceil((all.vMax.z - all.vMin.z) / kCell)), 1u);
    xr_vector<u32> cell_of(P.instances.size());
    xr_vector<u32> counts(size_t(P.grid_x) * P.grid_z, 0);
    for (u32 i = 0; i < P.instances.size(); ++i)
    {
        const Fvector& c = P.instances[i].bound.P;
        const u32 cx = std::min(u32(std::max((c.x - P.grid_origin.x) / kCell, 0.f)), P.grid_x - 1);
        const u32 cz = std::min(u32(std::max((c.z - P.grid_origin.y) / kCell, 0.f)), P.grid_z - 1);
        cell_of[i] = cz * P.grid_x + cx;
        ++counts[cell_of[i]];
    }
    P.cells.resize(counts.size());
    u32 at = 0;
    for (u32 c = 0; c < counts.size(); ++c)
    {
        P.cells[c].first = at;
        P.cells[c].count = 0;
        P.cells[c].box.invalidate();
        at += counts[c];
    }
    P.cell_items.resize(P.instances.size());
    float overhang = 0.f;
    for (u32 i = 0; i < P.instances.size(); ++i)
    {
        Cell& c = P.cells[cell_of[i]];
        P.cell_items[c.first + c.count++] = i;
        const Fbox& b = P.instances[i].box;
        c.box.merge(b);
        const float x = P.grid_origin.x + float(cell_of[i] % P.grid_x) * kCell;
        const float z = P.grid_origin.y + float(cell_of[i] / P.grid_x) * kCell;
        overhang = std::max({overhang, x - b.vMin.x, b.vMax.x - (x + kCell), z - b.vMin.z, b.vMax.z - (z + kCell)});
    }
    P.grid_margin = u32(std::ceil(std::max(overhang, 0.f) / kCell));
}

// ---- Census ----
u64 fnv(const void* data, size_t n, u64 h)
{
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * kFnvPrime;
    return h;
}

u64 fnv_shader(pcstr shader, u64 h)
{
    string512 sh;
    xr_strcpy(sh, shader ? shader : "");
    for (char* c = sh; *c; ++c)
        *c = char(tolower(u8(*c)));
    return fnv(sh, xr_strlen(sh) + 1, h);
}

namespace
{
// The parts' content keys, read back from $level$\level.geom (the GPU buffers keep no CPU copy): every vertex as its
// position in millimetres and its uv wrapped into one tile, every triangle by its three vertex hashes sorted, the part by
// its shader string, its triangle count and the wrapping sum of its triangle hashes - the same model hashes the same
// however xrLC ordered it in a level (BUSHES_ENGINE.md 1.3). The instances of a model share its geometry ranges: each
// distinct range is keyed once.
bool read_part_keys(xr_vector<StockPart>& parts)
{
    CStreamReader* geom = FS.rs_open("$level$", "level.geom");
    if (!geom)
        return false;
    // The distinct ranges (vertex buffer, base, count, index buffer, base, triangles, shader) and their parts.
    struct Range
    {
        u32 part;
        xr_vector<u32> same;
    };
    xr_vector<Range> ranges;
    {
        xr_unordered_map<u64, u32> by_sig;
        for (u32 i = 0; i < parts.size(); ++i)
        {
            const StockPart& P = parts[i];
            const u32 sig_data[6] = {P.vb, P.vbase, P.vcount, P.ib, P.ibase, P.tris};
            const u64 sig = fnv_shader(P.shader, fnv(sig_data, sizeof(sig_data)));
            const auto [it, fresh] = by_sig.try_emplace(sig, u32(ranges.size()));
            if (fresh)
                ranges.push_back(Range{i, {}});
            else
                ranges[it->second].same.push_back(i);
        }
    }
    xr_unordered_map<u32, xr_vector<u32>> by_ib, by_vb;
    for (u32 r = 0; r < ranges.size(); ++r)
    {
        by_ib[parts[ranges[r].part].ib].push_back(r);
        by_vb[parts[ranges[r].part].vb].push_back(r);
    }
    xr_vector<xr_vector<u16>> indices(ranges.size());
    if (CStreamReader* fs = geom->open_chunk(fsL_IB))
    {
        const u32 count = fs->r_u32();
        for (u32 b = 0; b < count; ++b)
        {
            const u32 n = fs->r_u32();
            const size_t bytes = size_t(n) * 2;
            if (const auto it = by_ib.find(b); it != by_ib.end())
            {
                const u16* data = static_cast<const u16*>(fs->pointer_contiguous(bytes));
                for (const u32 r : it->second)
                {
                    const StockPart& P = parts[ranges[r].part];
                    if (size_t(P.ibase) + size_t(P.tris) * 3 <= n)
                        indices[r].assign(data + P.ibase, data + P.ibase + size_t(P.tris) * 3);
                }
            }
            fs->advance(int(bytes));
        }
        fs->close();
    }
    if (CStreamReader* fs = geom->open_chunk(fsL_VB))
    {
        const u32 count = fs->r_u32();
        constexpr size_t decl_bytes = (MAXD3DDECLLENGTH + 1) * sizeof(VertexElement);
        xr_vector<u64> hv;
        xr_vector<Fvector> pos;
        for (u32 b = 0; b < count; ++b)
        {
            VertexElement dcl[MAXD3DDECLLENGTH + 1];
            fs->r(dcl, decl_bytes);
            fs->advance(-int(decl_bytes));
            const u32 dcl_len = GetDeclLength(dcl) + 1;
            fs->advance(int(dcl_len * sizeof(VertexElement)));
            const u32 n = fs->r_u32();
            const u32 stride = GetDeclVertexSize(dcl, 0);
            const size_t bytes = size_t(n) * stride;
            if (const auto it = by_vb.find(b); it != by_vb.end())
            {
                int pos_off = -1, tc_off = -1;
                for (u32 e = 0; e + 1 < dcl_len; ++e)
                {
                    if (dcl[e].Usage == D3DDECLUSAGE_POSITION && dcl[e].Type == D3DDECLTYPE_FLOAT3)
                        pos_off = dcl[e].Offset;
                    if (dcl[e].Usage == D3DDECLUSAGE_TEXCOORD && dcl[e].UsageIndex == 0 &&
                        (dcl[e].Type == D3DDECLTYPE_SHORT4 || dcl[e].Type == D3DDECLTYPE_SHORT2))
                        tc_off = dcl[e].Offset;
                }
                const u8* data = static_cast<const u8*>(fs->pointer_contiguous(bytes));
                for (const u32 r : it->second)
                {
                    StockPart& P = parts[ranges[r].part];
                    const xr_vector<u16>& idx = indices[r];
                    if (pos_off < 0 || tc_off < 0 || size_t(P.vbase) + P.vcount > n || idx.empty())
                        continue;
                    hv.resize(P.vcount);
                    pos.resize(P.vcount);
                    P.box.invalidate();
                    for (u32 v = 0; v < P.vcount; ++v)
                    {
                        const u8* rec = data + size_t(P.vbase + v) * stride;
                        Fvector q;
                        CopyMemory(&q, rec + pos_off, sizeof(q));
                        s16 tc[2];
                        CopyMemory(tc, rec + tc_off, sizeof(tc));
                        struct
                        {
                            s32 p[3];
                            s16 t[2];
                        } key{{s32(std::floor(double(q.x) * 1000.0 + 0.5)), s32(std::floor(double(q.y) * 1000.0 + 0.5)),
                                  s32(std::floor(double(q.z) * 1000.0 + 0.5))},
                            {s16(((s32(tc[0]) % 2048) + 2048) % 2048), s16(((s32(tc[1]) % 2048) + 2048) % 2048)}};
                        static_assert(sizeof(key) == 16);
                        hv[v] = fnv(&key, sizeof(key));
                        pos[v] = q;
                        P.box.modify(q);
                    }
                    u64 sum = 0;
                    P.area = 0.0;
                    for (u32 t = 0; t < P.tris; ++t)
                    {
                        const u16* tri = &idx[size_t(t) * 3];
                        if (tri[0] >= P.vcount || tri[1] >= P.vcount || tri[2] >= P.vcount)
                            continue;
                        u64 h3[3] = {hv[tri[0]], hv[tri[1]], hv[tri[2]]};
                        std::sort(std::begin(h3), std::end(h3));
                        sum += fnv(h3, sizeof(h3));
                        Fvector e1, e2, cr;
                        e1.sub(pos[tri[1]], pos[tri[0]]);
                        e2.sub(pos[tri[2]], pos[tri[0]]);
                        cr.crossproduct(e1, e2);
                        P.area += 0.5 * double(cr.magnitude());
                    }
                    u64 h = fnv_shader(P.shader, kFnvOffset);
                    h = fnv(&P.tris, sizeof(u32), h);
                    P.key = fnv(&sum, sizeof(sum), h);
                }
            }
            fs->advance(int(bytes));
        }
        fs->close();
    }
    FS.r_close(geom);
    // The other instances of each range take its key.
    for (const Range& r : ranges)
        for (const u32 i : r.same)
        {
            parts[i].key = parts[r.part].key;
            parts[i].box = parts[r.part].box;
            parts[i].area = parts[r.part].area;
        }
    return true;
}
} // namespace

Census& census()
{
    Census& C = g.census;
    if (C.done)
        return C;
    C.done = true;
    auto& dsgraph = RImplementation.get_imm_context();
    xr_unordered_map<const FTreeVisual*, u32> part_of;
    const auto& names = RImplementation.ShaderNames_dbg;
    const auto walk = [&](auto&& self, FHierrarhyVisual* hierarchy) -> void {
        for (dxRender_Visual* child : hierarchy->children)
        {
            if (child->Type == MT_LOD)
            {
                Candidate c;
                c.parent = hierarchy;
                c.flod = child;
                bool trees = true;
                for (dxRender_Visual* leaf : static_cast<FHierrarhyVisual*>(child)->children)
                {
                    if (leaf->Type != MT_TREE_ST && leaf->Type != MT_TREE_PM)
                    {
                        trees = false;
                        break;
                    }
                    const FTreeVisual* tree = static_cast<const FTreeVisual*>(leaf);
                    const auto [it, fresh] = part_of.try_emplace(tree, u32(C.parts.size()));
                    if (fresh)
                    {
                        FTreeVisualInstancedDraw w;
                        if (!tree->window(0, w))
                        {
                            part_of.erase(it);
                            trees = false;
                            break;
                        }
                        StockPart P;
                        P.visual = tree;
                        P.vb = tree->vb_id();
                        P.ib = tree->ib_id();
                        P.vbase = w.base_vertex;
                        P.vcount = w.vertex_count;
                        P.ibase = w.start_index;
                        P.tris = w.primitive_count;
                        P.shader = tree->level_shader < names.size() ? names[tree->level_shader].c_str() : nullptr;
                        C.parts.push_back(std::move(P));
                    }
                    c.parts.push_back(it->second);
                }
                if (trees && !c.parts.empty())
                    C.candidates.push_back(std::move(c));
            }
            else if (child->Type == MT_HIERRARHY)
                self(self, static_cast<FHierrarhyVisual*>(child));
        }
    };
    for (u32 s = 0; s < dsgraph.Sectors.size(); ++s)
    {
        dxRender_Visual* root = static_cast<CSector*>(dsgraph.Sectors[s])->root();
        if (root && root->Type == MT_HIERRARHY)
            walk(walk, static_cast<FHierrarhyVisual*>(root));
    }
    if (!C.candidates.empty() && !read_part_keys(C.parts))
        C.candidates.clear();
    return C;
}

u64 kind_key(const Census& C, const Candidate& c)
{
    xr_vector<u64> keys;
    for (u32 p : c.parts)
        keys.push_back(C.parts[p].key);
    std::sort(keys.begin(), keys.end());
    return fnv(keys.data(), keys.size() * sizeof(u64));
}

u64 family_signature(const Census& C, const Candidate& c, Fbox& box, double& area)
{
    xr_vector<const StockPart*> parts;
    for (u32 p : c.parts)
        parts.push_back(&C.parts[p]);
    std::sort(parts.begin(), parts.end(),
        [](const StockPart* a, const StockPart* b) { return xr_stricmp(a->shader ? a->shader : "", b->shader ? b->shader : "") < 0; });
    u64 sig = kFnvOffset;
    box.invalidate();
    area = 0.0;
    for (const StockPart* p : parts)
    {
        sig = fnv_shader(p->shader, sig);
        sig = fnv(&p->tris, sizeof(u32), sig);
        box.merge(p->box);
        area += p->area;
    }
    return sig;
}

void parse_families(const CInifile::Sect& sect, std::unordered_multimap<u64, FamilyRow>& out)
{
    for (const CInifile::Item& item : sect.Data)
    {
        if (!item.first.c_str() || !item.second.c_str())
            continue;
        const u64 sig = u64(strtoull(item.first.c_str(), nullptr, 16));
        string256 field;
        pcstr v = item.second.c_str();
        for (u32 i = 0, n = _GetItemCount(v); i < n; ++i)
        {
            _GetItem(v, i, field);
            char key[32]{};
            int b[6]{};
            double area = 0.0;
            if (sscanf_s(skip_space(field), "%16[0-9a-fA-F]:%d:%d:%d:%d:%d:%d:%lf", key, u32(sizeof(key)), &b[0], &b[1], &b[2],
                    &b[3], &b[4], &b[5], &area) != 8)
                continue;
            FamilyRow f;
            f.kind = u64(strtoull(key, nullptr, 16));
            f.box.set(float(b[0]) * 0.001f, float(b[1]) * 0.001f, float(b[2]) * 0.001f, float(b[3]) * 0.001f,
                float(b[4]) * 0.001f, float(b[5]) * 0.001f);
            f.area = area;
            out.emplace(sig, f);
        }
    }
}

float ground_at(float x, float z, float y_top, float span, float fallback, bool skip_trees)
{
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    const CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
    if (!model || !tris)
        return fallback;
    CDB::COLLIDER xrc;
    xrc.ray_query(0, model, Fvector{x, y_top, z}, Fvector{0.f, -1.f, 0.f}, span);
    float best = flt_max;
    for (size_t i = 0; i < xrc.r_count(); ++i)
    {
        const CDB::RESULT& r = xrc.r_begin()[i];
        const SGameMtl* m = GMLib.GetMaterialByIdx(u16(tris[r.id].material));
        if (m && (m->Flags.test(SGameMtl::flPassable) || (skip_trees && (strstr(m->m_Name.c_str(), "tree") || strstr(m->m_Name.c_str(), "bush")))))
            continue;
        best = std::min(best, r.range);
    }
    return best < flt_max ? y_top - best : fallback;
}

// ---- The frame ----
bool record_room(const Population& P, u32 i) { return g.records < kMaxInstances || P.frame_slot[i] != ~0u; }

u32 frame_record(Population& P, u32 i)
{
    u32& slot = P.frame_slot[i];
    if (slot != ~0u)
        return slot;
    slot = g.records++;
    const Instance& I = P.instances[i];
    const Template& T = P.templates[I.tmpl];
    u32 rec[kInstRec / 4]{};
    CopyMemory(rec, I.row, sizeof(I.row));
    const u32 flags = (I.dead ? kInstDead : 0u) | (P.index == kPopTrees ? kInstTree : 0u) | (I.red ? kInstRed : 0u);
    rec[12] = I.species | (flags << 8) | (u32(u8(I.pheno)) << 16) | (u32(I.lead) << 24);
    rec[13] = T.axis_first;
    // (Hemi and sun 8 bits each; a tree's ground over its root in centimetres, signed.)
    rec[14] = u8f(I.hemi) | (u8f(I.sun) << 8) | (u32(u16(s16(std::clamp(std::lround(I.ground * 100.f), -32767l, 32767l)))) << 16);
    rec[15] = f16(I.scale) | (u32(I.seed) << 16);
    // The wind at the instance's root now and a frame back (r3\da_shrub_common.h shrub_wind_at, the same maths): the
    // local heading, the pressure and the gust lean - one plant's worth, where every vertex of it evaluated the gust
    // field's noise twice.
    const Latch& L = g.latch;
    const float x = I.row[0][3], z = I.row[2][3];
    const auto wind = [&L, x, z](const Fvector4& f, u32& w_dir, u32& w_push) {
        const float gust = da_wind_field_gust(x, z, f.x, f.y);
        const float ang = da_wind_dev_angle(da_wind_field_dev(x, z, f.x, f.y), f.z);
        const float sa = std::sin(ang), ca = std::cos(ang);
        w_dir = h2(L.wind_hx * ca - L.wind_hz * sa, L.wind_hx * sa + L.wind_hz * ca);
        w_push = h2(DA_SAT(f.z) * da_wind_field_amp(gust), da_wind_field_lean(gust, f.w));
    };
    wind(L.wind_field, rec[16], rec[17]);
    wind(L.wind_field_prev, rec[18], rec[19]);
    P.record_tail(i, rec + 20);
    const size_t at = g.inst_records.size();
    g.inst_records.resize(at + kInstRec);
    CopyMemory(g.inst_records.data() + at, rec, kInstRec);
    return slot;
}

// The chunks of draw d straight into pass p's list (the CPU's choice draws them all).
void emit_direct(Population& P, u32 p, u32 d, u32 rec, u32 first, u32 n, float keep, float lo, float hi, u32 flags, float aux)
{
    xr_vector<ChunkRec>& list = P.pass_chunks[p][d];
    const u32 window = u8f(lo) << 8 | u8f(hi) << 16 | flags;
    for (u32 k = 0; k < n; k += kChunk)
        list.push_back(ChunkRec{rec, first + k, std::min(kChunk, n - k) | window, h2(keep, aux)});
    P.elements[d] += n;
}

void emit(Population& P, u32 p, u32 d, u32 rec, u32 first, u32 n, float keep, float lo, float hi, u32 flags, float aux)
{
    if (!n || hi <= lo)
        return;
    // The shrubs' view through the GPU cut (r3\da_tree_cut.cs CUT_KEEP): the draw chosen here, the elements its vertex
    // shaders would throw away - out of the view by their own bent place, behind the depth drawn so far (the opaque
    // world and every tree), the wood under the wire rule - left out on the GPU instead of run and collapsed. Behind a
    // near trunk half a million shrub elements ran their vertices for a few thousand pixels: Ghost of Tsushima culls its
    // proxy sets' instances on the GPU against the frame's depth.
    if (!p && P.index == kPopShrubs && P.cut_phase == kCutShrubs)
    {
        const u32 kind = kDrawKind[d];
        const Draw dd = Draw(d);
        const Draw ds[4] = {dd, dd, dd, dd};
        if (kind == kKindCluster || kind == kKindBlob)
            cut(P, ccMass, ds, rec, first, n, keep, lo, hi, flags, float(kind), 0.f, 0.f, aux);
        else
            cut(P, ccKeep, ds, rec, first, n, keep, lo, hi, flags, kind == kKindWood ? 2.f : (kind == kKindBloom ? 1.f : 0.f), 0.f, 0.f,
                aux);
        return;
    }
    emit_direct(P, p, d, rec, first, n, keep, lo, hi, flags, aux);
}

void thinned_runs(u32 n, float share, float keep, const std::function<void(u32 first, u32 count, float keep)>& run)
{
    if (!n)
        return;
    const u32 end = kept(n, share);
    const float target = std::min(share, 1.f) * float(n), band = kThinBand * target;
    // (Whole, or too few to fade: the prefix as it is; QA r__veg_soft without bit 1: the hard end.)
    if (end >= n || band < 1.f || !(ps_r__veg_soft & 1))
    {
        run(0, end, keep);
        return;
    }
    // The band's groups: eight at the least over it, a chunk each at the most; a group's shrink by its middle rank.
    const u32 n_full = u32(std::max(target - band, 0.f));
    const u32 group = std::clamp((end - n_full + 7u) / 8u, 1u, kChunk);
    const auto shrink = [&](u32 a, u32 c) { return std::clamp((target - (float(a) + 0.5f * float(c))) / band, 0.f, 1.f); };
    float area = float(n_full);
    for (u32 a = n_full; a < end; a += group)
    {
        const u32 c = std::min(group, end - a);
        const float f = shrink(a, c);
        area += f * f * float(c);
    }
    // (The survivors' growth makes up the band's lost area: the crown's cover holds through it.)
    const float kg = std::max(keep * area / std::max(target, 1.f), 1e-3f);
    if (n_full)
        run(0, n_full, kg);
    for (u32 a = n_full; a < end; a += group)
    {
        const u32 c = std::min(group, end - a);
        if (const float f = shrink(a, c); f > 0.02f)
            run(a, c, std::min(kg / (f * f), 60000.f));
    }
}

void emit_thinned(Population& P, u32 p, u32 d, u32 rec, u32 first, u32 n, float share, float keep, float lo, float hi, u32 flags)
{
    if (hi <= lo)
        return;
    thinned_runs(n, share, keep, [&](u32 a, u32 c, float k) { emit(P, p, d, rec, first + a, c, k, lo, hi, flags); });
}

// The population a phase of the cut culls (the trees' near and far phases, the shrubs').
u32 cut_owner(u32 phase) { return phase == kCutShrubs ? kPopShrubs : kPopTrees; }

bool cut_on()
{
    return ps_r__trees_cut && g.s_cut && g.s_cut_args && g.cut_routes_b.srv && g.cut_counts_b.uav && g.cut_args_b.uav && g.chunks_b.uav;
}

void cut(Population& P, CutClass cls, const Draw (&d)[4], u32 rec, u32 first, u32 n, float keep, float lo, float hi, u32 flags, float p0,
    float p1, float p2, float aux)
{
    if (!n || hi <= lo)
        return;
    // Each draw it may take holds its elements once more (whole instances), in its phase's lists.
    const u32 ph = std::min(P.cut_phase, kCutPhases - 1);
    u32 mask = 0, words = 0;
    for (const Draw k : d)
        mask |= 1u << k;
    for (u32 q = 0; q < kCutPhases; ++q)
        for (u32 k = 0; k < drCount; ++k)
            words += (g.cut_room[q][k] + (q == ph && ((mask >> k) & 1u) ? n : 0u) + kChunk - 1) / kChunk * kChunk;
    const u32 chunks = (n + kChunk - 1) / kChunk;
    if (words > kMaxCutWords || g.cut_chunk_total + chunks > kMaxCutChunks)
    {
        P.cut_fallback += n;
        emit_direct(P, 0, d[cls == ccPatch || cls == ccMass || cls == ccKeep ? 0 : 1], rec, first, n, keep, lo, hi, flags, aux);
        return;
    }
    g.cut_chunk_total += chunks;
    for (u32 k = 0; k < drCount; ++k)
        if ((mask >> k) & 1u)
            g.cut_room[ph][k] += n;
    u32 route = u32(cls);
    for (u32 k = 0; k < 4; ++k)
        route |= u32(d[k]) << (3 + 5 * k);
    const u32 window = u8f(lo) << 8 | u8f(hi) << 16 | flags;
    for (u32 k = 0; k < n; k += kChunk)
    {
        P.cut_chunks[ph].push_back(ChunkRec{rec, first + k, std::min(kChunk, n - k) | window, h2(keep, aux)});
        P.cut_routes[ph].insert(P.cut_routes[ph].end(), {route, std::bit_cast<u32>(p0), std::bit_cast<u32>(p1), std::bit_cast<u32>(p2)});
    }
    P.cut_elements += n;
}

void cut_readback_request()
{
    if (g.cut_staging && !g.cut_readback)
        g.cut_readback = 1;
}
} // namespace da_veg

namespace
{
// A cascade's reach: its box and the shadow of the population's tallest instance at a low sun on top.
float cascade_reach(const Population& P, u32 c)
{
    const float elev = std::max(g.latch.sun.y, 0.17f);
    const float shadow = P.max_height * std::sqrt(1.f - elev * elev) / elev;
    // (The shrubs keep their 60 m cap; a tree's shadow reaches as far as it falls.)
    return 0.75f * cascade_size(c) + (P.index == kPopShrubs ? std::min(shadow, 60.f) : std::min(shadow, 250.f));
}

// The bend slots of the frame's chunks from `first` on, in their order (r3\da_shrub_bend.cs and shrub_vertex read the
// slot from the chunk's keep word).
void number_bends(u32 first)
{
    for (u32 i = first; i < g.chunks.size(); ++i)
        if (ChunkRec& c = g.chunks[i]; !(c.packed & kChunkRigid))
            c.keep = (c.keep & 0xFFFFu) | (std::min(g.bend_n++, 0xFFFFu) << 16);
}

void build_frame()
{
    const Latch& L = g.latch;
    g.records = 0;
    g.inst_records.clear();
    g.cut_chunk_total = 0;
    g.chunks.clear();
    g.bend_blocks.clear();
    g.bend_n = g.bend_view_n = 0;
    CFrustum view;
    view.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    std::memset(g.cut_room, 0, sizeof(g.cut_room));
    {
        CFrustum all;
        all.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_ALL);
        for (u32 k = 0; k < 6; ++k)
            g.cut_planes[k] = k < all.p_count ? Fvector4{all.planes[k].n.x, all.planes[k].n.y, all.planes[k].n.z, all.planes[k].d}
                                              : Fvector4{0.f, 0.f, 0.f, -1e9f};
    }
    const float smap = float(std::max(u32(RImplementation.o.smapsize), 256u));
    for (u32 pi = 0; pi < kPops; ++pi)
    {
        Population* P = pop(pi);
        if (!pop_on(P))
            continue;
        CpuSpan span(*P);
        P->frame_slot.assign(P->instances.size(), ~0u);
        for (auto& pass : P->pass_chunks)
            for (xr_vector<ChunkRec>& list : pass)
                list.clear();
        std::fill(std::begin(P->seen), std::end(P->seen), 0u);
        std::fill(std::begin(P->elements), std::end(P->elements), 0u);
        for (u32 q = 0; q < kCutPhases; ++q)
        {
            P->cut_chunks[q].clear();
            P->cut_routes[q].clear();
        }
        P->cut_phase = P->index == kPopShrubs && cut_on() && ps_r__shrubs_cut ? kCutShrubs : 0u;
        P->cut_elements = 0;
        P->cut_fallback = 0;
        for (auto& pass : P->draw_count)
            std::fill(std::begin(pass), std::end(pass), 0u);
        // (No sun this frame - night, the static sun: no instance goes into a cascade.)
        P->cascades = RImplementation.sun_cascades_this_frame() ? std::min(P->frame_cascades(), kCascades) : 0;
        P->frame_begin();
        float reach[kCascades]{};
        for (u32 c = 0; c < P->cascades; ++c)
            reach[c] = cascade_reach(*P, c);
        const float reach_max = std::max({P->reach(), reach[0], reach[1], reach[2]});
        // A cascade takes an instance whose shadow can reach its box: the instance's ground footprint swept away from
        // the sun by its shadow's length passes within the box's reach round the camera (a sphere grown by the shadow
        // in every direction took every tree within a hundred and seventy metres into a twenty-metre map).
        const float elev = std::max(L.sun.y, 0.17f);
        const float shadow = std::min(P->max_height * std::sqrt(1.f - elev * elev) / elev, P->index == kPopShrubs ? 60.f : 250.f);
        float away_x = -L.sun.x, away_z = -L.sun.z;
        const float away_len = std::sqrt(away_x * away_x + away_z * away_z);
        away_x = away_len > 1e-4f ? away_x / away_len : 0.f;
        away_z = away_len > 1e-4f ? away_z / away_len : 0.f;
        // The view's instances go near to far (their chunks in that order within each draw): the near crowns' leaves
        // fill the depth buffer first and the alpha-tested leaves behind them fail the early depth test instead of
        // being shaded. They take their instance records before the ones only the cascades see (at the record cap a
        // shadow far out gives way to a plant in view).
        P->view_order.clear();
        P->cascade_order.clear();
        Fbox around;
        around.set(L.cam, L.cam);
        around.grow(reach_max);
        for_cells(*P, around, [&](const Cell& cell) {
            if (box_distance(cell.box, L.cam) > reach_max)
                return;
            u32 test_mask = FRUSTUM_P_LRTB | FRUSTUM_P_FAR;
            const bool cell_view = view.testAABB(&cell.box.vMin.x, test_mask) != fcvNone;
            for (u32 k = cell.first; k < cell.first + cell.count; ++k)
            {
                const u32 i = P->cell_items[k];
                const Instance& I = P->instances[i];
                const float d = std::max(I.bound.P.distance_to(L.cam) - 0.5f * I.bound.R, 0.f);
                if (cell_view && view.testSphere_dirty(I.bound.P, I.bound.R) && RImplementation.HOM.visible(I.box))
                    P->view_order.push_back({d, i});
                const float ax = L.cam.x - I.bound.P.x, az = L.cam.z - I.bound.P.z;
                const float u = std::clamp(ax * away_x + az * away_z, 0.f, shadow);
                const float gap = std::sqrt((ax - away_x * u) * (ax - away_x * u) + (az - away_z * u) * (az - away_z * u));
                for (u32 c = 0; c < P->cascades; ++c)
                    if (d < reach[c] && gap < 0.75f * cascade_size(c) + I.bound.R)
                        P->cascade_order.push_back({c, i});
            }
        });
        std::sort(P->view_order.begin(), P->view_order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
        {
            CpuSpan span(*P);
            for (const auto& [d, i] : P->view_order)
                if (!P->needs_record(i, d) || record_room(*P, i))
                    P->view(i, d);
        }
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
        {
            CpuSpan span(*P);
            for (const auto& [c, i] : P->cascade_order)
                if (record_room(*P, i))
                    P->cascade(c, i, cascade_size(c) / smap);
        }
    // The rain's occlusion map: the crowns within its reach, from above, in a frame that draws it (rain falls, and the
    // camera moved or the map aged).
    if (RImplementation.rain_map_this_frame())
    {
        const float rain_r = rain_reach();
        Fbox around;
        around.set(L.cam, L.cam);
        around.grow(rain_r);
        for (u32 pi = 0; pi < kPops; ++pi)
        {
            Population* P = pop(pi);
            if (!pop_on(P))
                continue;
            for_cells(*P, around, [&](const Cell& cell) {
                if (box_distance(cell.box, L.cam) > rain_r)
                    return;
                for (u32 k = cell.first; k < cell.first + cell.count; ++k)
                {
                    const u32 i = P->cell_items[k];
                    const Instance& I = P->instances[i];
                    if (I.bound.P.distance_to_xz(L.cam) > rain_r + I.bound.R || !record_room(*P, i))
                        continue;
                    P->rain(i);
                }
            });
        }
    }
    // Every draw's chunks one after another within the chunk buffer: the view's of every population first (the motion
    // vectors bend them a frame back too), then each population's shadow passes in one run (one bend dispatch).
    const auto bend_block = [](u32 pi, u32 start, u32 end) {
        if (end <= start)
            return;
        if (!g.bend_blocks.empty() && g.bend_blocks.back().pop == pi && g.bend_blocks.back().first + g.bend_blocks.back().count == start)
            g.bend_blocks.back().count += end - start;
        else
            g.bend_blocks.push_back({pi, start, end - start});
    };
    const auto append = [&bend_block](Population& P, u32 p) {
        const u32 start = u32(g.chunks.size());
        for (const Draw d : kAppendOrder)
        {
            const u32 room = kMaxChunks - std::min(u32(g.chunks.size()), kMaxChunks);
            const u32 n = std::min(u32(P.pass_chunks[p][d].size()), room);
            P.draw_first[p][d] = u32(g.chunks.size());
            P.draw_count[p][d] = n;
            g.chunks.insert(g.chunks.end(), P.pass_chunks[p][d].begin(), P.pass_chunks[p][d].begin() + n);
        }
        bend_block(P.index, start, u32(g.chunks.size()));
    };
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
        {
            CpuSpan span(*P);
            append(*P, 0);
        }
    // The GPU cut: its candidate chunks after the view's draws (they draw in the view: their bends a frame back too), the
    // near trees' phase first, then the far trees', then the shrubs'.
    g.cut_first = u32(g.chunks.size());
    g.cut_count = 0;
    std::fill(std::begin(g.cut_n), std::end(g.cut_n), 0u);
    for (u32 q = 0; q < kCutPhases; ++q)
    {
        Population* P = pop(cut_owner(q));
        if (!pop_on(P))
            continue;
        CpuSpan span(*P);
        const u32 start = u32(g.chunks.size());
        const u32 n = std::min(u32(P->cut_chunks[q].size()), kMaxChunks - std::min(start, kMaxChunks));
        g.chunks.insert(g.chunks.end(), P->cut_chunks[q].begin(), P->cut_chunks[q].begin() + n);
        g.cut_n[q] = n;
        g.cut_count += n;
        bend_block(P->index, start, start + n);
    }
    g.bend_view_end = u32(g.chunks.size());
    number_bends(0);
    g.bend_view_n = g.bend_n;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
        {
            CpuSpan span(*P);
            for (u32 p = 1; p < kPasses; ++p)
                append(*P, p);
        }
    number_bends(g.bend_view_end);
}

void latch_frame()
{
    Latch& L = g.latch;
    if (L.frame == Device.dwFrame)
        return;
    const bool continuous = L.frame + 1 == Device.dwFrame && !da_history_reset();
    L.frame = Device.dwFrame;
    L.cam = Device.vCameraPosition;
    L.fwd = Device.vCameraDirection;
    // Render pixels per metre at one metre of view depth (the upscaler's input height).
    const float render_h = float(std::max(Device.dwRenderHeight ? Device.dwRenderHeight : Device.dwHeight, 1u));
    L.px_per_m = render_h / (2.f * std::tan(deg2rad(Device.fFOV) * 0.5f));
    if (g_pGamePersistent)
    {
        const auto& env = g_pGamePersistent->Environment();
        L.wind_field.set(env.eff_wind_field_ofs.x, env.eff_wind_field_ofs.y, env.eff_wind_norm, env.eff_wind_gust);
        L.wind_field_prev = da_wind_prev().field;
        L.wind_hx = _sin(env.eff_wind_dir);
        L.wind_hz = _cos(env.eff_wind_dir);
        L.sun = g_pGamePersistent->Environment().CurrentEnv.sun_dir;
        L.sun.invert();
        if (L.sun.square_magnitude() < 1e-6f)
            L.sun.set(0.f, 1.f, 0.f);
        L.sun.normalize();
        L.far_plane = env.CurrentEnv.far_plane;
        FTreeVisual::FrameWind(L.tree_wave, L.tree_wind, L.tree_wave_prev, L.tree_wind_prev);
    }
    // The shaders' clock (r3\da_shrub_common.h shrub_fq): the frame times summed in double and wrapped at 64 s; the
    // float seconds since the start keep whole milliseconds only after a couple of hours.
    g.clock = std::fmod(g.clock + double(Device.fTimeDelta), 64.0);
    const float now = float(g.clock);
    L.time_prev = continuous ? L.time : now;
    L.time = now;
    L.motion = da_motion_vectors_active() || ps_r__motion_vectors;
    const SSnowFrame& snow = da_snow_frame();
    // Snow holds on the branches while it is fresh, less as it settles and ages - a thin line on their tops stays
    // all winter (old snow at a fifth of it left the crowns bare through most of January).
    L.snow = snow.active ? std::clamp(snow.cover.x * (snow.state.x + 0.75f * snow.state.y + 0.45f * snow.state.z), 0.f, 1.f) : 0.f;
    const float day = da_season_day();
    if (day != g.tab_day)
    {
        for (u32 pi = 0; pi < kPops; ++pi)
            if (Population* P = pop(pi); P && P->loaded)
                P->fill_tables(day, g.tables);
        g.tab_day = day;
        g.tab_upload = true;
    }
}

// The shaders' constants and resources, then `count` chunks from `first` of draw d of population P. pass: 0 the view,
// else a shadow map's (sun: the side its quads turn to, texel: its texel in metres, the wood's floor).
void draw_range(CBackend& cmd, const Population& P, const ref_shader& shader, u32 first, u32 count, u32 pass, u32 d, const Fvector& sun,
    float texel, s32 cut_phase = -1)
{
    const bool cut = cut_phase >= 0;
    if (!count || !shader)
        return;
    if (P.skip_draw(d, pass))
        return;
    const Latch& L = g.latch;
    const VertexClassInfo& vc = g.vclass[kDrawClass[d]];
    cmd.set_Element(shader->E[0]);
    cmd.set_Geometry(g.geom);
    cmd.set_c("da_shrub_draw", float(vc.verts / kChunk), float(kDrawKind[d]), float(d), float(pass));
    cmd.set_c("da_shrub_chunk0", float(first), float(ps_r__dbg_wood), cut ? 1.f : 0.f,
        (ps_r__dbg_trees & 65536) ? 2.f : ((ps_r__dbg_trees & 32768) ? 1.f : 0.f));
    cmd.set_c("da_shrub_cam", L.cam.x, L.cam.y, L.cam.z, 1.f / std::max(L.px_per_m, 1.f));
    cmd.set_c("da_shrub_fwd", L.fwd.x, L.fwd.y, L.fwd.z, float(Device.dwFrame & 63u));
    cmd.set_c("da_shrub_time", L.time, L.time_prev, L.motion ? 1.f : 0.f, texel);
    cmd.set_c("da_shrub_sun", sun.x, sun.y, sun.z, L.snow);
    // (the world's up in this view: the pixel shaders turn their view-space normals to the world by it)
    Fvector up;
    cmd.xforms.m_v.transform_dir(up, Fvector{0.f, 1.f, 0.f});
    cmd.set_c("da_shrub_up", up.x, up.y, up.z, 0.f);
    cmd.SRVSManager.SetVSResource(0, P.elems_b.srv);
    cmd.SRVSManager.SetVSResource(1, P.axes_b.srv);
    cmd.SRVSManager.SetVSResource(2, g.inst_b.srv);
    cmd.SRVSManager.SetVSResource(3, g.chunks_b.srv);
    cmd.SRVSManager.SetVSResource(4, g.tables_b.srv);
    cmd.SRVSManager.SetVSResource(5, g.bend_b.srv);
    cmd.SRVSManager.SetVSResource(6, P.aux_b.srv ? P.aux_b.srv : g.dummy_b.srv);
    cmd.set_c("da_shrub_bend_cap", float(g.bend_chunks * kChunk), float(g.bend_view_chunks * kChunk), 0.f, 0.f);
    cmd.SRVSManager.SetPSResource(4, g.tables_b.srv);
    if (cut)
        cmd.RenderIndirect(g.cut_args_b.buf, (u32(cut_phase) * drCount + d) * 20, 1);
    else
        cmd.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, vc.verts, vc.first, vc.indices / 3, count);
    for (u32 slot = 0; slot < 7; ++slot)
        cmd.SRVSManager.SetVSResource(slot, nullptr);
    cmd.SRVSManager.SetPSResource(4, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
}

// The frame's chunks of draw d of population P in pass `pass` (the view, a sun cascade, the rain map).
void draw(CBackend& cmd, const Population& P, const ref_shader& shader, u32 pass, u32 d)
{
    const float smap = float(std::max(u32(RImplementation.o.smapsize), 256u));
    float texel = 0.f;
    Fvector sun = g.latch.sun;
    if (pass == kRainPass)
    {
        texel = 2.f * rain_reach() / float(std::max(u32(RImplementation.o.rain_smapsize), 64u));
        sun.set(0.f, 1.f, 0.f);
    }
    else if (pass)
        texel = cascade_size(pass - 1) / smap;
    draw_range(cmd, P, shader, P.draw_first[pass][d], P.draw_count[pass][d], pass, d, sun, texel);
    // The trees' GPU cut's list of the draw, the near trees' (the view's: r3\da_tree_cut.cs).
    if (!pass && P.index == kPopTrees && g.cut_n[0] && g.cut_reg[0][d][1])
        draw_range(cmd, P, shader, g.cut_reg[0][d][0], 1, pass, d, sun, texel, 0);
    // The shrubs' (their every view draw goes through the cut while it runs).
    if (!pass && P.index == kPopShrubs && g.cut_n[kCutShrubs] && g.cut_reg[kCutShrubs][d][1])
        draw_range(cmd, P, shader, g.cut_reg[kCutShrubs][d][0], 1, pass, d, sun, texel, s32(kCutShrubs));
}

// The far trees' list of draw d of the cut (the view's second phase).
void draw_cut_far(CBackend& cmd, const Population& P, const ref_shader& shader, u32 d)
{
    if (g.cut_n[1] && g.cut_reg[1][d][1])
        draw_range(cmd, P, shader, g.cut_reg[1][d][0], 1, 0, d, g.latch.sun, 0.f, 1);
}

// ======================================================================================================
// Setup
// ======================================================================================================
// Every shader and texture array ships with the content: an install with older shaders has none of them, compiling
// a missing file is fatal, and a missing array would bind the engine's 2D stand-in (every leaf cut away).
constexpr pcstr kShaderFiles[] = {"da_shrub_wood.vs", "da_shrub_wood.ps", "da_shrub_leaf.vs", "da_shrub_leaf.ps",
    "da_shrub_bloom.vs", "da_shrub_bloom.ps", "da_shrub_cluster.vs", "da_shrub_cluster.ps",
    "da_shrub_wood_shadow.vs", "da_shrub_leaf_shadow.vs", "da_shrub_leaf_shadow.ps", "da_shrub_cluster_shadow.vs",
    "da_shrub_cluster_shadow.ps", "da_shrub_bend.cs"};
// The trees' own (their stock tubes and patches, their billboard atlas's fill; everything else is the shrubs', the
// billboards the stock lod shaders).
constexpr pcstr kTreeShaderFiles[] = {"da_tree_stock.vs", "da_tree_stock_shadow.vs", "da_tree_patch.vs", "da_tree_patch_shadow.vs",
    "da_tree_bb_copy.vs", "da_tree_bb_copy.ps", "da_tree_lod.ps", "da_shrub_wood_far.vs", "da_shrub_wood_far.ps", "da_tree_stock_far.vs",
    "da_tree_patch_far.vs", "da_tree_cut.cs", "da_tree_cut_args.cs"};

constexpr pcstr kTextureFiles[] = {"da\\shrub_leaf_d.dds", "da\\shrub_leaf_n.dds", "da\\shrub_bark_d.dds",
    "da\\shrub_bark_n.dds"};

bool shaders_present()
{
    for (pcstr file : kShaderFiles)
        if (!da_async_shader_exists(file))
            return false;
    for (pcstr file : kTextureFiles)
        if (!FS.exist("$game_textures$", file))
            return false;
    return true;
}

bool tree_shaders_present()
{
    for (pcstr file : kTreeShaderFiles)
        if (!da_async_shader_exists(file))
            return false;
    return true;
}

bool ready(const ref_shader& s)
{
    return s && s->E[0] && !s->E[0]->passes.empty() && s->E[0]->passes[0]->vs && s->E[0]->passes[0]->vs->sh;
}

bool create_shaders(bool trees)
{
    // The G-buffer: the wood, the leaves (hulls and quads, every one cut out by its texture), blooms, clusters and blobs
    // (one shader, the draw's kind tells them apart).
    static CBlender_da_shrub b_wood("da_shrub_wood", "da_shrub_wood", false), b_leaf("da_shrub_leaf", "da_shrub_leaf", false),
        b_bloom("da_shrub_bloom", "da_shrub_bloom", false), b_cluster("da_shrub_cluster", "da_shrub_cluster", false);
    static CBlender_da_shrub s_wood("da_shrub_wood_shadow", "null", true), s_leaf("da_shrub_leaf_shadow", "da_shrub_leaf_shadow", true),
        s_cluster("da_shrub_cluster_shadow", "da_shrub_cluster_shadow", true);
    g.s_draw[drWood].create(&b_wood, "r2" DELIMITER "da_shrub_wood");
    g.s_draw[drWood8] = g.s_draw[drWood];
    g.s_draw[drWood16] = g.s_draw[drWood];
    g.s_draw[drLeaf8].create(&b_leaf, "r2" DELIMITER "da_shrub_leaf");
    g.s_draw[drLeaf4] = g.s_draw[drLeaf8];
    g.s_draw[drSpray] = g.s_draw[drLeaf8];
    g.s_draw[drLeafCut] = g.s_draw[drLeaf8];
    g.s_draw[drSprayCut] = g.s_draw[drLeafCut];
    g.s_draw[drSpray4] = g.s_draw[drLeaf8];
    g.s_draw[drNeedle2] = g.s_draw[drLeaf8];
    g.s_draw[drBloom].create(&b_bloom, "r2" DELIMITER "da_shrub_bloom");
    g.s_draw[drCluster].create(&b_cluster, "r2" DELIMITER "da_shrub_cluster");
    g.s_draw[drBlob] = g.s_draw[drCluster];
    g.s_shadow[drWood].create(&s_wood, "r2" DELIMITER "da_shrub_wood_shadow");
    g.s_shadow[drWood8] = g.s_shadow[drWood];
    g.s_shadow[drWood16] = g.s_shadow[drWood];
    g.s_shadow[drLeaf4].create(&s_leaf, "r2" DELIMITER "da_shrub_leaf_shadow");
    g.s_shadow[drLeafCut] = g.s_shadow[drLeaf4];
    g.s_shadow[drCluster].create(&s_cluster, "r2" DELIMITER "da_shrub_cluster_shadow");
    g.s_shadow[drBlob] = g.s_shadow[drCluster];
    static CBlender_da_shrub_compute b_bend("da_shrub_bend");
    g.s_bend.create(&b_bend, "r2" DELIMITER "da_shrub_bend");
    if (!g.s_bend || !g.s_bend->E[0] || g.s_bend->E[0]->passes.empty() || !g.s_bend->E[0]->passes[0]->cs)
        return false;
    for (u32 d : {drWood, drLeaf8, drLeafCut, drBloom, drCluster})
        if (!ready(g.s_draw[d]))
            return false;
    for (u32 d : {drWood, drLeaf4, drLeafCut, drCluster})
        if (!ready(g.s_shadow[d]))
            return false;
    if (trees)
    {
        // The trees' stock wood: its tubes on the stock rings and the stock triangles, with the shrubs' bark shader.
        static CBlender_da_shrub b_stock("da_tree_stock", "da_shrub_wood", false), b_patch("da_tree_patch", "da_shrub_wood", false);
        static CBlender_da_shrub s_stock("da_tree_stock_shadow", "null", true), s_patch("da_tree_patch_shadow", "null", true);
        g.s_draw[drStock32].create(&b_stock, "r2" DELIMITER "da_tree_stock");
        g.s_draw[drStock16] = g.s_draw[drStock32];
        g.s_draw[drStock8] = g.s_draw[drStock32];
        g.s_draw[drPatch].create(&b_patch, "r2" DELIMITER "da_tree_patch");
        g.s_shadow[drStock32].create(&s_stock, "r2" DELIMITER "da_tree_stock_shadow");
        g.s_shadow[drStock16] = g.s_shadow[drStock32];
        g.s_shadow[drStock8] = g.s_shadow[drStock32];
        g.s_shadow[drPatch].create(&s_patch, "r2" DELIMITER "da_tree_patch_shadow");
        // The far wood: the bark's mean worked out per vertex (r3\da_shrub_wood_far.ps).
        static CBlender_da_shrub b_wood_far("da_shrub_wood_far", "da_shrub_wood_far", false),
            b_stock_far("da_tree_stock_far", "da_shrub_wood_far", false), b_patch_far("da_tree_patch_far", "da_shrub_wood_far", false);
        g.s_draw[drWoodFar].create(&b_wood_far, "r2" DELIMITER "da_shrub_wood_far");
        g.s_draw[drStockFar].create(&b_stock_far, "r2" DELIMITER "da_tree_stock_far");
        g.s_draw[drPatchFar].create(&b_patch_far, "r2" DELIMITER "da_tree_patch_far");
        // The view's GPU cut and its draws' arguments (optional: without them the CPU's cut stands).
        static CBlender_da_shrub_compute b_cut("da_tree_cut"), b_cut_args("da_tree_cut_args");
        g.s_cut.create(&b_cut, "r2" DELIMITER "da_tree_cut");
        g.s_cut_args.create(&b_cut_args, "r2" DELIMITER "da_tree_cut_args");
        for (ref_shader* s : {&g.s_cut, &g.s_cut_args})
            if (*s && (!(*s)->E[0] || (*s)->E[0]->passes.empty() || !(*s)->E[0]->passes[0]->cs))
                s->destroy();
    }
    return true;
}

bool trees_ready()
{
    return ready(g.s_draw[drStock32]) && ready(g.s_draw[drPatch]) && ready(g.s_shadow[drStock32]) && ready(g.s_shadow[drPatch]) &&
        ready(g.s_draw[drWoodFar]) && ready(g.s_draw[drStockFar]) && ready(g.s_draw[drPatchFar]);
}

// The static geometry: per vertex class one chunk of 64 elements as an index list (values element x V + local); the
// vertex stream is a placeholder (the vertex shaders read SV_VertexID only).
bool create_geometry()
{
    xr_vector<u16> indices;
    const auto add_class = [&indices](VertexClass c, u32 verts, std::initializer_list<u16> one) {
        g.vclass[c] = {verts * kChunk, u32(one.size()) * kChunk, u32(indices.size())};
        for (u32 e = 0; e < kChunk; ++e)
            for (const u16 i : one)
                indices.push_back(u16(e * verts + i));
    };
    // A tube segment of n sides: ring a (0..n) and ring b (n+1..2n+1), the last vertex of each a copy of its first with
    // the bark's u at a whole wrap.
    const auto add_tube = [&indices](VertexClass c, u16 n) {
        const u16 verts = u16(2 * (n + 1));
        g.vclass[c] = {u32(verts) * kChunk, u32(6 * n) * kChunk, u32(indices.size())};
        for (u32 e = 0; e < kChunk; ++e)
            for (u16 k = 0; k < n; ++k)
            {
                const u16 a0 = u16(e * verts + k), a1 = u16(a0 + 1), b0 = u16(a0 + n + 1), b1 = u16(b0 + 1);
                for (const u16 i : {a0, b0, a1, a1, b0, b1})
                    indices.push_back(i);
            }
    };
    add_tube(vcTube, 5);
    add_tube(vcTube8, 8);
    // A leaf's hull: four rows of a pair (0, 1), (2, 3), (4, 5), (6, 7), base to tip.
    add_class(vcLeaf8, 8, {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4, 4, 5, 6, 5, 7, 6});
    add_class(vcQuad, 4, {0, 1, 2, 2, 1, 3});
    add_class(vcQuad2, 8, {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7});
    // A near spray: kSprayLeaves leaf hulls of eight vertices.
    {
        constexpr u16 leaf[] = {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4, 4, 5, 6, 5, 7, 6};
        u16 one[kSprayLeaves * std::size(leaf)];
        for (u32 j = 0; j < kSprayLeaves; ++j)
            for (u32 k = 0; k < std::size(leaf); ++k)
                one[j * std::size(leaf) + k] = u16(j * 8 + leaf[k]);
        g.vclass[vcSpray] = {8 * kSprayLeaves * kChunk, u32(std::size(one)) * kChunk, u32(indices.size())};
        for (u32 e = 0; e < kChunk; ++e)
            for (const u16 i : one)
                indices.push_back(u16(e * 8 * kSprayLeaves + i));
    }
    // A spray past d0: the same leaves as diamonds (base 0, sides 1 and 2, tip 3).
    {
        constexpr u16 diamond[] = {0, 1, 2, 2, 1, 3};
        u16 one[kSprayLeaves * std::size(diamond)];
        for (u32 j = 0; j < kSprayLeaves; ++j)
            for (u32 k = 0; k < std::size(diamond); ++k)
                one[j * std::size(diamond) + k] = u16(j * 4 + diamond[k]);
        g.vclass[vcSpray4] = {4 * kSprayLeaves * kChunk, u32(std::size(one)) * kChunk, u32(indices.size())};
        for (u32 e = 0; e < kChunk; ++e)
            for (const u16 i : one)
                indices.push_back(u16(e * 4 * kSprayLeaves + i));
    }
    // The trees' near tubes (a trunk's silhouette at arm's length) and their stock triangles.
    add_tube(vcTube16, 16);
    add_tube(vcTube32, 32);
    add_class(vcTri, 3, {0, 1, 2});
    static const VertexElement decl[] = {{0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    // The placeholder stream covers the largest class's vertices.
    u32 verts = 0;
    for (const VertexClassInfo& c : g.vclass)
        verts = std::max(verts, c.verts);
    const xr_vector<float> zero(verts, 0.f);
    g.vb.Create(verts * sizeof(float));
    CopyMemory(g.vb.Map(), zero.data(), verts * sizeof(float));
    g.vb.Unmap(true);
    g.ib.Create(indices.size() * sizeof(u16));
    CopyMemory(g.ib.Map(), indices.data(), indices.size() * sizeof(u16));
    g.ib.Unmap(true);
    g.geom.create(decl, g.vb, g.ib);
    return bool(g.geom);
}

void release_gpu()
{
    for (u32 d = 0; d < drCount; ++d)
    {
        g.s_draw[d].destroy();
        g.s_shadow[d].destroy();
    }
    g.s_bend.destroy();
    g.s_cut.destroy();
    g.s_cut_args.destroy();
    for (GpuBuffer* b : {&g.inst_b, &g.chunks_b, &g.tables_b, &g.bend_b, &g.dummy_b, &g.cut_routes_b, &g.cut_counts_b, &g.cut_args_b})
        b->release();
    _RELEASE(g.cut_staging);
    g.cut_count = g.cut_readback = 0;
    std::fill(std::begin(g.cut_n), std::end(g.cut_n), 0u);
    g.bend_chunks = g.bend_view_chunks = g.bend_view_end = 0;
    g.bend_failed = g.bend_ready = false;
    if (g.geom)
        g.geom.destroy();
    if (g.ib.IsValid())
        g.ib.Release();
    if (g.vb.IsValid())
        g.vb.Release();
    g.gpu = false;
}

void release_all()
{
    // The stock models come back into their hierarchies before anything of the level goes.
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi))
            P->release();
    release_gpu();
    // Every file-scope value belongs to one level (second-load bugs): the next one starts from scratch.
    const u32 preset = g.preset;
    g.~State();
    new (&g) State();
    g.preset = preset;
}

// The bend buffer holds this frame's chunks (and a quarter more for the local lights' chunks appended later) and the
// view's a frame back: grown here only, in Calculate, before the cascades' tasks bind it.
void bend_reserve()
{
    if (g.bend_failed || !g.s_bend)
        return;
    const u32 need = std::min(g.bend_n + g.bend_n / 4 + 256u, kBendMaxChunks);
    const u32 need_view = std::min(g.bend_view_n, kBendMaxViewChunks);
    if (need <= g.bend_chunks && need_view <= g.bend_view_chunks)
        return;
    const u32 cap = std::max(g.bend_chunks, std::min(std::max(need + need / 4, 2048u), kBendMaxChunks));
    const u32 cap_view = std::max(g.bend_view_chunks, std::min(std::max(need_view + need_view / 4, 1024u), kBendMaxViewChunks));
    if (!make_buffer(g.bend_b, (cap + cap_view) * kChunk * kBendRec, nullptr, true))
    {
        Msg("! [veg] the bend buffer (%u + %u chunks) could not be created: the vertex shaders bend themselves", cap, cap_view);
        g.bend_b.release();
        g.bend_chunks = g.bend_view_chunks = 0;
        g.bend_failed = true;
        return;
    }
    g.bend_chunks = cap;
    g.bend_view_chunks = cap_view;
}

// The bends of chunks [first, first + count) of the frame, all of population P (r3\da_shrub_bend.cs), and of the
// view's a frame back (each lane past the buffer's slots leaves its vertices to bend themselves).
void bend_dispatch(CBackend& cmd, const Population& P, u32 first, u32 count)
{
    if (!count || !g.s_bend || !g.bend_b.uav)
        return;
    const Latch& L = g.latch;
    SPass& S = *g.s_bend->E[0]->passes[0];
    cmd.set_States(S.state);
    cmd.set_CS(S.cs);
    cmd.set_Constants(S.constants);
    const u32 gx = std::min(count, 32768u), gy = (count + gx - 1) / gx;
    cmd.set_c("da_shrub_bend0", float(first), float(count), float(gx), float(g.bend_view_end));
    cmd.set_c("da_shrub_bend_cap", float(g.bend_chunks * kChunk), float(g.bend_view_chunks * kChunk), 0.f, 0.f);
    cmd.set_c("da_shrub_time", L.time, L.time_prev, L.motion ? 1.f : 0.f, 0.f);
    cmd.SRVSManager.SetCSResource(0, P.elems_b.srv);
    cmd.SRVSManager.SetCSResource(1, P.axes_b.srv);
    cmd.SRVSManager.SetCSResource(2, g.inst_b.srv);
    cmd.SRVSManager.SetCSResource(3, g.chunks_b.srv);
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    const UINT initial = ~0u;
    ctx->CSSetUnorderedAccessViews(0, 1, &g.bend_b.uav, &initial);
    cmd.Compute(gx, gy, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
    for (u32 slot = 0; slot < 4; ++slot)
        cmd.SRVSManager.SetCSResource(slot, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
    cmd.clear_CS_resources();
}

dx11GpuTimers::Slot bend_timer(u32 pi) { return pi == kPopShrubs ? dx11GpuTimers::ShrubsBend : dx11GpuTimers::TreesBend; }

// The trees' GPU cut's lists of the frame: each draw's room the elements of the candidates that may take it (whole
// instances), one after another past the chunk records; the candidates' routes to the GPU.
void cut_prepare(ID3D11DeviceContext* ctx)
{
    std::memset(g.cut_reg, 0, sizeof(g.cut_reg));
    if (!g.cut_count)
    {
        g.cut_count = 0;
        std::fill(std::begin(g.cut_n), std::end(g.cut_n), 0u);
        return;
    }
    u32 words = 0, routes = 0;
    for (u32 q = 0; q < kCutPhases; ++q)
    {
        for (u32 d = 0; d < drCount; ++d)
        {
            const u32 room = (g.cut_room[q][d] + kChunk - 1) / kChunk * kChunk;
            const VertexClassInfo& vc = g.vclass[kDrawClass[d]];
            g.cut_reg[q][d][0] = words;
            g.cut_reg[q][d][1] = room;
            g.cut_reg[q][d][2] = vc.indices;
            g.cut_reg[q][d][3] = vc.first;
            words += room;
        }
        // (The phases' routes one after the other, as their chunks are.)
        if (g.cut_n[q])
        {
            const D3D11_BOX box{routes * kCutRoute, 0, 0, (routes + g.cut_n[q]) * kCutRoute, 1, 1};
            ctx->UpdateSubresource(g.cut_routes_b.buf, 0, &box, pop(cut_owner(q))->cut_routes[q].data(), 0, 0);
        }
        routes += g.cut_n[q];
    }
}

// The trees' GPU cut of phase `phase` (r3\da_tree_cut.cs: after the frame's bends, its elements bent as their vertices
// are), each element against this frame's depth pyramid (da_hzb.h: the opaque world's, and the near trees' too for the
// far phase), and its draws' arguments (da_tree_cut_args.cs); QA: a readback of the words each draw took, logged when it
// arrives.
void cut_dispatch(CBackend& cmd, u32 phase)
{
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    if (phase == 0 && g.cut_readback == 2)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.cut_staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            static constexpr pcstr names[drCount] = {"wood", "leaf 8", "leaf 4", "leaf cut", "bloom", "cluster", "blob", "spray",
                "spray cut", "spray 4", "wood 8", "needle 2", "wood 16", "stock 32", "stock 16", "stock 8", "patch", "wood far",
                "stock far", "patch far"};
            const u32* took = static_cast<const u32*>(m.pData);
            for (u32 q = 0; q < kCutPhases; ++q)
            {
                xr_string line;
                for (u32 d = 0; d < drCount; ++d)
                    if (const u32 n = took[q * drCount + d])
                    {
                        if (!line.empty())
                            line += ", ";
                        line += names[d];
                        line += " ";
                        line += std::to_string(n).c_str();
                    }
                Msg("* [trees] the GPU cut drew of the %s: %s", q == kCutShrubs ? "shrubs" : (q ? "far trees" : "near trees"),
                    line.empty() ? "nothing" : line.c_str());
            }
            ctx->Unmap(g.cut_staging, 0);
        }
        g.cut_readback = 0;
    }
    const Population* P = pop(cut_owner(phase));
    if (!g.cut_n[phase] || !pop_on(P))
        return;
    const bool hzb = ps_r__trees_occlusion && da_hzb_build(cmd);
    const Latch& L = g.latch;
    static constexpr u32 zero[drCount]{};
    {
        const D3D11_BOX box{phase * drCount * 4, 0, 0, (phase + 1) * drCount * 4, 1, 1};
        ctx->UpdateSubresource(g.cut_counts_b.buf, 0, &box, zero, 0, 0);
    }
    const auto lists = [&cmd, phase] {
        for (u32 d = 0; d < drCount; ++d)
        {
            const u32(&r)[4] = g.cut_reg[phase][d];
            cmd.set_ca("da_tree_cut_reg", d, float(r[0]), float(r[1]), float(r[2]), float(r[3]));
        }
        cmd.set_c("da_tree_cut1", float(phase * drCount), 0.f, 0.f, 0.f);
    };
    u32 first = 0;
    for (u32 q = 0; q < phase; ++q)
        first += g.cut_n[q];
    const u32 count = g.cut_n[phase];
    const UINT initial[3] = {~0u, ~0u, ~0u};
    ID3D11UnorderedAccessView* none[3]{};
    {
        SPass& S = *g.s_cut->E[0]->passes[0];
        cmd.set_States(S.state);
        cmd.set_CS(S.cs);
        cmd.set_Constants(S.constants);
        const u32 gx = std::min(count, 32768u), gy = (count + gx - 1) / gx;
        cmd.set_c("da_tree_cut0", float(g.cut_first + first), float(count), float(gx), float(first));
        for (u32 k = 0; k < 6; ++k)
            cmd.set_ca("da_tree_cut_planes", k, g.cut_planes[k].x, g.cut_planes[k].y, g.cut_planes[k].z, g.cut_planes[k].w);
        lists();
        cmd.set_c("da_shrub_cam", L.cam.x, L.cam.y, L.cam.z, 1.f / std::max(L.px_per_m, 1.f));
        cmd.set_c("da_shrub_fwd", L.fwd.x, L.fwd.y, L.fwd.z, 0.f);
        cmd.set_c("da_shrub_time", L.time, L.time_prev, L.motion ? 1.f : 0.f, 0.f);
        cmd.set_c("da_shrub_bend_cap", float(g.bend_chunks * kChunk), float(g.bend_view_chunks * kChunk), 0.f, 0.f);
        da_hzb_bind(cmd, 8, hzb);
        cmd.SRVSManager.SetCSResource(0, P->elems_b.srv);
        cmd.SRVSManager.SetCSResource(1, P->axes_b.srv);
        cmd.SRVSManager.SetCSResource(2, g.inst_b.srv);
        cmd.SRVSManager.SetCSResource(4, g.tables_b.srv);
        cmd.SRVSManager.SetCSResource(5, g.bend_b.srv ? g.bend_b.srv : g.dummy_b.srv);
        cmd.SRVSManager.SetCSResource(6, P->aux_b.srv ? P->aux_b.srv : g.dummy_b.srv);
        cmd.SRVSManager.SetCSResource(7, g.cut_routes_b.srv);
        ID3D11UnorderedAccessView* uavs[2] = {g.chunks_b.uav, g.cut_counts_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, initial);
        cmd.Compute(gx, gy, 1);
        ctx->CSSetUnorderedAccessViews(0, 2, none, initial);
        for (u32 slot = 0; slot < 9; ++slot)
            cmd.SRVSManager.SetCSResource(slot, nullptr);
        cmd.SRVSManager.Apply(cmd.context_id);
        cmd.clear_CS_resources();
    }
    {
        SPass& S = *g.s_cut_args->E[0]->passes[0];
        cmd.set_States(S.state);
        cmd.set_CS(S.cs);
        cmd.set_Constants(S.constants);
        lists();
        ID3D11UnorderedAccessView* uavs[3] = {g.chunks_b.uav, g.cut_counts_b.uav, g.cut_args_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, initial);
        cmd.Compute(drCount, 1, 1);
        ctx->CSSetUnorderedAccessViews(0, 3, none, initial);
        cmd.clear_CS_resources();
    }
    u32 last = 0;
    for (u32 q = 0; q < kCutPhases; ++q)
        if (g.cut_n[q])
            last = q;
    if (phase == last && g.cut_readback == 1)
    {
        ctx->CopyResource(g.cut_staging, g.cut_counts_b.buf);
        g.cut_readback = 2;
    }
    // The compute passes leave the G-buffer's targets, viewport and stencil to the draws after them.
    RImplementation.Target->phase_scene_begin_parallel(cmd, cmd.xforms.m_motion_pass);
}
} // namespace

// ======================================================================================================
// Interface
// ======================================================================================================
bool da_veg_available()
{
    static const bool present = shaders_present();
    return present && !g.failed;
}

bool da_veg_active()
{
    for (u32 pi = 0; pi < kPops; ++pi)
        if (pop_on(pop(pi)))
            return true;
    return false;
}

void da_veg_apply_preset(u32 preset)
{
    g.preset = std::min(preset, 4u);
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi))
            P->apply_preset(g.preset);
}

void da_veg_level_unload()
{
    release_all();
    g.failed = false;
}

void da_veg_destroy()
{
    release_all();
    g.failed = false;
}

void da_veg_level_load()
{
    release_all();
    g.failed = false;
    dx11GpuTimers::RegisterField("veg_wait", [] {
        const u64 ticks = g_wait_ticks.exchange(0, std::memory_order_relaxed);
        const u32 frames = g_wait_frames.exchange(0, std::memory_order_relaxed);
        return frames ? float(double(ticks) * 1000.0 / double(CPU::qpc_freq) / frames) : 0.f;
    });
    if (!g_pGameLevel)
        return;
    const bool shaders = shaders_present();
    bool any = false;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi))
        {
            P->apply_preset(g.preset);
            any |= P->prepare(shaders, pi == kPopTrees ? shaders && tree_shaders_present() : shaders);
        }
    if (!any)
        return;
    const auto fail = [](pcstr why) {
        Msg("! [veg] the procedural vegetation is off on this level (%s): the stock models stand", why);
        release_all();
        g.failed = true;
    };
    Population* trees = pop(kPopTrees);
    const bool want_trees = trees && trees->pending();
    if (!create_shaders(want_trees))
        return fail("the shaders failed to compile");
    if (want_trees && !trees_ready())
    {
        Msg("! [trees] the trees' shaders failed to compile: the stock trees stand");
        trees->release();
    }
    if (!create_geometry())
        return fail("the geometry could not be created");
    if (!make_buffer(g.inst_b, kMaxInstances * kInstRec, nullptr) || !make_buffer(g.tables_b, kTabTotal * 16, nullptr) ||
        !make_buffer(g.dummy_b, 64, nullptr))
        return fail("the frame buffers could not be created");
    // The chunk buffer: the frame's chunk records, the trees' GPU cut's lists past them (written by the cut: a vertex
    // shader has no resource slot to spare). Without the cut's room the records alone, the CPU's cut standing.
    if (!g.s_cut || !make_buffer(g.chunks_b, kMaxChunks * kChunkRec + kMaxCutWords * 4, nullptr, true))
    {
        if (!make_buffer(g.chunks_b, kMaxChunks * kChunkRec, nullptr))
            return fail("the frame buffers could not be created");
    }
    else
    {
        static constexpr u32 zero_args[kCutPhases * drCount * 5]{};
        D3D11_BUFFER_DESC staging{};
        staging.ByteWidth = kCutPhases * drCount * 4;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!make_buffer(g.cut_routes_b, kMaxCutChunks * kCutRoute, nullptr) || !make_buffer(g.cut_counts_b, kCutPhases * drCount * 4, nullptr, true) ||
            !make_buffer(g.cut_args_b, sizeof(zero_args), zero_args, true, false, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) ||
            FAILED(HW.pDevice->CreateBuffer(&staging, nullptr, &g.cut_staging)))
        {
            Msg("! [trees] the GPU cut's buffers could not be created: the CPU's cut stands");
            for (GpuBuffer* b : {&g.cut_routes_b, &g.cut_counts_b, &g.cut_args_b})
                b->release();
            _RELEASE(g.cut_staging);
        }
    }
    g.gpu = true;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); P && P->pending())
            P->finish_load();
    g.tab_day = -1000.f;
    g.latch.frame = u32(-1);
}

void da_veg_apply_switch()
{
    bool change = false;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (const Population* P = pop(pi); P && P->loaded && P->detached != P->wanted())
            change = true;
    if (!change)
        return;
    // The GPU-driven registry and the ray tracing scene hold the hierarchies' trees: they let go of them first and
    // gather them again after the stock models left (or rejoined) their parents.
    da_gpu_trees_level_unload();
    da_rt_level_unload();
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); P && P->loaded && P->detached != P->wanted())
            P->set_detached(P->wanted(), true);
    da_gpu_trees_level_load();
    da_rt_level_load();
    da_history_request_reset("r__shrubs/r__trees");
}

namespace
{
// The frame's pick on a worker (da_veg_frame_begin), joined by da_veg_frame_prepare.
Task* g_frame_task = nullptr;

void frame_pick()
{
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
            P->cpu_frames.fetch_add(1, std::memory_order_relaxed);
    {
        Population* first = pop_on(pop(kPopShrubs)) ? pop(kPopShrubs) : pop(kPopTrees);
        CpuSpan span(*first);
        latch_frame();
    }
    build_frame();
}
} // namespace

void da_veg_frame_begin()
{
    if (g_frame_task || !da_veg_active())
        return;
    // It reads the camera, the environment, the occlusion raster and the sun's and rain's decisions, and writes only
    // the vegetation's own frame: the main view's walk beside it touches none of that.
    g_frame_task = &TaskScheduler->AddTask([] { frame_pick(); });
}

void da_veg_frame_prepare(CBackend& cmd)
{
    const u64 t0 = CPU::QPC();
    if (Task* task = std::exchange(g_frame_task, nullptr))
        TaskScheduler->Wait(*task);
    else if (da_veg_active())
        frame_pick();
    g_wait_ticks.fetch_add(CPU::QPC() - t0, std::memory_order_relaxed);
    g_wait_frames.fetch_add(1, std::memory_order_relaxed);
    if (!da_veg_active())
        return;
    Population* first = pop_on(pop(kPopShrubs)) ? pop(kPopShrubs) : pop(kPopTrees);
    CpuSpan span(*first);
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    if (g.tab_upload)
    {
        ctx->UpdateSubresource(g.tables_b.buf, 0, nullptr, g.tables, 0, 0);
        g.tab_upload = false;
    }
    if (!g.inst_records.empty())
    {
        const D3D11_BOX box{0, 0, 0, u32(g.inst_records.size()), 1, 1};
        ctx->UpdateSubresource(g.inst_b.buf, 0, &box, g.inst_records.data(), 0, 0);
    }
    if (!g.chunks.empty())
    {
        const D3D11_BOX box{0, 0, 0, u32(g.chunks.size()) * kChunkRec, 1, 1};
        ctx->UpdateSubresource(g.chunks_b.buf, 0, &box, g.chunks.data(), 0, 0);
    }
    cut_prepare(ctx);
    bend_reserve();
    g.bend_ready = true;
}

void da_veg_bend(CBackend& cmd)
{
    if (!da_veg_active() || g.latch.frame != Device.dwFrame)
        return;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (Population* P = pop(pi); pop_on(P))
            P->pre_render(cmd);
    if (!g.bend_ready)
        return;
    g.bend_ready = false;
    for (const BendBlock& b : g.bend_blocks)
        if (const Population* P = pop(b.pop))
        {
            dx11GpuTimerScope gpu(bend_timer(b.pop));
            bend_dispatch(cmd, *P, b.first, b.count);
        }
}

void da_veg_render_gbuffer(CBackend& cmd)
{
    if (!da_veg_active() || g.latch.frame != Device.dwFrame)
        return;
    // The trees before the shrubs: their trunks and crowns hide the shrubs behind them from their pixel shaders.
    for (u32 pi = kPops; pi-- > 0;)
    {
        Population* P = pop(pi);
        if (!pop_on(P))
            continue;
        {
            dx11GpuTimerScope gpu(pi == kPopShrubs ? dx11GpuTimers::Shrubs : dx11GpuTimers::Trees);
            // The trees' cut in two phases: the near trees against the depth the opaque world left, drawn with every chunk
            // the CPU chose; then the far ones against it with the near crowns in it - what a wall, a hill or a near
            // crown hides runs no vertex.
            const bool trees = pi == kPopTrees;
            if (trees)
                cut_dispatch(cmd, 0);
            else
                cut_dispatch(cmd, kCutShrubs);
            cmd.set_xform_world(Fidentity);
            for (const u32 d : P->view_draws())
                draw(cmd, *P, g.s_draw[d], 0, d);
            if (trees && g.cut_n[1])
            {
                cut_dispatch(cmd, 1);
                cmd.set_xform_world(Fidentity);
                for (const u32 d : P->view_draws())
                    draw_cut_far(cmd, *P, g.s_draw[d], d);
            }
            cmd.set_CullMode(CULL_CCW);
        }
        P->render_extra(cmd);
    }
}

bool da_veg_casts(u32 cascade)
{
    if (!da_veg_active())
        return false;
    if (g.latch.frame != Device.dwFrame)
    {
        g.stale_passes.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (cascade >= kCascades)
        return false;
    for (u32 pi = 0; pi < kPops; ++pi)
    {
        const Population* P = pop(pi);
        if (!pop_on(P) || cascade >= P->cascades)
            continue;
        for (u32 d = 0; d < drCount; ++d)
            if (P->draw_count[1 + cascade][d])
                return true;
    }
    return false;
}

void da_veg_render_cascade(CBackend& cmd, u32 cascade)
{
    if (!da_veg_casts(cascade))
        return;
    cmd.set_xform_world(Fidentity);
    for (u32 pi = 0; pi < kPops; ++pi)
    {
        const Population* P = pop(pi);
        if (!pop_on(P) || cascade >= P->cascades)
            continue;
        for (const u32 d : P->shadow_draws())
            draw(cmd, *P, g.s_shadow[d], 1 + cascade, d);
    }
    cmd.set_CullMode(CULL_CCW);
}

bool da_veg_casts_rain()
{
    if (!da_veg_active() || g.latch.frame != Device.dwFrame)
        return false;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (const Population* P = pop(pi); pop_on(P) && P->draw_count[kRainPass][drBlob] > 0)
            return true;
    return false;
}

void da_veg_render_rain(CBackend& cmd)
{
    if (!da_veg_casts_rain())
        return;
    for (u32 pi = 0; pi < kPops; ++pi)
        if (const Population* P = pop(pi); pop_on(P))
            draw(cmd, *P, g.s_shadow[drBlob], kRainPass, drBlob);
    cmd.set_CullMode(CULL_CCW);
}

void da_veg_lights_prepare(CBackend& cmd, const DaVegLight* lights, u32 count)
{
    if (g.lights_frame != Device.dwFrame)
    {
        g.lights.clear();
        g.lights_frame = Device.dwFrame;
        g.lights_offered = g.lights_near = 0;
    }
    if (!da_veg_active() || g.latch.frame != Device.dwFrame || !count)
        return;
    const Latch& L = g.latch;
    const u32 rec_before = u32(g.inst_records.size()), chunk_before = u32(g.chunks.size());
    xr_vector<BendBlock> blocks;
    g.lights_offered += count;
    for (u32 li = 0; li < count; ++li)
    {
        const DaVegLight& light = lights[li];
        if (light.position.distance_to(L.cam) > kLightReach || light.range <= 0.f)
            continue;
        ++g.lights_near;
        CFrustum frustum;
        frustum.CreateFromMatrix(const_cast<Fmatrix&>(light.combine), FRUSTUM_P_ALL & ~FRUSTUM_P_NEAR);
        // The light's texel at half its reach: its cone over its map.
        const float texel = light.range * std::tan(std::clamp(light.cone, 0.1f, 3.0f) * 0.5f) / float(std::max(light.smap_size, 32u));
        Fbox reach;
        reach.set(light.position, light.position);
        reach.grow(light.range);
        LightDraw draw_l;
        draw_l.key = light.key;
        draw_l.to_light.invert(light.direction);
        if (draw_l.to_light.square_magnitude() < 1e-6f)
            draw_l.to_light.set(0.f, 1.f, 0.f);
        draw_l.to_light.normalize();
        draw_l.texel = texel;
        bool any = false;
        for (u32 pi = 0; pi < kPops; ++pi)
        {
            Population* P = pop(pi);
            if (!pop_on(P))
                continue;
            CpuSpan span(*P);
            for (auto& list : P->pass_chunks[0])
                list.clear();
            for_cells(*P, reach, [&](const Cell& cell) {
                for (u32 k = cell.first; k < cell.first + cell.count; ++k)
                {
                    const u32 i = P->cell_items[k];
                    const Instance& I = P->instances[i];
                    if (I.bound.P.distance_to(light.position) > light.range + I.bound.R || !frustum.testSphere_dirty(I.bound.P, I.bound.R))
                        continue;
                    if (!record_room(*P, i))
                        continue;
                    P->light(i, texel);
                }
            });
            const u32 start = u32(g.chunks.size());
            for (const Draw d : kAppendOrder)
            {
                const u32 room = kMaxChunks - std::min(u32(g.chunks.size()), kMaxChunks);
                const u32 n = std::min(u32(P->pass_chunks[0][d].size()), room);
                draw_l.first[pi][d] = u32(g.chunks.size());
                draw_l.count[pi][d] = n;
                g.chunks.insert(g.chunks.end(), P->pass_chunks[0][d].begin(), P->pass_chunks[0][d].begin() + n);
                any |= n > 0;
            }
            number_bends(start);
            if (u32(g.chunks.size()) > start)
                blocks.push_back({pi, start, u32(g.chunks.size()) - start});
            for (auto& list : P->pass_chunks[0])
                list.clear();
        }
        if (any)
            g.lights.push_back(draw_l);
    }
    // The records and chunks the lights added, after the frame's.
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    if (g.inst_records.size() > rec_before)
    {
        const D3D11_BOX box{rec_before, 0, 0, u32(g.inst_records.size()), 1, 1};
        ctx->UpdateSubresource(g.inst_b.buf, 0, &box, g.inst_records.data() + rec_before, 0, 0);
    }
    if (g.chunks.size() > chunk_before)
    {
        const D3D11_BOX box{chunk_before * kChunkRec, 0, 0, u32(g.chunks.size()) * kChunkRec, 1, 1};
        ctx->UpdateSubresource(g.chunks_b.buf, 0, &box, g.chunks.data() + chunk_before, 0, 0);
        for (const BendBlock& b : blocks)
            if (const Population* P = pop(b.pop))
            {
                // (The lights' bends add to their population's: the timers sum a slot's brackets.)
                if (cmd.context_id == HW.IMM_CTX_ID)
                {
                    dx11GpuTimerScope gpu(bend_timer(b.pop));
                    bend_dispatch(cmd, *P, b.first, b.count);
                }
                else
                    bend_dispatch(cmd, *P, b.first, b.count);
            }
    }
}

bool da_veg_light_casts(const void* light)
{
    for (const LightDraw& l : g.lights)
        if (l.key == light)
            return true;
    return false;
}

void da_veg_render_light(CBackend& cmd, const void* light)
{
    for (const LightDraw& l : g.lights)
    {
        if (l.key != light)
            continue;
        cmd.set_xform_world(Fidentity);
        for (u32 pi = 0; pi < kPops; ++pi)
            if (const Population* P = pop(pi); pop_on(P))
                for (u32 d = 0; d < drCount; ++d)
                    if (l.count[pi][d])
                        draw_range(cmd, *P, g.s_shadow[d], l.first[pi][d], l.count[pi][d], 1, d, l.to_light, l.texel);
        cmd.set_CullMode(CULL_CCW);
        return;
    }
}

void da_veg_replaced_visuals(xr_vector<dxRender_Visual*>& out)
{
    out.clear();
    for (u32 pi = 0; pi < kPops; ++pi)
        if (const Population* P = pop(pi))
            P->replaced_visuals(out);
    da_reeds_replaced_visuals(out);
}

bool da_veg_stand_in(const dxRender_Visual* visual, DaVegStandIn& out)
{
    for (u32 pi = 0; pi < kPops; ++pi)
        if (const Population* P = pop(pi); P && P->detached && P->stand_in(visual, out.species, out.tmpl, out.dead))
        {
            out.population = pi;
            return true;
        }
    return false;
}

bool da_veg_rt_tables(ID3D11Buffer*& buffer, u32& prof_f4, u32& seas_first, u32& seas_f4, float& snow)
{
    if (!da_veg_active() || !g.tables_b.buf)
        return false;
    buffer = g.tables_b.buf;
    prof_f4 = kProfF4;
    seas_first = kTabSeas;
    seas_f4 = kSeasF4;
    snow = g.latch.snow;
    return true;
}

namespace da_veg
{
// The frame's figures the populations' stats print.
u32 frame_records() { return g.records; }
u32 frame_chunks() { return u32(g.chunks.size()); }
u32 frame_bends() { return g.bend_n; }
u32 bend_capacity() { return g.bend_chunks; }
u32 bend_view_capacity() { return g.bend_view_chunks; }
u32 lights_cast() { return u32(g.lights.size()); }
u32 lights_offered() { return g.lights_offered; }
u32 lights_near() { return g.lights_near; }
u32 light_chunks(u32 pi)
{
    u32 n = 0;
    for (const LightDraw& l : g.lights)
        for (u32 d = 0; d < drCount; ++d)
            n += l.count[pi][d];
    return n;
}
u32 stale_passes() { return g.stale_passes.load(std::memory_order_relaxed); }
u32 preset() { return g.preset; }
bool gpu_ready() { return g.gpu; }
void draw_quads(CBackend& cmd, u32 count)
{
    if (!count || !g.geom)
        return;
    const VertexClassInfo& vc = g.vclass[vcQuad];
    cmd.set_Geometry(g.geom);
    // (Whole instances of 64 quads; the vertex shader drops the ones past the count.)
    cmd.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, vc.verts, vc.first, vc.indices / 3, (count + kChunk - 1) / kChunk);
}
u32 chunk_verts(u32 d) { return d < drCount ? g.vclass[kDrawClass[d]].verts : 0; }
void draw_chunks(CBackend& cmd, u32 d, u32 chunks)
{
    if (!chunks || d >= drCount || !g.geom)
        return;
    const VertexClassInfo& vc = g.vclass[kDrawClass[d]];
    cmd.set_Geometry(g.geom);
    cmd.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, vc.verts, vc.first, vc.indices / 3, chunks);
}
Fvector4* tables() { return g.tables; }
} // namespace da_veg
} // namespace xray::render::RENDER_NAMESPACE
