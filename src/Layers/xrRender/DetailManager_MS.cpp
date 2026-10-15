// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Mesh-shader grass (docs/dead-air/dx12-features/mesh.md 3.2-3.10): GPU-resident rows, the per-pass
// mesh draw, the A/B parity tool and the GPU row check; the frame reaches it through da_grass_mesh.h. DirectX 12 only: the R4 module compiles the stubs at the end of the file.
//
// The calc task keeps the rows of every visible part in a pool on the GPU (a buddy allocator over
// units of 8 rows) and writes the rows that changed plus the frame's row list - the pool rows of the
// visible parts, part after part in draw order - into an upload ring; the render thread copies both
// at the top of the frame. A pass then costs one mesh dispatch per run of consecutive visible parts
// per object; an amplification group takes 32 consecutive list entries, whatever part they belong to.
// Parts that do not fit (pool, ring) or whose model does not fit a mesh group are drawn by the vertex
// path in the same pass (hybrid).

#include "stdafx.h"

#include "DetailManager.h"
#include "Layers/xrRenderPC_R4/da_grass_mesh.h"
#include "Layers/xrRenderPC_R4/da_offload.h"

#if defined(XR_D3D12)
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/da_wind_history.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "Layers/xrRenderDX12/d12_api.h"
#include "Layers/xrRenderPC_R4/da_dxc.h"
#include "xrCore/ProductVersion.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(XR_D3D12)
namespace detail_manager
{
extern const int quant;
short QC(float v);
} // namespace detail_manager

namespace
{
constexpr u32 kRowBytes = sizeof(CDetailManager::InstanceRows);
constexpr u32 kUnitRows = 8;       // allocation unit of the pool
constexpr u32 kGroupRows = 32;     // list entries per amplification group (GRASS_AS_THREADS)
constexpr u32 kMaxOrder = 12;      // a block of up to 4096 units (32768 rows)
constexpr u32 kRingSlots = 4;      // more than the layer's frames in flight
constexpr u32 kRingSlotBytes = 8u << 20;
constexpr u32 kMaxGroups = 65535;  // DispatchMesh, per dimension
constexpr u32 kMaxPlanes = 12;     // grass_planes (FRUSTUM_MAXPLANES)
constexpr u32 kMsMaxVerts = 64, kMsMaxPrims = 126, kMaxIpg = 32; // da_detail_mesh.h
constexpr u32 kNotResident = u32(-1);
// The row list takes at most half a ring slot, the other half is left for the rows themselves.
constexpr u32 kMaxListEntries = kRingSlotBytes / 2 / sizeof(u32);
static_assert(kRowBytes == 64, "InstanceRows is the shader's detail_rows");

// detail_vtx of da_detail_mesh.h.
struct DetailVertex
{
    float pos[3];
    u32 uv;   // u | v << 16, signed shorts
    u32 tmid; // frac | matrix-id << 16
};
static_assert(sizeof(DetailVertex) == 20);

// Buddy allocator over the pool's units: free blocks by order in intrusive lists.
class UnitBuddy
{
public:
    void Init(u32 units)
    {
        m_units = units;
        m_next.assign(units, kNone);
        m_prev.assign(units, kNone);
        m_order.assign(units, kNotFree);
        for (u32& head : m_head)
            head = kNone;
        m_free = 0;
        for (u32 at = 0; at < units;)
        {
            u32 order = kMaxOrder;
            while (order && ((at & ((1u << order) - 1)) || at + (1u << order) > units))
                --order;
            Push(at, order);
            m_free += 1u << order;
            at += 1u << order;
        }
    }

    bool Alloc(u32 order, u32& unit)
    {
        u32 o = order;
        while (o <= kMaxOrder && m_head[o] == kNone)
            ++o;
        if (o > kMaxOrder)
            return false;
        const u32 block = m_head[o];
        Remove(block);
        // Split down: the upper halves go back as free blocks.
        while (o > order)
        {
            --o;
            Push(block + (1u << o), o);
        }
        m_free -= 1u << order;
        unit = block;
        return true;
    }

    void Free(u32 unit, u32 order)
    {
        m_free += 1u << order;
        while (order < kMaxOrder)
        {
            const u32 buddy = unit ^ (1u << order);
            if (buddy >= m_units || m_order[buddy] != order)
                break;
            Remove(buddy);
            unit = std::min(unit, buddy);
            ++order;
        }
        Push(unit, order);
    }

    u32 FreeUnits() const { return m_free; }

private:
    static constexpr u32 kNone = u32(-1);
    static constexpr u8 kNotFree = 0xff;

    void Push(u32 block, u32 order)
    {
        m_order[block] = u8(order);
        m_prev[block] = kNone;
        m_next[block] = m_head[order];
        if (m_head[order] != kNone)
            m_prev[m_head[order]] = block;
        m_head[order] = block;
    }

    void Remove(u32 block)
    {
        const u32 order = m_order[block];
        if (m_prev[block] != kNone)
            m_next[m_prev[block]] = m_next[block];
        else
            m_head[order] = m_next[block];
        if (m_next[block] != kNone)
            m_prev[m_next[block]] = m_prev[block];
        m_order[block] = kNotFree;
    }

