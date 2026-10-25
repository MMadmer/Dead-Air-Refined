// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The level's static geometry by clusters (da_clusters.h, docs/dead-air/CLUSTERS.md).
#include "stdafx.h"
#include "da_clusters.h"
#include "da_cluster_dag.h"
#include "da_hzb.h"
#include "da_veg_internal.h"
#include "FProgressive.h"
#include "FVisual.h"
#include "Common/LevelStructure.hpp"
#include "xrCore/stream_reader.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderDX11/dx11Native.h"
#include "xrCore/FMesh.hpp"

#include <atomic>
#include <cmath>
#include <thread>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using da_veg::GpuBuffer;
using da_veg::make_buffer;

// Raise when the hierarchy's builder or its cache's layout changes: the cache of other versions is grown again.
constexpr u32 kVersion = 3;

// The frame's pools: views (a main view takes two, its second pass's), draws (a pass and vertex buffer of a view), the
// instances and the cull's table, the kept clusters' list, the second pass's list, the index pool.
constexpr u32 kMaxViews = 96;
constexpr u32 kMaxDraws = 32768;
constexpr u32 kMaxInstances = 1u << 18;
constexpr u32 kMaxTable = 1u << 18;
constexpr u32 kMaxVisible = 1u << 19;
constexpr u32 kMaxSecond = 1u << 18;
constexpr u32 kPoolIndices = 12u << 20;
// The counters: the index pool's top, each view's kept and hidden clusters, then the draws' indices.
constexpr u32 kCountsHead = 1 + kMaxViews * 2;
// The arguments: each view's two dispatches (32 bytes), then the draws (20 bytes each).
constexpr u32 kArgsDraws = kMaxViews * 32;

// A cluster's record (r3\da_cluster.h DaCluster): 80 bytes.
struct GpuCluster
{
    float box_c[3];
    float lod_error;
    float box_e[3];
    float parent_error;
    float lod_sphere[4];
    float parent_sphere[4];
    u32 tri_first;
    u32 vert_first;
    u32 counts; // triangles | (vertices - 1) << 8
    u32 level;
};
static_assert(sizeof(GpuCluster) == 80);

// A level visual drawn by clusters: the clusters of its full geometry (the G-buffer, the alpha-tested casters) and of its
// fast one (positions alone, welded: the shadow maps' opaque casters, as the stock draws take it), each with the vertex
// buffer they read.
struct VisualRec
{
    u32 first{}, count{}, vb{};
    u32 fast_first{}, fast_count{}, fast_vb{};
};

// A visual's geometry in the level's buffers (level.geom, or level.geomX for the fast one).
struct Mesh
{
    u32 vb{}, vbase{}, vcount{};
    u32 ib{}, ibase{}, icount{};
    bool valid{};
};

// A candidate visual, gathered on the main thread at the level's load.
struct Source
{
    u32 index{}; // in the level's visuals
    Mesh full, fast;
};

struct CacheHeader
{
    u32 magic{0x4C434144u}; // "DACL"
    u32 version{kVersion};
    u64 key{};
    u32 visuals{}, clusters{}, tris{}, verts{};
};

// The builder's output.
struct Built
{
    xr_vector<GpuCluster> clusters;
    xr_vector<u8> tris;
    xr_vector<u16> verts;
    xr_vector<VisualRec> visuals; // by source
    u32 levels{}, stuck{};
    u64 source_tris{};
    double ms{};
    bool from_cache{};
};

class CBlender_da_cluster : public IBlender
{
    pcstr m_cs;

public:
    explicit CBlender_da_cluster(pcstr cs) : m_cs(cs) {}
    LPCSTR getComment() override { return "INTERNAL: cluster hierarchy"; }
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

struct DrawRec
{
    SPass* pass;
    u32 pass_index;
    u32 vb;
    u32 index; // in the frame's draw pool
    bool fast;
};

// A context's view this frame: its slots and draws.
struct ViewState
{
    u32 frame{u32(-1)};
    u32 slot{}, second_slot{u32(-1)};
    u32 draw_first{}, draw_count{};
    u32 visible_first{}, visible_room{};
    u32 second_first{}, second_room{};
    bool consumed{};
    bool second_done{};
    xr_vector<DrawRec> draws;
    // Scratch of the gather.
    xr_vector<u32> inst;
    xr_vector<u32> table;
};

struct State
{
    // The level.
    xr_vector<Source> sources;
    xr_vector<dxRender_Visual*> source_visuals;
    shared_str level_name;
    u64 key{};
    std::thread builder;
    std::atomic<bool> cancel{false};
    std::atomic<int> build_state{0}; // 0 none, 1 building, 2 built (waiting for the GPU), 3 on the GPU, -1 failed
    Built built;
    xr_vector<VisualRec> visuals; // by source, on the GPU
    xr_vector<ref_geom> geoms[2]; // full and fast, by vertex buffer: its declaration, the buffer, the index pool
    GpuBuffer clusters_b, tris_b, verts_b;
    u32 cluster_count{};

    // The frame's pools.
    GpuBuffer counts_b, visible_b, second_b, args_b, inst_b, table_b;
    ID3D11Buffer* indices{};
    ID3D11UnorderedAccessView* indices_uav{};
    bool pools{};
    bool pools_failed{};
    std::atomic<u32> view_alloc{0}, draw_alloc{0}, inst_alloc{0}, table_alloc{0}, visible_alloc{0}, second_alloc{0};
    u32 frame{u32(-1)};
    bool driver_lists{true};

    ref_shader s_cull, s_args, s_expand;
    bool shaders_tried{};
    bool shaders_ok{};

    ViewState views[R__NUM_CONTEXTS];

