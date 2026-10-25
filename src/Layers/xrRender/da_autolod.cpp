// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The automatic levels of detail (da_autolod.h).
#include "stdafx.h"
#include "da_autolod.h"
#include "da_mesh_simplify.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace xray::render::RENDER_NAMESPACE
{
// Raise when the reducer's output or the cache's layout changes.
constexpr u32 kAutoLodVersion = 1;
constexpr u32 kMaxLods = 4;

struct DaAutoLod
{
    std::atomic<int> refs{1};
    std::atomic<int> state{0}; // 0 queued, 1 built, 2 on the GPU, -1 nothing to add
    // The source.
    xr_vector<float> pos, attr;
    xr_vector<u16> bones, idx;
    u32 verts{};
    xr_vector<VertexElement> decl;
    ID3D11Buffer* vb{};
    float radius{};
    u64 hash{};
    // The chain: every level's triangles one after the other, each level's first index, triangles and screen size.
    xr_vector<u16> indices;
    u32 first[kMaxLods]{}, tris[kMaxLods]{};
    float size[kMaxLods]{};
    float deviation[kMaxLods]{};
    u32 lods{1};
    bool from_cache{};
    ID3D11Buffer* ib{};
    ref_geom geom;

    ~DaAutoLod()
    {
        geom.destroy();
        _RELEASE(ib);
        _RELEASE(vb);
    }
};

namespace
{
// The reducer's attribute weights (the normal, the tangent and the binormal per component; the texture set's per its
// range) and constants: the border line, the degree penalty, positions in centimetres.
constexpr float kNormalWeight = 16.f, kTangentWeight = 0.1f, kUvWeight = 0.5f;
constexpr float kEdgeWeight = 512.f, kDegreePenalty = 100.f, kPositionScale = 100.f;
// The levels: a half of the full one each step, the reducer's floor of 64 triangles; the size of the reference view's
// half width that a level's deviation may cover, and the crossfade's time.
constexpr u32 kFloorTris = 64;
constexpr float kFadeTime = 0.5f;

struct State
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<DaAutoLod*> queue;
    xr_vector<std::thread> workers;
    bool stop{};
    std::mutex done_mutex;
    xr_vector<DaAutoLod*> done;
    std::atomic<u32> built{0}, cached{0}, uploaded{0}, skipped{0};
    std::atomic<u64> tris_in{0}, tris_lods{0};
};
State g;

u64 fnv64(const void* data, size_t bytes, u64 h = 0xcbf29ce484222325ull)
{
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < bytes; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void cache_path(string_path& out, u64 hash)
{
    string256 name;
    xr_sprintf(name, "cache" DELIMITER "autolod" DELIMITER "%016llx_%u.bin", (unsigned long long)hash, kAutoLodVersion);
    FS.update_path(out, "$app_data_root$", name);
}

struct CacheHeader
{
    u32 magic{0x4C414144u}; // "DAAL"
    u32 version{kAutoLodVersion};
    u64 hash{};
    u32 lods{}, indices{};
    u32 first[kMaxLods]{}, tris[kMaxLods]{};
    float deviation[kMaxLods]{};
};

bool cache_read(DaAutoLod& L)
{
    string_path path;
    cache_path(path, L.hash);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f)
        return false;
    CacheHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == CacheHeader{}.magic && h.version == kAutoLodVersion &&
        h.hash == L.hash && h.lods >= 1 && h.lods <= kMaxLods && h.indices < (1u << 26);
    if (ok)
    {
        L.indices.resize(h.indices);
        ok = fread(L.indices.data(), 2, L.indices.size(), f) == L.indices.size();
        L.lods = h.lods;
        for (u32 i = 0; i < kMaxLods; ++i)
        {
            L.first[i] = h.first[i];
            L.tris[i] = h.tris[i];
            L.deviation[i] = h.deviation[i];
            ok = ok && size_t(h.first[i]) + size_t(h.tris[i]) * 3 <= L.indices.size();
        }
        for (u16 i : L.indices)
            ok = ok && i < L.verts;
    }
    fclose(f);
    return ok;
}

void cache_write(const DaAutoLod& L)
{
    string_path dir, path, tmp;
    FS.update_path(dir, "$app_data_root$", "cache" DELIMITER "autolod" DELIMITER);
    VerifyPath(dir);
    cache_path(path, L.hash);
    xr_sprintf(tmp, "%s.%u.tmp", path, GetCurrentThreadId());
    CacheHeader h;
    h.hash = L.hash;
    h.lods = L.lods;
    h.indices = u32(L.indices.size());
    for (u32 i = 0; i < kMaxLods; ++i)
    {
        h.first[i] = L.first[i];
        h.tris[i] = L.tris[i];
        h.deviation[i] = L.deviation[i];
    }
    FILE* f = nullptr;
    if (fopen_s(&f, tmp, "wb") || !f)
        return;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(L.indices.data(), 2, L.indices.size(), f) == L.indices.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

// The chain: each level from the full one at half the last one's target, until a level stops gaining.
void build(DaAutoLod& L)
{
    const u32 tris0 = u32(L.idx.size() / 3);
    L.indices.assign(L.idx.begin(), L.idx.end());
    L.first[0] = 0;
    L.tris[0] = tris0;
    L.lods = 1;
    if (cache_read(L))
    {
        L.from_cache = true;
        ++g.cached;
        return;
    }
    // The weights: the texture set's by its range (0.5 over the larger of its extents and 1).
    constexpr u32 na = 11;
    float lo[2] = {3.4e38f, 3.4e38f}, hi[2] = {-3.4e38f, -3.4e38f};
    for (u32 v = 0; v < L.verts; ++v)
        for (u32 k = 0; k < 2; ++k)
        {
            lo[k] = std::min(lo[k], L.attr[size_t(v) * na + 9 + k]);
            hi[k] = std::max(hi[k], L.attr[size_t(v) * na + 9 + k]);
        }
    const float uv_range = std::max({hi[0] - lo[0], hi[1] - lo[1], 0.f});
    const float uvw = kUvWeight / std::max(1.f, uv_range);
    const float weights[na] = {kNormalWeight, kNormalWeight, kNormalWeight, kTangentWeight, kTangentWeight, kTangentWeight,
        kTangentWeight, kTangentWeight, kTangentWeight, uvw, uvw};
    // The bones' regions as seams: a triangle by its first corner's strongest bone.
    xr_vector<u32> tri_ids(tris0);
    for (u32 t = 0; t < tris0; ++t)
        tri_ids[t] = L.bones[L.idx[t * 3]];
    da_mesh::SimplifyInput in;
    in.positions = L.pos.data();
    in.attributes = L.attr.data();
    in.vertex_count = L.verts;
    in.attr_count = na;
    in.attr_weights = weights;
    in.tri_ids = tri_ids.data();
    da_mesh::SimplifyOptions opt;
    opt.edge_weight = kEdgeWeight;
    opt.degree_penalty = kDegreePenalty;
    opt.limit_error_to_area = false;
    opt.position_scale = kPositionScale;
    u32 last = tris0;
    for (u32 level = 1; level < kMaxLods; ++level)
    {
        const u32 target = std::max(kFloorTris, tris0 >> level);
        if (target >= last)
            break;
        std::vector<u32> idx(L.idx.begin(), L.idx.end());
        opt.target_tris = target;
        const da_mesh::SimplifyResult r = da_mesh::simplify(in, idx, opt);
        // A level that saves little more than the last is not worth a switch.
        if (!r.tris || r.tris * 10 > last * 9)
            break;
        L.first[level] = u32(L.indices.size());
        L.tris[level] = r.tris;
        L.deviation[level] = r.deviation;
        for (u32 i : idx)
            L.indices.push_back(u16(i));
        L.lods = level + 1;
        last = r.tris;
    }
    cache_write(L);
}

void worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;)
    {
        DaAutoLod* L = nullptr;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            g.wake.wait(lock, [] { return g.stop || !g.queue.empty(); });
            if (g.stop)
                return;
            L = g.queue.front();
            g.queue.pop_front();
        }
        // A chain whose models are all gone is not worth building.
        if (L->refs.load() > 1)
        {
            build(*L);
            ++g.built;
            L->state = 1;
            std::lock_guard<std::mutex> lock(g.done_mutex);
            g.done.push_back(L);
            continue;
        }
        da_autolod_release(L);
    }
}