    u32 m_units = 0;
    u32 m_free = 0;
    u32 m_head[kMaxOrder + 1]{};
    xr_vector<u32> m_next, m_prev;
    xr_vector<u8> m_order;
};

u32 OrderFor(u32 rows)
{
    const u32 units = (rows + kUnitRows - 1) / kUnitRows;
    u32 order = 0;
    while ((1u << order) < units)
        ++order;
    return order;
}

// ---- availability ----------------------------------------------------------------------------

// A mesh pipeline the driver refused this session: the vertex path from the next frame on.
std::atomic_bool s_refused{false};
bool s_refusal_handled = false;

// render-grass-mesh.cache (PLAN C21): a refusal is stored under the adapter, its driver, the D3D12
// runtime and the game version, so the next start picks the base preset column at once and a new
// driver or release tries again.
constexpr u32 kVerdictMagic = 0x3147534d; // "MSG1"
constexpr u32 kVerdictVersion = 1;

u32 VerdictSignature()
{
    u32 signature = crc32(&kVerdictVersion, sizeof(kVerdictVersion));
    if (HW.m_pAdapter)
    {
        DXGI_ADAPTER_DESC1 description{};
        LARGE_INTEGER driver{};
        // The stable fields only: AdapterLuid changes on every boot.
        if (SUCCEEDED(HW.m_pAdapter->GetDesc1(&description)))
        {
            const u32 ids[] = {description.VendorId, description.DeviceId, description.SubSysId, description.Revision};
            const size_t name = wcsnlen(description.Description, std::size(description.Description));
            signature = crc32(ids, sizeof(ids), signature);
            signature = crc32(description.Description, u32(name * sizeof(wchar_t)), signature);
            signature = crc32(&description.DedicatedVideoMemory, sizeof(description.DedicatedVideoMemory), signature);
        }
        HW.m_pAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        signature = crc32(&driver, sizeof(driver), signature);
    }
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (length && length < MAX_PATH && wcscat_s(path, L"\\d3d12.dll") == 0 &&
        GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    {
        signature = crc32(&data.nFileSizeLow, sizeof(data.nFileSizeLow), signature);
        signature = crc32(&data.ftLastWriteTime, sizeof(data.ftLastWriteTime), signature);
    }
    return crc32(DAR_VERSION_STRING, sizeof(DAR_VERSION_STRING) - 1, signature);
}

void VerdictPath(string_path& path)
{
    FS.update_path(path, "$app_data_root$", "render-grass-mesh.cache");
}

bool VerdictRefused()
{
    static const bool refused = [] {
        string_path path;
        VerdictPath(path);
        IReader* reader = FS.r_open(path);
        if (!reader)
            return false;
        bool stored = reader->elapsed() == static_cast<intptr_t>(sizeof(u32) * 4);
        if (stored)
        {
            stored = reader->r_u32() == kVerdictMagic;
            stored = stored && reader->r_u32() == kVerdictVersion;
            stored = stored && reader->r_u32() == VerdictSignature();
            stored = stored && reader->r_u32() == 1;
        }
        FS.r_close(reader);
        return stored;
    }();
    return refused;
}

void VerdictStore()
{
    string_path path;
    VerdictPath(path);
    CMemoryWriter writer;
    writer.w_u32(kVerdictMagic);
    writer.w_u32(kVerdictVersion);
    writer.w_u32(VerdictSignature());
    writer.w_u32(1);
    xr_string temporary = path;
    temporary += ".tmp";
    if (writer.save_to(temporary.c_str()))
        FS.file_rename(temporary.c_str(), path, true);
}

// The level's grass vertex shaders still share their maths with the mesh shader: a replacement
// that brings its own wind keeps its look on the vertex path (a mechanism, no names).
bool DetailShadersShared()
{
    static const bool shared = [] {
        for (pcstr name : {"deffer_detail_w_flat.vs", "deffer_detail_s_flat.vs"})
        {
            string_path path;
            strconcat(path, RImplementation.getShaderPath(), name);
            FS.update_path(path, "$game_shaders$", path);
            IReader* file = FS.r_open(path);
            if (!file)
                return false;
            const xr_string text(static_cast<const char*>(file->pointer()), size_t(file->length()));
            FS.r_close(file);
            if (text.find("da_detail_common.h") == xr_string::npos)
                return false;
        }
        return true;
    }();
    return shared;
}

// mesh_ready of mesh.md 3.7 as far as it is known before a level (the pipeline's acceptance comes at
// the first dispatch).
bool Available(pcstr& reason)
{
    const CHWNativeCaps& caps = HW.native;
    const u32 model = caps.shaderModel && caps.dxcShaderModel ? std::min(caps.shaderModel, caps.dxcShaderModel) : 0;
    if (!caps.meshShaderTier)
        reason = "no mesh shaders on this device (or -dx12_no_ms)";
    else if (model < 0x65)
        reason = "shader model below 6.5";
    else if (!da_dxc_active())
        reason = "the engine shaders are not DXIL this session (r__dxc 0, -dx12_no_sm6 or no DXC)";
    else if (ps_r3_msaa)
        reason = "MSAA is on (its alpha-to-coverage passes exist only in the vertex path)";
    else if (!DetailShadersShared())
        reason = "the detail vertex shaders were replaced (no da_detail_common.h)";
    else if (VerdictRefused())
        reason = "a grass mesh pipeline was refused on this adapter before (render-grass-mesh.cache)";
    else if (s_refused.load(std::memory_order_acquire))
        reason = "a grass mesh pipeline was refused this session";
    else
        return true;
    return false;
}

// A shader element that can draw a mesh pass: one pass, mesh + amplification + DXIL pixel shader.
bool MeshElement(const ref_shader& shader, u32 element)
{
    if (!shader)
        return false;
    const ShaderElement* E = shader->E[element]._get();
    if (!E || E->passes.size() != 1)
        return false;
    const SPass& pass = *E->passes[0];
    return pass.vs && pass.vs->sh && pass.gs && pass.gs->sh && pass.ps && pass.ps->sh && pass.ps->dxil;
}

template <class T>
void SafeRelease(T*& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

} // namespace

// ================================================================================================
// State
// ================================================================================================

struct CDetailManager::MeshPath
{
    struct Model
    {
        u32 first_vertex{}, first_index{}, nv{}, nt{}, ipg{};
        // The farthest the bend carries a vertex, per unit of height scale (grass_dispatch.w).
        float margin{};
        bool wave{}, still{};             // elements 2 and 3 draw it
        bool wave_depth{}, still_depth{}; // elements 4 and 5, its shadow-pass variants
    };
    Model models[dm_max_objects];

    ID3D11Buffer* vertices{};
    ID3D11ShaderResourceView* vertices_srv{};
    ID3D11Buffer* indices{};
    ID3D11ShaderResourceView* indices_srv{};
    ID3D11Buffer* pool{};
    ID3D11ShaderResourceView* pool_srv{};
    ID3D11Buffer* list{};
    ID3D11ShaderResourceView* list_srv{};
    ID3D12Resource* ring{};
    u8* ring_cpu{};

    u32 pool_units{};
    u32 list_capacity{};
    UnitBuddy buddy;
    xr_vector<GpuRows*> live;

    // The frame's batch (calc task -> render thread).
    struct Upload
    {
        u32 dst_row, src_offset, rows;
    };
    xr_vector<Upload> uploads;
    xr_vector<u32> row_list;
    u32 ring_slot{};
    u32 list_offset{};
    bool batch_pending{};
    // Parts the vertex path draws this frame, per wave group and object.
    u16 hybrid[3][dm_max_objects]{};

    struct Stats
    {
        u32 parts{}, rows{}, upload_rows{}, uploads{}, hybrid_parts{}, evicted{}, resets{};
    };
    Stats stats, last_stats;
    std::atomic<u32> dispatches_gbuffer{0}, dispatches_shadow{0};
    u32 last_dispatches_gbuffer{}, last_dispatches_shadow{};
    bool ready{};

    // r__grass_stats' GPU check: the frame's pool rows and row list read back and compared with what
    // the calc task meant to upload.
    struct Check
    {
        enum : u32
        {
            Idle,
            Requested,
            Copied
        };
        u32 state{Idle};
        u32 frame{};
        ID3D11Buffer* pool_staging{};
        ID3D11Buffer* list_staging{};
        u32 list_entries{};
        struct Part
        {
            u32 list_first, count, first_row, rows_at;
        };
        xr_vector<Part> parts;
        xr_vector<CDetailManager::InstanceRows> rows;

        void Release()
        {
            SafeRelease(pool_staging);
            SafeRelease(list_staging);
            parts.clear();
            parts.shrink_to_fit();
            rows.clear();
            rows.shrink_to_fit();
            state = Idle;
        }
    } check;

    // r__dbg_grass_ab (mesh.md 3.8): the frame it runs in, the captures between a pass's begin and end
    // hooks (per context), and the ones waiting for their read-back.
    struct AbTarget
    {
        ID3D11Texture2D* source{};
        u32 subresource{};
        ID3D11Texture2D* scratch{};
        ID3D11Texture2D* a{};
        ID3D11Texture2D* b{};
        D3D11_TEXTURE2D_DESC desc{}; // one subresource
        bool depth{};
        u32 slot{};
    };
    struct AbJob
    {
        string64 name{};
        u32 frame{};
        xr_vector<AbTarget> targets;
    };
    u32 ab_frame{u32(-1)};
    AbJob* ab_open[R__NUM_CONTEXTS]{};
    bool ab_vertex[R__NUM_CONTEXTS]{};
    std::mutex ab_lock;
    xr_vector<AbJob*> ab_done;

    void ReleaseBlock(GpuRows& gpu)
    {
        if (gpu.order == kNotResident)
            return;
        buddy.Free(gpu.first_unit, gpu.order);
        GpuRows* last = live.back();
        live[gpu.live_index] = last;
        last->live_index = gpu.live_index;
        live.pop_back();
        gpu.order = kNotResident;
    }

    void ReleaseAll()
    {
        for (GpuRows* gpu : live)
            gpu->order = kNotResident;
        live.clear();
        buddy.Init(pool_units);
    }

    // Blocks of parts not visible this frame go first, the longest unseen first, until a quarter of
    // the pool is free.
    void Evict(u32 frame)
    {
        xr_vector<GpuRows*> candidates;
        for (GpuRows* gpu : live)
        {
            if (gpu->last_frame != frame)
                candidates.push_back(gpu);
        }
        std::sort(candidates.begin(), candidates.end(), [](const GpuRows* a, const GpuRows* b) { return a->last_frame < b->last_frame; });
        for (GpuRows* gpu : candidates)
        {
            if (buddy.FreeUnits() >= pool_units / 4)
                break;
            ReleaseBlock(*gpu);
            ++stats.evicted;
        }
    }

    bool Allocate(GpuRows& gpu, u32 order, u32 frame, bool& evicted)
    {
        u32 unit;
        if (!buddy.Alloc(order, unit))
        {
            if (evicted)
                return false;
            evicted = true;
            Evict(frame);
            if (!buddy.Alloc(order, unit))
                return false;
        }
        gpu.first_unit = unit;
        gpu.order = order;
        gpu.live_index = u32(live.size());
        live.push_back(&gpu);
        return true;
    }
};

namespace
{
// r__dbg_grass_ab, below.
void AbRelease(CDetailManager::MeshPath::AbJob* job);
void AbReport(const CDetailManager::MeshPath::AbJob& job);
// r__grass_stats' GPU check, below.
void CheckSnapshot(CDetailManager& dm, CBackend& cmd_list);
void CheckReport(CDetailManager& dm);
} // namespace

// ================================================================================================
// Level load
// ================================================================================================

void CDetailManager::ms_Load()
{
    ms_Unload();
    // [gpu] line: the grass passes' CPU time per frame (every context) and which path drew them.
    dx11GpuTimers::RegisterField("grass_cpu", [] {
        CDetailManager* details = RImplementation.Details;
        if (!details)
            return 0.f;
        const u64 ticks = details->m_render_ticks.exchange(0, std::memory_order_relaxed);
        const u32 frames = details->m_render_frames.exchange(0, std::memory_order_relaxed);
        return frames ? float(double(ticks) * 1000.0 / double(CPU::qpc_freq) / frames) : 0.f;
    });
    dx11GpuTimers::RegisterField("grass_mesh", [] {
        const CDetailManager* details = RImplementation.Details;
        return details && details->m_mesh_frame ? 1.f : 0.f;
    });
    pcstr reason = nullptr;
    if (!Available(reason))
    {
        Msg("* [grass-mesh] off: %s", reason);
        return;
    }
    using namespace detail_manager;

    MeshPath* m = xr_new<MeshPath>();
    xr_vector<DetailVertex> vertices;
    xr_vector<u16> indices;
    u32 ipg_min = kMaxIpg, ipg_max = 0, mesh_models = 0, vertex_models = 0, depth_models = 0;
    for (u32 id = 0; id < objects.size(); ++id)
    {
        const CDetail& D = *objects[id];
        MeshPath::Model& md = m->models[id];
        md.first_vertex = u32(vertices.size());
        md.first_index = u32(indices.size());
        md.nv = D.number_vertices;
        md.nt = D.number_indices / 3;
        md.ipg = md.nv && md.nt ? std::min({kMaxIpg, kMsMaxVerts / md.nv, kMsMaxPrims / md.nt}) : 0;
        // The numbers hw_Load_Geom gives the vertex path, bit for bit.
        const float height = D.bv_bb.vMax.y - D.bv_bb.vMin.y;
        for (u32 v = 0; v < D.number_vertices; ++v)
        {
            const Fvector& P = D.vertices[v].P;
            DetailVertex& out = vertices.emplace_back();
            out.pos[0] = P.x;
            out.pos[1] = P.y;
            out.pos[2] = P.z;
            out.uv = u32(u16(QC(D.vertices[v].u))) | (u32(u16(QC(D.vertices[v].v))) << 16);
            out.tmid = u32(u16(QC(P.y / height)));
        }
        indices.insert(indices.end(), D.indices, D.indices + D.number_indices);
        // A vertex above the root moves by at most 1.103 of its height (the 0.92 bend cap plus the
        // arc-length drop), one below it by at most 2.2.
        md.margin = std::max({0.f, 1.11f * D.bv_bb.vMax.y, -2.21f * D.bv_bb.vMin.y});
        md.wave = md.ipg && MeshElement(D.shader, 2);
        md.still = md.ipg && MeshElement(D.shader, 3);
        md.wave_depth = md.wave && MeshElement(D.shader, 4);
        md.still_depth = md.still && MeshElement(D.shader, 5);
        if (md.wave || md.still)
        {
            ++mesh_models;
            ipg_min = std::min(ipg_min, md.ipg);
            ipg_max = std::max(ipg_max, md.ipg);
        }
        if (!md.wave || !md.still)
            ++vertex_models;
        if (md.wave_depth && md.still_depth)
            ++depth_models;
    }
    if (!mesh_models)
    {
        Msg("* [grass-mesh] off: no detail model has a mesh pass (%u models)", u32(objects.size()));
        xr_delete(m);
        return;
    }
    if (indices.size() & 1)
        indices.push_back(0);

    // Pool: from the adapter's memory budget (mesh.md 3.2).
    UINT64 budget = 0, usage = 0;
    xrd3d12::QueryVideoMemory(HW.pDevice, budget, usage);
    constexpr UINT64 GB = 1ull << 30;
    const u32 pool_mb = budget && budget <= 4 * GB ? 24 : budget && budget <= 8 * GB ? 48 : 96;
    m->pool_units = pool_mb * (1u << 20) / (kUnitRows * kRowBytes);
    m->list_capacity = std::min(m->pool_units * kUnitRows, kMaxListEntries);
    m->buddy.Init(m->pool_units);

    ID3D11Device* device = HW.pDevice;
    bool ok = true;
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = UINT(vertices.size() * sizeof(DetailVertex));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(DetailVertex);
        const D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = UINT(vertices.size());
        ok = ok && SUCCEEDED(device->CreateBuffer(&desc, &data, &m->vertices)) &&
            SUCCEEDED(device->CreateShaderResourceView(m->vertices, &view, &m->vertices_srv));
    }
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = UINT(indices.size() * sizeof(u16));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        const D3D11_SUBRESOURCE_DATA data{indices.data(), 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = desc.ByteWidth / 4;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ok = ok && SUCCEEDED(device->CreateBuffer(&desc, &data, &m->indices)) &&
            SUCCEEDED(device->CreateShaderResourceView(m->indices, &view, &m->indices_srv));
    }
    const auto structured = [device](u32 elements, u32 stride, ID3D11Buffer*& buffer, ID3D11ShaderResourceView*& srv) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = elements * stride;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = stride;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = elements;
        return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)) &&
            SUCCEEDED(device->CreateShaderResourceView(buffer, &view, &srv));
    };
    ok = ok && structured(m->pool_units * kUnitRows, kRowBytes, m->pool, m->pool_srv);
    ok = ok && structured(m->list_capacity, sizeof(u32), m->list, m->list_srv);
    if (ok)
    {
        ok = SUCCEEDED(xrd3d12::CreateNativeBuffer(device, u64(kRingSlots) * kRingSlotBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"grass mesh upload ring", &m->ring));
        // Persistently mapped; the CPU never reads it.
        const D3D12_RANGE none{0, 0};
        void* cpu = nullptr;
        ok = ok && SUCCEEDED(m->ring->Map(0, &none, &cpu)) && cpu;
        m->ring_cpu = static_cast<u8*>(cpu);
    }
    m_mesh = m;
    if (!ok)
    {
        Msg("! [grass-mesh] off: the GPU buffers could not be created");
        ms_Unload();
        return;
    }

    m->ready = true;
    Msg("* [grass-mesh] on: %u objects (%u by the vertex path, %u with shadow variants), ipg %u..%u, pool %u MB (%u rows), "
        "row list %u, budget %llu MB",
        mesh_models, vertex_models, depth_models, ipg_min, ipg_max, pool_mb, m->pool_units * kUnitRows, m->list_capacity,
        static_cast<unsigned long long>(budget >> 20));
}