    // Stats of the frame (CPU side) and a readback of the counters.
    std::atomic<u32> st_views{0}, st_instances{0}, st_candidates{0}, st_draws{0}, st_full{0};
    ID3D11Buffer* staging{};
    int readback{0};
    u32 readback_frame{};
    u32 readback_views{};
};
State g;

// ---------------------------------------------------------------------------------------------------------------------
// The build
// ---------------------------------------------------------------------------------------------------------------------

u64 fnv64(const void* data, size_t bytes, u64 h = 0xcbf29ce484222325ull)
{
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// A vertex attribute decoded to floats, with its kind for the simplifier.
struct AttrDecl
{
    u32 offset;
    u8 type;
    u8 floats;
    u8 kinds[4];
};

// The declaration's position and its attributes: a normal (its fourth byte the hemisphere term: a colour), a tangent or
// a binormal, a set of texture coordinates (a pair), a colour.
bool parse_decl(const VertexElement* dcl, u32 len, u32& pos_offset, xr_vector<AttrDecl>& attrs)
{
    pos_offset = u32(-1);
    attrs.clear();
    for (u32 e = 0; e < len; ++e)
    {
        const VertexElement& el = dcl[e];
        if (el.Stream != 0)
            continue;
        if (el.Usage == D3DDECLUSAGE_POSITION && el.Type == D3DDECLTYPE_FLOAT3)
        {
            pos_offset = el.Offset;
            continue;
        }
        AttrDecl a{el.Offset, el.Type, 0, {da_mesh::akIgnore, da_mesh::akIgnore, da_mesh::akIgnore, da_mesh::akIgnore}};
        switch (el.Type)
        {
        case D3DDECLTYPE_FLOAT1: a.floats = 1; break;
        case D3DDECLTYPE_FLOAT2:
        case D3DDECLTYPE_SHORT2:
        case D3DDECLTYPE_SHORT2N:
        case D3DDECLTYPE_USHORT2N:
        case D3DDECLTYPE_FLOAT16_2: a.floats = 2; break;
        case D3DDECLTYPE_FLOAT3: a.floats = 3; break;
        case D3DDECLTYPE_FLOAT4:
        case D3DDECLTYPE_D3DCOLOR:
        case D3DDECLTYPE_UBYTE4:
        case D3DDECLTYPE_UBYTE4N:
        case D3DDECLTYPE_SHORT4:
        case D3DDECLTYPE_SHORT4N:
        case D3DDECLTYPE_USHORT4N:
        case D3DDECLTYPE_FLOAT16_4: a.floats = 4; break;
        default: continue;
        }
        switch (el.Usage)
        {
        case D3DDECLUSAGE_NORMAL:
            a.kinds[0] = a.kinds[1] = a.kinds[2] = da_mesh::akNormal;
            a.kinds[3] = da_mesh::akColor;
            break;
        case D3DDECLUSAGE_TANGENT:
        case D3DDECLUSAGE_BINORMAL: a.kinds[0] = a.kinds[1] = a.kinds[2] = da_mesh::akTangent; break;
        case D3DDECLUSAGE_TEXCOORD:
            // A pair a set: the first two of a four-component set (the lightmap's second pair is the stock's hemi there).
            a.kinds[0] = a.kinds[1] = da_mesh::akUV;
            if (a.floats > 2)
                a.kinds[2] = a.kinds[3] = da_mesh::akColor;
            break;
        case D3DDECLUSAGE_COLOR: a.kinds[0] = a.kinds[1] = a.kinds[2] = a.kinds[3] = da_mesh::akColor; break;
        default: continue;
        }
        attrs.push_back(a);
    }
    return pos_offset != u32(-1);
}

float half_to_float(u16 h)
{
    const u32 s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : (e == 31 ? 65504.f : std::ldexp(float(m | 1024), int(e) - 25));
    return s ? -v : v;
}

// Its components as floats: the unsigned normalized bytes to [-1, 1] for the direction kinds (any fixed linear map
// is the simplifier's: its weights are relative), shorts as they stand (the texture sets are weighed by their own size).
void decode_attr(const u8* v, const AttrDecl& a, float* out)
{
    const u8* p = v + a.offset;
    switch (a.type)
    {
    case D3DDECLTYPE_FLOAT1:
    case D3DDECLTYPE_FLOAT2:
    case D3DDECLTYPE_FLOAT3:
    case D3DDECLTYPE_FLOAT4: std::memcpy(out, p, a.floats * 4); break;
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4N:
    case D3DDECLTYPE_UBYTE4:
        for (u32 k = 0; k < 4; ++k)
            out[k] = a.kinds[k] == da_mesh::akColor ? p[k] / 255.f : p[k] / 127.5f - 1.f;
        break;
    case D3DDECLTYPE_SHORT2:
    case D3DDECLTYPE_SHORT4:
    case D3DDECLTYPE_SHORT2N:
    case D3DDECLTYPE_SHORT4N:
        for (u32 k = 0; k < a.floats; ++k)
        {
            s16 s;
            std::memcpy(&s, p + k * 2, 2);
            out[k] = float(s);
        }
        break;
    case D3DDECLTYPE_USHORT2N:
    case D3DDECLTYPE_USHORT4N:
        for (u32 k = 0; k < a.floats; ++k)
        {
            u16 s;
            std::memcpy(&s, p + k * 2, 2);
            out[k] = float(s);
        }
        break;
    case D3DDECLTYPE_FLOAT16_2:
    case D3DDECLTYPE_FLOAT16_4:
        for (u32 k = 0; k < a.floats; ++k)
        {
            u16 h;
            std::memcpy(&h, p + k * 2, 2);
            out[k] = half_to_float(h);
        }
        break;
    default: break;
    }
}

struct GeomFile
{
    struct VB
    {
        xr_vector<VertexElement> decl;
        u32 stride{}, count{};
        xr_vector<u8> data;
    };
    xr_vector<VB> vbs;
    xr_vector<xr_vector<u16>> ibs;
};

// The level's vertex and index buffers read back from level.geom, or level.geomX for the fast ones (the GPU buffers
// keep no CPU copy): only those the sources use.
bool read_geom(const xr_vector<Source>& sources, bool fast, GeomFile& out)
{
    CStreamReader* geom = FS.rs_open("$level$", fast ? "level.geomX" : "level.geom");
    if (!geom)
        return false;
    xr_vector<u8> want_vb, want_ib;
    for (const Source& src : sources)
    {
        const Mesh& s = fast ? src.fast : src.full;
        if (!s.valid)
            continue;
        if (s.vb >= want_vb.size())
            want_vb.resize(s.vb + 1, 0);
        if (s.ib >= want_ib.size())
            want_ib.resize(s.ib + 1, 0);
        want_vb[s.vb] = want_ib[s.ib] = 1;
    }
    bool ok = true;
    if (CStreamReader* fs = geom->open_chunk(fsL_VB))
    {
        const u32 count = fs->r_u32();
        out.vbs.resize(count);
        constexpr size_t decl_bytes = (MAXD3DDECLLENGTH + 1) * sizeof(VertexElement);
        for (u32 b = 0; b < count && !g.cancel; ++b)
        {
            VertexElement dcl[MAXD3DDECLLENGTH + 1];
            fs->r(dcl, decl_bytes);
            fs->advance(-int(decl_bytes));
            const u32 dcl_len = GetDeclLength(dcl) + 1;
            fs->advance(int(dcl_len * sizeof(VertexElement)));
            const u32 n = fs->r_u32();
            const u32 stride = GetDeclVertexSize(dcl, 0);
            const size_t bytes = size_t(n) * stride;
            GeomFile::VB& vb = out.vbs[b];
            vb.decl.assign(dcl, dcl + dcl_len);
            vb.stride = stride;
            vb.count = n;
            if (b < want_vb.size() && want_vb[b])
            {
                const u8* data = static_cast<const u8*>(fs->pointer_contiguous(bytes));
                vb.data.assign(data, data + bytes);
            }
            fs->advance(int(bytes));
        }
        fs->close();
    }
    else
        ok = false;
    if (CStreamReader* fs = geom->open_chunk(fsL_IB))
    {
        const u32 count = fs->r_u32();
        out.ibs.resize(count);
        for (u32 b = 0; b < count && !g.cancel; ++b)
        {
            const u32 n = fs->r_u32();
            const size_t bytes = size_t(n) * 2;
            if (b < want_ib.size() && want_ib[b])
            {
                const u16* data = static_cast<const u16*>(fs->pointer_contiguous(bytes));
                out.ibs[b].assign(data, data + n);
            }
            fs->advance(int(bytes));
        }
        fs->close();
    }
    else
        ok = false;
    FS.r_close(geom);
    return ok && !g.cancel;
}

// One visual's hierarchy: its vertices decoded, its open border locked (the next visual's border lies on it), the
// clusters out with their vertex tables in the vertex buffer's own numbering.
struct VisualOut
{
    xr_vector<GpuCluster> clusters;
    xr_vector<u8> tris;
    xr_vector<u16> verts;
    u32 levels{}, stuck{};
};

void build_visual(const Mesh& s, const GeomFile& geom, VisualOut& out)
{
    if (!s.valid || s.vb >= geom.vbs.size() || s.ib >= geom.ibs.size())
        return;
    const GeomFile::VB& vb = geom.vbs[s.vb];
    const xr_vector<u16>& ib = geom.ibs[s.ib];
    if (vb.data.empty() || size_t(s.vbase) + s.vcount > vb.count || size_t(s.ibase) + s.icount > ib.size())
        return;
    u32 pos_offset;
    xr_vector<AttrDecl> decl;
    if (!parse_decl(vb.decl.data(), u32(vb.decl.size()) - 1, pos_offset, decl))
        return;
    u32 na = 0;
    xr_vector<u8> kinds;
    for (const AttrDecl& a : decl)
        for (u32 k = 0; k < a.floats; ++k)
        {
            kinds.push_back(a.kinds[k]);
            ++na;
        }
    const u32 nv = s.vcount;
    xr_vector<float> pos(size_t(nv) * 3), attr(size_t(nv) * na);
    for (u32 v = 0; v < nv; ++v)
    {
        const u8* src = vb.data.data() + size_t(s.vbase + v) * vb.stride;
        std::memcpy(&pos[size_t(v) * 3], src + pos_offset, 12);
        float* dst = &attr[size_t(v) * na];
        for (const AttrDecl& a : decl)
        {
            float tmp[4]{};
            decode_attr(src, a, tmp);
            std::memcpy(dst, tmp, a.floats * 4);
            dst += a.floats;
        }
    }
    xr_vector<u32> idx(s.icount);
    for (u32 i = 0; i < s.icount; ++i)
        idx[i] = ib[s.ibase + i];
    for (u32 i : idx)
        if (i >= nv)
            return;

    // The open border by position: an edge of one triangle.
    xr_vector<u8> locked(nv, 0);
    {
        xr_unordered_map<u64, u32> pos_id;
        xr_vector<u32> pid(nv);
        for (u32 v = 0; v < nv; ++v)
        {
            float p[3];
            for (u32 k = 0; k < 3; ++k)
                p[k] = pos[size_t(v) * 3 + k] + 0.f;
            const u64 key = fnv64(p, 12);
            pid[v] = pos_id.try_emplace(key, u32(pos_id.size())).first->second;
        }
        xr_unordered_map<u64, u32> edges;
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
            for (u32 c = 0; c < 3; ++c)
            {
                u32 a = pid[idx[t + c]], b = pid[idx[t + (c + 1) % 3]];
                if (a > b)
                    std::swap(a, b);
                ++edges[(u64(a) << 32) | b];
            }
        xr_vector<u8> border_pos(pos_id.size(), 0);
        for (const auto& [e, n] : edges)
            if (n == 1)
                border_pos[u32(e >> 32)] = border_pos[u32(e)] = 1;
        for (u32 v = 0; v < nv; ++v)
            locked[v] = border_pos[pid[v]];
    }

    da_mesh::DagInput in;
    in.positions = pos.data();
    in.vertex_count = nv;
    in.attributes = na ? attr.data() : nullptr;
    in.attr_count = na;
    in.attr_kinds = kinds.data();
    in.indices = idx.data();
    in.tri_count = u32(idx.size() / 3);
    in.locked = locked.data();
    da_mesh::DagOutput dag;
    da_mesh::build_dag(in, dag);
    out.levels = dag.levels;
    out.stuck = dag.stuck;
    out.clusters.reserve(dag.clusters.size());
    for (const da_mesh::DagCluster& c : dag.clusters)
    {
        GpuCluster r{};
        for (u32 k = 0; k < 3; ++k)
        {
            r.box_c[k] = 0.5f * (c.box_min[k] + c.box_max[k]);
            r.box_e[k] = 0.5f * (c.box_max[k] - c.box_min[k]);
        }
        r.lod_error = c.lod_error;
        r.parent_error = c.parent_error;
        std::memcpy(r.lod_sphere, c.lod_sphere, 16);
        std::memcpy(r.parent_sphere, c.parent_sphere, 16);
        r.tri_first = c.tri_first; // made global by the caller
        r.vert_first = c.vert_first;
        r.counts = c.tri_count | ((c.vert_count - 1) << 8);
        r.level = c.level;
        out.clusters.push_back(r);
    }
    out.tris.assign(dag.tris.begin(), dag.tris.end());
    out.verts.resize(dag.verts.size());
    for (size_t i = 0; i < dag.verts.size(); ++i)
        out.verts[i] = u16(s.vbase + dag.verts[i]);
}

void cache_path(string_path& out, u64 key)
{
    string256 name;
    xr_sprintf(name, "cache" DELIMITER "clusters" DELIMITER "%s_%016llx_%u.bin", g.level_name.c_str(),
        (unsigned long long)key, kVersion);
    FS.update_path(out, "$app_data_root$", name);
}

bool cache_read(Built& b)
{
    string_path path;
    cache_path(path, g.key);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f)
        return false;
    CacheHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CacheHeader{}.magic && h.version == kVersion && h.key == g.key &&
        h.visuals == g.sources.size() && h.clusters < (1u << 26) && h.tris < (1u << 30) && h.verts < (1u << 30);
    if (ok)
    {
        b.visuals.resize(h.visuals);
        b.clusters.resize(h.clusters);
        b.tris.resize(size_t(h.tris) * 3);
        b.verts.resize(h.verts);
        ok = fread(b.visuals.data(), sizeof(VisualRec), b.visuals.size(), f) == b.visuals.size() &&
            (b.clusters.empty() || fread(b.clusters.data(), sizeof(GpuCluster), b.clusters.size(), f) == b.clusters.size()) &&
            (b.tris.empty() || fread(b.tris.data(), 1, b.tris.size(), f) == b.tris.size()) &&
            (b.verts.empty() || fread(b.verts.data(), 2, b.verts.size(), f) == b.verts.size());
    }
    fclose(f);
    if (ok)
        for (const VisualRec& v : b.visuals)
            ok = ok && size_t(v.first) + v.count <= b.clusters.size() &&
                size_t(v.fast_first) + v.fast_count <= b.clusters.size();
    return ok;
}

void cache_write(const Built& b)
{
    string_path dir, path, tmp;
    FS.update_path(dir, "$app_data_root$", "cache" DELIMITER "clusters" DELIMITER);
    VerifyPath(dir);
    cache_path(path, g.key);
    xr_sprintf(tmp, "%s.tmp", path);
    CacheHeader h;
    h.key = g.key;
    h.visuals = u32(b.visuals.size());
    h.clusters = u32(b.clusters.size());
    h.tris = u32(b.tris.size() / 3);
    h.verts = u32(b.verts.size());
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") || !f)
        return;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
        fwrite(b.visuals.data(), sizeof(VisualRec), b.visuals.size(), f) == b.visuals.size() &&
        (b.clusters.empty() || fwrite(b.clusters.data(), sizeof(GpuCluster), b.clusters.size(), f) == b.clusters.size()) &&
        (b.tris.empty() || fwrite(b.tris.data(), 1, b.tris.size(), f) == b.tris.size()) &&
        (b.verts.empty() || fwrite(b.verts.data(), 2, b.verts.size(), f) == b.verts.size());
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

// The worker: the cache, or every source's hierarchy on threads of its own under the game's (the biggest first, each
// visual's build on one thread), then the cache written.
void build_thread()
{
    const u64 t0 = CPU::QPC();
    Built b;
    if (cache_read(b))
    {
        b.from_cache = true;
    }
    else
    {
        // Both geometries: the full one always, the fast one where the level has it.
        GeomFile geom[2];
        if (!read_geom(g.sources, false, geom[0]))
        {
            g.build_state = -1;
            return;
        }
        bool any_fast = false;
        for (const Source& s : g.sources)
            any_fast |= s.fast.valid;
        if (any_fast && !read_geom(g.sources, true, geom[1]))
            for (Source& s : g.sources)
                s.fast.valid = false;
        if (g.cancel)
            return;
        // The work: each source's two geometries, the biggest first.
        const u32 n = u32(g.sources.size());
        xr_vector<u32> order(n * 2);
        for (u32 i = 0; i < n * 2; ++i)
            order[i] = i;
        const auto mesh_of = [](u32 job) -> const Mesh& { return job < u32(g.sources.size()) ? g.sources[job].full : g.sources[job - g.sources.size()].fast; };
        std::sort(order.begin(), order.end(), [&](u32 a, u32 c) { return mesh_of(a).icount > mesh_of(c).icount; });
        xr_vector<VisualOut> outs(n * 2);
        std::atomic<u32> next{0};
        const u32 hw = std::thread::hardware_concurrency();
        const u32 threads = std::max(1u, std::min(n * 2, hw > 2 ? hw - 2 : 1u));
        xr_vector<std::thread> pool;
        for (u32 t = 0; t < threads; ++t)
            pool.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (u32 i; !g.cancel && (i = next++) < n * 2;)
                {
                    const u32 job = order[i];
                    build_visual(mesh_of(job), geom[job < n ? 0 : 1], outs[job]);
                }
            });
        for (std::thread& t : pool)
            t.join();
        if (g.cancel)
            return;
        b.visuals.resize(n);
        const auto append = [&b](VisualOut& o, u32& first, u32& count) {
            first = u32(b.clusters.size());
            count = u32(o.clusters.size());
            const u32 tri_base = u32(b.tris.size() / 3), vert_base = u32(b.verts.size());
            for (GpuCluster& c : o.clusters)
            {
                c.tri_first += tri_base;
                c.vert_first += vert_base;
                b.clusters.push_back(c);
            }
            b.tris.insert(b.tris.end(), o.tris.begin(), o.tris.end());
            b.verts.insert(b.verts.end(), o.verts.begin(), o.verts.end());
            b.levels = std::max(b.levels, o.levels);
            b.stuck += o.stuck;
            o = {};
        };
        for (u32 i = 0; i < n; ++i)
        {
            VisualRec& r = b.visuals[i];
            append(outs[i], r.first, r.count);
            r.vb = g.sources[i].full.vb;
            append(outs[n + i], r.fast_first, r.fast_count);
            r.fast_vb = g.sources[i].fast.vb;
        }
        cache_write(b);
    }
    for (const Source& s : g.sources)
        b.source_tris += s.full.icount / 3;
    b.ms = CPU::QPC() > t0 ? double(CPU::QPC() - t0) * 1000.0 / double(CPU::qpc_freq) : 0.0;
    g.built = std::move(b);
    g.build_state = 2;
}