void start_workers()
{
    if (!g.workers.empty())
        return;
    const u32 hw = std::thread::hardware_concurrency();
    const u32 n = std::clamp(hw > 4 ? hw / 2 : 1u, 1u, 6u);
    g.stop = false;
    for (u32 i = 0; i < n; ++i)
        g.workers.emplace_back(worker);
}
} // namespace

DaAutoLod* da_autolod_request(const DaAutoLodSource& s)
{
    if (!ps_r__autolod || !s.positions || !s.attributes || !s.bones || !s.indices || !s.geom || !s.geom->dcl ||
        s.index_count / 3 < u32(ps_r__autolod_min_tris) || s.vertex_count == 0)
    {
        ++g.skipped;
        return nullptr;
    }
    DaAutoLod* L = xr_new<DaAutoLod>();
    L->verts = s.vertex_count;
    L->pos.assign(s.positions, s.positions + size_t(s.vertex_count) * 3);
    L->attr.assign(s.attributes, s.attributes + size_t(s.vertex_count) * 11);
    L->bones.assign(s.bones, s.bones + s.vertex_count);
    L->idx.assign(s.indices, s.indices + s.index_count);
    L->decl = s.geom->dcl->dcl_code;
    L->vb = s.geom->vb;
    L->vb->AddRef();
    L->radius = s.radius;
    u64 h = fnv64(&kAutoLodVersion, 4);
    h = fnv64(L->pos.data(), L->pos.size() * 4, h);
    h = fnv64(L->idx.data(), L->idx.size() * 2, h);
    h = fnv64(L->bones.data(), L->bones.size() * 2, h);
    L->hash = h;
    g.tris_in += s.index_count / 3;
    start_workers();
    // The queue's own reference.
    L->refs.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.queue.push_back(L);
    }
    g.wake.notify_one();
    return L;
}