void CDetailManager::ms_Unload()
{
    MeshPath* m = m_mesh;
    if (!m)
        return;
    m_mesh = nullptr;
    m_mesh_frame = false;
    for (GpuRows* gpu : m->live)
        gpu->order = kNotResident;
    for (MeshPath::AbJob*& job : m->ab_open)
    {
        if (job)
            AbRelease(job);
        job = nullptr;
    }
    for (MeshPath::AbJob* job : m->ab_done)
        AbRelease(job);
    m->ab_done.clear();
    m->check.Release();
    SafeRelease(m->vertices_srv);
    SafeRelease(m->vertices);
    SafeRelease(m->indices_srv);
    SafeRelease(m->indices);
    SafeRelease(m->pool_srv);
    SafeRelease(m->pool);
    SafeRelease(m->list_srv);
    SafeRelease(m->list);
    if (m->ring)
    {
        if (m->ring_cpu)
            m->ring->Unmap(0, nullptr);
        // After the GPU finished the copies recorded so far.
        xrd3d12::ReleaseNative(HW.pDevice, m->ring);
        m->ring = nullptr;
    }
    xr_delete(m);
}

// ================================================================================================
// Per frame
// ================================================================================================

bool CDetailManager::ms_Latch()
{
    MeshPath* m = m_mesh;
    if (!m)
        return false;
    // Main thread: the refusal a pass met is logged and stored here.
    if (s_refused.load(std::memory_order_acquire))
    {
        if (!s_refusal_handled)
        {
            s_refusal_handled = true;
            Msg("! [grass-mesh] a grass mesh pipeline was refused: the vertex path for this session (verdict stored)");
            VerdictStore();
        }
        return false;
    }
    m->last_dispatches_gbuffer = m->dispatches_gbuffer.exchange(0, std::memory_order_relaxed);
    m->last_dispatches_shadow = m->dispatches_shadow.exchange(0, std::memory_order_relaxed);
    // Auto (-1) follows the offloads' decision of the previous frame's end (da_offload.h).
    const bool on = m->ready && da_offload_resolve(ps_r__grass_mesh) != 0;
    // r__dbg_grass_ab: one A/B frame, this one.
    if (ps_r__dbg_grass_ab)
    {
        ps_r__dbg_grass_ab = 0;
        if (on)
        {
            m->ab_frame = Device.dwFrame;
            Msg("* [grass-ab] frame %u: the G-buffer grass and sun cascades 0/1 through both paths", Device.dwFrame);
        }
        else
            Msg("! [grass-ab] needs the mesh path on (r__grass_mesh 1)");
    }
    return on;
}

void CDetailManager::ms_Release(SlotPart& part)
{
    if (MeshPath* m = m_mesh)
    {
        for (GpuRows& gpu : part.gpu)
            m->ReleaseBlock(gpu);
    }
}