// ---------------------------------------------------------------------------------------------------------------------
// The GPU
// ---------------------------------------------------------------------------------------------------------------------

bool shaders()
{
    if (g.shaders_tried)
        return g.shaders_ok;
    g.shaders_tried = true;
    // (An install with older shaders has no passes: compiling a missing file is fatal.)
    if (!da_async_shader_exists("da_cluster_cull.cs") || !da_async_shader_exists("da_cluster_args.cs") ||
        !da_async_shader_exists("da_cluster_expand.cs"))
        return false;
    static CBlender_da_cluster cull("da_cluster_cull"), args("da_cluster_args"), expand("da_cluster_expand");
    g.s_cull.create(&cull, "r2" DELIMITER "da_cluster_cull");
    g.s_args.create(&args, "r2" DELIMITER "da_cluster_args");
    g.s_expand.create(&expand, "r2" DELIMITER "da_cluster_expand");
    const auto ok = [](ref_shader& s) { return s && s->E[0] && !s->E[0]->passes.empty() && s->E[0]->passes[0]->cs; };
    g.shaders_ok = ok(g.s_cull) && ok(g.s_args) && ok(g.s_expand);
    return g.shaders_ok;
}

bool make_pools()
{
    if (g.pools || g.pools_failed)
        return g.pools;
    g.pools_failed = true;
    if (!make_buffer(g.counts_b, (kCountsHead + kMaxDraws) * 4, nullptr, true, true) ||
        !make_buffer(g.visible_b, kMaxVisible * 12, nullptr, true, true) ||
        !make_buffer(g.second_b, kMaxSecond * 8, nullptr, true, true) ||
        !make_buffer(g.args_b, kArgsDraws + kMaxDraws * 20, nullptr, true, true, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) ||
        !make_buffer(g.inst_b, kMaxInstances * 16, nullptr, false, true) ||
        !make_buffer(g.table_b, kMaxTable * 8, nullptr, false, true))
        return false;
    // The index pool: the draws' index buffer and the expansion's raw target.
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = kPoolIndices * 2;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(HW.pDevice->CreateBuffer(&desc, nullptr, &g.indices)))
        return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = desc.ByteWidth / 4;
    uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(HW.pDevice->CreateUnorderedAccessView(g.indices, &uav, &g.indices_uav)))
        return false;
    D3D11_BUFFER_DESC st{};
    st.ByteWidth = kCountsHead * 4;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HW.pDevice->CreateBuffer(&st, nullptr, &g.staging);
    // A deferred context's UpdateSubresource with a box needs the driver's own command lists (or the runtime offsets the
    // source wrongly): without them the views on worker contexts keep the stock path.
    D3D11_FEATURE_DATA_THREADING threading{};
    if (SUCCEEDED(HW.pDevice->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading))))
        g.driver_lists = threading.DriverCommandLists != FALSE;
