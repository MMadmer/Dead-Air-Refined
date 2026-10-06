// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The level's collision mesh on the GPU and the near ground window (docs/dead-air/SNOW.md, da_ground.h).
//
// Upload, once per level: every CDB triangle as (i0, i1, i2, flags) and every vertex, for the grass and
// for the window below, plus a CPU index of the triangles that can carry snow by 16 m cells.
// Window: N x N texels around the camera, toroidal (a texel is the world texel modulo N), moved in steps
// of 16 texels. A strip the camera moves into is cleared, rasterised from the triangles of its cells
// (da_ground_raster.cs: conservative, the topmost surface wins) and resolved together with an apron
// (da_ground_resolve.cs): Htop, the opened Hg the snow lies on, the cliff taper of the snow grid and the
// continuous quantities of the waterline (the bed under the water, the level of the water within reach).

#include "stdafx.h"
#include "da_ground.h"
#include "da_snow.h"
#include "da_snow_gpu.h"

#include "xrMaterialSystem/GameMtlLib.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using namespace da_snow_gpu;

constexpr u32 k_flag_passable = 1u << 16;
constexpr u32 k_flag_water = 1u << 17;
constexpr u32 k_flag_road = 1u << 18;
constexpr float k_cell = 16.f; // metres per cell of the triangle index
constexpr int k_step = 16;     // window texels per move
constexpr int k_chunk = 32;    // texels per side of a raster work item (da_ground_raster.cs)
constexpr int k_apron = 8;     // texels a raster change reaches through the resolve (DA_GR_AP, da_ground_resolve.cs)
constexpr int k_edge = 8;      // texels at the window's edge whose neighbourhood is cut off
constexpr float k_band = 4.f;  // metres over which the snow shaders blend into the level base
constexpr float k_rail_cell = 4.f;

struct WindowSize
{
    u32 n;
    float texel;
};
// Presets 0..4: 512 @ 0.5 m, 512 @ 0.25 m, 512 @ 0.25 m, 1024 @ 0.25 m, 1024 @ 0.25 m.
constexpr WindowSize k_sizes[5] = {{512, 0.5f}, {512, 0.25f}, {512, 0.25f}, {1024, 0.25f}, {1024, 0.25f}};

constexpr pcstr k_name_raw_solid = "$user$da_ground_raw_solid";
constexpr pcstr k_name_raw_water = "$user$da_ground_raw_water";
constexpr pcstr k_name_top = "$user$da_ground_top";
constexpr pcstr k_name_base = "$user$da_ground_base";
constexpr pcstr k_name_shore = "$user$da_ground_shore";

struct Ground
{
    // The collision mesh: GPU copies, and the level's own arrays the index points into.
    ID3D11Buffer* verts = nullptr;
    ID3D11ShaderResourceView* verts_srv = nullptr;
    ID3D11Buffer* tris = nullptr;
    ID3D11ShaderResourceView* tris_srv = nullptr;
    u32 tri_count = 0;
    Fbox bounds{};
    const Fvector* cpu_verts = nullptr;
    const CDB::TRI* cpu_tris = nullptr;
    // Triangles that can carry snow by 16 m cells (a triangle is listed in every cell it overlaps).
    u32 cells_x = 0, cells_z = 0;
    xr_vector<u32> cell_first;
    xr_vector<u32> cell_tris;
    xr_vector<u32> tri_mark;
    u32 mark = 0;

    // Raster work items of one strip: (triangle, chunk origin in the strip).
    Buffer work;
    xr_vector<u32> work_cpu;

    u32 preset = 4;
    u32 n = 0;
    float texel = 0.f;
    Texture raw_solid, raw_water, top, base, shore;
    int org_x = 0, org_z = 0;
    bool valid = false;

    Compute clear, raster, resolve;
    bool shaders_tried = false;
    bool shaders_ok = false;
};
Ground g;

pcstr short_name(pcstr name)
{
    pcstr slash = strrchr(name, '\\');
    pcstr fwd = strrchr(name, '/');
    if (fwd && (!slash || fwd > slash))
        slash = fwd;
    return slash ? slash + 1 : name;
}