void CDetailManager::ms_Calc()
{
    ZoneScoped;
    MeshPath* m = m_mesh;
    if (!m || !m->ready)
        return;
    const u32 frame = Device.dwFrame;
    // A batch the render thread never took (a frame drawn without the grass): the pool lacks its rows.
    if (m->batch_pending)
    {
        m->ReleaseAll();
        m->batch_pending = false;
        ++m->stats.resets;
    }
    m->last_stats = m->stats;
    m->stats = {};
    m->uploads.clear();
    m->row_list.clear();
    std::memset(m->hybrid, 0, sizeof(m->hybrid));

    // Every visible part counts as seen before anything is evicted, and the row list's room is known
    // before the rows fill the slot.
    u32 list_bound = 0;
    for (u32 group = 0; group < 3; ++group)
    {
        for (const u8 id : m_visibleObjectIds[group])
        {
            for (VisiblePart& part : m_visibles[group][id])
            {
                part.part->gpu[group].last_frame = frame;
                list_bound += u32(part.rows->size());
            }
        }
    }
    list_bound = std::min(list_bound, m->list_capacity);
    m->row_list.reserve(list_bound);
    const u32 list_bytes = (list_bound * u32(sizeof(u32)) + 255) & ~255u;
    const u32 row_budget = kRingSlotBytes - list_bytes;
    const u32 slot = frame % kRingSlots;
    u8* const base = m->ring_cpu + size_t(slot) * kRingSlotBytes;
    u32 row_used = 0;
    bool evicted = false;

    for (u32 group = 0; group < 3; ++group)
    {
        for (const u8 id : m_visibleObjectIds[group])
        {
            const MeshPath::Model& md = m->models[id];
            const bool mesh = group == 0 ? md.still : md.wave;
            for (VisiblePart& part : m_visibles[group][id])
            {
                part.index_first = part.index_count = 0;
                const u32 n = u32(part.rows->size());
                SlotPart& sp = *part.part;
                GpuRows& gpu = sp.gpu[group];
                if (!mesh || m->row_list.size() + n > list_bound)
                {
                    ++m->hybrid[group][id];
                    ++m->stats.hybrid_parts;
                    continue;
                }
                if (gpu.order == kNotResident || gpu.gen != sp.gen)
                {
                    const u32 order = OrderFor(n);
                    const u32 bytes = n * kRowBytes;
                    if (gpu.order != order)
                        m->ReleaseBlock(gpu);
                    if (row_used + bytes > row_budget || (gpu.order == kNotResident && !m->Allocate(gpu, order, frame, evicted)))
                    {
                        // No room this frame: the vertex path draws it, the next frame uploads it.
                        m->ReleaseBlock(gpu);
                        ++m->hybrid[group][id];
                        ++m->stats.hybrid_parts;
                        continue;
                    }
                    std::memcpy(base + row_used, part.rows->data(), bytes);
                    const u32 dst_row = gpu.first_unit * kUnitRows;
                    MeshPath::Upload* last = m->uploads.empty() ? nullptr : &m->uploads.back();
                    if (last && last->dst_row + last->rows == dst_row && last->src_offset + last->rows * kRowBytes == row_used)
                        last->rows += n;
                    else
                        m->uploads.push_back({dst_row, row_used, n});
                    row_used += bytes;
                    m->stats.upload_rows += n;
                    gpu.gen = sp.gen;
                }
                part.index_first = u32(m->row_list.size());
                part.index_count = n;
                const u32 first_row = gpu.first_unit * kUnitRows;
                for (u32 k = 0; k < n; ++k)
                    m->row_list.push_back(first_row + k);
                ++m->stats.parts;
                m->stats.rows += n;
            }
        }
    }

    m->list_offset = row_budget;
    if (!m->row_list.empty())
        std::memcpy(base + row_budget, m->row_list.data(), m->row_list.size() * sizeof(u32));
    m->stats.uploads = u32(m->uploads.size());
    m->ring_slot = slot;
    m->batch_pending = !m->uploads.empty() || !m->row_list.empty();
}