#if defined(XR_NATIVE)
    g.driver_lists = true;
#endif
    g.pools = true;
    g.pools_failed = false;
    return true;
}

void release_level_gpu()
{
    for (dxRender_Visual* v : g.source_visuals)
        if (v)
            v->da_cluster = u32(-1);
    g.geoms[0].clear();
    g.geoms[1].clear();
    g.clusters_b.release();
    g.tris_b.release();
    g.verts_b.release();
    g.visuals.clear();
    g.cluster_count = 0;
}

// The finished hierarchy to the GPU: the three buffers, a geometry for each vertex buffer over the index pool, each
// visual's slot.
void upload()
{
    Built& b = g.built;
    if (b.clusters.empty() || !make_pools())
    {
        g.build_state = -1;
        return;
    }
    // (The byte reads past a buffer's last triangle and table entry stay inside it.)
    b.tris.resize(b.tris.size() + 8, 0);
    if (b.verts.size() & 1)
        b.verts.push_back(0);
    if (!make_buffer(g.clusters_b, u32(b.clusters.size() * sizeof(GpuCluster)), b.clusters.data()) ||
        !make_buffer(g.tris_b, u32(b.tris.size()), b.tris.data()) ||
        !make_buffer(g.verts_b, u32(b.verts.size() * 2), b.verts.data()))
    {
        release_level_gpu();
        g.build_state = -1;
        return;
    }
    g.cluster_count = u32(b.clusters.size());
    g.visuals = b.visuals;
    // A geometry for each vertex buffer the clusters read, full and fast, over the index pool.
    u32 vb_count[2]{};
    for (const VisualRec& v : g.visuals)
    {
        vb_count[0] = std::max(vb_count[0], v.vb + 1);
        vb_count[1] = std::max(vb_count[1], v.fast_count ? v.fast_vb + 1 : 0u);
    }
    g.geoms[0].resize(vb_count[0]);
    g.geoms[1].resize(vb_count[1]);
    for (u32 i = 0; i < g.visuals.size(); ++i)
    {
        const VisualRec& v = g.visuals[i];
        if (!v.count)
            continue;
        if (!g.geoms[0][v.vb])
            g.geoms[0][v.vb].create(RImplementation.getVB_Format(v.vb), *RImplementation.getVB(v.vb), g.indices);
        if (v.fast_count && !g.geoms[1][v.fast_vb])
            g.geoms[1][v.fast_vb].create(
                RImplementation.getVB_Format(v.fast_vb, true), *RImplementation.getVB(v.fast_vb, true), g.indices);
        g.source_visuals[i]->da_cluster = i;
    }
    u64 tris = 0;
    for (const GpuCluster& c : b.clusters)
        tris += c.counts & 0xFF;
    Msg("* [clusters] %s: %u visuals, %llu triangles -> %u clusters (%llu triangles over %u levels, %u stuck groups), %.1f MB, "
        "%s in %.0f ms",
        g.level_name.c_str(), u32(g.visuals.size()), (unsigned long long)b.source_tris, g.cluster_count,
        (unsigned long long)tris, b.levels, b.stuck,
        double(g.clusters_b.bytes + g.tris_b.bytes + g.verts_b.bytes) / (1024.0 * 1024.0),
        b.from_cache ? "from the disk cache" : "built", b.ms);
    g.built = {};
    g.build_state = 3;
}