// The lists hold GMLib names with or without the "materials\\" folder; both sides compare short.
bool in_list(const xr_string& list, pcstr name)
{
    const int count = _GetItemCount(list.c_str());
    string128 item;
    for (int i = 0; i < count; ++i)
    {
        if (!xr_stricmp(short_name(_GetItem(list.c_str(), i, item)), name))
            return true;
    }
    return false;
}

// [snow_bake] of dead_air_x64_seasons.ltx: material lists by GMLib name (docs/dead-air/SEASON_DATA.md).
struct MaterialLists
{
    xr_string road{"asphalt, gravel, concrete"};
    xr_string skip{"bush, bush_sux, death, fake, occ, water, water_radiation"};
    xr_string rail{"metal, metal_pipe"};
    xr_string ballast{"gravel"};
    // Tree trunks and their branches carry no snow surface: the snow lies on the ground round them.
    xr_string trunk{"tree_trunk"};
    float rail_dilation = 8.f;
};

MaterialLists read_lists()
{
    MaterialLists lists;
    if (!FS.exist("$game_config$", "dead_air_x64_seasons.ltx"))
        return lists;
    string_path fn;
    FS.update_path(fn, "$game_config$", "dead_air_x64_seasons.ltx");
    CInifile ini(fn, TRUE);
    constexpr pcstr sect = "snow_bake";
    if (!ini.section_exist(sect))
        return lists;
    const auto read = [&](pcstr key, xr_string& out) {
        if (ini.line_exist(sect, key))
            out = ini.r_string(sect, key);
    };
    read("road", lists.road);
    read("skip", lists.skip);
    read("rail", lists.rail);
    read("rail_ballast", lists.ballast);
    read("trunk", lists.trunk);
    if (ini.line_exist(sect, "rail_dilation"))
        lists.rail_dilation = std::max(ini.r_float(sect, "rail_dilation"), 0.f);
    return lists;
}

Fvector centroid(const CDB::TRI& t, const Fvector* v)
{
    Fvector c;
    c.add(v[t.verts[0]], v[t.verts[1]]).add(v[t.verts[2]]).div(3.f);
    return c;
}

