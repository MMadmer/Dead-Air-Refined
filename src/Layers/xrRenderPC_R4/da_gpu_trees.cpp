// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// GPU-driven vegetation registry (da_gpu_trees.h, gpudriven.md 3.2-3.7).
//
// The first frame with r__gpu_driven on after a level load: every tree visual the sector graph reaches
// is checked (tree format, a wind root, its elements drawable by a GD pass: the pass's vertex shader
// compiled again with DA_TREE_GD takes a per-instance GDREF). Roots are all-or-nothing (a crown never
// sways on the GPU while its trunk sways on the CPU); nodes are the hierarchies' children made of
// eligible trees only (an FLOD family or a single tree), at any depth below the sector root. Nodes
// leave their hierarchy while the registry draws them (the per-sector lists of da_gpu_trees_sector
// take them) and return with r__gpu_driven 0 or the level's unload.
//
// r__gpu_driven 1 (3.1): the wind of every root integrates in da_gd_tree_wind.cs at the frame's start;
// each view culls on the GPU (da_gd_tree_cull.cs over the cells of its visited sectors, or over the node
// list of the main view's CPU walk), turns the counts into DrawIndexedInstanced records
// (da_gd_tree_args.cs) and the kept instances into the per-instance refs stream (da_gd_tree_scatter.cs),
// and draws one ExecuteIndirect per group (GD pass, vertex/index buffers, declaration) after its CPU
// passes. Local light faces cull on the CPU and draw through da_gpu_trees_draw_pass (3.13).
//
// r__gpu_driven -1 (auto, the default): 1 or 0 as da_offload.h decides from the frame's bottleneck.
//
// r__gpu_driven 2 (QA, the G1/G2 path of 3.11): every view walks the nodes on the CPU as the hierarchy
// had them and render_graph hands each pass's registered trees to da_gpu_trees_draw_pass, which lays
// them out by bin and draws them through the same GD passes; the wind comes from the CPU oscillators.

#include "stdafx.h"

#include "da_gpu_trees.h"
#include "da_offload.h"

#ifdef XR_D3D12
#include "Layers/xrRender/FHierrarhyVisual.h"
#include "Layers/xrRender/FLOD.h"
#include "Layers/xrRender/FTreeVisual.h"
#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/r__sector.h"
#include "Layers/xrRender/r__dsgraph_types.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "Layers/xrRenderDX12/d12_api.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <unordered_map>
#endif

extern ENGINE_API int ps_e_wind_dbg;

namespace xray::render::RENDER_NAMESPACE
{
#ifdef XR_D3D12
extern float r_ssaDISCARD, r_ssaVEG_DISCARD, r_ssaLOD_A;
extern float r_ssaGLOD_start, r_ssaGLOD_end;
extern float r_dtex_range;
IReader* open_shader(pcstr shader); // ResourceManager.cpp

namespace
{
constexpr u32 kNone = u32(-1);
// Record sizes of the shaders' buffers (da_gd_tree.h, da_gd_tree_*.cs).
constexpr u32 kInstStride = 96;
constexpr u32 kCullStride = 32;
constexpr u32 kNodeStride = 64;
constexpr u32 kKindStride = 32;
constexpr u32 kBinStride = 32;
constexpr u32 kRootStride = 48;
constexpr u32 kRootStaticStride = 32;
constexpr u32 kArgsStride = 5 * sizeof(u32);
constexpr u32 kVisStride = 12;
constexpr float kCellSize = 32.f;
// Per-context upload streams of the CPU-cull draw: at most one 64 KB version per DISCARD.
constexpr u32 kStreamBytes = 64u << 10;
constexpr u32 kListMinBytes = 256u << 10;
// VS registers of the registry's buffers (da_gd_tree.h).
constexpr u32 kSlotInst = 2;
constexpr u32 kSlotRoot = 3;
constexpr u32 kInstFlagSampleRoot = 1u << 0;
constexpr u32 kInstFlagRtStatic = 1u << 1;
constexpr u32 kPhaseNormal = 1u << 0;
constexpr u32 kPhaseShadow = 1u << 1;
// da_gd_tree_cull.cs
constexpr u32 kFlagSmap = 1u << 0;
constexpr u32 kFlagRtSkip = 1u << 1;
constexpr u32 kFlagNodeList = 1u << 2;
constexpr u32 kFlagFilter = 1u << 3;
constexpr u32 kFrameMask = 0xFFFFFFu; // the shaders count frames in 24 bits (exact as a float)
static_assert(R__NUM_CONTEXTS == 9, "da_gd_tree_cull.cs keeps value-filter stamps for 9 contexts");

// One pass of a registered element and its GD variant; its bins and groups are contiguous.
struct PassEntry
{
    SPass* pass{};
    ref_pass gd;
    u32 first_bin{}, bin_count{};
    u32 first_group{}, group_count{};
    u32 pass_index{kNone}; // its index in the elements that use it: the dsgraph's pass order
    u32 phases{}; // kPhase*: the elements it belongs to
};

struct Window
{
    u32 index_count, start_index, base_vertex, vertex_count;
};

struct ProtoPass
{
    u32 pass; // PassEntry
    u32 first_bin; // global bin of window 0
    u32 group;
    u32 region{}, capacity{}; // the refs region of this (prototype, pass)
};

// A distinct geometry range: the MU model's visual every instance of it shares.
struct Proto
{
    SGeometry* geometry{};
    u32 first_window{}, window_count{};
    xr_vector<ProtoPass> passes;

    ProtoPass* find(u32 pass)
    {
        for (ProtoPass& p : passes)
            if (p.pass == pass)
                return &p;
        return nullptr;
    }
    const ProtoPass* find(u32 pass) const { return const_cast<Proto*>(this)->find(pass); }
};

struct Bin
{
    u32 proto, window, group;
};

// One ExecuteIndirect: a GD pass over one vertex/index buffer pair and declaration.
struct Group
{
    u32 pass{};
    ref_geom geometry;
    u32 first_bin{}, bin_count{};
};

// A prototype with the passes of each element an instance can be drawn with: the first bin of window 0
// per (HQ, LQ, shadow) x (pass 0, pass 1).
struct Kind
{
    u32 proto{};
    u32 bins[6]{kNone, kNone, kNone, kNone, kNone, kNone};
};

struct Instance
{
    FTreeVisual* visual{};
    u32 root{}, node{}, proto{}, kind{};
    u32 flags{};
};

struct Node
{
    dxRender_Visual* visual{};
    u32 sector{}, cell{};
    u32 first_instance{}, instance_count{};
    u32 first_ancestor{}, ancestor_count{}; // value-filter slots in Registry::ancestor_slots
};

// 32 m of one sector: the nodes whose centre lies in it, the box around them.
struct Cell
{
    Fbox box;
    u32 sector{};
    u32 first_node{}, node_count{};
    u32 first_instance{}, instance_count{};
};

struct Root
{
    FTreeVisual* sample{}; // the visual whose position the root's wind is sampled at
};

// A hierarchy's children as the level built them; the registry's nodes leave them while applied.
struct ParentSurgery
{
    FHierrarhyVisual* parent{};
    xr_vector<dxRender_Visual*> original;
    xr_vector<dxRender_Visual*> remaining;
    xr_vector<dxRender_Visual*> nodes;
};

struct Stream
{
    ID3D11Buffer* buffer{};
    u32 cursor{};
    u32 frame{kNone};
};

// The GPU view a build set up (da_gpu_trees_view_begin/end).
struct View
{
    u32 serial{};
    u32 flags{};
    u32 phase_bit{};
    bool main{};
    u32 entries{};
    u32 bound{}; // upper bound of the cull's (instance, bin) entries: the scatter's threads
    u32 plane_offset{}, frustum_offset{}, entry_offset{};
    xr_vector<Fvector4> planes;
    xr_vector<u32> frustums; // (first plane, planes)
    xr_vector<u32> list; // node indices, or (first node, nodes, first frustum, frustums) per cell
    xr_vector<u32> data; // the upload: planes, frustums, list
    xr_vector<u32> group_mask;
    // The main view's node list: stamp per node against duplicates through a second frustum.
    xr_vector<u32> node_stamp;
    u32 stamp{};
};

struct ContextData
{
    // The CPU-cull draw (r__gpu_driven 2)
    Stream refs_stream, args_stream;
    xr_vector<u32> counts, offsets, fill;
    xr_vector<std::pair<u32, u32>> items; // (local bin, instance)
    xr_vector<u32> refs_data;
    xr_vector<u32> args_data;
    xr_vector<std::pair<u32, u32>> group_records; // (group, records)

    // The GPU cull (r__gpu_driven 1)
    ID3D11Buffer* counts_buffer{};
    ID3D11UnorderedAccessView* counts_uav{};
    ID3D11Buffer* vis{};
    ID3D11UnorderedAccessView* vis_uav{};
    ID3D11Buffer* args{};
    ID3D11UnorderedAccessView* args_uav{};
    ID3D11Buffer* refs{};
    ID3D11UnorderedAccessView* refs_uav{};
    ID3D11Buffer* list{};
    ID3D11ShaderResourceView* list_srv{};
    u32 list_capacity{};
    View view;
};

struct FrameStats
{
    std::atomic<u32> main{}, shadow{}, draws{}, records{};
};

struct Registry
{
    bool built{};
    bool applied{};
    bool gpu_ready{}; // the GPU cull and wind (r__gpu_driven 1); without them the CPU culls
    xr_vector<PassEntry> passes;
    std::unordered_map<SPass*, u32> pass_index;
    xr_vector<Window> windows;
    xr_vector<Proto> protos;
    xr_vector<Bin> bins;
    xr_vector<Group> groups;
    xr_vector<Kind> kinds;
    xr_vector<Instance> instances;
    xr_vector<Node> nodes;
    xr_vector<Cell> cells;
    xr_vector<std::pair<u32, u32>> sector_cells; // (first cell, cells) by sector id
    xr_vector<u32> cell_groups; // group bitmask per cell, group_words each
    xr_vector<u32> group_phases; // kPhase* per group
    u32 group_words{};
    xr_vector<Root> roots;
    xr_vector<FTreeVisual*> root_samples;
    xr_vector<ParentSurgery> parents;
    // By sector id: what each context's walk of the sector visits after its hierarchy.
    xr_vector<DaGpuTreeSector> sectors;
    // Value filter: slots 0..nodes-1 are the nodes, the rest the hierarchies above them (roots first).
    xr_vector<dxRender_Visual*> filter_visuals;
    xr_vector<u32> ancestor_slots;
    u32 refs_total{};

    ID3D11Buffer* inst_buffer{};
    ID3D11ShaderResourceView* inst_srv{};
    ID3D11Buffer* root_buffer{};
    ID3D11ShaderResourceView* root_srv{};
    ID3D11UnorderedAccessView* root_uav{};
    xr_vector<float> root_upload;