void update(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, u32 offset, const void* data, u32 bytes)
{
    const D3D11_BOX box{offset, 0, 0, offset + bytes, 1, 1};
    ctx->UpdateSubresource(buf, 0, &box, data, 0, 0);
}

void barrier(ID3D11DeviceContext* ctx)
{
#if defined(XR_NATIVE)
    xrnative::UavBarrier(ctx, nullptr);
#else
    UNUSED(ctx);
#endif
}

// The view's frustum planes from its view-projection (rows as X-Ray multiplies: a point times the matrix), inside
// positive: left, right, bottom, top, then near and far.
void planes_of(const Fmatrix& m, Fvector4 (&out)[6])
{
    const Fvector4 c0{m._11, m._21, m._31, m._41}, c1{m._12, m._22, m._32, m._42}, c2{m._13, m._23, m._33, m._43},
        c3{m._14, m._24, m._34, m._44};
    const auto add = [](const Fvector4& a, const Fvector4& b, float s) {
        return Fvector4{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s};
    };
    out[0] = add(c3, c0, 1.f);
    out[1] = add(c3, c0, -1.f);
    out[2] = add(c3, c1, 1.f);
    out[3] = add(c3, c1, -1.f);
    out[4] = c2;
    out[5] = add(c3, c2, -1.f);
    for (Fvector4& p : out)
    {
        const float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (l > 1e-20f)
            p.mul(1.f / l);
    }
}

void set_pass(CBackend& cmd, ref_shader& s)
{
    SPass& S = *s->E[0]->passes[0];
    cmd.set_CS(S.cs);
    cmd.set_Constants(S.constants);
}

void clear_cs(CBackend& cmd, ID3D11DeviceContext* ctx, u32 srvs, u32 uavs)
{
    ID3D11UnorderedAccessView* none[4]{};
    const UINT initial[4] = {~0u, ~0u, ~0u, ~0u};
    ctx->CSSetUnorderedAccessViews(0, uavs, none, initial);
    for (u32 slot = 0; slot < srvs; ++slot)
        cmd.SRVSManager.SetCSResource(slot, nullptr);
    cmd.SRVSManager.Apply(cmd.context_id);
    cmd.clear_CS_resources();
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------------------------------------------------

void da_clusters_level_load()
{
    da_clusters_level_unload();
    if (!ps_r__clusters)
        return;
    // The candidates: the level's own normal and progressive visuals over its vertex and index buffers (an XMS overlay or
    // a tree is left to its own path).
    const auto& visuals = RImplementation.Visuals;
    u64 key = fnv64(&kVersion, sizeof(kVersion));
    for (u32 i = 0; i < visuals.size(); ++i)
    {
        dxRender_Visual* v = visuals[i];
        if (!v || (v->Type != MT_NORMAL && v->Type != MT_PROGRESSIVE))
            continue;
        Fvisual* f = static_cast<Fvisual*>(v);
        if (!f->p_rm_Vertices || !f->p_rm_Indices || f->vCount == 0 || f->dwPrimitives == 0)
            continue;
        int vb = -1, ib = -1;
        for (u32 k = 0; k < RImplementation.nVB.size(); ++k)
            if (&RImplementation.nVB[k] == f->p_rm_Vertices)
                vb = int(k);
        for (u32 k = 0; k < RImplementation.nIB.size(); ++k)
            if (&RImplementation.nIB[k] == f->p_rm_Indices)
                ib = int(k);
        if (vb < 0 || ib < 0)
            continue;
        Source s;
        s.index = i;
        s.full = {u32(vb), f->vBase, f->vCount, u32(ib), f->iBase, f->dwPrimitives * 3, true};
        // A progressive visual's full detail: its first window.
        if (v->Type == MT_PROGRESSIVE)
        {
            const FSlideWindowItem& w = static_cast<const FProgressive*>(v)->windows();
            if (!w.count || !w.sw)
                continue;
            s.full.ibase = f->iBase + w.sw[0].offset;
            s.full.icount = u32(w.sw[0].num_tris) * 3;
            s.full.vcount = w.sw[0].num_verts;
        }
        // Small visuals keep the stock path: one cluster saves nothing over one draw.
        if (s.full.icount / 3 < u32(ps_r__clusters_min_tris))
            continue;
        // The fast geometry (level.geomX, positions alone): the shadow maps' opaque casters.
        if (const IRender_Mesh* m = f->m_fast; m && m->p_rm_Vertices && m->p_rm_Indices && m->dwPrimitives)
        {
            int fvb = -1, fib = -1;
            for (u32 k = 0; k < RImplementation.xVB.size(); ++k)
                if (&RImplementation.xVB[k] == m->p_rm_Vertices)
                    fvb = int(k);
            for (u32 k = 0; k < RImplementation.xIB.size(); ++k)
                if (&RImplementation.xIB[k] == m->p_rm_Indices)
                    fib = int(k);
            if (fvb >= 0 && fib >= 0)
            {
                s.fast = {u32(fvb), m->vBase, m->vCount, u32(fib), m->iBase, m->dwPrimitives * 3, true};
                if (v->Type == MT_PROGRESSIVE)
                {
                    const FSlideWindowItem* w = static_cast<const FProgressive*>(v)->windows_fast();
                    if (w && w->count && w->sw)
                    {
                        s.fast.ibase = m->iBase + w->sw[0].offset;
                        s.fast.icount = u32(w->sw[0].num_tris) * 3;
                        s.fast.vcount = w->sw[0].num_verts;
                    }
                    else
                        s.fast.valid = false;
                }
            }
        }
        g.sources.push_back(s);
        g.source_visuals.push_back(v);
        const u32 key_words[10] = {s.index, s.full.vb, s.full.vbase, s.full.icount, s.full.ibase, s.fast.vb, s.fast.vbase,
            s.fast.icount, s.fast.ibase, s.fast.valid ? 1u : 0u};
        key = fnv64(key_words, sizeof(key_words), key);
    }
    if (g.sources.empty())
        return;
    if (IReader* r = FS.r_open("$level$", "level.geom"))
    {
        const u32 len = u32(r->length());
        key = fnv64(&len, 4, key);
        FS.r_close(r);
    }
    g.key = key;
    g.level_name = g_pGameLevel ? g_pGameLevel->name() : shared_str("level");
    g.cancel = false;
    g.build_state = 1;
    g.builder = std::thread(build_thread);
}

void da_clusters_level_unload()
{
    g.cancel = true;
    if (g.builder.joinable())
        g.builder.join();
    g.cancel = false;
    release_level_gpu();
    g.sources.clear();
    g.source_visuals.clear();
    g.built = {};
    g.build_state = 0;
    for (ViewState& v : g.views)
        v.frame = u32(-1);
}

void da_clusters_frame_prepare()
{
    if (g.build_state == 2)
    {
        if (g.builder.joinable())
            g.builder.join();
        upload();
    }
    else if (g.build_state == -1 && g.builder.joinable())
    {
        g.builder.join();
        Msg("! [clusters] %s: the hierarchy could not be built; the stock path draws the level", g.level_name.c_str());
    }
    if (g.build_state != 3 || !g.pools || !shaders())
        return;
    // The last frame's numbers, and its counters read back.
    if (ps_r__clusters_stats_frames > 0 && g.frame != u32(-1))
    {
        --ps_r__clusters_stats_frames;
        Msg("* [clusters] frame %u: %u views, %u instances, %u candidate clusters, %u draws, %u views left to the stock path",
            g.frame, u32(g.st_views), u32(g.st_instances), u32(g.st_candidates), u32(g.st_draws), u32(g.st_full));
        if (!g.readback && g.staging)
        {
            g.readback = 1;
            g.readback_frame = g.frame;
            g.readback_views = g.view_alloc;
        }
    }
    g.frame = Device.dwFrame;
    g.view_alloc = g.draw_alloc = g.inst_alloc = g.table_alloc = g.visible_alloc = g.second_alloc = 0;
    g.st_views = g.st_instances = g.st_candidates = g.st_draws = g.st_full = 0;
}

void da_clusters_frame_begin(CBackend& cmd)
{
    if (g.build_state != 3 || !g.pools || !g.shaders_ok)
        return;
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    if (g.readback == 2 && Device.dwFrame > g.readback_frame + 2)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            const u32* c = static_cast<const u32*>(m.pData);
            u32 kept = 0, hidden = 0;
            for (u32 v = 0; v < g.readback_views && v < kMaxViews; ++v)
            {
                kept += c[1 + v * 2];
                hidden += c[2 + v * 2];
            }
            Msg("* [clusters] frame %u: %u views kept %u clusters (%u hidden by the pyramid), %u indices of %u in the pool%s",
                g.readback_frame, g.readback_views, kept, hidden, c[0], kPoolIndices, c[0] > kPoolIndices ? " - FULL" : "");
            ctx->Unmap(g.staging, 0);
        }
        g.readback = 0;
    }
    // (The counters still hold the last frame's: every view of it ran before this.)
    if (g.readback == 1)
    {
        const D3D11_BOX box{0, 0, 0, kCountsHead * 4, 1, 1};
        ctx->CopySubresourceRegion(g.staging, 0, 0, 0, 0, g.counts_b.buf, 0, &box);
        g.readback = 2;
    }
    // On the immediate context ahead of every view's culls: the cascades record theirs on contexts of their own, which
    // run after it.
    const UINT zero[4]{};
    ctx->ClearUnorderedAccessViewUint(g.counts_b.uav, zero);
}