void CDetailManager::ms_FramePrepare(CBackend& cmd_list)
{
    MeshPath* m = m_mesh;
    if (!m)
        return;
    // A/B results a few frames old: read back and reported.
    {
        std::lock_guard lock(m->ab_lock);
        for (auto it = m->ab_done.begin(); it != m->ab_done.end();)
        {
            if (Device.dwFrame < (*it)->frame + 3)
            {
                ++it;
                continue;
            }
            AbReport(**it);
            AbRelease(*it);
            it = m->ab_done.erase(it);
        }
    }
    if (m->check.state == MeshPath::Check::Copied && Device.dwFrame >= m->check.frame + 3)
        CheckReport(*this);
    WaitForCalc();
    if (!m->batch_pending)
        return;
    ZoneScoped;
    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    xrd3d12::NativeScope scope;
    xrd3d12::NativeBuffer pool, list;
    if (!xrd3d12::NativeBufferRange(m->pool, pool) || !xrd3d12::NativeBufferRange(m->list, list) ||
        !xrd3d12::BeginNative(context, scope))
    {
        // Nothing reached the GPU: the next calc starts the pool over.
        return;
    }
    ID3D11Resource* targets[] = {m->pool, m->list};
    xrd3d12::RequireMany(context, targets, 2, D3D12_RESOURCE_STATE_COPY_DEST);
    const u64 ring_base = u64(m->ring_slot) * kRingSlotBytes;
    for (const MeshPath::Upload& upload : m->uploads)
    {
        scope.list->CopyBufferRegion(pool.resource, pool.offset + u64(upload.dst_row) * kRowBytes, m->ring,
            ring_base + upload.src_offset, u64(upload.rows) * kRowBytes);
    }
    if (!m->row_list.empty())
    {
        scope.list->CopyBufferRegion(list.resource, list.offset, m->ring, ring_base + m->list_offset,
            u64(m->row_list.size()) * sizeof(u32));
    }
    xrd3d12::RequireMany(context, targets, 2, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    xrd3d12::EndNative(context, 0);
    m->batch_pending = false;
    if (m->check.state == MeshPath::Check::Requested)
        CheckSnapshot(*this, cmd_list);
}

bool CDetailManager::ms_Resident(u32 /*object_id*/, const VisiblePart& part) const
{
    return m_mesh && part.index_count && !s_refused.load(std::memory_order_relaxed);
}

bool CDetailManager::ms_HasHybrid(u32 var_id, u32 object_id) const
{
    return !m_mesh || s_refused.load(std::memory_order_relaxed) || m_mesh->hybrid[var_id][object_id];
}

// ================================================================================================
// Passes
// ================================================================================================

namespace
{
// One dump of the mesh path; false when the layer refused the pipeline (nothing more drawn).
bool RenderMeshDump(CDetailManager& dm, CBackend& cmd_list, const CDetailManager::DumpInputs& in, const bool collectStats,
    const CFrustum* frustum)
{
    static shared_str strConsts("consts");
    static shared_str strWave("wave");
    static shared_str strDir2D("dir2D");
    static shared_str strSFade("grass_sfade");
    static shared_str strSFadeEye("grass_sfade_eye");
    static shared_str strGrassTint("grass_tint");
    static shared_str strWavePrev("wave_prev");
    static shared_str strDir2DPrev("dir2D_prev");
    static shared_str strWindPrevInfo("da_wm_info_prev");
    static shared_str strObj("grass_obj");
    static shared_str strDispatch("grass_dispatch");
    static shared_str strBound("grass_bound");
    static shared_str strPlanes("grass_planes");
    static shared_str strStockSeason("stock_season");
    static shared_str strStockSeasonPrev("stock_season_prev");
    static shared_str strStockObj("stock_obj");

    CDetailManager::MeshPath& m = *dm.m_mesh;
    const bool shadow_pass = static_cast<bool>(frustum);

    // The per-pass constants of hw_Render_dump, the same values.
    Fvector4 sfade;
    if (shadow_pass && ps_r__grass_shadow_fade > 0)
    {
        const float e = float(ps_r__grass_shadow_dist);
        sfade.set(_max(e - float(ps_r__grass_shadow_fade), 0.f), e, 0.f, 0.f);
    }
    else
        sfade.set(0.f, 0.f, 0.f, 0.f);
    const Fvector& ep = Device.vCameraPosition;
    Fvector4 eye;
    eye.set(ep.x, ep.y, ep.z, 0.f);
    Fvector4 tint;
    tint.set(ps_r__grass_tint, 1.f / _max(ps_r__grass_tint_scale, 0.1f), ps_r__grass_tint_base, 0.f);
    const float grass_shadow_dist_sq = float(ps_r__grass_shadow_dist) * float(ps_r__grass_shadow_dist);
    const bool prev_bend = !shadow_pass && (da_motion_vectors_active() || ps_r__motion_vectors);

    // The amplification shader culls against the volume the pass rasterizes (its WVP, world identity
    // here: jittered view, cascade map, light face), not the culling frustum: in the sun cascades that
    // is the narrower caster volume, and the vertex path still draws every tuft of a slot touching it.
    // Depth clip is always on, so near and far are valid too.
    CFrustum raster;
    raster.CreateFromMatrix(cmd_list.xforms.m_wvp, FRUSTUM_P_ALL);
    Fvector4 planes[kMaxPlanes];
    for (u32 i = 0; i < kMaxPlanes; ++i)
    {
        if (i < u32(raster.p_count))
            planes[i].set(raster.planes[i].n.x, raster.planes[i].n.y, raster.planes[i].n.z, raster.planes[i].d);
        else
            planes[i].set(0.f, 0.f, 0.f, -1e30f);
    }

    thread_local xr_vector<std::pair<u32, u32>> runs;
    const R_constant_table* resolved_table = nullptr;
    R_constant *cConsts = nullptr, *cWave = nullptr, *cDir2D = nullptr, *cSFade = nullptr, *cSFadeEye = nullptr,
               *cGrassTint = nullptr, *cWavePrev = nullptr, *cDir2DPrev = nullptr, *cWindPrevInfo = nullptr,
               *cObj = nullptr, *cDispatch = nullptr, *cBound = nullptr, *cPlanes = nullptr,
               *cStockSeason = nullptr, *cStockSeasonPrev = nullptr, *cStockObj = nullptr;
    bool bound_srvs = false;
    bool accepted = true;
    u32 dispatches = 0;
    CDetail* last_object = nullptr;

    for (const u8 objectId : dm.m_visibleObjectIds[in.var_id])
    {
        const CDetailManager::MeshPath::Model& md = m.models[objectId];
        // Shadow passes take the depth-only elements where they compiled (mesh.md 3.10): the same
        // tufts, sway and alpha test as 2/3, without the G-buffer outputs no shadow pass binds.
        u32 element;
        if (in.lod_id == 0)
        {
            if (!md.wave)
                continue;
            element = shadow_pass && md.wave_depth ? 4 : 2;
        }
        else
        {
            if (!md.still)
                continue;
            element = shadow_pass && md.still_depth ? 5 : 3;
        }
        // Runs of consecutive list entries over the parts this pass draws: the vertex path's own tests.
        runs.clear();
        u32 rows = 0;
        for (const CDetailManager::VisiblePart& part : dm.m_visibles[in.var_id][objectId])
        {
            if (!part.index_count)
                continue;
            if (shadow_pass && ep.distance_to_sqr(part.slot->vis.sphere.P) > grass_shadow_dist_sq)
                continue;
            if (!dm.IsPartVisible(part, frustum, cmd_list.context_id))
                continue;
            if (!runs.empty() && runs.back().first + runs.back().second == part.index_first)
                runs.back().second += part.index_count;
            else
                runs.emplace_back(part.index_first, part.index_count);
            rows += part.index_count;
        }
        if (runs.empty())
            continue;

        CDetail& Object = *dm.objects[objectId];
        last_object = &Object;
        cmd_list.set_Element(Object.shader->E[element], 0);
        cmd_list.apply_lmaterial();
        if (cmd_list.get_ctable() != resolved_table)
        {
            resolved_table = cmd_list.get_ctable();
            cConsts = cmd_list.get_c(strConsts)._get();
            cWave = cmd_list.get_c(strWave)._get();
            cDir2D = cmd_list.get_c(strDir2D)._get();
            cSFade = cmd_list.get_c(strSFade)._get();
            cSFadeEye = cmd_list.get_c(strSFadeEye)._get();
            cGrassTint = cmd_list.get_c(strGrassTint)._get();
            cWavePrev = cmd_list.get_c(strWavePrev)._get();
            cDir2DPrev = cmd_list.get_c(strDir2DPrev)._get();
            cWindPrevInfo = cmd_list.get_c(strWindPrevInfo)._get();
            cObj = cmd_list.get_c(strObj)._get();
            cDispatch = cmd_list.get_c(strDispatch)._get();
            cBound = cmd_list.get_c(strBound)._get();
            cPlanes = cmd_list.get_c(strPlanes)._get();
            cStockSeason = cmd_list.get_c(strStockSeason)._get();
            cStockSeasonPrev = cmd_list.get_c(strStockSeasonPrev)._get();
            cStockObj = cmd_list.get_c(strStockObj)._get();
        }
        // The season of the object's class: the amplification shader drops what it does not keep, the
        // mesh shader scales the height and buries under snow (da_detail_common.h).
        {
            Fvector4 season, season_prev, season_obj;
            dm.StockSeasonConstants(objectId, season, season_prev, season_obj);
            cmd_list.set_c(cStockSeason, season);
            cmd_list.set_c(cStockSeasonPrev, season_prev);
            cmd_list.set_c(cStockObj, season_obj);
        }
        cmd_list.set_c(cConsts, in.consts);
        cmd_list.set_c(cWave, in.wave);
        cmd_list.set_c(cDir2D, in.wind);
        cmd_list.set_c(cSFade, sfade);
        cmd_list.set_c(cSFadeEye, eye);
        cmd_list.set_c(cGrassTint, tint);
        cmd_list.set_c(cWavePrev, in.wave_prev);
        cmd_list.set_c(cDir2DPrev, in.wind_prev);
        if (cWindPrevInfo)
            cmd_list.set_c(cWindPrevInfo, da_wind_prev().wm_active, prev_bend ? 1.f : 0.f, 0.f, 0.f);
        cmd_list.set_c(cObj, float(md.first_vertex), float(md.first_index), float(md.nv + md.nt * 65536), float(md.ipg));
        cmd_list.set_c(cBound, Object.bv_sphere.P.x, Object.bv_sphere.P.y, Object.bv_sphere.P.z, Object.bv_sphere.R);
        if (cPlanes)
        {
            for (u32 i = 0; i < kMaxPlanes; ++i)
                cmd_list.set_ca(cPlanes, i, planes[i]);
        }
        if (!bound_srvs)
        {
            bound_srvs = true;
            cmd_list.SRVSManager.SetVSResource(0, m.vertices_srv);
            cmd_list.SRVSManager.SetVSResource(1, m.indices_srv);
            cmd_list.SRVSManager.SetVSResource(2, m.pool_srv);
            cmd_list.SRVSManager.SetGSResource(2, m.pool_srv);
            cmd_list.SRVSManager.SetGSResource(3, m.list_srv);
        }
        for (const auto& [first_entry, entry_count] : runs)
        {
            constexpr u32 max_entries = kMaxGroups * kGroupRows;
            for (u32 first = first_entry, left = entry_count; left && accepted;)
            {
                const u32 take = std::min(left, max_entries);
                cmd_list.set_c(cDispatch, float(first), float(take), 0.f, md.margin);
                accepted = cmd_list.RenderMesh((take + kGroupRows - 1) / kGroupRows, 1, 1);
                ++dispatches;
                first += take;
                left -= take;
            }
        }
        if (!accepted)
            break;
        if (collectStats)
            RImplementation.BasicStats.DetailCount += rows;
    }
    if (bound_srvs)
    {
        cmd_list.SRVSManager.SetVSResource(0, nullptr);
        cmd_list.SRVSManager.SetVSResource(1, nullptr);
        cmd_list.SRVSManager.SetVSResource(2, nullptr);
        cmd_list.SRVSManager.SetGSResource(2, nullptr);
        cmd_list.SRVSManager.SetGSResource(3, nullptr);
        // Now rather than at the next draw: the pool is written again next frame.
        cmd_list.SRVSManager.Apply(cmd_list.context_id);
    }
    // The amplification shader leaves the geometry slot through the vertex path's element of the same
    // object (the one its hybrid parts draw with): other backends on this context start from a null
    // cache and would never clear it, and every vertex pipeline after them would carry it and be refused.
    if (last_object)
        cmd_list.set_Element(last_object->shader->E[in.lod_id], 0);
    (shadow_pass ? m.dispatches_shadow : m.dispatches_gbuffer).fetch_add(dispatches, std::memory_order_relaxed);
    if (!accepted)
        s_refused.store(true, std::memory_order_release);
    return accepted;
}
} // namespace

void CDetailManager::ms_Render(CBackend& cmd_list, const bool collectStats, const CFrustum* frustum, const u32 dumps)
{
    ZoneScoped;
    // The vertex path's geometry, for the parts it draws in the same pass.
    cmd_list.set_Geometry(hw_Geom);
    for (u32 dump = 0; dump < dump_count; ++dump)
    {
        if (!(dumps & (1u << dump)))
            continue;
        const DumpInputs in = hw_DumpInputs(dump);
        // A refused pipeline: this dump (and the frame's rest) through the vertex path; a part the
        // mesh path drew before the refusal is drawn once more with the same result.
        const bool hybrid = !s_refused.load(std::memory_order_acquire) && RenderMeshDump(*this, cmd_list, in, collectStats, frustum);
        hw_Render_dump(cmd_list, in.consts, in.wave, in.wind, in.wave_prev, in.wind_prev, in.var_id, in.lod_id, collectStats,
            frustum, hybrid);
    }
}

void CDetailManager::ms_Stats()
{
    MeshPath* m = m_mesh;
    if (!m)
        return;
    // The calc task writes these counters, the pool and the backlog: a console command can land while it runs.
    WaitForCalc();
    const MeshPath::Stats& s = m->last_stats;
    const u32 used = m->pool_units - m->buddy.FreeUnits();
    Msg("* [grass-mesh] %s: %u parts, %u rows, %u parts by the vertex path; pool %u of %u units (%.1f of %.1f MB, %u blocks), "
        "uploads %u rows in %u copies, evicted %u, resets %u; dispatches g-buffer %u, shadows %u; decompress backlog %u slots",
        m_mesh_frame ? "on" : "off (r__grass_mesh 0 or refused)", s.parts, s.rows, s.hybrid_parts, used, m->pool_units,
        float(used) * kUnitRows * kRowBytes / float(1 << 20), float(m->pool_units) * kUnitRows * kRowBytes / float(1 << 20),
        u32(m->live.size()), s.upload_rows, s.uploads, s.evicted, s.resets, m->last_dispatches_gbuffer,
        m->last_dispatches_shadow, u32(cache_task.size()));
    // The GPU side of the same frame, a few frames later.
    if (m_mesh_frame && m->check.state == MeshPath::Check::Idle)
        m->check.state = MeshPath::Check::Requested;
}

// ================================================================================================
// r__grass_stats' GPU check: the pool rows and the row list as the GPU holds them
// ================================================================================================

namespace
{
// Right after the frame's copies: the parts' expected rows and a copy of both buffers.
void CheckSnapshot(CDetailManager& dm, CBackend& cmd_list)
{
    CDetailManager::MeshPath& m = *dm.m_mesh;
    CDetailManager::MeshPath::Check& c = m.check;
    c.parts.clear();
    c.rows.clear();
    for (u32 group = 0; group < 3; ++group)
    {
        for (const u8 id : dm.m_visibleObjectIds[group])
        {
            for (const CDetailManager::VisiblePart& part : dm.m_visibles[group][id])
            {
                if (!part.index_count)
                    continue;
                const CDetailManager::GpuRows& gpu = part.part->gpu[group];
                c.parts.push_back({part.index_first, part.index_count, gpu.first_unit * kUnitRows, u32(c.rows.size())});
                c.rows.insert(c.rows.end(), part.rows->begin(), part.rows->end());
            }
        }
    }
    c.list_entries = u32(m.row_list.size());
    ID3D11Device* device = HW.pDevice;
    const auto staging = [device](ID3D11Buffer* source, ID3D11Buffer*& copy) {
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &copy));
    };
    if (!staging(m.pool, c.pool_staging) || !staging(m.list, c.list_staging))
    {
        Msg("! [grass-mesh] row check: the read-back buffers could not be created");
        c.Release();
        return;
    }
    ID3D11DeviceContext* context = HW.get_context(cmd_list.context_id);
    context->CopyResource(c.pool_staging, m.pool);
    context->CopyResource(c.list_staging, m.list);
    c.frame = Device.dwFrame;
    c.state = CDetailManager::MeshPath::Check::Copied;
}