// The rows of the triangle buffer: indices and flags (da_ground.h). Railway ballast is gravel too: a
// gravel triangle within the dilation of a rail (a rail material lying just above gravel) is no road.
void build_rows(const CDB::MODEL& model, const Fbox& bounds, xr_vector<u32>& rows)
{
    const MaterialLists lists = read_lists();
    const u32 materials = u32(GMLib.CountMaterial());
    xr_vector<u32> mflags(materials, 0);
    xr_vector<u8> mkind(materials, 0); // 1 rail, 2 ballast
    for (u32 i = 0; i < materials; ++i)
    {
        const SGameMtl* m = GMLib.GetMaterialByIdx(u16(i));
        if (!m)
            continue;
        pcstr name = short_name(m->m_Name.c_str());
        const bool liquid_passable = m->Flags.test(SGameMtl::flLiquid) && m->Flags.test(SGameMtl::flPassable);
        const bool passable =
            m->Flags.test(SGameMtl::flPassable) || in_list(lists.skip, name) || in_list(lists.trunk, name);
        u32 f = i & 0xFFFFu;
        if (passable)
            f |= k_flag_passable;
        if (liquid_passable)
            f |= k_flag_water;
        if (!passable && in_list(lists.road, name))
            f |= k_flag_road;
        mflags[i] = f;
        mkind[i] = in_list(lists.rail, name) ? 1 : (in_list(lists.ballast, name) ? 2 : 0);
    }

    const u32 count = u32(model.get_tris_count());
    const CDB::TRI* tris = model.get_tris();
    const Fvector* verts = model.get_verts();
    rows.resize(size_t(count) * 4);
    for (u32 i = 0; i < count; ++i)
    {
        const CDB::TRI& t = tris[i];
        u32* r = &rows[size_t(i) * 4];
        r[0] = t.verts[0];
        r[1] = t.verts[1];
        r[2] = t.verts[2];
        r[3] = t.material < materials ? mflags[t.material] : (t.material & 0xFFFFu);
    }

    // Rails: rail triangles lying within a metre above ballast in a 4 m cell or its neighbours.
    const u32 rx = u32(std::ceil((bounds.vMax.x - bounds.vMin.x) / k_rail_cell)) + 1;
    const u32 rz = u32(std::ceil((bounds.vMax.z - bounds.vMin.z) / k_rail_cell)) + 1;
    const auto cell_of = [&](const Fvector& p) {
        const int x = clampr(int((p.x - bounds.vMin.x) / k_rail_cell), 0, int(rx) - 1);
        const int z = clampr(int((p.z - bounds.vMin.z) / k_rail_cell), 0, int(rz) - 1);
        return std::pair<int, int>{x, z};
    };
    xr_vector<float> gmin(size_t(rx) * rz, flt_max), gmax(size_t(rx) * rz, -flt_max);
    bool any_ballast = false;
    for (u32 i = 0; i < count; ++i)
    {
        const CDB::TRI& t = tris[i];
        if (t.material >= materials || mkind[t.material] != 2 || !(rows[size_t(i) * 4 + 3] & k_flag_road))
            continue;
        const Fvector c = centroid(t, verts);
        const auto [x, z] = cell_of(c);
        const size_t k = size_t(z) * rx + x;
        gmin[k] = std::min(gmin[k], c.y);
        gmax[k] = std::max(gmax[k], c.y);
        any_ballast = true;
    }
    if (!any_ballast)
        return;
    xr_vector<u8> rail(size_t(rx) * rz, 0);
    for (u32 i = 0; i < count; ++i)
    {
        const CDB::TRI& t = tris[i];
        if (t.material >= materials || mkind[t.material] != 1)
            continue;
        const Fvector c = centroid(t, verts);
        const auto [x, z] = cell_of(c);
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int nx = x + dx, nz = z + dz;
                if (nx < 0 || nz < 0 || nx >= int(rx) || nz >= int(rz))
                    continue;
                const size_t k = size_t(nz) * rx + nx;
                if (gmax[k] >= gmin[k] && c.y >= gmin[k] - 0.5f && c.y <= gmax[k] + 1.f)
                    rail[size_t(z) * rx + x] = 1;
            }
        }
    }
    // Dilation, separable: a square of the given half-width.
    const int r = int(std::ceil(lists.rail_dilation / k_rail_cell));
    xr_vector<u8> tmp(rail.size(), 0);
    for (u32 z = 0; z < rz; ++z)
        for (u32 x = 0; x < rx; ++x)
            for (int d = -r; d <= r && !tmp[size_t(z) * rx + x]; ++d)
            {
                const int sx = int(x) + d;
                if (sx >= 0 && sx < int(rx) && rail[size_t(z) * rx + sx])
                    tmp[size_t(z) * rx + x] = 1;
            }
    for (u32 z = 0; z < rz; ++z)
        for (u32 x = 0; x < rx; ++x)
        {
            u8 v = 0;
            for (int d = -r; d <= r && !v; ++d)
            {
                const int sz = int(z) + d;
                if (sz >= 0 && sz < int(rz) && tmp[size_t(sz) * rx + x])
                    v = 1;
            }
            rail[size_t(z) * rx + x] = v;
        }
    u32 cleared = 0;
    for (u32 i = 0; i < count; ++i)
    {
        const CDB::TRI& t = tris[i];
        u32& f = rows[size_t(i) * 4 + 3];
        if (t.material >= materials || mkind[t.material] != 2 || !(f & k_flag_road))
            continue;
        const auto [x, z] = cell_of(centroid(t, verts));
        if (rail[size_t(z) * rx + x])
        {
            f &= ~k_flag_road;
            ++cleared;
        }
    }
    if (cleared)
        Msg("* [ground] %u ballast triangles along rails are not road", cleared);
}