DaClusterView da_clusters_main_view()
{
    DaClusterView v;
    v.view = Device.mView;
    v.project = Device.mProject;
    v.width = float(Device.dwRenderWidth ? Device.dwRenderWidth : Device.dwWidth);
    v.height = float(Device.dwRenderHeight ? Device.dwRenderHeight : Device.dwHeight);
    v.threshold_px = ps_r__clusters_px;
    v.main = true;
    return v;
}

DaClusterView da_clusters_shadow_view(const Fmatrix& view, const Fmatrix& project, float size, u32 kind)
{
    DaClusterView v;
    v.kind = kind;
    v.view = view;
    v.project = project;
    v.width = v.height = size;
    v.threshold_px = ps_r__clusters_shadow_px;
    v.shadow = true;
    return v;
}

// (The shaders are made at the frame's start on the main thread: the views cull on worker threads.)
bool da_clusters_ready() { return g.build_state == 3 && g.pools && ps_r__clusters && g.shaders_ok; }

void da_clusters_cull(R_dsgraph_structure& g_, const DaClusterView& desc)
{
    ViewState& V = g.views[g_.context_id];
    V.frame = u32(-1);
    if (!da_clusters_ready() || !(u32(ps_r__clusters_views) & desc.kind))
        return;
    const bool immediate = g_.context_id == R_dsgraph_structure::IMM_CTX_ID;
    if (!immediate && !g.driver_lists)
        return;
    const bool fade = g_.fade_view();

    // The view's cluster visuals and their draws, counted before anything leaves the lists.
    V.draws.clear();
    V.inst.clear();
    V.table.clear();
    u32 candidates = 0;
    const auto eligible = [&](dxRender_Visual* v) {
        return v->da_cluster < g.visuals.size() && g.visuals[v->da_cluster].count &&
            !(fade && v->vis.fade_frame == Device.dwFrame && v->vis.fade_draw < 1.f);
    };
    // A shadow map's pass that reads the position alone takes the fast geometry, as the stock draws do.
    const auto fast_pass = [&](SPass* pass) {
        return ps_r__clusters_fast && desc.shadow && pass && pass->vs && pass->vs->signature && pass->vs->signature->position_only && !pass->hs._get();
    };
    const auto range = [&](dxRender_Visual* v, bool fast, u32& first, u32& count, u32& vb) {
        const VisualRec& r = g.visuals[v->da_cluster];
        const bool use_fast = fast && r.fast_count;
        first = use_fast ? r.fast_first : r.first;
        count = use_fast ? r.fast_count : r.count;
        vb = use_fast ? r.fast_vb : r.vb;
        return use_fast;
    };
    using PassList = xr_vector<R_dsgraph::mapNormal_T::value_type*>;
    static thread_local PassList passes[SHADER_PASSES_MAX];
    for (u32 ip = 0; ip < SHADER_PASSES_MAX; ++ip)
    {
        passes[ip].clear();
        g_.mapNormalPasses[0][ip].get_any_p(passes[ip]);
        for (auto* p : passes[ip])
        {
            const bool fast = fast_pass(p->first);
            for (const auto& item : p->second)
                if (eligible(item.pVisual))
                {
                    u32 first, count, vb;
                    range(item.pVisual, fast, first, count, vb);
                    candidates += count;
                }
        }
    }
    if (!candidates)
        return;

    // Each visual one instance, a draw a pass and vertex buffer; the table a row a 64 clusters.
    u32 local_draws = 0;
    for (u32 ip = 0; ip < SHADER_PASSES_MAX; ++ip)
        for (auto* p : passes[ip])
        {
            const u32 first_draw = u32(V.draws.size());
            const bool fast = fast_pass(p->first);
            for (const auto& item : p->second)
            {
                dxRender_Visual* v = item.pVisual;
                if (!eligible(v))
                    continue;
                u32 first, count, vb;
                const bool use_fast = range(v, fast, first, count, vb);
                u32 d = first_draw;
                while (d < V.draws.size() && (V.draws[d].vb != vb || V.draws[d].fast != use_fast))
                    ++d;
                if (d == V.draws.size())
                    V.draws.push_back({p->first, ip, vb, local_draws++, use_fast});
                const u32 inst = u32(V.inst.size() / 4);
                V.inst.insert(V.inst.end(), {first, count, d, 0u});
                for (u32 k = 0; k < count; k += 64)
                    V.table.insert(V.table.end(), {inst, k});
            }
        }
    const u32 n_inst = u32(V.inst.size() / 4), n_table = u32(V.table.size() / 2), n_draws = u32(V.draws.size());
    const u32 slots = desc.main && ps_r__clusters_hzb ? 2 : 1;

    // The frame's pools; a view that does not fit keeps the stock path.
    const u32 slot = g.view_alloc.fetch_add(slots);
    const u32 draw_first = g.draw_alloc.fetch_add(n_draws * slots);
    const u32 inst_first = g.inst_alloc.fetch_add(n_inst);
    const u32 table_first = g.table_alloc.fetch_add(n_table);
    const u32 visible_first = g.visible_alloc.fetch_add(candidates);
    const u32 second_first = slots > 1 ? g.second_alloc.fetch_add(candidates) : 0;
    if (slot + slots > kMaxViews || draw_first + n_draws * slots > kMaxDraws || inst_first + n_inst > kMaxInstances ||
        table_first + n_table > kMaxTable || visible_first + candidates > kMaxVisible ||
        (slots > 1 && second_first + candidates > kMaxSecond))
    {
        ++g.st_full;
        return;
    }
    // Now the visuals leave the lists.
    for (u32 ip = 0; ip < SHADER_PASSES_MAX; ++ip)
        for (auto* p : passes[ip])
        {
            auto& items = p->second;
            size_t kept = 0;
            for (size_t i = 0; i < items.size(); ++i)
                if (!eligible(items[i].pVisual))
                    items[kept++] = items[i];
            items.resize(kept);
        }
    for (DrawRec& d : V.draws)
        d.index += draw_first;
    // Each instance's draw as the pool's index.
    for (u32 i = 0; i < n_inst; ++i)
        V.inst[i * 4 + 2] = V.draws[V.inst[i * 4 + 2]].index;
    std::sort(V.draws.begin(), V.draws.end(), [](const DrawRec& a, const DrawRec& b) {
        return a.pass_index != b.pass_index ? a.pass_index < b.pass_index : a.pass < b.pass;
    });
    V.slot = slot;
    V.second_slot = slots > 1 ? slot + 1 : u32(-1);
    V.draw_first = draw_first;
    V.draw_count = n_draws;
    V.visible_first = visible_first;
    V.visible_room = candidates;
    V.second_first = second_first;
    V.second_room = slots > 1 ? candidates : 0;
    V.consumed = false;
    V.second_done = slots == 1;
    V.frame = Device.dwFrame;
    g.st_views += 1;
    g.st_instances += n_inst;
    g.st_candidates += candidates;
    g.st_draws += n_draws;

    CBackend& cmd = g_.cmd_list;
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    update(ctx, g.inst_b.buf, inst_first * 16, V.inst.data(), n_inst * 16);
    update(ctx, g.table_b.buf, table_first * 8, V.table.data(), n_table * 8);

    // The view's constants.
    Fmatrix vp;
    vp.mul(desc.project, desc.view);
    Fvector4 planes[6];
    planes_of(vp, planes);
    const bool perspective = desc.project._34 != 0.f;
    Fvector eye{}, fwd{};
    float lod = 0.f, znear = 0.05f;
    if (perspective)
    {
        Fmatrix inv;
        inv.invert(desc.view);
        eye = inv.c;
        fwd.set(desc.view._13, desc.view._23, desc.view._33).normalize_safe();
        znear = desc.project._33 != 0.f ? std::max(-desc.project._43 / desc.project._33, 0.01f) : 0.05f;
        lod = desc.project._22 * 0.5f * desc.height / std::max(desc.threshold_px, 0.01f);
    }
    else
    {
        const float sx = std::sqrt(vp._11 * vp._11 + vp._21 * vp._21 + vp._31 * vp._31);
        lod = sx * 0.5f * desc.width / std::max(desc.threshold_px, 0.01f);
    }
    // Shadow views keep what lies before their near plane or past their far one: a caster clamped onto the map's depth
    // range still casts.
    const u32 plane_count = desc.shadow ? 4 : 6;
    const bool hzb = desc.main && ps_r__clusters_hzb && da_hzb_bind_previous_ready();

    const UINT initial[4] = {~0u, ~0u, ~0u, ~0u};
    // The cull.
    {
        set_pass(cmd, g.s_cull);
        for (u32 k = 0; k < 6; ++k)
        {
            const Fvector4& p = k < plane_count ? planes[k] : Fvector4{0.f, 0.f, 0.f, 1.f};
            cmd.set_ca("da_cl_planes", k, p.x, p.y, p.z, p.w);
        }
        cmd.set_c("da_cl_eye", eye.x, eye.y, eye.z, perspective ? 1.f : 0.f);
        cmd.set_c("da_cl_fwd", fwd.x, fwd.y, fwd.z, znear);
        cmd.set_c("da_cl_lod", lod, 0.f, 0.f, 0.f);
        const u32 gx = std::min(n_table, 65535u), gy = (n_table + gx - 1) / gx;
        cmd.set_c("da_cl_view", float(inst_first), float(table_first), float(gx), float(slot));
        cmd.set_c("da_cl_view2", float(kCountsHead), float(visible_first), float(candidates), hzb ? 3.f : 0.f);
        cmd.set_c("da_cl_view3", float(second_first), float(V.second_room), float(slot), 0.f);
        cmd.set_c("da_cl_view4", float(n_table), 0.f, 0.f, 0.f);
        if (hzb)
            da_hzb_bind_previous(cmd, 3);
        cmd.SRVSManager.SetCSResource(0, g.clusters_b.srv);
        cmd.SRVSManager.SetCSResource(1, g.inst_b.srv);
        cmd.SRVSManager.SetCSResource(2, g.table_b.srv);
        ID3D11UnorderedAccessView* uavs[3] = {g.counts_b.uav, g.visible_b.uav, g.second_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, initial);
        cmd.Compute(gx, gy, 1);
        clear_cs(cmd, ctx, 4, 3);
    }
    barrier(ctx);
    // The draws' arguments and the dispatches.
    {
        set_pass(cmd, g.s_args);
        cmd.set_c("da_cl_view", float(draw_first), float(n_draws), float(kPoolIndices), float(slot));
        cmd.set_c("da_cl_view2", float(kCountsHead), float(kArgsDraws), float(candidates), float(V.second_room));
        ID3D11UnorderedAccessView* uavs[2] = {g.counts_b.uav, g.args_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, initial);
        cmd.Compute(1, 1, 1);
        clear_cs(cmd, ctx, 0, 2);
    }
    barrier(ctx);
    // The indices.
    {
        set_pass(cmd, g.s_expand);
        cmd.set_c("da_cl_view", float(visible_first), float(candidates), float(kArgsDraws), float(slot));
        cmd.SRVSManager.SetCSResource(0, g.clusters_b.srv);
        cmd.SRVSManager.SetCSResource(1, g.tris_b.srv);
        cmd.SRVSManager.SetCSResource(2, g.verts_b.srv);
        cmd.SRVSManager.SetCSResource(3, g.visible_b.srv);
        cmd.SRVSManager.SetCSResource(4, g.args_b.srv);
        cmd.SRVSManager.SetCSResource(5, g.counts_b.srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g.indices_uav, initial);
        cmd.ComputeIndirect(g.args_b.buf, slot * 32);
        clear_cs(cmd, ctx, 6, 1);
        // The pool was a compute target just now: the runtime took it off the input assembler, so the backend's cached
        // index buffer is forgotten and the next draw binds it again.
        cmd.set_Indices(nullptr);
    }
    barrier(ctx);
}