void da_autolod_addref(DaAutoLod* L)
{
    if (L)
        L->refs.fetch_add(1);
}

void da_autolod_release(DaAutoLod* L)
{
    if (L && L->refs.fetch_sub(1) == 1)
        xr_delete(L);
}

void da_autolod_frame_begin()
{
    xr_vector<DaAutoLod*> done;
    {
        std::lock_guard<std::mutex> lock(g.done_mutex);
        done.swap(g.done);
    }
    for (DaAutoLod* L : done)
    {
        // The levels' indices as the chain's own index buffer, drawn over the model's vertex buffer.
        if (L->lods > 1 && L->refs.load() > 1)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = u32(L->indices.size() * 2 + 3) & ~3u;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            xr_vector<u16> padded = L->indices;
            padded.resize(desc.ByteWidth / 2, 0);
            D3D11_SUBRESOURCE_DATA data{padded.data(), 0, 0};
            if (SUCCEEDED(HW.pDevice->CreateBuffer(&desc, &data, &L->ib)))
            {
                L->decl.push_back(D3DDECL_END());
                L->geom.create(L->decl.data(), L->vb, L->ib);
                // The screen sizes: the full level 2, each next where its deviation is the pixel error's share of the
                // reference view's half width (960 pixels), never above half the last one's.
                L->size[0] = 2.f;
                const float px = std::max(ps_r__autolod_px, 0.1f);
                for (u32 i = 1; i < L->lods; ++i)
                {
                    const float distance = L->deviation[i] * 960.f / px;
                    float s = 1.7778f * L->radius / std::max(0.01f, distance + L->radius);
                    if (s > L->size[i - 1])
                        s = L->size[i - 1] * 0.5f;
                    L->size[i] = s;
                }
                for (u32 i = 1; i < L->lods; ++i)
                    g.tris_lods += L->tris[i];
                L->state = 2;
                ++g.uploaded;
            }
            else
                L->state = -1;
        }
        else
            L->state = -1;
        // The source data is no longer needed.
        L->pos = {};
        L->attr = {};
        L->bones = {};
        L->idx = {};
        da_autolod_release(L);
    }
}