void CheckReport(CDetailManager& dm)
{
    CDetailManager::MeshPath& m = *dm.m_mesh;
    CDetailManager::MeshPath::Check& c = m.check;
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    D3D11_MAPPED_SUBRESOURCE pool{}, list{};
    const bool mapped = SUCCEEDED(context->Map(c.pool_staging, 0, D3D11_MAP_READ, 0, &pool)) && pool.pData &&
        SUCCEEDED(context->Map(c.list_staging, 0, D3D11_MAP_READ, 0, &list)) && list.pData;
    if (!mapped)
    {
        if (pool.pData)
            context->Unmap(c.pool_staging, 0);
        Msg("! [grass-mesh] row check: read-back failed");
        c.Release();
        return;
    }
    const auto* gpu_rows = static_cast<const CDetailManager::InstanceRows*>(pool.pData);
    const auto* gpu_list = static_cast<const u32*>(list.pData);
    const u32 pool_rows = m.pool_units * kUnitRows;
    u32 compared = 0, list_bad = 0, rows_bad = 0;
    string256 first_bad{};
    for (const CDetailManager::MeshPath::Check::Part& part : c.parts)
    {
        for (u32 k = 0; k < part.count; ++k)
        {
            ++compared;
            const u32 entry = part.list_first + k;
            const u32 row = entry < c.list_entries ? gpu_list[entry] : u32(-1);
            if (row != part.first_row + k)
            {
                if (!list_bad++ && !rows_bad)
                    xr_sprintf(first_bad, "list entry %u holds row %u, expected %u", entry, row, part.first_row + k);
                continue;
            }
            if (row >= pool_rows || std::memcmp(&gpu_rows[row], &c.rows[part.rows_at + k], kRowBytes) != 0)
            {
                if (!rows_bad++ && !list_bad)
                    xr_sprintf(first_bad, "pool row %u (list entry %u) differs from the part's row %u", row, entry, k);
            }
        }
    }
    context->Unmap(c.pool_staging, 0);
    context->Unmap(c.list_staging, 0);
    Msg("* [grass-mesh] row check (frame %u): %u parts, %u rows compared, %u list entries wrong, %u pool rows wrong%s%s -> %s",
        c.frame, u32(c.parts.size()), compared, list_bad, rows_bad, first_bad[0] ? "; first: " : "", first_bad,
        list_bad || rows_bad ? "FAIL" : "PASS");
    c.Release();
}
} // namespace

// ================================================================================================
// r__dbg_grass_ab (mesh.md 3.8): the same pass through both paths over the same state
// ================================================================================================

namespace
{
using AbTarget = CDetailManager::MeshPath::AbTarget;
using AbJob = CDetailManager::MeshPath::AbJob;

// Depth formats have no staging form: the copy goes to the typeless format of their group.
DXGI_FORMAT StagingFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return format;
    }
}

// The subresource a view covers (one mip of one slice); false when it covers several slices.
template <class View, class Desc>
bool ViewSubresource(View* view, u32 mips, u32 dim_2d, u32 dim_2d_array, u32& subresource)
{
    Desc desc{};
    view->GetDesc(&desc);
    u32 mip = 0, slice = 0;
    if (u32(desc.ViewDimension) == dim_2d)
        mip = desc.Texture2D.MipSlice;
    else if (u32(desc.ViewDimension) == dim_2d_array && desc.Texture2DArray.ArraySize == 1)
    {
        mip = desc.Texture2DArray.MipSlice;
        slice = desc.Texture2DArray.FirstArraySlice;
    }
    else
        return false;
    subresource = D3D11CalcSubresource(mip, slice, mips);
    return true;
}

// False with one log line naming the step that failed; the job goes on with its other targets.
bool AbCapture(ID3D11DeviceContext* context, ID3D11View* view, bool depth, u32 slot, pcstr name, AbTarget& t)
{
    const pcstr what = depth ? "depth" : "rt";
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource)
    {
        Msg("! [grass-ab] %s %s%u: the view has no resource", name, what, slot);
        return false;
    }
    resource->QueryInterface(IID_PPV_ARGS(&t.source));
    resource->Release();
    if (!t.source)
    {
        Msg("! [grass-ab] %s %s%u: not a 2D texture", name, what, slot);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    t.source->GetDesc(&desc);
    const bool covered = depth ?
        ViewSubresource<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(static_cast<ID3D11DepthStencilView*>(view),
            desc.MipLevels, D3D11_DSV_DIMENSION_TEXTURE2D, D3D11_DSV_DIMENSION_TEXTURE2DARRAY, t.subresource) :
        ViewSubresource<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(static_cast<ID3D11RenderTargetView*>(view),
            desc.MipLevels, D3D11_RTV_DIMENSION_TEXTURE2D, D3D11_RTV_DIMENSION_TEXTURE2DARRAY, t.subresource);
    if (!covered || desc.SampleDesc.Count != 1)
    {
        Msg("! [grass-ab] %s %s%u: a view of dimension/samples the tool does not copy (%u samples)", name, what, slot,
            desc.SampleDesc.Count);
        return false;
    }
    const u32 mip = t.subresource % desc.MipLevels;
    t.desc = desc;
    t.desc.Width = std::max(1u, desc.Width >> mip);
    t.desc.Height = std::max(1u, desc.Height >> mip);
    t.desc.MipLevels = 1;
    t.desc.ArraySize = 1;
    t.desc.MiscFlags = 0;
    t.desc.CPUAccessFlags = 0;
    t.desc.Usage = D3D11_USAGE_DEFAULT;
    t.desc.BindFlags = desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    t.depth = depth;
    t.slot = slot;
    D3D11_TEXTURE2D_DESC staging = t.desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.Format = StagingFormat(t.desc.Format);
    ID3D11Device* device = HW.pDevice;
    HRESULT hr = device->CreateTexture2D(&t.desc, nullptr, &t.scratch);
    pcstr step = "scratch copy";
    if (SUCCEEDED(hr))
    {
        step = "read-back copy";
        hr = device->CreateTexture2D(&staging, nullptr, &t.a);
        if (SUCCEEDED(hr))
            hr = device->CreateTexture2D(&staging, nullptr, &t.b);
    }
    if (FAILED(hr))
    {
        Msg("! [grass-ab] %s %s%u: its %s (format %u, %ux%u, bind 0x%x) could not be created: 0x%08x", name, what, slot, step,
            unsigned(step[0] == 's' ? t.desc.Format : staging.Format), t.desc.Width, t.desc.Height, t.desc.BindFlags,
            unsigned(hr));
        SafeRelease(t.source);
        SafeRelease(t.scratch);
        SafeRelease(t.a);
        SafeRelease(t.b);
        return false;
    }
    context->CopySubresourceRegion(t.scratch, 0, 0, 0, 0, t.source, t.subresource, nullptr);
    return true;
}