    // The GPU cull's static data
    ID3D11Buffer* cull_buffer{};
    ID3D11ShaderResourceView* cull_srv{};
    ID3D11Buffer* node_buffer{};
    ID3D11ShaderResourceView* node_srv{};
    ID3D11Buffer* filter_buffer{};
    ID3D11ShaderResourceView* filter_srv{};
    ID3D11Buffer* anc_buffer{};
    ID3D11ShaderResourceView* anc_srv{};
    ID3D11Buffer* kind_buffer{};
    ID3D11ShaderResourceView* kind_srv{};
    ID3D11Buffer* bin_buffer{};
    ID3D11ShaderResourceView* bin_srv{};
    ID3D11Buffer* root_static{};
    ID3D11ShaderResourceView* root_static_srv{};
    ID3D11Buffer* valuable{};
    ID3D11UnorderedAccessView* valuable_uav{};
    ID3D11Buffer* stats{};
    ID3D11UnorderedAccessView* stats_uav{};
    ID3D11Buffer* stats_staging[3]{};
    u32 stats_copied[3]{kNone, kNone, kNone};
    ID3D11Buffer* wind_dbg{};
    ID3D11UnorderedAccessView* wind_dbg_uav{};
    ID3D11Buffer* wind_dbg_staging[3]{};
    u32 wind_dbg_copied[3]{kNone, kNone, kNone};
    float wind_dbg_next{};

    ref_shader s_cull, s_args, s_scatter, s_wind;