// The CPU index of the triangles the window rasterises (everything but the passable non-water ones).
void build_index(const xr_vector<u32>& rows)
{
    const Fvector2 size{g.bounds.vMax.x - g.bounds.vMin.x, g.bounds.vMax.z - g.bounds.vMin.z};
    g.cells_x = std::max(1u, u32(std::ceil(size.x / k_cell)));
    g.cells_z = std::max(1u, u32(std::ceil(size.y / k_cell)));
    const u32 cells = g.cells_x * g.cells_z;
    const auto cell_range = [](const CDB::TRI& t, int& x0, int& z0, int& x1, int& z1) {
        const Fvector& a = g.cpu_verts[t.verts[0]];
        const Fvector& b = g.cpu_verts[t.verts[1]];
        const Fvector& c = g.cpu_verts[t.verts[2]];
        const float mnx = std::min(a.x, std::min(b.x, c.x)), mxx = std::max(a.x, std::max(b.x, c.x));
        const float mnz = std::min(a.z, std::min(b.z, c.z)), mxz = std::max(a.z, std::max(b.z, c.z));
        x0 = clampr(int((mnx - g.bounds.vMin.x) / k_cell), 0, int(g.cells_x) - 1);
        x1 = clampr(int((mxx - g.bounds.vMin.x) / k_cell), 0, int(g.cells_x) - 1);
        z0 = clampr(int((mnz - g.bounds.vMin.z) / k_cell), 0, int(g.cells_z) - 1);
        z1 = clampr(int((mxz - g.bounds.vMin.z) / k_cell), 0, int(g.cells_z) - 1);
    };
    const auto rasterised = [&rows](u32 i) {
        const u32 f = rows[size_t(i) * 4 + 3];
        return !(f & k_flag_passable) || (f & k_flag_water);
    };

    g.cell_first.assign(size_t(cells) + 1, 0);
    for (u32 i = 0; i < g.tri_count; ++i)
    {
        if (!rasterised(i))
            continue;
        int x0, z0, x1, z1;
        cell_range(g.cpu_tris[i], x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                ++g.cell_first[size_t(z) * g.cells_x + x + 1];
    }
    for (u32 c = 0; c < cells; ++c)
        g.cell_first[c + 1] += g.cell_first[c];
    g.cell_tris.resize(g.cell_first[cells]);
    xr_vector<u32> fill(g.cell_first.begin(), g.cell_first.end() - 1);
    for (u32 i = 0; i < g.tri_count; ++i)
    {
        if (!rasterised(i))
            continue;
        int x0, z0, x1, z1;
        cell_range(g.cpu_tris[i], x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                g.cell_tris[fill[size_t(z) * g.cells_x + x]++] = i;
    }
    g.tri_mark.assign(g.tri_count, 0);
    g.mark = 0;
}

bool raw_buffer(const void* data, u32 bytes, ID3D11Buffer*& buffer, ID3D11ShaderResourceView*& srv)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (bytes + 3) & ~3u;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    return SUCCEEDED(HW.pDevice->CreateBuffer(&desc, &init, &buffer)) && buffer &&
        SUCCEEDED(HW.pDevice->CreateShaderResourceView(buffer, &view, &srv));
}

void release_cdb()
{
    release(g.verts_srv);
    release(g.verts);
    release(g.tris_srv);
    release(g.tris);
    g.tri_count = 0;
    g.cpu_verts = nullptr;
    g.cpu_tris = nullptr;
    g.cells_x = g.cells_z = 0;
    g.cell_first.clear();
    g.cell_first.shrink_to_fit();
    g.cell_tris.clear();
    g.cell_tris.shrink_to_fit();
    g.tri_mark.clear();
    g.tri_mark.shrink_to_fit();
    g.work_cpu.clear();
    g.work_cpu.shrink_to_fit();
    g.work.destroy();
}

void window_placeholders()
{
    g.raw_solid.placeholder(k_name_raw_solid, DXGI_FORMAT_R32_UINT);
    g.raw_water.placeholder(k_name_raw_water, DXGI_FORMAT_R32_UINT);
    g.top.placeholder(k_name_top, DXGI_FORMAT_R32_FLOAT);
    g.base.placeholder(k_name_base, DXGI_FORMAT_R32G32_FLOAT);
    g.shore.placeholder(k_name_shore, DXGI_FORMAT_R32G32B32A32_FLOAT);
    g.n = 0;
    g.texel = 0.f;
    g.valid = false;
}

bool window_create()
{
    const WindowSize& ws = k_sizes[g.preset];
    const bool ok = g.raw_solid.create(k_name_raw_solid, ws.n, ws.n, DXGI_FORMAT_R32_UINT, true) &&
        g.raw_water.create(k_name_raw_water, ws.n, ws.n, DXGI_FORMAT_R32_UINT, true) &&
        g.top.create(k_name_top, ws.n, ws.n, DXGI_FORMAT_R32_FLOAT, true) &&
        g.base.create(k_name_base, ws.n, ws.n, DXGI_FORMAT_R32G32_FLOAT, true) &&
        g.shore.create(k_name_shore, ws.n, ws.n, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    if (!ok)
    {
        Msg("! [ground] the %ux%u window could not be created", ws.n, ws.n);
        window_placeholders();
        return false;
    }
    g.n = ws.n;
    g.texel = ws.texel;
    g.valid = false;
    return true;
}

bool ensure_shaders()
{
    if (g.shaders_tried)
        return g.shaders_ok;
    g.shaders_tried = true;
    g.shaders_ok = g.clear.create("da_snow_clear", {}) && g.raster.create("da_ground_raster", {}) &&
        g.resolve.create("da_ground_resolve", {{"s_gr_solid", k_name_raw_solid}, {"s_gr_water", k_name_raw_water}});
    if (!g.shaders_ok)
        Msg("! [ground] the ground window's compute shaders are missing from the content: no snow base");
    return g.shaders_ok;
}

// The raster work of one strip: every listed triangle near it once, split into 32 x 32 texel chunks.
void gather(const Rect& r)
{
    if (++g.mark == 0)
    {
        std::fill(g.tri_mark.begin(), g.tri_mark.end(), 0u);
        g.mark = 1;
    }
    const float t = g.texel;
    const float grow = 0.70711f * t;
    const int cx0 = clampr(int((r.x0 * t - grow - g.bounds.vMin.x) / k_cell), 0, int(g.cells_x) - 1);
    const int cx1 = clampr(int((r.x1 * t + grow - g.bounds.vMin.x) / k_cell), 0, int(g.cells_x) - 1);
    const int cz0 = clampr(int((r.z0 * t - grow - g.bounds.vMin.z) / k_cell), 0, int(g.cells_z) - 1);
    const int cz1 = clampr(int((r.z1 * t + grow - g.bounds.vMin.z) / k_cell), 0, int(g.cells_z) - 1);
    if (r.x1 * t + grow < g.bounds.vMin.x || r.x0 * t - grow > g.bounds.vMax.x || r.z1 * t + grow < g.bounds.vMin.z ||
        r.z0 * t - grow > g.bounds.vMax.z)
        return;
    for (int cz = cz0; cz <= cz1; ++cz)
    {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const size_t cell = size_t(cz) * g.cells_x + cx;
            for (u32 k = g.cell_first[cell]; k < g.cell_first[cell + 1]; ++k)
            {
                const u32 i = g.cell_tris[k];
                if (g.tri_mark[i] == g.mark)
                    continue;
                g.tri_mark[i] = g.mark;
                const CDB::TRI& tri = g.cpu_tris[i];
                const Fvector& a = g.cpu_verts[tri.verts[0]];
                const Fvector& b = g.cpu_verts[tri.verts[1]];
                const Fvector& c = g.cpu_verts[tri.verts[2]];
                const int lx = std::max(r.x0, int(std::floor((std::min(a.x, std::min(b.x, c.x)) - grow) / t)));
                const int lz = std::max(r.z0, int(std::floor((std::min(a.z, std::min(b.z, c.z)) - grow) / t)));
                const int hx = std::min(r.x1, int(std::floor((std::max(a.x, std::max(b.x, c.x)) + grow) / t)) + 1);
                const int hz = std::min(r.z1, int(std::floor((std::max(a.z, std::max(b.z, c.z)) + grow) / t)) + 1);
                for (int z = lz; z < hz; z += k_chunk)
                {
                    for (int x = lx; x < hx; x += k_chunk)
                    {
                        g.work_cpu.push_back(i);
                        g.work_cpu.push_back(u32(x - r.x0) | (u32(z - r.z0) << 16));
                    }
                }
            }
        }
    }
}

void clear_rect(CBackend& cmd, const Texture& map, const Rect& r)
{
    static const shared_str c_rect("da_sn_rect"), c_clear("da_sn_clear");
    dispatch(cmd, g.clear, u32(r.width() + 7) / 8, u32(r.height() + 7) / 8, 1, {}, {map.uav}, [&] {
        cmd.set_c(c_rect, float(r.x0), float(r.z0), float(r.width()), float(r.height()));
        cmd.set_c(c_clear, float(g.n), 0.f, 0.f, 0.f);
    });
}

void raster_rect(CBackend& cmd, const Rect& r)
{
    g.work_cpu.clear();
    gather(r);
    const u32 items = u32(g.work_cpu.size() / 2);
    if (!items)
        return;
    if (!g.work.reserve(items, 8, true))
    {
        Msg("! [ground] no buffer for %u raster items", items);
        return;
    }
    g.work.upload(cmd, g.work_cpu.data(), items);
    static const shared_str c_rect("da_gr_rect"), c_win("da_gr_win"), c_base("da_gr_base");
    constexpr u32 per_dispatch = 65535u * 64u;
    for (u32 first = 0; first < items; first += per_dispatch)
    {
        const u32 groups = (std::min(items - first, per_dispatch) + 63) / 64;
        dispatch(cmd, g.raster, groups, 1, 1, {{8, g.verts_srv}, {9, g.tris_srv}, {10, g.work.srv}},
            {g.raw_solid.uav, g.raw_water.uav}, [&] {
                cmd.set_c(c_rect, float(r.x0), float(r.z0), float(r.x1), float(r.z1));
                cmd.set_c(c_win, g.texel, 1.f / g.texel, float(g.n), float(items));
                cmd.set_c(c_base, float(first), 0.f, 0.f, 0.f);
            });
    }
}

void resolve_rect(CBackend& cmd, const Rect& r, const Rect& window)
{
    const Rect o{std::max(r.x0 - k_apron, window.x0), std::max(r.z0 - k_apron, window.z0),
        std::min(r.x1 + k_apron, window.x1), std::min(r.z1 + k_apron, window.z1)};
    if (o.empty())
        return;
    static const shared_str c_rect("da_gr_rect"), c_win("da_gr_win"), c_valid("da_gr_valid");
    const float radius = g.texel <= 0.26f ? 2.f : 1.f;
    dispatch(cmd, g.resolve, u32(o.width() + 15) / 16, u32(o.height() + 15) / 16, 1, {}, {g.top.uav, g.base.uav, g.shore.uav}, [&] {
        cmd.set_c(c_rect, float(o.x0), float(o.z0), float(o.x1), float(o.z1));
        cmd.set_c(c_win, g.texel, 1.f / g.texel, float(g.n), radius);
        cmd.set_c(c_valid, float(window.x0), float(window.z0), float(window.x1), float(window.z1));
    });
}
} // namespace