void da_clusters_draw_pass(R_dsgraph_structure& g_, SPass* pass, u32 pass_index)
{
    ViewState& V = g.views[g_.context_id];
    if (V.frame != Device.dwFrame || V.consumed || V.draws.empty())
        return;
    // The draws sorted by pass index and pass: the run of this one.
    const auto lo = std::lower_bound(V.draws.begin(), V.draws.end(), std::make_pair(pass_index, pass),
        [](const DrawRec& d, const std::pair<u32, SPass*>& k) {
            return d.pass_index != k.first ? d.pass_index < k.first : d.pass < k.second;
        });
    CBackend& cmd = g_.cmd_list;
    for (auto it = lo; it != V.draws.end() && it->pass_index == pass_index && it->pass == pass; ++it)
    {
        xr_vector<ref_geom>& geoms = g.geoms[it->fast ? 1 : 0];
        if (it->vb >= geoms.size() || !geoms[it->vb])
            continue;
        cmd.set_Geometry(geoms[it->vb]);
        cmd.RenderIndirect(g.args_b.buf, kArgsDraws + it->index * 20, 1);
    }
}

void da_clusters_view_end(R_dsgraph_structure& g_)
{
    ViewState& V = g.views[g_.context_id];
    if (V.frame != Device.dwFrame)
        return;
    // A lamp's cache fill drew the statics into its slot; the live map's call after it draws none of them again.
    if (g_.o.smap_draw == 1)
        V.consumed = true;
    else
        V.frame = u32(-1);
}