    ContextData contexts[R__NUM_CONTEXTS];
    FrameStats stats_cpu[4];
};

Registry g_gd;
// Eligibility of a dsgraph pass, per load (null: the pass has no GD variant).
std::unordered_map<SPass*, u32> g_pass_verdict;
// A level is loaded that the registry may serve; the registry was built (or found nothing) for it.
// The build waits for the first r__gpu_driven on, so a session with the switch off runs none of it.
bool g_level_ready{};
bool g_build_tried{};
// r__gpu_driven as this frame resolved it (auto included), latched with the switch at the top of Calculate.
int g_mode{};
// One log line per level for a pass whose instances or records overflow a stream, or a view list.
std::atomic_flag g_stream_full_logged;
std::atomic_flag g_list_full_logged;
std::atomic<u32> g_view_serial{};

// The compute passes: a CS, no textures (the passes bind their buffers themselves).
class CBlender_da_gd_compute : public IBlender
{
    pcstr m_cs;

public:
    explicit CBlender_da_gd_compute(pcstr cs) : m_cs(cs) {}
    LPCSTR getComment() override { return "INTERNAL: GPU-driven trees"; }
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

float calc_lod(float ssa)
{
    // calcLOD of r__dsgraph_render.cpp: the LOD the CPU path selects its window with.
    return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

template <typename T>
void release(T*& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

bool gpu_cull() { return g_gd.applied && g_gd.gpu_ready && g_mode == 1; }

// Never 0: a graph's gd_view of 0 means "no view".
u32 next_serial()
{
    u32 serial = ++g_view_serial;
    if (!serial)
        serial = ++g_view_serial;
    return serial;
}

bool shader_file_exists(pcstr name)
{
    IReader* file = open_shader(name);
    const bool exists = !!file;
    FS.r_close(file);
    return exists;
}

bool compute_ready(const ref_shader& shader)
{
    return shader && shader->E[0] && !shader->E[0]->passes.empty() && shader->E[0]->passes[0]->cs &&
        shader->E[0]->passes[0]->cs->sh;
}

bool signature_has_gdref(const SVS* vs)
{
    // The semantic names are plain strings in the signature part of both DXBC (ISGN) and DXIL (ISG1).
    if (!vs || !vs->sh || !vs->signature || !vs->signature->signature)
        return false;
    ID3DBlob* blob = vs->signature->signature;
    const char* data = static_cast<const char*>(blob->GetBufferPointer());
    const size_t size = blob->GetBufferSize();
    static constexpr char name[] = "GDREF";
    return std::search(data, data + size, name, name + sizeof(name)) != data + size;
}

// The GD variant of a dsgraph pass: its vertex shader with DA_TREE_GD, the same pixel shader, state,
// textures, and a constant table merged from both with the original's handlers (SetMapping's
// binders, the blender's own constants) copied by name.
ref_pass make_gd_pass(SPass* pass)
{
    if (!pass || !pass->vs || !pass->vs->sh || !pass->ps)
        return {};
    if ((pass->hs && pass->hs->sh) || (pass->ds && pass->ds->sh) || (pass->gs && pass->gs->sh))
        return {};
    const char* source = pass->vs->source.c_str();
    if (!source || !source[0] || strchr(source, '('))
        return {};

    string_path name;
    strconcat(sizeof(name), name, source, "(DA_TREE_GD,)");
    const u32 flags = pass->ps->constants.dx9compatibility ? D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY : 0;
    // The blenders' way of an option in the name (uber_deffer.cpp): the define reaches the compiler
    // and its cache key, the name keeps the variant apart in the shader map.
    RImplementation.addShaderOption("DA_TREE_GD", "1");
    ref_vs vs = RImplementation.Resources->_CreateVS(name, flags);
    RImplementation.clearAllShaderOptions();
    if (!signature_has_gdref(&*vs))
        return {};
    // A pipeline never mixes DXIL and DXBC.
    if (pass->ps->sh && vs->dxil != pass->ps->dxil)
        return {};

    R_constant_table table;
    table.merge(&pass->ps->constants);
    table.merge(&vs->constants);
    if (pass->gs)
        table.merge(&pass->gs->constants);
    if (pass->constants)
    {
        for (ref_constant& constant : table.table)
        {
            if (const ref_constant original = pass->constants->get(constant->name))
                constant->handler = original->handler;
        }
    }
    table.refresh_handlers();

    SPass proto;
    proto.state = pass->state;
    proto.ps = pass->ps;
    proto.vs = vs;
    proto.gs = pass->gs;
    proto.hs = pass->hs;
    proto.ds = pass->ds;
    proto.cs = pass->cs;
    proto.constants = RImplementation.Resources->_CreateConstantTable(table);
    proto.T = pass->T;
    proto.C = pass->C;
    proto.M = pass->M;
    return RImplementation.Resources->_CreatePass(proto);
}

// kNone: the pass has no GD variant; otherwise its PassEntry.
u32 pass_entry(SPass* pass)
{
    if (const auto found = g_pass_verdict.find(pass); found != g_pass_verdict.end())
        return found->second;
    u32 index = kNone;
    if (ref_pass gd = make_gd_pass(pass))
    {
        index = u32(g_gd.passes.size());
        PassEntry& entry = g_gd.passes.emplace_back();
        entry.pass = pass;
        entry.gd = gd;
    }
    g_pass_verdict.emplace(pass, index);
    return index;
}

// The elements the dsgraph selects for a static tree (rimp_select_sh_static): HQ, LQ, shadow.
constexpr int kElements[] = {SE_R2_NORMAL_HQ, SE_R2_NORMAL_LQ, SE_R2_SHADOW};

// A tree visual the registry can draw in every view.
bool visual_eligible(FTreeVisual* tree)
{
    if (!tree->wind_root() || !tree->shader || !tree->window_count())
        return false;
    FTreeVisualInstancedDraw draw;
    for (u32 w = 0; w < tree->window_count(); ++w)
        if (!tree->window(w, draw) || !draw.geometry)
            return false;
    Shader* shader = &*tree->shader;
    // insert_static would also put the visual into the distortion map.
    if (shader->E[4] && shader->E[4]->flags.bDistort)
        return false;
    for (const int e : kElements)
    {
        ShaderElement* element = shader->E[e]._get();
        if (!element)
            continue;
        const auto& f = element->flags;
        if (f.iPriority / 2 != 0 || f.bStrictB2F || f.bEmissive || f.bWmark || element->passes.empty())
            return false;
        for (const ref_pass& pass : element->passes)
            if (pass_entry(&*pass) == kNone)
                return false;
    }
    return true;
}

bool is_tree(const dxRender_Visual* visual) { return visual->Type == MT_TREE_ST || visual->Type == MT_TREE_PM; }

u32 stream_write(ID3D11DeviceContext* context, Stream& stream, const void* data, u32 bytes)
{
    if (!stream.buffer || bytes > kStreamBytes)
        return kNone;
    D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (stream.frame != Device.dwFrame || stream.cursor + bytes > kStreamBytes)
    {
        type = D3D11_MAP_WRITE_DISCARD;
        stream.cursor = 0;
        stream.frame = Device.dwFrame;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(stream.buffer, 0, type, 0, &mapped)))
        return kNone;
    CopyMemory(static_cast<u8*>(mapped.pData) + stream.cursor, data, bytes);
    context->Unmap(stream.buffer, 0);
    const u32 at = stream.cursor;
    stream.cursor += (bytes + 15) & ~15u;
    return at;
}

void apply_surgery()
{
    for (ParentSurgery& p : g_gd.parents)
    {
        p.original = p.parent->children;
        p.parent->children = p.remaining;
    }
}

void restore_surgery()
{
    u32 changed = 0;
    for (ParentSurgery& p : g_gd.parents)
    {
        auto& children = p.parent->children;
        if (children == p.remaining)
            children = p.original;
        else
        {
            // Somebody changed the hierarchy while the nodes were out: keep that, add the nodes back.
            ++changed;
            children.insert(children.end(), p.nodes.begin(), p.nodes.end());
        }
    }
    if (changed)
        Msg("! [gd] %u hierarchies changed while their trees were on the GPU path; their nodes were appended", changed);
}

void destroy_registry()
{
    if (g_gd.applied)
    {
        restore_surgery();
        g_gd.applied = false;
    }
    for (Instance& instance : g_gd.instances)
        instance.visual->gd_instance = kNone;
    for (ContextData& context : g_gd.contexts)
    {
        release(context.refs_stream.buffer);
        release(context.args_stream.buffer);
        release(context.counts_uav);
        release(context.counts_buffer);
        release(context.vis_uav);
        release(context.vis);
        release(context.args_uav);
        release(context.args);
        release(context.refs_uav);
        release(context.refs);
        release(context.list_srv);
        release(context.list);
        context = {};
    }
    release(g_gd.inst_srv);
    release(g_gd.inst_buffer);
    release(g_gd.root_uav);
    release(g_gd.root_srv);
    release(g_gd.root_buffer);
    release(g_gd.cull_srv);
    release(g_gd.cull_buffer);
    release(g_gd.node_srv);
    release(g_gd.node_buffer);
    release(g_gd.filter_srv);
    release(g_gd.filter_buffer);
    release(g_gd.anc_srv);
    release(g_gd.anc_buffer);
    release(g_gd.kind_srv);
    release(g_gd.kind_buffer);
    release(g_gd.bin_srv);
    release(g_gd.bin_buffer);
    release(g_gd.root_static_srv);
    release(g_gd.root_static);
    release(g_gd.valuable_uav);
    release(g_gd.valuable);
    release(g_gd.stats_uav);
    release(g_gd.stats);
    release(g_gd.wind_dbg_uav);
    release(g_gd.wind_dbg);
    for (u32 i = 0; i < 3; ++i)
    {
        release(g_gd.stats_staging[i]);
        release(g_gd.wind_dbg_staging[i]);
        g_gd.stats_copied[i] = g_gd.wind_dbg_copied[i] = kNone;
    }
    g_gd.s_cull.destroy();
    g_gd.s_args.destroy();
    g_gd.s_scatter.destroy();
    g_gd.s_wind.destroy();
    for (FrameStats& stats : g_gd.stats_cpu)
        stats.main = stats.shadow = stats.draws = stats.records = 0;
    g_gd.passes.clear();
    g_gd.pass_index.clear();
    g_gd.windows.clear();
    g_gd.protos.clear();
    g_gd.bins.clear();
    g_gd.groups.clear();
    g_gd.kinds.clear();
    g_gd.instances.clear();
    g_gd.nodes.clear();
    g_gd.cells.clear();
    g_gd.sector_cells.clear();
    g_gd.cell_groups.clear();
    g_gd.group_phases.clear();
    g_gd.group_words = 0;
    g_gd.roots.clear();
    g_gd.root_samples.clear();
    g_gd.parents.clear();
    g_gd.sectors.clear();
    g_gd.filter_visuals.clear();
    g_gd.ancestor_slots.clear();
    g_gd.refs_total = 0;
    g_gd.root_upload.clear();
    g_pass_verdict.clear();
    g_stream_full_logged.clear();
    g_list_full_logged.clear();
    g_gd.built = false;
    g_gd.gpu_ready = false;
}

// da_gd_inst: the static rows of every instance (da_gd_tree.h).
void fill_instance_rows(xr_vector<u32>& rows)
{
    const u32 instances = u32(g_gd.instances.size());
    rows.assign(size_t(instances) * (kInstStride / 4), 0);
    for (u32 i = 0; i < instances; ++i)
    {
        const Instance& instance = g_gd.instances[i];
        const Fmatrix& x = instance.visual->world();
        const FTreeVisualColors c = instance.visual->colors();
        float* f = reinterpret_cast<float*>(&rows[size_t(i) * (kInstStride / 4)]);
        // FillInstanceData's packing of the world matrix.
        const float packed[20] = {x._11, x._21, x._31, x._41, x._12, x._22, x._32, x._42, x._13, x._23, x._33,
            x._43, c.scale.x, c.scale.y, c.scale.z, c.scale.w, c.bias.x, c.bias.y, c.bias.z, c.bias.w};
        CopyMemory(f, packed, sizeof(packed));
        f[20] = c.sun_scale;
        f[21] = c.sun_bias;
        u32* u = &rows[size_t(i) * (kInstStride / 4)];
        u[22] = instance.root;
        u[23] = instance.flags;
    }
}

// gd_inst of da_gd_tree_cull.cs: sphere, kind, rt_static.
void fill_cull_rows(xr_vector<u32>& rows)
{
    const u32 instances = u32(g_gd.instances.size());
    rows.assign(size_t(instances) * (kCullStride / 4), 0);
    for (u32 i = 0; i < instances; ++i)
    {
        const Instance& instance = g_gd.instances[i];
        const Fsphere& sphere = instance.visual->vis.sphere;
        float* f = reinterpret_cast<float*>(&rows[size_t(i) * (kCullStride / 4)]);
        u32* u = &rows[size_t(i) * (kCullStride / 4)];
        f[0] = sphere.P.x;
        f[1] = sphere.P.y;
        f[2] = sphere.P.z;
        f[3] = sphere.R;
        u[4] = instance.kind;
        u[5] = (instance.flags & kInstFlagRtStatic) ? 1u : 0u;
    }
}

template <typename T>
bool raw_buffer(const xr_vector<T>& data, u32 bytes, UINT bind, UINT misc, D3D11_USAGE usage, ID3D11Buffer** out)
{
    // Views need a non-empty buffer; the shaders never read past the data.
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = std::max((bytes + 15) & ~15u, 16u);
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | misc;
    xr_vector<u8> padded(desc.ByteWidth, 0);
    if (!data.empty())
        CopyMemory(padded.data(), data.data(), std::min<size_t>(data.size() * sizeof(T), bytes));
    const D3D11_SUBRESOURCE_DATA init{padded.data(), 0, 0};
    return SUCCEEDED(HW.pDevice->CreateBuffer(&desc, &init, out));
}

bool raw_srv(ID3D11Buffer* buffer, ID3D11ShaderResourceView** out)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    return SUCCEEDED(HW.pDevice->CreateShaderResourceView(buffer, &view, out));
}

bool raw_uav(ID3D11Buffer* buffer, ID3D11UnorderedAccessView** out)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    view.Buffer.NumElements = desc.ByteWidth / 4;
    return SUCCEEDED(HW.pDevice->CreateUnorderedAccessView(buffer, &view, out));
}

bool staging_buffer(u32 bytes, ID3D11Buffer** out)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(HW.pDevice->CreateBuffer(&desc, nullptr, out));
}

// What both paths draw with: the instance rows, the root rows, the CPU cull's streams.
bool create_buffers()
{
    ID3D11Device* device = HW.pDevice;
    const u32 instances = u32(g_gd.instances.size());
    const u32 roots = u32(g_gd.roots.size());

    xr_vector<u32> rows;
    fill_instance_rows(rows);
    // DEFAULT: da_gpu_trees_refresh_flags rewrites the flags when the ray tracing lane reclassifies.
    if (!raw_buffer(rows, instances * kInstStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, &g_gd.inst_buffer) ||
        !raw_srv(g_gd.inst_buffer, &g_gd.inst_srv))
        return false;

    // The wind pass writes it (UAV), the tree vertex shaders read it; unsettled until the first frame.
    g_gd.root_upload.assign(size_t(roots) * (kRootStride / 4), 0.f);
    for (u32 r = 0; r < roots; ++r)
    {
        float* row = &g_gd.root_upload[size_t(r) * (kRootStride / 4)];
        row[0] = 1.f;
        const u32 unsettled = kNone;
        CopyMemory(&row[11], &unsettled, sizeof(unsettled));
    }
    if (!raw_buffer(g_gd.root_upload, roots * kRootStride, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0,
            D3D11_USAGE_DEFAULT, &g_gd.root_buffer) ||
        !raw_srv(g_gd.root_buffer, &g_gd.root_srv) || !raw_uav(g_gd.root_buffer, &g_gd.root_uav))
        return false;

    D3D11_BUFFER_DESC stream{};
    stream.ByteWidth = kStreamBytes;
    stream.Usage = D3D11_USAGE_DYNAMIC;
    stream.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    stream.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (ContextData& context : g_gd.contexts)
    {
        stream.MiscFlags = 0;
        if (FAILED(device->CreateBuffer(&stream, nullptr, &context.refs_stream.buffer)))
            return false;
        stream.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if (FAILED(device->CreateBuffer(&stream, nullptr, &context.args_stream.buffer)))
            return false;
    }
    return true;
}

// The GPU cull and wind (r__gpu_driven 1): static tables, per-context work buffers, the passes. False
// with the reason: r__gpu_driven 1 then culls on the CPU like 2.
bool create_gpu_path(pcstr& reason)
{
    for (pcstr file : {"da_gd_tree_cull.cs", "da_gd_tree_args.cs", "da_gd_tree_scatter.cs", "da_gd_tree_wind.cs"})
    {
        if (!shader_file_exists(file))
        {
            reason = "shaders missing";
            return false;
        }
    }
    static CBlender_da_gd_compute b_cull("da_gd_tree_cull"), b_args("da_gd_tree_args"), b_scatter("da_gd_tree_scatter"),
        b_wind("da_gd_tree_wind");
    g_gd.s_cull.create(&b_cull, "r2" DELIMITER "da_gd_tree_cull");
    g_gd.s_args.create(&b_args, "r2" DELIMITER "da_gd_tree_args");
    g_gd.s_scatter.create(&b_scatter, "r2" DELIMITER "da_gd_tree_scatter");
    g_gd.s_wind.create(&b_wind, "r2" DELIMITER "da_gd_tree_wind");
    if (!compute_ready(g_gd.s_cull) || !compute_ready(g_gd.s_args) || !compute_ready(g_gd.s_scatter) ||
        !compute_ready(g_gd.s_wind))
    {
        reason = "compute shaders failed to compile";
        return false;
    }

    const u32 instances = u32(g_gd.instances.size());
    const u32 nodes = u32(g_gd.nodes.size());
    const u32 bins = u32(g_gd.bins.size());
    reason = "GPU buffers could not be created";

    xr_vector<u32> rows;
    fill_cull_rows(rows);
    if (!raw_buffer(rows, instances * kCullStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, &g_gd.cull_buffer) ||
        !raw_srv(g_gd.cull_buffer, &g_gd.cull_srv))
        return false;

    // Nodes: box, first instance, instances, sphere, lod_factor, ancestors.
    rows.assign(size_t(nodes) * (kNodeStride / 4), 0);
    for (u32 n = 0; n < nodes; ++n)
    {
        const Node& node = g_gd.nodes[n];
        const vis_data& vis = node.visual->vis;
        u32* u = &rows[size_t(n) * (kNodeStride / 4)];
        float* f = reinterpret_cast<float*>(u);
        f[0] = vis.box.vMin.x;
        f[1] = vis.box.vMin.y;
        f[2] = vis.box.vMin.z;
        u[3] = node.first_instance;
        f[4] = vis.box.vMax.x;
        f[5] = vis.box.vMax.y;
        f[6] = vis.box.vMax.z;
        u[7] = node.instance_count;
        f[8] = vis.sphere.P.x;
        f[9] = vis.sphere.P.y;
        f[10] = vis.sphere.P.z;
        f[11] = vis.sphere.R;
        // Negative: a single tree, no FLOD switch.
        f[12] = node.visual->Type == MT_LOD ? std::max(static_cast<FLOD*>(node.visual)->lod_factor, 0.f) : -1.f;
        u[13] = node.first_ancestor;
        u[14] = node.ancestor_count;
    }
    if (!raw_buffer(rows, nodes * kNodeStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, &g_gd.node_buffer) ||
        !raw_srv(g_gd.node_buffer, &g_gd.node_srv))
        return false;

    const u32 slots = u32(g_gd.filter_visuals.size());
    xr_vector<float> spheres(size_t(slots) * 4);
    for (u32 s = 0; s < slots; ++s)
    {
        const Fsphere& sphere = g_gd.filter_visuals[s]->vis.sphere;
        spheres[s * 4 + 0] = sphere.P.x;
        spheres[s * 4 + 1] = sphere.P.y;
        spheres[s * 4 + 2] = sphere.P.z;
        spheres[s * 4 + 3] = sphere.R;
    }
    if (!raw_buffer(spheres, slots * 16, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, &g_gd.filter_buffer) ||
        !raw_srv(g_gd.filter_buffer, &g_gd.filter_srv) ||
        !raw_buffer(g_gd.ancestor_slots, u32(g_gd.ancestor_slots.size() * 4), D3D11_BIND_SHADER_RESOURCE, 0,
            D3D11_USAGE_IMMUTABLE, &g_gd.anc_buffer) ||
        !raw_srv(g_gd.anc_buffer, &g_gd.anc_srv))
        return false;

    rows.assign(g_gd.kinds.size() * (kKindStride / 4), 0);
    for (u32 k = 0; k < g_gd.kinds.size(); ++k)
    {
        const Kind& kind = g_gd.kinds[k];
        u32* u = &rows[size_t(k) * (kKindStride / 4)];
        CopyMemory(u, kind.bins, sizeof(kind.bins));
        u[6] = g_gd.protos[kind.proto].window_count;
    }
    if (!raw_buffer(rows, u32(g_gd.kinds.size()) * kKindStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE,
            &g_gd.kind_buffer) ||
        !raw_srv(g_gd.kind_buffer, &g_gd.kind_srv))
        return false;

    rows.assign(size_t(bins) * (kBinStride / 4), 0);
    for (u32 b = 0; b < bins; ++b)
    {
        const Bin& bin = g_gd.bins[b];
        const Proto& proto = g_gd.protos[bin.proto];
        const Window& w = g_gd.windows[proto.first_window + bin.window];
        const ProtoPass* use = proto.find(g_gd.groups[bin.group].pass);
        u32* u = &rows[size_t(b) * (kBinStride / 4)];
        u[0] = w.index_count;
        u[1] = w.start_index;
        u[2] = w.base_vertex;
        u[3] = use->region;
        u[4] = bin.window;
        u[5] = b - bin.window;
    }
    if (!raw_buffer(rows, bins * kBinStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, &g_gd.bin_buffer) ||
        !raw_srv(g_gd.bin_buffer, &g_gd.bin_srv))
        return false;

    const u32 roots = u32(g_gd.roots.size());
    xr_vector<float> statics(size_t(roots) * (kRootStaticStride / 4), 0.f);
    for (u32 r = 0; r < roots; ++r)
    {
        FTreeVisual* sample = g_gd.roots[r].sample;
        float* f = &statics[size_t(r) * (kRootStaticStride / 4)];
        f[0] = sample->world().c.x;
        f[1] = sample->world().c.z;
        f[2] = sample->wind_omega();
        f[3] = sample->wind_height();
        f[4] = sample->rigid() ? 0.f : 1.f;
    }
    if (!raw_buffer(statics, roots * kRootStaticStride, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, &g_gd.root_static) ||
        !raw_srv(g_gd.root_static, &g_gd.root_static_srv))
        return false;

    const xr_vector<u32> none;
    if (!raw_buffer(none, slots * R__NUM_CONTEXTS * 4, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, &g_gd.valuable) ||
        !raw_uav(g_gd.valuable, &g_gd.valuable_uav))
        return false;

    // Statistics (4 frames x 8 counters) and the wind_dbg row, read back three frames later.
    if (!raw_buffer(none, 4 * 32, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, &g_gd.stats) ||
        !raw_uav(g_gd.stats, &g_gd.stats_uav) ||
        !raw_buffer(none, 32, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, &g_gd.wind_dbg) ||
        !raw_uav(g_gd.wind_dbg, &g_gd.wind_dbg_uav))
        return false;
    for (u32 i = 0; i < 3; ++i)
        if (!staging_buffer(4 * 32, &g_gd.stats_staging[i]) || !staging_buffer(32, &g_gd.wind_dbg_staging[i]))
            return false;

    // Per context: counts (+ entry count), entries, records, refs, the view list.
    const u32 list_bytes = std::max(kListMinBytes, (nodes * 4 + 4096 + 15) & ~15u);
    for (ContextData& context : g_gd.contexts)
    {
        if (!raw_buffer(none, (bins + 1) * 4, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, &context.counts_buffer) ||
            !raw_uav(context.counts_buffer, &context.counts_uav) ||
            !raw_buffer(none, instances * 2 * kVisStride, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, &context.vis) ||
            !raw_uav(context.vis, &context.vis_uav) ||
            !raw_buffer(none, bins * kArgsStride, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,
                D3D11_USAGE_DEFAULT, &context.args) ||
            !raw_uav(context.args, &context.args_uav) ||
            !raw_buffer(none, g_gd.refs_total * 4, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_VERTEX_BUFFER, 0,
                D3D11_USAGE_DEFAULT, &context.refs) ||
            !raw_uav(context.refs, &context.refs_uav) ||
            !raw_buffer(none, list_bytes, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, &context.list) ||
            !raw_srv(context.list, &context.list_srv))
            return false;
        context.list_capacity = list_bytes;
        context.view.node_stamp.assign(nodes, 0);
        context.view.group_mask.assign(g_gd.group_words, 0);
    }
    reason = nullptr;
    return true;
}

struct BuildCounts
{
    u32 trees{}, shader{}, mixed{}, family{};
};

void build_registry()
{
    auto& dsgraph = RImplementation.get_imm_context();
    BuildCounts counts;
    CTimer timer;
    timer.Start();

    // 1. Candidates: the children of the sector hierarchies (at any depth) that are a tree or an FLOD
    // family of trees, with the hierarchy holding them and the ones between it and the sector root.
    struct Candidate
    {
        dxRender_Visual* visual;
        FHierrarhyVisual* parent;
        u32 sector;
        xr_vector<dxRender_Visual*> ancestors;
        xr_vector<FTreeVisual*> trees;
        bool ok;
    };
    xr_vector<Candidate> candidates;
    std::unordered_map<const TreeWindShared*, bool> root_ok;
    xr_vector<dxRender_Visual*> chain;
    const auto walk = [&](auto&& self, FHierrarhyVisual* hierarchy, u32 sector) -> void {
        for (dxRender_Visual* child : hierarchy->children)
        {
            if (is_tree(child))
                candidates.push_back({child, hierarchy, sector, chain, {static_cast<FTreeVisual*>(child)}, true});
            else if (child->Type == MT_LOD)
            {
                Candidate lod{child, hierarchy, sector, chain, {}, true};
                for (dxRender_Visual* leaf : static_cast<FHierrarhyVisual*>(child)->children)
                {
                    if (is_tree(leaf))
                        lod.trees.push_back(static_cast<FTreeVisual*>(leaf));
                    else
                        lod.ok = false;
                }
                if (!lod.ok && !lod.trees.empty())
                    ++counts.family;
                if (!lod.trees.empty())
                    candidates.push_back(std::move(lod));
            }
            else if (child->Type == MT_HIERRARHY)
            {
                chain.push_back(child);
                self(self, static_cast<FHierrarhyVisual*>(child), sector);
                chain.pop_back();
            }
        }
    };
    for (u32 s = 0; s < dsgraph.Sectors.size(); ++s)
    {
        dxRender_Visual* root = static_cast<CSector*>(dsgraph.Sectors[s])->root();
        if (root && root->Type == MT_HIERRARHY)
            walk(walk, static_cast<FHierrarhyVisual*>(root), s);
    }

    // 2. Visual eligibility; a root is out when any of its visuals is.
    for (Candidate& c : candidates)
    {
        for (FTreeVisual* tree : c.trees)
        {
            ++counts.trees;
            const bool ok = visual_eligible(tree);
            if (!ok)
            {
                ++counts.shader;
                c.ok = false;
            }
            if (const TreeWindShared* shared = tree->wind_root())
            {
                const auto [it, inserted] = root_ok.emplace(shared, ok);
                if (!inserted)
                    it->second = it->second && ok;
            }
        }
    }
    // 3. A node needs every root of its trees; a rejected node takes its roots out with it.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Candidate& c : candidates)
        {
            if (!c.ok)
            {
                for (FTreeVisual* tree : c.trees)
                    if (const TreeWindShared* shared = tree->wind_root(); shared && root_ok[shared])
                    {
                        root_ok[shared] = false;
                        changed = true;
                    }
                continue;
            }
            for (FTreeVisual* tree : c.trees)
            {
                const TreeWindShared* shared = tree->wind_root();
                if (!shared || !root_ok[shared])
                {
                    c.ok = false;
                    changed = true;
                    break;
                }
            }
        }
    }
    for (const Candidate& c : candidates)
        if (!c.ok)
            for (FTreeVisual* tree : c.trees)
                if (visual_eligible(tree))
                    ++counts.mixed;