void AbRelease(AbJob* job)
{
    for (AbTarget& t : job->targets)
    {
        SafeRelease(t.source);
        SafeRelease(t.scratch);
        SafeRelease(t.a);
        SafeRelease(t.b);
    }
    xr_delete(job);
}

// Before the pass's grass: its targets are saved, the vertex path draws first.
void AbBegin(CDetailManager& dm, CBackend& cmd_list, pcstr name)
{
    CDetailManager::MeshPath& m = *dm.m_mesh;
    const u32 id = cmd_list.context_id;
    ID3D11DeviceContext* context = HW.get_context(id);
    AbJob* job = xr_new<AbJob>();
    xr_strcpy(job->name, name);
    job->frame = Device.dwFrame;
    const auto capture = [&](ID3D11View* view, bool depth, u32 slot) {
        AbTarget t;
        if (AbCapture(context, view, depth, slot, name, t))
            job->targets.push_back(t);
    };
    for (u32 slot = 0; slot < 4; ++slot)
    {
        if (ID3D11RenderTargetView* rtv = cmd_list.get_RT(slot))
            capture(rtv, false, slot);
    }
    if (ID3D11DepthStencilView* dsv = cmd_list.get_ZB())
        capture(dsv, true, 0);
    if (job->targets.empty())
    {
        Msg("! [grass-ab] %s: no target could be captured", name);
        AbRelease(job);
        return;
    }
    m.ab_open[id] = job;
    m.ab_vertex[id] = true;
}

// After the vertex path's grass: its result is kept, the saved state goes back, the mesh path draws,
// its result is kept too.
void AbEnd(CDetailManager& dm, CBackend& cmd_list, const CFrustum* frustum)
{
    CDetailManager::MeshPath& m = *dm.m_mesh;
    const u32 id = cmd_list.context_id;
    AbJob* job = m.ab_open[id];
    if (!job)
        return;
    m.ab_open[id] = nullptr;
    ID3D11DeviceContext* context = HW.get_context(id);
    for (AbTarget& t : job->targets)
    {
        context->CopySubresourceRegion(t.a, 0, 0, 0, 0, t.source, t.subresource, nullptr);
        context->CopySubresourceRegion(t.source, t.subresource, 0, 0, 0, t.scratch, 0, nullptr);
    }
    m.ab_vertex[id] = false;
    dm.Render(cmd_list, false, frustum);
    for (AbTarget& t : job->targets)
        context->CopySubresourceRegion(t.b, 0, 0, 0, 0, t.source, t.subresource, nullptr);
    std::lock_guard lock(m.ab_lock);
    m.ab_done.push_back(job);
}

float HalfToFloat(u16 h)
{
    const u32 sign = u32(h >> 15) << 31;
    u32 exponent = (h >> 10) & 0x1f;
    u32 mantissa = h & 0x3ff;
    u32 bits;
    if (!exponent)
    {
        if (!mantissa)
            bits = sign;
        else
        {
            // Subnormal: normalized.
            exponent = 113;
            while (!(mantissa & 0x400))
            {
                mantissa <<= 1;
                --exponent;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3ff) << 13);
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7f800000 | (mantissa << 13);
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct Mapped
{
    ID3D11Texture2D* texture{};
    u32 sub{};
    D3D11_MAPPED_SUBRESOURCE data{};

    bool Map(ID3D11DeviceContext* context, ID3D11Texture2D* t, u32 s)
    {
        texture = t;
        sub = s;
        return SUCCEEDED(context->Map(t, s, D3D11_MAP_READ, 0, &data)) && data.pData;
    }

    void Unmap(ID3D11DeviceContext* context)
    {
        if (data.pData)
            context->Unmap(texture, sub);
        data.pData = nullptr;
    }

    const u8* Row(u32 y) const { return static_cast<const u8*>(data.pData) + size_t(y) * data.RowPitch; }
};

// One colour texel pair: the largest difference and whether it is over the tolerance.
void CompareTexel(DXGI_FORMAT f, const u8* ta, const u8* tb, u32 texel, u32 width, u32 height, double& diff, bool& beyond)
{
    diff = 0.0;
    beyond = false;
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        for (u32 c = 0; c < 4; ++c)
            diff = std::max(diff, double(std::abs(int(ta[c]) - int(tb[c]))));
        beyond = diff > 2.0;
        return;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    {
        const u32 channels = f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 4 : f == DXGI_FORMAT_R16G16_FLOAT ? 2 : 1;
        for (u32 c = 0; c < channels; ++c)
        {
            u16 ha, hb;
            std::memcpy(&ha, ta + c * 2, 2);
            std::memcpy(&hb, tb + c * 2, 2);
            const double va = HalfToFloat(ha), vb = HalfToFloat(hb);
            const double d = std::abs(va - vb);
            if (f == DXGI_FORMAT_R16G16_FLOAT)
            {
                // Velocity: NDC, in pixels of the target.
                const double px = d * (c ? height : width) * 0.5;
                diff = std::max(diff, px);
                beyond = beyond || px > 0.05;
            }
            else
            {
                const double scale = std::max({std::abs(va), std::abs(vb), 1e-3});
                diff = std::max(diff, d / scale);
                beyond = beyond || d > 1e-3 * scale;
            }
        }
        return;
    }
    default:
        // Anything else: the bytes, exact.
        beyond = std::memcmp(ta, tb, texel) != 0;
        diff = beyond ? 1.0 : 0.0;
        return;
    }
}

u32 TexelBytes(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R16G16_FLOAT: return 4;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 4;
    }
}