void da_ground_level_load()
{
    da_ground_level_unload();
    if (!g_pGameLevel)
        return;
    CDB::MODEL* model = g_pGameLevel->ObjectSpace.GetStaticModel();
    if (!model || !model->get_tris_count() || !model->get_verts_count())
        return;

    CTimer timer;
    timer.Start();
    g.bounds.invalidate();
    const Fvector* verts = model->get_verts();
    const u32 vcount = u32(model->get_verts_count());
    for (u32 i = 0; i < vcount; ++i)
        g.bounds.modify(verts[i]);
    xr_vector<u32> rows;
    build_rows(*model, g.bounds, rows);
    if (!raw_buffer(verts, vcount * sizeof(Fvector), g.verts, g.verts_srv) ||
        !raw_buffer(rows.data(), u32(rows.size() * sizeof(u32)), g.tris, g.tris_srv))
    {
        Msg("! [ground] the collision mesh could not be uploaded (%u triangles)", u32(model->get_tris_count()));
        release_cdb();
        return;
    }
    g.tri_count = u32(model->get_tris_count());
    g.cpu_verts = verts;
    g.cpu_tris = model->get_tris();
    build_index(rows);
    window_create();
    Msg("* [ground] collision mesh: %u triangles, %u vertices, %.1f MB, index %u entries, %.0f ms", g.tri_count,
        vcount, (vcount * 12.f + g.tri_count * 16.f) / (1024.f * 1024.f), u32(g.cell_tris.size()),
        timer.GetElapsed_sec() * 1000.f);
}