    // 4. Cells: 32 m of one sector over the accepted nodes, in (sector, x, z) order; nodes and instances
    // sorted by (cell, level order).
    const auto cell_key = [](const Candidate& c) {
        const Fvector& centre = c.visual->vis.sphere.P;
        return std::make_tuple(c.sector, iFloor(centre.x / kCellSize), iFloor(centre.z / kCellSize));
    };
    xr_map<std::tuple<u32, int, int>, u32> cell_of;
    for (const Candidate& c : candidates)
        if (c.ok)
            cell_of.emplace(cell_key(c), 0);
    g_gd.sector_cells.assign(dsgraph.Sectors.size(), {0u, 0u});
    for (auto& [key, id] : cell_of)
    {
        id = u32(g_gd.cells.size());
        Cell& cell = g_gd.cells.emplace_back();
        cell.box.invalidate();
        cell.sector = std::get<0>(key);
        auto& range = g_gd.sector_cells[cell.sector];
        if (!range.second)
            range.first = id;
        ++range.second;
    }
    xr_vector<std::pair<u32, u32>> order; // (cell, candidate)
    for (u32 i = 0; i < candidates.size(); ++i)
    {
        const Candidate& c = candidates[i];
        if (!c.ok)
            continue;
        const u32 cell = cell_of[cell_key(c)];
        g_gd.cells[cell].box.merge(c.visual->vis.box);
        order.emplace_back(cell, i);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::unordered_map<const TreeWindShared*, u32> root_index;
    // (geometry, base vertex, first index, primitives, windows) -> proto
    xr_map<std::tuple<const SGeometry*, u32, u32, u32, u32>, u32> proto_index;
    xr_vector<u32> node_candidate;
    for (const auto& [cell_id, candidate] : order)
    {
        const Candidate& c = candidates[candidate];
        Cell& cell = g_gd.cells[cell_id];
        if (!cell.node_count)
        {
            cell.first_node = u32(g_gd.nodes.size());
            cell.first_instance = u32(g_gd.instances.size());
        }
        ++cell.node_count;
        node_candidate.push_back(candidate);
        Node& node = g_gd.nodes.emplace_back();
        node.visual = c.visual;
        node.sector = c.sector;
        node.cell = cell_id;
        node.first_instance = u32(g_gd.instances.size());
        for (FTreeVisual* tree : c.trees)
        {
            Instance& instance = g_gd.instances.emplace_back();
            instance.visual = tree;
            instance.node = u32(g_gd.nodes.size() - 1);
            const auto [root_it, new_root] = root_index.emplace(tree->wind_root(), u32(g_gd.roots.size()));
            if (new_root)
                g_gd.roots.push_back({tree});
            instance.root = root_it->second;
            // The CPU path samples a root's wind at the position of the visual that integrates it.
            const Fvector& sample = g_gd.roots[instance.root].sample->world().c;
            if (tree->world().c.x == sample.x && tree->world().c.z == sample.z)
                instance.flags |= kInstFlagSampleRoot;

            FTreeVisualInstancedDraw draw;
            tree->window(0, draw);
            const auto key = std::make_tuple(static_cast<const SGeometry*>(draw.geometry), draw.base_vertex,
                draw.start_index, draw.primitive_count, tree->window_count());
            const auto [proto_it, new_proto] = proto_index.emplace(key, u32(g_gd.protos.size()));
            if (new_proto)
            {
                Proto& proto = g_gd.protos.emplace_back();
                proto.geometry = draw.geometry;
                proto.first_window = u32(g_gd.windows.size());
                proto.window_count = tree->window_count();
                for (u32 w = 0; w < proto.window_count; ++w)
                {
                    tree->window(w, draw);
                    g_gd.windows.push_back({draw.primitive_count * 3, draw.start_index, draw.base_vertex, draw.vertex_count});
                }
            }
            instance.proto = proto_it->second;
            // Every pass the instance can be drawn with belongs to its prototype's bins; the pass order
            // and the phases of each pass as the elements have them.
            Proto& proto = g_gd.protos[instance.proto];
            for (u32 e = 0; e < std::size(kElements); ++e)
                if (ShaderElement* element = tree->shader->E[kElements[e]]._get())
                    for (u32 k = 0; k < element->passes.size(); ++k)
                    {
                        const u32 entry = pass_entry(&*element->passes[k]);
                        PassEntry& pe = g_gd.passes[entry];
                        if (pe.pass_index == kNone)
                            pe.pass_index = k;
                        pe.phases |= e < 2 ? kPhaseNormal : kPhaseShadow;
                        if (!proto.find(entry))
                            proto.passes.push_back({entry, kNone, kNone});
                    }
            ++node.instance_count;
            ++cell.instance_count;
        }
    }

    // 5. Bins (pass, geometry, prototype, window) and groups (pass, geometry) in that order.
    xr_vector<u32> proto_order(g_gd.protos.size());
    for (u32 p = 0; p < proto_order.size(); ++p)
        proto_order[p] = p;
    std::sort(proto_order.begin(), proto_order.end(), [](u32 a, u32 b) {
        const SGeometry* ga = g_gd.protos[a].geometry;
        const SGeometry* gb = g_gd.protos[b].geometry;
        if (ga->vb != gb->vb)
            return std::less<>{}(ga->vb, gb->vb);
        if (ga->ib != gb->ib)
            return std::less<>{}(ga->ib, gb->ib);
        if (ga->dcl._get() != gb->dcl._get())
            return std::less<>{}(ga->dcl._get(), gb->dcl._get());
        return a < b;
    });
    for (u32 entry = 0; entry < g_gd.passes.size(); ++entry)
    {
        PassEntry& pass = g_gd.passes[entry];
        pass.first_bin = u32(g_gd.bins.size());
        pass.first_group = u32(g_gd.groups.size());
        const SGeometry* last = nullptr;
        for (const u32 p : proto_order)
        {
            Proto& proto = g_gd.protos[p];
            ProtoPass* use = proto.find(entry);
            if (!use)
                continue;
            const SGeometry* g = proto.geometry;
            if (!last || last->vb != g->vb || last->ib != g->ib || last->dcl._get() != g->dcl._get())
            {
                Group& group = g_gd.groups.emplace_back();
                group.pass = entry;
                SDeclaration* dcl = RImplementation.Resources->_CreateDeclGD(&*g->dcl);
                group.geometry = RImplementation.Resources->CreateGeomGD(dcl, g->vb, g->ib, g->vb_stride);
                group.first_bin = u32(g_gd.bins.size());
                g_gd.group_phases.push_back(pass.phases);
                last = g;
            }
            use->first_bin = u32(g_gd.bins.size());
            use->group = u32(g_gd.groups.size() - 1);
            for (u32 w = 0; w < proto.window_count; ++w)
                g_gd.bins.push_back({p, w, use->group});
            g_gd.groups.back().bin_count += proto.window_count;
        }
        pass.bin_count = u32(g_gd.bins.size()) - pass.first_bin;
        pass.group_count = u32(g_gd.groups.size()) - pass.first_group;
        g_gd.pass_index.emplace(pass.pass, entry);
    }

    // 6. Kinds (prototype + the passes of each element) and the refs regions: a (prototype, pass) holds
    // every instance that can be drawn with it once, whichever element a view selects.
    xr_map<std::array<u32, 7>, u32> kind_index;
    for (Instance& instance : g_gd.instances)
    {
        std::array<u32, 7> key{instance.proto, kNone, kNone, kNone, kNone, kNone, kNone};
        for (u32 e = 0; e < std::size(kElements); ++e)
            if (ShaderElement* element = instance.visual->shader->E[kElements[e]]._get())
                for (u32 k = 0; k < element->passes.size() && k < 2; ++k)
                    key[1 + e * 2 + k] = pass_entry(&*element->passes[k]);
        const auto [it, inserted] = kind_index.emplace(key, u32(g_gd.kinds.size()));
        if (inserted)
        {
            Kind& kind = g_gd.kinds.emplace_back();
            kind.proto = instance.proto;
            for (u32 i = 0; i < 6; ++i)
                if (key[1 + i] != kNone)
                    kind.bins[i] = g_gd.protos[instance.proto].find(key[1 + i])->first_bin;
        }
        instance.kind = it->second;
        std::array<u32, 6> distinct{};
        u32 count = 0;
        for (u32 i = 0; i < 6; ++i)
            if (key[1 + i] != kNone && std::find(distinct.begin(), distinct.begin() + count, key[1 + i]) == distinct.begin() + count)
                distinct[count++] = key[1 + i];
        for (u32 i = 0; i < count; ++i)
            ++g_gd.protos[instance.proto].find(distinct[i])->capacity;
    }
    g_gd.refs_total = 0;
    for (Proto& proto : g_gd.protos)
        for (ProtoPass& use : proto.passes)
        {
            use.region = g_gd.refs_total;
            g_gd.refs_total += use.capacity;
        }

    // Group bitmask per cell (which ExecuteIndirects a view that sees the cell may need).
    g_gd.group_words = (u32(g_gd.groups.size()) + 31) / 32;
    g_gd.cell_groups.assign(size_t(g_gd.cells.size()) * g_gd.group_words, 0);
    for (const Instance& instance : g_gd.instances)
    {
        u32* mask = &g_gd.cell_groups[size_t(g_gd.nodes[instance.node].cell) * g_gd.group_words];
        for (const ProtoPass& pp : g_gd.protos[instance.proto].passes)
            mask[pp.group / 32] |= 1u << (pp.group % 32);
    }

    // 7. The surgery plan: every hierarchy without its nodes; every sector's nodes in the order the
    // hierarchy walk met them, each with the hierarchies above it; the value-filter slots (the nodes,
    // then the sector roots and the hierarchies between).
    std::unordered_map<FHierrarhyVisual*, xr_vector<dxRender_Visual*>> taken;
    xr_vector<xr_vector<u32>> sector_nodes(dsgraph.Sectors.size());
    for (u32 n = 0; n < node_candidate.size(); ++n)
    {
        const Candidate& c = candidates[node_candidate[n]];
        taken[c.parent].push_back(c.visual);
        sector_nodes[c.sector].push_back(n);
        g_gd.filter_visuals.push_back(c.visual);
    }
    for (auto& [parent, nodes] : taken)
    {
        ParentSurgery& plan = g_gd.parents.emplace_back();
        plan.parent = parent;
        std::sort(nodes.begin(), nodes.end());
        for (dxRender_Visual* child : parent->children)
            (std::binary_search(nodes.begin(), nodes.end(), child) ? plan.nodes : plan.remaining).push_back(child);
    }
    std::unordered_map<dxRender_Visual*, u32> slot_of;
    const auto filter_slot = [&](dxRender_Visual* visual) {
        const auto [it, inserted] = slot_of.emplace(visual, u32(g_gd.filter_visuals.size()));
        if (inserted)
            g_gd.filter_visuals.push_back(visual);
        return it->second;
    };
    for (u32 n = 0; n < node_candidate.size(); ++n)
    {
        const Candidate& c = candidates[node_candidate[n]];
        Node& node = g_gd.nodes[n];
        node.first_ancestor = u32(g_gd.ancestor_slots.size());
        g_gd.ancestor_slots.push_back(filter_slot(static_cast<CSector*>(dsgraph.Sectors[c.sector])->root()));
        for (dxRender_Visual* ancestor : c.ancestors)
            g_gd.ancestor_slots.push_back(filter_slot(ancestor));
        node.ancestor_count = u32(g_gd.ancestor_slots.size()) - node.first_ancestor;
    }
    g_gd.sectors.resize(sector_nodes.size());
    for (u32 s = 0; s < sector_nodes.size(); ++s)
    {
        xr_vector<u32>& list = sector_nodes[s];
        // The hierarchy walk's order.
        std::sort(list.begin(), list.end(), [&](u32 a, u32 b) { return node_candidate[a] < node_candidate[b]; });
        DaGpuTreeSector& plan = g_gd.sectors[s];
        for (const u32 n : list)
        {
            const Candidate& c = candidates[node_candidate[n]];
            plan.nodes.push_back({c.visual, n, u32(plan.ancestors.size()), u32(c.ancestors.size())});
            plan.ancestors.insert(plan.ancestors.end(), c.ancestors.begin(), c.ancestors.end());
        }
    }

    for (u32 i = 0; i < g_gd.instances.size(); ++i)
        g_gd.instances[i].visual->gd_instance = i;
    g_gd.root_samples.clear();
    for (const Root& root : g_gd.roots)
        g_gd.root_samples.push_back(root.sample);
    // The rt_static bits as the ray tracing lane last classified (the buffers do not exist yet).
    for (Instance& instance : g_gd.instances)
        if (instance.visual->rt_static && instance.visual->rigid())
            instance.flags |= kInstFlagRtStatic;

    if (g_gd.instances.empty())
    {
        Msg("* [gd] trees: none registered (%u tree visuals: shader %u, mixed root %u; %u mixed families)", counts.trees,
            counts.shader, counts.mixed, counts.family);
        destroy_registry();
        return;
    }
    if (!create_buffers())
    {
        Msg("! [gd] trees: GPU buffers could not be created, the CPU path draws them");
        destroy_registry();
        return;
    }
    pcstr reason = nullptr;
    g_gd.gpu_ready = create_gpu_path(reason);
    if (!g_gd.gpu_ready)
        Msg("! [gd] GPU cull off (%s): r__gpu_driven 1 culls the registered trees on the CPU", reason ? reason : "?");
    g_gd.built = true;
    const u32 kept = counts.trees - u32(g_gd.instances.size());
    Msg("* [gd] trees: %u instances, %u roots, %u nodes, %u prototypes, %u kinds, %u bins, %u groups, %u cells, %u passes; "
        "%u visuals kept on the CPU (shader %u, mixed root %u; %u mixed families); built in %.1f ms",
        u32(g_gd.instances.size()), u32(g_gd.roots.size()), u32(g_gd.nodes.size()), u32(g_gd.protos.size()),
        u32(g_gd.kinds.size()), u32(g_gd.bins.size()), u32(g_gd.groups.size()), u32(g_gd.cells.size()),
        u32(g_gd.passes.size()), kept, counts.shader, counts.mixed, counts.family, timer.GetElapsed_sec() * 1000.f);
}

// ---- GPU passes ---------------------------------------------------------------------------------------

void compute(CBackend& cmd_list, const ref_shader& shader, u32 groups_x, u32 groups_y,
    std::initializer_list<ID3D11ShaderResourceView*> srvs, std::initializer_list<ID3D11UnorderedAccessView*> uavs,
    const std::function<void()>& constants)
{
    SPass& P = *shader->E[0]->passes[0];
    cmd_list.set_States(P.state);
    cmd_list.set_CS(P.cs);
    cmd_list.set_Constants(P.constants);
    if (P.T)
        cmd_list.set_Textures(P.T);
    constants();
    u32 slots = 0;
    for (ID3D11ShaderResourceView* srv : srvs)
        cmd_list.SRVSManager.SetCSResource(slots++, srv);
    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    ID3D11UnorderedAccessView* bound[8]{};
    const UINT initial[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    u32 count = 0;
    for (ID3D11UnorderedAccessView* uav : uavs)
        bound[count++] = uav;
    context->CSSetUnorderedAccessViews(0, count, bound, initial);
    cmd_list.Compute(groups_x, groups_y, 1);
    ID3D11UnorderedAccessView* none[8]{};
    context->CSSetUnorderedAccessViews(0, count, none, initial);
    for (u32 slot = 0; slot < slots; ++slot)
        cmd_list.SRVSManager.SetCSResource(slot, nullptr);
    cmd_list.SRVSManager.Apply(cmd_list.context_id);
    cmd_list.clear_CS_resources();
}

// Every GD group of the view's cells or nodes, pass 0 first, one ExecuteIndirect each.
void draw_view(CBackend& cmd_list, ContextData& ctx, u32& draws, u32& records)
{
    const View& view = ctx.view;
    u32 current = kNone;
    for (u32 pass_index = 0; pass_index < SHADER_PASSES_MAX; ++pass_index)
    {
        for (u32 g = 0; g < g_gd.groups.size(); ++g)
        {
            if (!(view.group_mask[g / 32] & (1u << (g % 32))) || !(g_gd.group_phases[g] & view.phase_bit))
                continue;
            Group& group = g_gd.groups[g];
            const PassEntry& pe = g_gd.passes[group.pass];
            if (pe.pass_index != pass_index)
                continue;
            if (current != group.pass)
            {
                current = group.pass;
                cmd_list.set_Pass(pe.gd._get());
                cmd_list.apply_lmaterial();
                FTreeVisual::SetupInstancedGlobals(cmd_list);
                static const shared_str c_gd_params = "da_gd_params";
                cmd_list.set_c(c_gd_params, ps_r__Tree_SBC * 1.3333f, 0.f, 0.f, 0.f);
                cmd_list.SRVSManager.SetVSResource(kSlotInst, g_gd.inst_srv);
                cmd_list.SRVSManager.SetVSResource(kSlotRoot, g_gd.root_srv);
            }
            cmd_list.set_Geometry(group.geometry);
            cmd_list.set_InstanceStream(ctx.refs, sizeof(u32));
            cmd_list.RenderIndirect(ctx.args, group.first_bin * kArgsStride, group.bin_count);
            ++draws;
            records += group.bin_count;
        }
    }
    if (current != kNone)
    {
        cmd_list.set_InstanceStream(nullptr, 0);
        cmd_list.SRVSManager.SetVSResource(kSlotInst, nullptr);
        cmd_list.SRVSManager.SetVSResource(kSlotRoot, nullptr);
    }
}

void print_stats(u32 frame)
{
    if (!g_gd.applied || ps_r__gpu_log <= 0 || frame % u32(ps_r__gpu_log))
        return;
    const FrameStats& cpu = g_gd.stats_cpu[(frame - 3) & 3];
    if (!gpu_cull())
    {
        Msg("* [gd] instances main=%u shadow=%u draws=%u records=%u (CPU cull)", cpu.main.load(), cpu.shadow.load(),
            cpu.draws.load(), cpu.records.load());
        return;
    }
    // The copy made two frames ago holds the counts of the frame before it.
    const u32 slot = (frame + 1) % 3;
    if (g_gd.stats_copied[slot] != frame - 2)
        return;
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_gd.stats_staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        return;
    u32 counters[8];
    CopyMemory(counters, static_cast<const u8*>(mapped.pData) + ((frame - 3) & 3) * 32, sizeof(counters));
    context->Unmap(g_gd.stats_staging[slot], 0);
    // The views that culled on the CPU (local light faces) add their instances to the GPU cull's.
    Msg("* [gd] instances main=%u shadow=%u draws=%u records=%u views=%u nodes=%u entries=%u/%u",
        counters[0] + cpu.main.load(), counters[1] + cpu.shadow.load(), cpu.draws.load(), cpu.records.load(),
        counters[2], counters[5], counters[3], counters[4]);
}

void wind_debug(ID3D11DeviceContext* context, u32 frame)
{
    if (!ps_e_wind_dbg)
        return;
    const u32 slot = (frame + 1) % 3;
    if (g_gd.wind_dbg_copied[slot] == frame - 2 && Device.fTimeGlobal >= g_gd.wind_dbg_next)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(g_gd.wind_dbg_staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        {
            float row[5];
            CopyMemory(row, mapped.pData, sizeof(row));
            context->Unmap(g_gd.wind_dbg_staging[slot], 0);
            g_gd.wind_dbg_next = Device.fTimeGlobal + 0.5f;
            Msg("* [wind-tree] target=%.3f q=%.3f qd=%.3f omega=%.2f top=%.1f (gd root 0)", row[0], row[1], row[2], row[3], row[4]);
        }
    }
    context->CopyResource(g_gd.wind_dbg_staging[frame % 3], g_gd.wind_dbg);
    g_gd.wind_dbg_copied[frame % 3] = frame;
}

// The crown oscillators of every root (3.4), on the immediate context before any view draws.
void wind_pass(CBackend& cmd_list, u32 frame)
{
    const auto& env = g_pGamePersistent->Environment();
    float dt = Device.fTimeDelta;
    if (dt <= 0.f || dt > 0.25f)
        dt = 0.016f;
    float speed, speed_prev;
    FTreeVisual::FrameSpeeds(speed, speed_prev);
    const u32 roots = u32(g_gd.roots.size());
    compute(cmd_list, g_gd.s_wind, (roots + 63) / 64, 1, {g_gd.root_static_srv}, {g_gd.root_uav, g_gd.wind_dbg_uav}, [&] {
        cmd_list.set_c("gd_wind0", env.eff_wind_field_ofs.x, env.eff_wind_field_ofs.y, env.eff_wind_gust_event,
            env.eff_wind_freeze ? 1.f : 0.f);
        cmd_list.set_c("gd_wind1", dt, da_history_reset() ? 1.f : 0.f, speed, speed_prev);
        cmd_list.set_c("gd_wind2", float(roots), float(frame & kFrameMask), 0.f, ps_e_wind_dbg ? 1.f : 0.f);
    });
}
} // namespace
#endif

bool da_gpu_trees_live()
{
#ifdef XR_D3D12
    return g_gd.applied;
#else
    return false;
#endif
}

void da_gpu_trees_level_load()
{
#ifdef XR_D3D12
    destroy_registry();
    g_build_tried = false;
    g_level_ready = !RImplementation.o.oldshadowcascades;
    if (!g_level_ready)
    {
        // The old cascades walk the sector roots on their own (render_phase_sun_old.cpp).
        Msg("* [gd] trees: off with the old sun cascades");
        return;
    }
    da_gpu_trees_apply_switch();
#endif
}

void da_gpu_trees_level_unload()
{
#ifdef XR_D3D12
    destroy_registry();
    g_level_ready = false;
#endif
}

void da_gpu_trees_apply_switch()
{
#ifdef XR_D3D12
    // The frame's CPU stats slot starts here, before any view: the sun and rain views count from
    // worker tasks that run ahead of Render's frame_begin.
    FrameStats& stats = g_gd.stats_cpu[Device.dwFrame & 3];
    stats.main = stats.shadow = stats.draws = stats.records = 0;

    g_mode = da_offload_resolve(ps_r__gpu_driven);
    const bool on = g_level_ready && g_mode != 0;
    // Auto builds the registry with the level even on the CPU path: a later switch to the GPU costs no hitch.
    const bool build = on || (g_level_ready && ps_r__gpu_driven == kOffloadAuto && da_offload_auto_possible());
    if (build && !g_gd.built && !g_build_tried)
    {
        g_build_tried = true;
        build_registry();
    }
    const bool want = on && g_gd.built;
    if (want == g_gd.applied)
        return;
    if (want)
        apply_surgery();
    else
        restore_surgery();
    g_gd.applied = want;
    Msg("* [gd] %s: %u instances in %u nodes", want ? "on" : "off", u32(g_gd.instances.size()), u32(g_gd.nodes.size()));
#endif
}

void da_gpu_trees_frame_begin(CBackend& cmd_list)
{
#ifdef XR_D3D12
    const u32 frame = Device.dwFrame;
    print_stats(frame);
    if (!g_gd.applied)
        return;

    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    if (gpu_cull() && g_pGamePersistent)
    {
        wind_pass(cmd_list, frame);
        wind_debug(context, frame);
        // The previous frames' counters to the readback ring, this frame's slot to zero.
        context->CopyResource(g_gd.stats_staging[frame % 3], g_gd.stats);
        g_gd.stats_copied[frame % 3] = frame;
        const u32 zero[8]{};
        const D3D11_BOX box{(frame & 3) * 32, 0, 0, (frame & 3) * 32 + 32, 1, 1};
        context->UpdateSubresource(g_gd.stats, 0, &box, zero, 0, 0);
        return;
    }

    // The CPU cull's wind: every root's CPU oscillator (every root integrates every frame, drawn or
    // not); frozen wind holds each at its field.
    FTreeVisual::PrepareWind(g_gd.root_samples);
    float* out = g_gd.root_upload.data();
    for (FTreeVisual* sample : g_gd.root_samples)
    {
        const FTreeVisualWind wind = sample->wind_state();
        const float row[kRootStride / 4] = {wind.now.x, wind.now.y, wind.now.z, wind.now.w, wind.previous.x,
            wind.previous.y, wind.previous.z, wind.previous.w, wind.height, wind.gate, 0.f, 0.f};
        CopyMemory(out, row, sizeof(row));
        out += kRootStride / 4;
    }
    context->UpdateSubresource(g_gd.root_buffer, 0, nullptr, g_gd.root_upload.data(), 0, 0);
#else
    UNUSED(cmd_list);
#endif
}

void da_gpu_trees_refresh_flags()
{
#ifdef XR_D3D12
    // Ray traced sun shadows take the static casters of the near cascades; a swaying root never is one.
    bool changed = false;
    for (Instance& instance : g_gd.instances)
    {
        const bool rt_static = instance.visual->rt_static && instance.visual->rigid();
        const u32 flags = (instance.flags & ~kInstFlagRtStatic) | (rt_static ? kInstFlagRtStatic : 0u);
        changed |= flags != instance.flags;
        instance.flags = flags;
    }
    if (!changed)
        return;
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    xr_vector<u32> rows;
    if (g_gd.inst_buffer)
    {
        fill_instance_rows(rows);
        context->UpdateSubresource(g_gd.inst_buffer, 0, nullptr, rows.data(), 0, 0);
    }
    if (g_gd.cull_buffer)
    {
        fill_cull_rows(rows);
        context->UpdateSubresource(g_gd.cull_buffer, 0, nullptr, rows.data(), 0, 0);
    }
#endif
}

bool da_gpu_trees_draws(const FTreeVisual* tree)
{
#ifdef XR_D3D12
    return g_gd.applied && tree->gd_instance != u32(-1);
#else
    UNUSED(tree);
    return false;
#endif
}

bool da_gpu_trees_draw_pass(CBackend& cmd_list, SPass* pass, R_dsgraph::mapNormalItems& items, u32 phase)
{
#ifdef XR_D3D12
    if (!g_gd.applied || items.empty())
        return false;
    const auto found = g_gd.pass_index.find(pass);
    if (found == g_gd.pass_index.end())
        return false;
    const u32 entry = found->second;
    const PassEntry& pe = g_gd.passes[entry];
    ContextData& ctx = g_gd.contexts[cmd_list.context_id];

    // The registry's instance of an item drawn with this pass, kNone for the CPU path's items.
    const auto registered = [entry](const R_dsgraph::_NormalItem& item) {
        const u32 index = is_tree(item.pVisual) ? static_cast<FTreeVisual*>(item.pVisual)->gd_instance : kNone;
        return index != kNone && g_gd.protos[g_gd.instances[index].proto].find(entry) ? index : kNone;
    };

    // Lay the registered items out by bin; the others stay for the CPU path, in their order.
    ctx.counts.assign(pe.bin_count, 0);
    ctx.items.clear();
    for (const R_dsgraph::_NormalItem& item : items)
    {
        const u32 index = registered(item);
        if (index == kNone)
            continue;
        const Proto& proto = g_gd.protos[g_gd.instances[index].proto];
        const ProtoPass* use = proto.find(entry);
        u32 window = 0;
        if (proto.window_count > 1)
        {
            // FTreeVisual_PM::SelectLOD.
            const int w = iFloor((1.f - calc_lod(item.ssa)) * float(proto.window_count - 1) + 0.5f);
            window = u32(clampr(w, 0, int(proto.window_count) - 1));
        }
        const u32 bin = use->first_bin + window - pe.first_bin;
        ++ctx.counts[bin];
        ctx.items.emplace_back(bin, index);
    }
    if (ctx.items.empty())
        return false;

    // refs: the instances bin by bin, each bin in the dsgraph's (front to back) order.
    ctx.offsets.resize(pe.bin_count);
    u32 total = 0;
    for (u32 b = 0; b < pe.bin_count; ++b)
    {
        ctx.offsets[b] = total;
        total += ctx.counts[b];
    }
    ctx.fill.assign(ctx.offsets.begin(), ctx.offsets.end());
    ctx.refs_data.resize(total);
    for (const auto& [bin, index] : ctx.items)
        ctx.refs_data[ctx.fill[bin]++] = index;

    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    const u32 refs_at = stream_write(context, ctx.refs_stream, ctx.refs_data.data(), total * sizeof(u32));
    if (refs_at == kNone)
    {
        if (!g_stream_full_logged.test_and_set())
            Msg("! [gd] instance stream full (%u instances in one pass), the CPU path draws them", total);
        return false;
    }
    const u32 refs_base = refs_at / sizeof(u32);

    // One record per bin with instances, a group's records contiguous: the CPU knows the counts, so
    // the empty bins a GPU cull would leave at zero need no record here.
    ctx.args_data.clear();
    ctx.group_records.clear();
    u32 records = 0;
    for (u32 g = pe.first_group; g < pe.first_group + pe.group_count; ++g)
    {
        const Group& group = g_gd.groups[g];
        const u32 local = group.first_bin - pe.first_bin;
        u32 in_group = 0;
        for (u32 b = 0; b < group.bin_count; ++b)
        {
            const u32 count = ctx.counts[local + b];
            if (!count)
                continue;
            const Bin& bin = g_gd.bins[group.first_bin + b];
            const Window& w = g_gd.windows[g_gd.protos[bin.proto].first_window + bin.window];
            const u32 record[5] = {w.index_count, count, w.start_index, w.base_vertex, refs_base + ctx.offsets[local + b]};
            ctx.args_data.insert(ctx.args_data.end(), std::begin(record), std::end(record));
            ++in_group;
        }
        if (in_group)
            ctx.group_records.emplace_back(g, in_group);
        records += in_group;
    }
    const u32 args_at = stream_write(context, ctx.args_stream, ctx.args_data.data(), u32(ctx.args_data.size() * sizeof(u32)));
    if (args_at == kNone)
    {
        if (!g_stream_full_logged.test_and_set())
            Msg("! [gd] argument stream full (%u records in one pass), the CPU path draws them", records);
        return false;
    }

    // Written: the registered items leave the pass's list, the rest keep their order.
    size_t keep = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (registered(items[i]) == kNone)
            items[keep++] = items[i];
    items.resize(keep);

    cmd_list.set_Pass(pe.gd._get());
    cmd_list.apply_lmaterial();
    FTreeVisual::SetupInstancedGlobals(cmd_list);
    static const shared_str c_gd_params = "da_gd_params";
    cmd_list.set_c(c_gd_params, ps_r__Tree_SBC * 1.3333f, 0.f, 0.f, 0.f);
    cmd_list.SRVSManager.SetVSResource(kSlotInst, g_gd.inst_srv);
    cmd_list.SRVSManager.SetVSResource(kSlotRoot, g_gd.root_srv);
    u32 offset = args_at;
    for (const auto& [g, count] : ctx.group_records)
    {
        cmd_list.set_Geometry(g_gd.groups[g].geometry);
        cmd_list.set_InstanceStream(ctx.refs_stream.buffer, sizeof(u32));
        cmd_list.RenderIndirect(ctx.args_stream.buffer, offset, count);
        offset += count * kArgsStride;
    }
    const u32 draws = u32(ctx.group_records.size());
    cmd_list.set_InstanceStream(nullptr, 0);
    cmd_list.SRVSManager.SetVSResource(kSlotInst, nullptr);
    cmd_list.SRVSManager.SetVSResource(kSlotRoot, nullptr);

    FrameStats& stats = g_gd.stats_cpu[Device.dwFrame & 3];
    (phase == CRender::PHASE_NORMAL ? stats.main : stats.shadow) += total;
    stats.draws += draws;
    stats.records += records;
    return true;
#else
    UNUSED(cmd_list);
    UNUSED(pass);
    UNUSED(items);
    UNUSED(phase);
    return false;
#endif
}

DaGpuTreeWalk da_gpu_trees_view_begin(R_dsgraph_structure& graph)
{
#ifdef XR_D3D12
    graph.gd_view = 0;
    if (!gpu_cull() || graph.context_id >= R__NUM_CONTEXTS)
        return DaGpuTreeWalk::cpu;
    const bool normal = graph.o.phase == CRender::PHASE_NORMAL;
    // No static geometry, no priority 0, or a normal view other than the main one: the CPU way. So does a
    // local light's face (3.13): it sees a few trees, while a GPU view costs a cull of its cells, the
    // records of every bin and the barriers between (a point light's six faces: +1.6 ms on k00_marsh).
    if (!psDeviceFlags.test(rsDrawStatic) || !graph.o.pmask[0] || (normal && !graph.o.is_main_pass) ||
        graph.o.local_light)
        return DaGpuTreeWalk::cpu;

    View& view = g_gd.contexts[graph.context_id].view;
    view.serial = 0;
    view.planes.clear();
    view.frustums.clear();
    view.list.clear();
    view.bound = 0;
    std::fill(view.group_mask.begin(), view.group_mask.end(), 0u);
    view.main = normal;
    view.phase_bit = normal ? kPhaseNormal : kPhaseShadow;
    float volume[5], distance[5];
    view.flags = (normal ? 0u : kFlagSmap) | (graph.o.rt_skip_static ? kFlagRtSkip : 0u) |
        (dsgraph_static_value_thresholds(volume, distance) ? kFlagFilter : 0u);

    if (normal)
    {
        // The CPU walk decides the nodes (gd_node_main) and lists them here.
        view.flags |= kFlagNodeList;
        if (++view.stamp == 0)
        {
            std::fill(view.node_stamp.begin(), view.node_stamp.end(), 0u);
            view.stamp = 1;
        }
        view.serial = next_serial();
        graph.gd_view = view.serial;
        return DaGpuTreeWalk::main;
    }

    // A shadow view: the cells of every visited sector that one of its frustums sees; the node test on
    // the GPU uses the same frustums.
    const ContextData& ctx = g_gd.contexts[graph.context_id];
    u32 bytes = 0;
    for (CSector* sector : graph.PortalTraverser.r_sectors)
    {
        const u32 s = u32(sector->unique_id);
        if (s >= g_gd.sector_cells.size() || !g_gd.sector_cells[s].second)
            continue;
        const auto& frustums = sector->r_frustums[graph.context_id];
        if (frustums.empty())
            continue;
        const auto [first, count] = g_gd.sector_cells[s];
        u32 first_frustum = kNone;
        for (u32 c = first; c < first + count; ++c)
        {
            const Cell& cell = g_gd.cells[c];
            Fbox box = cell.box;
            bool visible = false;
            for (const CFrustum& frustum : frustums)
            {
                u32 mask = frustum.getMask();
                if (frustum.testAABB(box.data(), mask) != fcvNone)
                {
                    visible = true;
                    break;
                }
            }
            if (!visible)
                continue;
            if (first_frustum == kNone)
            {
                first_frustum = u32(view.frustums.size() / 2);
                for (const CFrustum& frustum : frustums)
                {
                    view.frustums.push_back(u32(view.planes.size()));
                    view.frustums.push_back(u32(frustum.p_count));
                    for (size_t p = 0; p < frustum.p_count; ++p)
                    {
                        const Fplane& plane = frustum.planes[p];
                        view.planes.emplace_back().set(plane.n.x, plane.n.y, plane.n.z, plane.d);
                    }
                }
            }
            view.list.insert(view.list.end(), {cell.first_node, cell.node_count, first_frustum, u32(frustums.size())});
            const u32* mask = &g_gd.cell_groups[size_t(c) * g_gd.group_words];
            for (u32 w = 0; w < g_gd.group_words; ++w)
                view.group_mask[w] |= mask[w];
            view.bound += cell.instance_count * 2;
        }
    }
    bytes = u32(view.planes.size() * 16 + view.frustums.size() * 4 + view.list.size() * 4);
    if (bytes > ctx.list_capacity)
    {
        // Too many sector frustums for the view list: this view culls the nodes on the CPU.
        if (!g_list_full_logged.test_and_set())
            Msg("! [gd] view list full (%u bytes), a view culls its trees on the CPU", bytes);
        return DaGpuTreeWalk::cpu;
    }
    if (!view.list.empty())
    {
        view.serial = next_serial();
        graph.gd_view = view.serial;
    }
    return DaGpuTreeWalk::skip;
#else
    UNUSED(graph);
    return DaGpuTreeWalk::cpu;
#endif
}

bool da_gpu_trees_view_listed(const R_dsgraph_structure& graph, u32 node)
{
#ifdef XR_D3D12
    const View& view = g_gd.contexts[graph.context_id].view;
    return node < view.node_stamp.size() && view.node_stamp[node] == view.stamp;
#else
    UNUSED(graph);
    UNUSED(node);
    return false;
#endif
}

bool da_gpu_trees_view_node(R_dsgraph_structure& graph, u32 node)
{
#ifdef XR_D3D12
    View& view = g_gd.contexts[graph.context_id].view;
    if (!graph.gd_view || node >= view.node_stamp.size() || view.node_stamp[node] == view.stamp)
        return false;
    view.node_stamp[node] = view.stamp;
    view.list.push_back(node);
    const Node& n = g_gd.nodes[node];
    const u32* mask = &g_gd.cell_groups[size_t(n.cell) * g_gd.group_words];
    for (u32 w = 0; w < g_gd.group_words; ++w)
        view.group_mask[w] |= mask[w];
    view.bound += n.instance_count * 2;
    return true;
#else
    UNUSED(graph);
    UNUSED(node);
    return false;
#endif
}

void da_gpu_trees_view_end(R_dsgraph_structure& graph)
{
#ifdef XR_D3D12
    if (!graph.gd_view || graph.context_id >= R__NUM_CONTEXTS)
        return;
    View& view = g_gd.contexts[graph.context_id].view;
    const bool node_list = (view.flags & kFlagNodeList) != 0;
    view.entries = u32(node_list ? view.list.size() : view.list.size() / 4);
    if (!view.entries)
    {
        graph.gd_view = 0;
        return;
    }
    // The upload: planes, frustums, entries (byte offsets for the cull).
    view.data.clear();
    view.plane_offset = 0;
    for (const Fvector4& plane : view.planes)
    {
        const float p[4] = {plane.x, plane.y, plane.z, plane.w};
        u32 u[4];
        CopyMemory(u, p, sizeof(u));
        view.data.insert(view.data.end(), std::begin(u), std::end(u));
    }
    view.frustum_offset = u32(view.data.size() * 4);
    view.data.insert(view.data.end(), view.frustums.begin(), view.frustums.end());
    view.entry_offset = u32(view.data.size() * 4);
    view.data.insert(view.data.end(), view.list.begin(), view.list.end());

    // A view whose static lists came out empty would not be drawn at all (the callers' bNormal): an empty
    // item list of one of the view's passes keeps it drawn.
    if (graph.mapNormalPasses[0][0].empty())
    {
        for (u32 g = 0; g < g_gd.groups.size(); ++g)
        {
            if ((view.group_mask[g / 32] & (1u << (g % 32))) && (g_gd.group_phases[g] & view.phase_bit))
            {
                graph.get_normal_pass_items(0, 0, g_gd.passes[g_gd.groups[g].pass].pass);
                break;
            }
        }
    }
#else
    UNUSED(graph);
#endif
}

void da_gpu_trees_view_render(R_dsgraph_structure& graph)
{
#ifdef XR_D3D12
    const u32 serial = graph.gd_view;
    graph.gd_view = 0;
    if (!serial || !gpu_cull() || graph.context_id >= R__NUM_CONTEXTS)
        return;
    ContextData& ctx = g_gd.contexts[graph.context_id];
    View& view = ctx.view;
    if (view.serial != serial || !view.entries)
        return;
    view.serial = 0;

    CBackend& cmd_list = graph.cmd_list;
    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    const u32 bins = u32(g_gd.bins.size());
    {
        // The main view's cull, records and refs: the gd field of r__gpu_log.
        std::optional<dx11GpuTimerScope> timer;
        if (view.main && cmd_list.context_id == CHW::IMM_CTX_ID)
            timer.emplace(dx11GpuTimers::GdCull);

        const D3D11_BOX box{0, 0, 0, u32(view.data.size() * 4), 1, 1};
        context->UpdateSubresource(ctx.list, 0, &box, view.data.data(), 0, 0);
        const UINT zero[4]{};
        context->ClearUnorderedAccessViewUint(ctx.counts_uav, zero);
        xrd3d12::UavBarrier(context, nullptr);

        const bool node_list = (view.flags & kFlagNodeList) != 0;
        float volume[5]{}, distance[5]{};
        dsgraph_static_value_thresholds(volume, distance);
        const Fvector& cam = Device.vCameraPosition;
        const float fov = Device.fFOV / 67.f;
        const u32 frame = Device.dwFrame;
        const u32 groups = node_list ? (view.entries + 63) / 64 : view.entries;
        compute(cmd_list, g_gd.s_cull, groups, 1,
            {g_gd.cull_srv, g_gd.node_srv, g_gd.filter_srv, g_gd.anc_srv, g_gd.kind_srv, ctx.list_srv},
            {ctx.counts_uav, ctx.vis_uav, g_gd.valuable_uav, g_gd.stats_uav}, [&] {
                cmd_list.set_c("gd_cam", cam.x, cam.y, cam.z, fov * fov);
                cmd_list.set_c("gd_ssa", r_ssaDISCARD, r_ssaLOD_A, r_ssaVEG_DISCARD, 0.f);
                cmd_list.set_c("gd_lod", r_ssaGLOD_start, r_ssaGLOD_end, r_dtex_range, 0.f);
                cmd_list.set_c("gd_view", float(view.flags), float(graph.context_id), float(frame & kFrameMask), float(frame & 3));
                cmd_list.set_c("gd_layout", float(view.entries), float(view.plane_offset), float(view.frustum_offset),
                    float(view.entry_offset));
                cmd_list.set_c("gd_filter_v", volume[0], volume[1], volume[2], volume[3]);
                cmd_list.set_c("gd_filter_d", distance[0], distance[1], distance[2], distance[3]);
                cmd_list.set_c("gd_filter_e", volume[4], distance[4], float(bins), float(g_gd.instances.size() * 2));
            });
        compute(cmd_list, g_gd.s_args, (bins + 63) / 64, 1, {g_gd.bin_srv}, {ctx.counts_uav, ctx.args_uav},
            [&] { cmd_list.set_c("gd_args_size", float(bins), 0.f, 0.f, 0.f); });
        if (view.bound)
        {
            compute(cmd_list, g_gd.s_scatter, (view.bound + 63) / 64, 1, {}, {ctx.counts_uav, ctx.vis_uav, ctx.args_uav, ctx.refs_uav},
                [&] { cmd_list.set_c("gd_scatter_size", float(bins), float(g_gd.instances.size() * 2), 0.f, 0.f); });
        }
    }

    u32 draws = 0, records = 0;
    draw_view(cmd_list, ctx, draws, records);
    FrameStats& stats = g_gd.stats_cpu[Device.dwFrame & 3];
    stats.draws += draws;
    stats.records += records;
#else
    UNUSED(graph);
#endif
}

const DaGpuTreeSector* da_gpu_trees_sector(u32 sector_id)
{
#ifdef XR_D3D12
    if (!g_gd.applied || sector_id >= g_gd.sectors.size())
        return nullptr;
    const DaGpuTreeSector& sector = g_gd.sectors[sector_id];
    return sector.nodes.empty() ? nullptr : &sector;
#else
    UNUSED(sector_id);
    return nullptr;
#endif
}

void da_gpu_trees_for_each_node(CSector* sector, DaGpuTreeNodeFn fn, void* user)
{
#ifdef XR_D3D12
    if (!sector || !fn)
        return;
    if (const DaGpuTreeSector* nodes = da_gpu_trees_sector(u32(sector->unique_id)))
        for (const DaGpuTreeNode& node : nodes->nodes)
            fn(node.visual, user);
#else
    UNUSED(sector);
    UNUSED(fn);
    UNUSED(user);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