void da_clusters_second_pass(R_dsgraph_structure& g_)
{
    ViewState& V = g.views[g_.context_id];
    if (V.frame != Device.dwFrame || V.second_done || V.second_slot == u32(-1))
        return;
    V.second_done = true;
    CBackend& cmd = g_.cmd_list;
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    // The pyramid of the frame's own first draws, then the clusters the last frame's pyramid hid against it.
    if (!da_hzb_build(cmd))
    {
        RImplementation.Target->phase_scene_begin_parallel(cmd, cmd.xforms.m_motion_pass);
        return;
    }
    const UINT initial[4] = {~0u, ~0u, ~0u, ~0u};
    const u32 slot = V.second_slot;
    {
        set_pass(cmd, g.s_cull);
        cmd.set_c("da_cl_view", 0.f, 0.f, 65535.f, float(slot));
        cmd.set_c("da_cl_view2", float(kCountsHead), float(V.visible_first), float(V.visible_room), 5.f);
        cmd.set_c("da_cl_view3", float(V.second_first), float(V.second_room), float(V.slot), float(V.draw_count));
        cmd.set_c("da_cl_view4", 0.f, 0.f, 0.f, 0.f);
        da_hzb_bind(cmd, 3, true);
        cmd.SRVSManager.SetCSResource(0, g.clusters_b.srv);
        ID3D11UnorderedAccessView* uavs[3] = {g.counts_b.uav, g.visible_b.uav, g.second_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 3, uavs, initial);
        cmd.ComputeIndirect(g.args_b.buf, V.slot * 32 + 16);
        clear_cs(cmd, ctx, 4, 3);
    }
    barrier(ctx);
    {
        set_pass(cmd, g.s_args);
        cmd.set_c("da_cl_view", float(V.draw_first + V.draw_count), float(V.draw_count), float(kPoolIndices), float(slot));
        cmd.set_c("da_cl_view2", float(kCountsHead), float(kArgsDraws), float(V.visible_room), 0.f);
        ID3D11UnorderedAccessView* uavs[2] = {g.counts_b.uav, g.args_b.uav};
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, initial);
        cmd.Compute(1, 1, 1);
        clear_cs(cmd, ctx, 0, 2);
    }
    barrier(ctx);
    {
        set_pass(cmd, g.s_expand);
        cmd.set_c("da_cl_view", float(V.visible_first), float(V.visible_room), float(kArgsDraws), float(slot));
        cmd.SRVSManager.SetCSResource(0, g.clusters_b.srv);
        cmd.SRVSManager.SetCSResource(1, g.tris_b.srv);
        cmd.SRVSManager.SetCSResource(2, g.verts_b.srv);
        cmd.SRVSManager.SetCSResource(3, g.visible_b.srv);
        cmd.SRVSManager.SetCSResource(4, g.args_b.srv);
        cmd.SRVSManager.SetCSResource(5, g.counts_b.srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g.indices_uav, initial);
        cmd.ComputeIndirect(g.args_b.buf, slot * 32);
        clear_cs(cmd, ctx, 6, 1);
        // The pool was a compute target just now: the runtime took it off the input assembler, so the backend's cached
        // index buffer is forgotten and the next draw binds it again.
        cmd.set_Indices(nullptr);
    }
    barrier(ctx);
    // The G-buffer's targets back, then the second pass's draws pass by pass (they follow the first's in the pool by the
    // view's draw count).
    RImplementation.Target->phase_scene_begin_parallel(cmd, cmd.xforms.m_motion_pass);
    SPass* last = nullptr;
    for (const DrawRec& d : V.draws)
    {
        xr_vector<ref_geom>& geoms = g.geoms[d.fast ? 1 : 0];
        if (d.vb >= geoms.size() || !geoms[d.vb])
            continue;
        if (d.pass != last)
        {
            cmd.set_Pass(d.pass);
            cmd.apply_lmaterial();
            last = d.pass;
        }
        cmd.set_Geometry(geoms[d.vb]);
        cmd.RenderIndirect(g.args_b.buf, kArgsDraws + (d.index + V.draw_count) * 20, 1);
    }
}

void da_clusters_stats()
{
    Msg("* [clusters] state %d, %u visuals, %u clusters, pools %s, shaders %s; next frames logged",
        int(g.build_state), u32(g.visuals.size()), g.cluster_count, g.pools ? "on" : "off",
        g.shaders_ok ? "on" : (g.shaders_tried ? "missing" : "not yet"));
    ps_r__clusters_stats_frames = 2;
}

void da_clusters_destroy()
{
    da_clusters_level_unload();
    g.counts_b.release();
    g.visible_b.release();
    g.second_b.release();
    g.args_b.release();
    g.inst_b.release();
    g.table_b.release();
    _RELEASE(g.indices_uav);
    _RELEASE(g.indices);
    _RELEASE(g.staging);
    g.pools = g.pools_failed = false;
    g.s_cull.destroy();
    g.s_args.destroy();
    g.s_expand.destroy();
    g.shaders_tried = g.shaders_ok = false;
}
} // namespace xray::render::RENDER_NAMESPACE