void da_ground_level_unload()
{
    release_cdb();
    window_placeholders();
}

void da_ground_frame_prepare(CBackend& cmd_list)
{
    if (!g.tris || !ensure_shaders())
        return;
    // The snow is the window's only reader: nothing is rasterised while it has nothing to draw, and the
    // first snowy frame rebuilds the whole window before the snow reads it.
    if (!da_snow_depth_active())
    {
        g.valid = false;
        return;
    }
    const WindowSize& ws = k_sizes[g.preset];
    if ((ws.n != g.n || ws.texel != g.texel || !g.base.live()) && !window_create())
        return;

    const Fvector& cam = Device.vCameraPosition;
    const int half = int(g.n) / 2;
    const int nx = floor_div(int(std::floor(cam.x / g.texel)) - half, k_step) * k_step;
    const int nz = floor_div(int(std::floor(cam.z / g.texel)) - half, k_step) * k_step;
    Rect strips[2];
    const u32 count = window_strips(g.org_x, g.org_z, nx, nz, int(g.n), g.valid, strips);
    if (!count)
        return;
    g.org_x = nx;
    g.org_z = nz;
    g.valid = true;
    const Rect window{nx, nz, nx + int(g.n), nz + int(g.n)};

    PIX_EVENT_CTX(cmd_list, DA_ground_window);
    for (u32 i = 0; i < count; ++i)
    {
        clear_rect(cmd_list, g.raw_solid, strips[i]);
        clear_rect(cmd_list, g.raw_water, strips[i]);
        raster_rect(cmd_list, strips[i]);
    }
    for (u32 i = 0; i < count; ++i)
        resolve_rect(cmd_list, strips[i], window);
}