bool da_autolod_choose(DaAutoLod* L, CBackend& cmd, const Fsphere& sphere, DaAutoLodState& st, DaAutoLodDraw& out)
{
    if (!L || L->state.load() != 2 || !ps_r__autolod)
        return false;
    const Fmatrix& W = cmd.xforms.m_w;
    // The main view's passes by their projection (the camera's, its jitter aside); a shadow map's or the first-person
    // view's (its own near plane) takes the current level.
    const Fmatrix& P = cmd.xforms.m_p;
    const auto near_eq = [](float a, float b) { return std::abs(a - b) <= 1e-4f * std::max(std::abs(b), 1e-6f); };
    const bool main = near_eq(P._11, Device.mProject._11) && near_eq(P._22, Device.mProject._22) &&
        near_eq(P._43, Device.mProject._43) && P._34 != 0.f;
    if (main)
    {
        // The screen size: the bounding sphere's diameter over the view's smaller extent (the larger projection term).
        Fvector c;
        W.transform_tiny(c, sphere.P);
        const float scale = std::sqrt(std::max({W.i.square_magnitude(), W.j.square_magnitude(), W.k.square_magnitude()}));
        const float r = sphere.R * scale;
        const float multiple = std::max(0.5f * Device.mProject._11, 0.5f * Device.mProject._22);
        const float size =
            2.f * multiple * r / std::max(0.01f, Device.vCameraPosition.distance_to(c)) * std::max(ps_r__autolod_scale, 0.01f);
        // The coarsest level whose size the model's still reaches; a coarser one only a twentieth under its size (the
        // value filter's dead band against a model standing on a threshold).
        u32 want = 0;
        for (u32 i = L->lods; i-- > 1;)
        {
            const float limit = i > st.current ? L->size[i] * 0.95f : L->size[i];
            if (size <= limit)
            {
                want = i;
                break;
            }
        }
        const float now = Device.fTimeGlobal;
        if (st.since < 0.f)
        {
            st.current = st.previous = u8(want);
            st.since = now - kFadeTime;
        }
        else if (want != st.current)
        {
            st.previous = st.current;
            st.current = u8(want);
            st.since = now;
        }
        out.t = std::clamp((now - st.since) / kFadeTime, 0.f, 1.f);
    }
    else
        out.t = 1.f;
    const u32 cur = std::min<u32>(st.current, L->lods - 1), prev = std::min<u32>(st.previous, L->lods - 1);
    out.geom = &L->geom;
    out.first[0] = L->first[cur];
    out.tris[0] = L->tris[cur];
    out.count = 1;
    if (main && out.t < 1.f && prev != cur)
    {
        out.first[1] = L->first[prev];
        out.tris[1] = L->tris[prev];
        out.count = 2;
    }
    return true;
}

void da_autolod_stats()
{
    Msg("* [autolod] %u chains built (%u from the disk cache), %u on the GPU, %u meshes left whole; %llu triangles in, "
        "%llu in their coarser levels",
        u32(g.built), u32(g.cached), u32(g.uploaded), u32(g.skipped), (unsigned long long)g.tris_in.load(),
        (unsigned long long)g.tris_lods.load());
}

void da_autolod_destroy()
{
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.stop = true;
    }
    g.wake.notify_all();
    for (std::thread& t : g.workers)
        t.join();
    g.workers.clear();
    for (DaAutoLod* L : g.queue)
        da_autolod_release(L);
    g.queue.clear();
    std::lock_guard<std::mutex> lock(g.done_mutex);
    for (DaAutoLod* L : g.done)
        da_autolod_release(L);
    g.done.clear();
}
} // namespace xray::render::RENDER_NAMESPACE