// Compares one job's A and B and logs it (render thread, a few frames after the pass).
void AbReport(const AbJob& job)
{
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    const AbTarget* depth_target = nullptr;
    u32 width = 0, height = 0, colour_targets = 0;
    for (const AbTarget& t : job.targets)
    {
        width = std::max(width, t.desc.Width);
        height = std::max(height, t.desc.Height);
        if (t.depth)
            depth_target = &t;
        else
            ++colour_targets;
    }
    // Pixels over the tolerance in any colour target.
    xr_vector<u8> differs(size_t(width) * height, 0);
    const double pixels = std::max(1.0, double(width) * height);
    const bool gbuffer = colour_targets > 0;
    bool pass = true;

    for (const AbTarget& t : job.targets)
    {
        if (t.depth)
            continue;
        Mapped a, b;
        if (!a.Map(context, t.a, 0) || !b.Map(context, t.b, 0))
        {
            a.Unmap(context);
            b.Unmap(context);
            Msg("! [grass-ab] %s rt%u: read-back failed", job.name, t.slot);
            pass = false;
            continue;
        }
        const DXGI_FORMAT f = t.desc.Format;
        const u32 texel = TexelBytes(f);
        u32 over = 0, changed = 0;
        double max_diff = 0.0;
        for (u32 y = 0; y < t.desc.Height; ++y)
        {
            const u8* ra = a.Row(y);
            const u8* rb = b.Row(y);
            for (u32 x = 0; x < t.desc.Width; ++x)
            {
                double diff;
                bool beyond;
                CompareTexel(f, ra + x * texel, rb + x * texel, texel, t.desc.Width, t.desc.Height, diff, beyond);
                changed += diff > 0.0;
                max_diff = std::max(max_diff, diff);
                if (beyond)
                {
                    ++over;
                    differs[size_t(y) * width + x] = 1;
                }
            }
        }
        a.Unmap(context);
        b.Unmap(context);
        const pcstr unit = f == DXGI_FORMAT_R16G16_FLOAT ? " px (velocity)" :
            (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16_FLOAT) ? " rel" :
            texel == 4 ? "/255" : "";
        Msg("* [grass-ab] %s rt%u (format %u): over the tolerance %.4f%% (%u px), changed at all %.4f%%, max %.4f%s", job.name, t.slot,
            unsigned(f), 100.0 * over / pixels, over, 100.0 * changed / pixels, max_diff, unit);
    }

    // Depth and stencil: the layer maps a depth-stencil staging texture per plane (depth, then stencil).
    u32 depth_over = 0, depth_outside = 0, stencil_outside = 0;
    u64 grass_a = 0, grass_b = 0;
    if (depth_target)
    {
        const AbTarget& t = *depth_target;
        const DXGI_FORMAT f = t.desc.Format;
        const bool d24 = f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R24G8_TYPELESS;
        const bool d32 = f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
            f == DXGI_FORMAT_R32G8X24_TYPELESS;
        const bool stencil = d24 || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS;
        Mapped a, b, sa, sb;
        if ((d24 || d32) && a.Map(context, t.a, 0) && b.Map(context, t.b, 0))
        {
            const bool planes = stencil && sa.Map(context, t.a, 1) && sb.Map(context, t.b, 1);
            for (u32 y = 0; y < t.desc.Height; ++y)
            {
                const u8* ra = a.Row(y);
                const u8* rb = b.Row(y);
                for (u32 x = 0; x < t.desc.Width; ++x)
                {
                    u32 za, zb;
                    std::memcpy(&za, ra + x * 4, 4);
                    std::memcpy(&zb, rb + x * 4, 4);
                    double dz;
                    if (d24)
                        dz = std::abs(double(za & 0xffffff) - double(zb & 0xffffff)) / 16777215.0;
                    else
                    {
                        float fa, fb;
                        std::memcpy(&fa, &za, 4);
                        std::memcpy(&fb, &zb, 4);
                        dz = std::abs(double(fa) - double(fb));
                    }
                    const bool outside = gbuffer && !differs[size_t(y) * width + x];
                    if (dz > 1e-6)
                    {
                        ++depth_over;
                        depth_outside += outside;
                    }
                    if (planes)
                    {
                        const u8 s0 = sa.Row(y)[x], s1 = sb.Row(y)[x];
                        grass_a += s0 & 1;
                        grass_b += s1 & 1;
                        stencil_outside += s0 != s1 && outside;
                    }
                }
            }
            sa.Unmap(context);
            sb.Unmap(context);
        }
        else
        {
            Msg("! [grass-ab] %s depth (format %u): read-back failed", job.name, unsigned(f));
            pass = false;
        }
        a.Unmap(context);
        b.Unmap(context);
    }

    if (gbuffer)
    {
        u32 union_px = 0;
        for (const u8 d : differs)
            union_px += d;
        const double share = 100.0 * union_px / pixels;
        const double grass_delta = grass_a ? 100.0 * std::abs(double(grass_a) - double(grass_b)) / double(grass_a) : 0.0;
        pass = pass && share <= 0.10 && !depth_outside && !stencil_outside && grass_delta <= 0.1;
        Msg("* [grass-ab] %s: pixels over the tolerance in any target %.4f%% (limit 0.10%%), depth diffs outside them %u (of %u)%s, "
            "stencil diffs outside them %u, grass px %llu vs %llu (%.3f%%, limit 0.1%%) -> %s",
            job.name, share, depth_outside, depth_over, depth_target ? "" : " [depth not captured]", stencil_outside,
            static_cast<unsigned long long>(grass_a), static_cast<unsigned long long>(grass_b), grass_delta, pass ? "PASS" : "FAIL");
    }
    else
    {
        const double share = 100.0 * depth_over / pixels;
        pass = pass && share <= 0.05;
        Msg("* [grass-ab] %s: texels with a depth diff > 1e-6 %.4f%% (%u of %ux%u, limit 0.05%%) -> %s", job.name, share, depth_over,
            width, height, pass ? "PASS" : "FAIL");
    }
}
} // namespace

bool CDetailManager::ms_ForceVertex(u32 context_id) const
{
    return m_mesh && m_mesh->ab_vertex[context_id];
}

// ================================================================================================
// Frame hooks (da_grass_mesh.h)
// ================================================================================================

// The preset's grass rung follows the path r__grass_mesh selects when the preset replays (auto: the
// offloads' decision then); a later runtime switch keeps that rung (equal-setting A/B).
bool da_grass_mesh_expected()
{
    pcstr reason = nullptr;
    return da_offload_resolve(ps_r__grass_mesh) != 0 && Available(reason);
}

// Not gated by r__grass_mesh: the elements must exist for a runtime switch.
bool da_grass_mesh_blender()
{
    pcstr reason = nullptr;
    return Available(reason);
}

bool da_grass_mesh_frame()
{
    return RImplementation.Details && RImplementation.Details->m_mesh_frame;
}

void da_grass_mesh_frame_prepare(CBackend& cmd_list)
{
    if (CDetailManager* details = RImplementation.Details; details && details->m_mesh_frame)
        details->ms_FramePrepare(cmd_list);
}

namespace
{
// The A/B runs in this pass: mesh grass on, r__dbg_grass_ab asked for this frame.
CDetailManager* AbDetails()
{
    CDetailManager* details = RImplementation.Details;
    return details && details->m_mesh && details->m_mesh_frame && details->m_mesh->ab_frame == Device.dwFrame ? details : nullptr;
}
} // namespace

void da_grass_ab_gbuffer_begin()
{
    if (CDetailManager* details = AbDetails())
        AbBegin(*details, RCache, "gbuffer");
}

void da_grass_ab_gbuffer_end()
{
    if (CDetailManager* details = AbDetails())
        AbEnd(*details, RCache, nullptr);
}

void da_grass_ab_cascade_begin(u32 cascade, CBackend& cmd_list)
{
    if (CDetailManager* details = AbDetails(); details && cascade < 2)
    {
        string32 name;
        xr_sprintf(name, "cascade %u", cascade);
        AbBegin(*details, cmd_list, name);
    }
}

void da_grass_ab_cascade_end(u32 cascade, CBackend& cmd_list)
{
    if (CDetailManager* details = AbDetails(); details && cascade < 2)
        AbEnd(*details, cmd_list, &RImplementation.get_context(cmd_list.context_id).o.view_frustum);
}

void da_grass_mesh_stats()
{
    if (CDetailManager* details = RImplementation.Details; details && details->m_mesh)
    {
        details->ms_Stats();
        return;
    }
    pcstr reason = nullptr;
    if (!Available(reason))
        Msg("* [grass-mesh] off: %s", reason);
    else
        Msg("* [grass-mesh] no level grass loaded");
}
#else
// ================================================================================================
// DirectX 11: the vertex path only
// ================================================================================================

struct CDetailManager::MeshPath
{
};

void CDetailManager::ms_Load() {}
void CDetailManager::ms_Unload() {}
bool CDetailManager::ms_Latch() { return false; }
void CDetailManager::ms_Calc() {}
void CDetailManager::ms_Release(SlotPart& /*part*/) {}
void CDetailManager::ms_FramePrepare(CBackend& /*cmd_list*/) {}
void CDetailManager::ms_Render(CBackend& cmd_list, bool collectStats, const CFrustum* frustum, u32 dumps)
{
    hw_Render(cmd_list, collectStats, frustum, dumps);
}
bool CDetailManager::ms_Resident(u32 /*object_id*/, const VisiblePart& /*part*/) const { return false; }
bool CDetailManager::ms_ForceVertex(u32 /*context_id*/) const { return false; }
bool CDetailManager::ms_HasHybrid(u32 /*var_id*/, u32 /*object_id*/) const { return true; }
void CDetailManager::ms_Stats() {}

bool da_grass_mesh_expected() { return false; }

bool da_grass_mesh_blender() { return false; }

bool da_grass_mesh_frame() { return false; }

void da_grass_mesh_frame_prepare(CBackend& /*cmd_list*/) {}

void da_grass_ab_gbuffer_begin() {}

void da_grass_ab_gbuffer_end() {}

void da_grass_ab_cascade_begin(u32 /*cascade*/, CBackend& /*cmd_list*/) {}

void da_grass_ab_cascade_end(u32 /*cascade*/, CBackend& /*cmd_list*/) {}

void da_grass_mesh_stats()
{
    Msg("* [grass-mesh] off: DirectX 12 only");
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