bool da_ground_cdb(DaGroundCdb& out)
{
    if (!g.tris)
        return false;
    out.verts = g.verts_srv;
    out.tris = g.tris_srv;
    out.tri_count = g.tri_count;
    out.bounds = g.bounds;
    return true;
}

ID3D11ShaderResourceView* da_ground_top_srv() { return g.top.srv(); }

Fvector4 da_ground_top_xf()
{
    const float w = g.n ? 1.f / (float(g.n) * g.texel) : 0.f;
    return {w, w, 0.f, 0.f};
}

ID3D11ShaderResourceView* da_ground_base_srv() { return g.base.srv(); }

bool da_ground_window(Fvector4& win, Fvector4& win2)
{
    if (!g.valid || !g.n)
    {
        win.set(0.f, 0.f, 0.f, k_band);
        win2.set(1.f, 1.f, 0.f, 0.f);
        return false;
    }
    const float half = float(g.n) * 0.5f;
    win.set((float(g.org_x) + half) * g.texel, (float(g.org_z) + half) * g.texel, (half - float(k_edge)) * g.texel, k_band);
    win2.set(float(g.n), 1.f / g.texel, 0.f, 0.f);
    return true;
}

void da_ground_apply_preset(u32 preset) { g.preset = std::min(preset, 4u); }

void da_ground_destroy()
{
    release_cdb();
    g.raw_solid.destroy();
    g.raw_water.destroy();
    g.top.destroy();
    g.base.destroy();
    g.shore.destroy();
    g.n = 0;
    g.texel = 0.f;
    g.valid = false;
    g.clear.destroy();
    g.raster.destroy();
    g.resolve.destroy();
    g.shaders_tried = false;
    g.shaders_ok = false;
}
} // namespace xray::render::RENDER_NAMESPACE
