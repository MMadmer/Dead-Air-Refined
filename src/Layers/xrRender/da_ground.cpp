// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The level's collision mesh on the GPU and the near ground window (docs/dead-air/SNOW.md, da_ground.h).
//
// Upload, once per level: every CDB triangle as (i0, i1, i2, flags) and every vertex, for the grass and
// for the window below, plus a CPU index of the triangles that can carry snow by 16 m cells.
// Window: N x N texels around the camera, toroidal (a texel is the world texel modulo N), moved in steps
// of 16 texels. A strip the camera moves into is cleared, rasterised from the triangles of its cells
// (da_ground_raster.cs: conservative, the topmost surface wins; the lowest floor besides) and resolved
// together with an apron (da_ground_resolve.cs): Htop, the base the ground's snow lies on (the opened Hg, or
// the floor under a roof, a deck or a vehicle), the shelter such a cover gives, the cliff taper of the snow
// grid and the continuous quantities of the waterline (the bed under the water, the level of the water
// within reach).
// Snow shells, once per bake: the snow lying on everything standing over the terrain - roofs, decks,
// vehicles, crates, slabs, fence tops - as real geometry built from the collision's faces that look up
// (da_snow_shell.vs raises them by the roof's snow, rounded off toward their open edges), so a roof's snow
// follows its own edges at every distance instead of a height field's cells.

#include "stdafx.h"
#include "da_ground.h"
#include "da_snow.h"
#include "da_snow_gpu.h"

#include "xrMaterialSystem/GameMtlLib.h"
#include "xrEngine/da_snow_field.h"
#include "xrCDB/Frustum.h"
#include "xrCore/Threading/ParallelFor.hpp"

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
constexpr pcstr k_name_raw_low = "$user$da_ground_raw_low";
constexpr pcstr k_name_terrain = "$user$da_snow_ground"; // da_snow.cpp
constexpr pcstr k_name_walk = "$user$da_snow_walk";      // da_snow.cpp
constexpr pcstr k_name_overhang = "$user$da_snow_overhang"; // da_snow.cpp
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
    xr_vector<u32> tri_flags; // the flags word of each row (material, passable, water, road)
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
    Texture raw_solid, raw_water, raw_low, top, base, shore;
    u32 terrain_version = 0; // da_snow_level_terrain's upload the window was built with
    int org_x = 0, org_z = 0;
    bool valid = false;

    Compute clear, raster, resolve;
    bool shaders_tried = false;
    bool shaders_ok = false;

    // snow shells: chunks of up to 65536 vertices (16-bit indices from each chunk's first vertex)
    struct ShellChunk
    {
        Fbox box;
        u32 base_vertex, vcount, first_index, tris;
    };
    ID3D11Buffer* shell_vb = nullptr;
    ID3D11Buffer* shell_ib = nullptr;
    ref_geom shell_geom;
    xr_vector<ShellChunk> shell_chunks;
    u32 shell_field = u32(-1);
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
    g.tri_flags.clear();
    g.tri_flags.shrink_to_fit();
    g.work_cpu.clear();
    g.work_cpu.shrink_to_fit();
    g.work.destroy();
}

void window_placeholders()
{
    g.raw_solid.placeholder(k_name_raw_solid, DXGI_FORMAT_R32_UINT);
    g.raw_water.placeholder(k_name_raw_water, DXGI_FORMAT_R32_UINT);
    g.raw_low.placeholder(k_name_raw_low, DXGI_FORMAT_R32_UINT);
    g.top.placeholder(k_name_top, DXGI_FORMAT_R32_FLOAT);
    g.base.placeholder(k_name_base, DXGI_FORMAT_R32G32B32A32_FLOAT);
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
        g.raw_low.create(k_name_raw_low, ws.n, ws.n, DXGI_FORMAT_R32_UINT, true) &&
        g.top.create(k_name_top, ws.n, ws.n, DXGI_FORMAT_R32_FLOAT, true) &&
        g.base.create(k_name_base, ws.n, ws.n, DXGI_FORMAT_R32G32B32A32_FLOAT, true) &&
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
        g.resolve.create("da_ground_resolve", {{"s_gr_solid", k_name_raw_solid}, {"s_gr_water", k_name_raw_water},
                                                  {"s_gr_low", k_name_raw_low}, {"s_gr_ground", k_name_terrain},
                                                  {"s_gr_walk", k_name_walk}, {"s_gr_overhang", k_name_overhang}});
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
            {g.raw_solid.uav, g.raw_water.uav, g.raw_low.uav}, [&] {
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
    static const shared_str c_rect("da_gr_rect"), c_win("da_gr_win"), c_valid("da_gr_valid"), c_level("da_gr_level"),
        c_lvl("da_gr_lvl");
    const float radius = g.texel <= 0.26f ? 2.f : 1.f;
    // the level's terrain tells a roof from a hill (the floor under a cover is the low map's)
    Fvector4 level_xf, level_y;
    const bool terrain = da_snow_level_terrain(level_xf, level_y) != 0;
    dispatch(cmd, g.resolve, u32(o.width() + 15) / 16, u32(o.height() + 15) / 16, 1, {}, {g.top.uav, g.base.uav, g.shore.uav}, [&] {
        cmd.set_c(c_rect, float(o.x0), float(o.z0), float(o.x1), float(o.z1));
        cmd.set_c(c_win, g.texel, 1.f / g.texel, float(g.n), radius);
        cmd.set_c(c_valid, float(window.x0), float(window.z0), float(window.x1), float(window.z1));
        cmd.set_c(c_level, level_xf);
        cmd.set_c(c_lvl, level_y.x, level_y.y, terrain ? 1.f : 0.f, 0.f);
    });
}

// ---- snow shells ------------------------------------------------------------------------------------------

constexpr float k_shell_step = 0.3f;   // longest edge of a shell triangle near an open edge (m)
constexpr float k_shell_cell = 32.f;   // metres per side of a chunk's cell
constexpr float k_shell_ny = 0.5f;     // the steepest face snow lies on (60 degrees)
// A face this far over the terrain carries a shell: a roof, a bus, a trailer, a container. A lower cover's top (a
// block, a car, a stump) keeps the layer's cover on its drawn surface: a collision box stood a sheet of snow off it,
// crumpled and torn at the edges.
constexpr float k_shell_over = 1.8f;
constexpr float k_shell_reach = 0.6f;  // metres from an open edge the rounding reads (the deepest snow and change)
constexpr u32 k_shell_max_verts = 2u << 20;
constexpr int k_shell_max_segs = 24;    // segments an edge near an open edge is cut into at most
// An island of faces carries a shell when it is a broad, smooth surface: a roof, a slab, a tank's top. Small or
// narrow ones (sills, fence tops, crates) and crumpled ones (a wreck's roof panels) keep the snow layer's cover
// on their own drawn surface: a shell there showed the collision's simplifications - stacked sheets on a bus,
// a triangle of snow in a fence's gap.
constexpr float k_island_area = 1.5f;   // m2
constexpr float k_island_width = 0.15f; // area over open edge length (m): about half the width
constexpr float k_island_crease = 1.0f; // creases (folds over 20 degrees) per square metre at most

struct ShellVert
{
    Fvector4 p; // xyz on the face, w metres to the nearest open edge (0 under a cover above)
    Fvector4 n; // the face's normal x, z (y up and positive), the direction away from that edge x, z
};

void shells_release()
{
    release(g.shell_vb);
    release(g.shell_ib);
    g.shell_geom = nullptr;
    g.shell_chunks.clear();
    g.shell_field = u32(-1);
}

// A field at a world position, bilinear over the texel centres; nothing outside.
float field_at(const SSnowField& f, const xr_vector<float>& v, float x, float z)
{
    const float fx = (x - f.origin.x) / f.texel - 0.5f, fz = (z - f.origin.y) / f.texel - 0.5f;
    const int x0 = iFloor(fx), z0 = iFloor(fz);
    const float tx = fx - float(x0), tz = fz - float(z0);
    const auto at = [&](int xi, int zi) {
        xi = clampr(xi, 0, int(f.dim_x) - 1);
        zi = clampr(zi, 0, int(f.dim_z) - 1);
        return v[size_t(zi) * f.dim_x + xi];
    };
    return (at(x0, z0) * (1.f - tx) + at(x0 + 1, z0) * tx) * (1.f - tz) + (at(x0, z0 + 1) * (1.f - tx) + at(x0 + 1, z0 + 1) * tx) * tz;
}

// A solid surface over p (within 60 m): a floor under a roof, a lower roof under an eave.
bool covered_at(CDB::MODEL* model, const Fvector& p)
{
    CDB::COLLIDER xrc;
    xrc.ray_query(0, model, Fvector{p.x, p.y + 0.05f, p.z}, Fvector{0.f, 1.f, 0.f}, 60.f);
    for (size_t r = 0; r < xrc.r_count(); ++r)
    {
        const u32 id = u32(xrc.r_begin()[r].id);
        if (id < g.tri_count && !(g.tri_flags[id] & (k_flag_passable | k_flag_water)))
            return true;
    }
    return false;
}

// A surface the level's AI walks on at about y round xz (the 3 x 3 texels of the bake's nodes): a deck, ground.
bool walked_at(const SSnowField& f, float x, float z, float y)
{
    if (f.walk.size() != size_t(f.dim_x) * f.dim_z)
        return false;
    const int cx = iFloor((x - f.origin.x) / f.texel), cz = iFloor((z - f.origin.y) / f.texel);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int sx = cx + dx, sz = cz + dz;
            const size_t j = size_t(sz) * f.dim_x + sx;
            if (sx >= 0 && sz >= 0 && sx < int(f.dim_x) && sz < int(f.dim_z) && da_snow_walkable(y, f.walk[j]) &&
                da_snow_deck_node(f.walk[j], f.ground[j]))
                return true;
        }
    return false;
}

// Squared distance from p to the segment ab, and its nearest point.
float seg_dist2(const Fvector& p, const Fvector& a, const Fvector& b, Fvector& q)
{
    Fvector ab;
    ab.sub(b, a);
    const float l2 = ab.square_magnitude();
    const float t = l2 > 1e-12f ? clampr((p.x - a.x) * ab.x + (p.y - a.y) * ab.y + (p.z - a.z) * ab.z, 0.f, l2) / l2 : 0.f;
    q.mad(a, ab, t);
    return p.distance_to_sqr(q);
}

void shells_build(const SSnowField& f)
{
    shells_release();
    const size_t cells = size_t(f.dim_x) * f.dim_z;
    if (!g.cpu_tris || !g.cpu_verts || f.ground.size() != cells || f.top.size() != cells || g.tri_flags.size() != g.tri_count)
        return;
    CDB::MODEL* model = g_pGameLevel ? g_pGameLevel->ObjectSpace.GetStaticModel() : nullptr;
    if (!model)
        return;
    CTimer timer;
    timer.Start();

    // 1. The faces snow lies on over the terrain: solid, facing up under 60 degrees, every corner over the
    //    terrain and the face's middle a cover's height over it.
    struct Face
    {
        u32 v[3]; // welded corners
        u32 cell;
        Fvector n;
        float area;
    };
    xr_vector<Face> faces;
    xr_vector<Fvector> wv; // welded positions
    xr_unordered_map<u64, u32> weld;
    const auto weld_of = [&](const Fvector& p) {
        const auto q = [](float x) { return u64(s64(std::floor(x * 100.f + 0.5f)) + (s64(1) << 20)) & 0x1FFFFFu; };
        const u64 key = q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
        const auto [it, added] = weld.emplace(key, u32(wv.size()));
        if (added)
            wv.push_back(p);
        return it->second;
    };
    const u32 cx = std::max(1u, u32(std::ceil((g.bounds.vMax.x - g.bounds.vMin.x) / k_shell_cell)));
    for (u32 i = 0; i < g.tri_count; ++i)
    {
        if (g.tri_flags[i] & (k_flag_passable | k_flag_water))
            continue;
        const CDB::TRI& t = g.cpu_tris[i];
        const Fvector& a = g.cpu_verts[t.verts[0]];
        const Fvector& b = g.cpu_verts[t.verts[1]];
        const Fvector& c = g.cpu_verts[t.verts[2]];
        Fvector n;
        n.mknormal(a, b, c);
        if (!(n.y >= k_shell_ny))
            continue;
        const Fvector m = centroid(t, g.cpu_verts);
        const float terrain = field_at(f, f.ground, m.x, m.z);
        if (m.y - terrain < k_shell_over)
            continue;
        if (a.y - field_at(f, f.ground, a.x, a.z) < 0.3f || b.y - field_at(f, f.ground, b.x, b.z) < 0.3f ||
            c.y - field_at(f, f.ground, c.x, c.z) < 0.3f)
            continue;
        // a deck the AI walks is ground (the grid's, with its trails); a low block with no floor under it too (the
        // window's resolve keeps its top as the base: a shell there stood on the grid's own snow)
        if (walked_at(f, m.x, m.z, m.y))
            continue;
        // a face under another surface (a floor under a roof): the 1 m top field first, a ray to be sure
        if (field_at(f, f.top, m.x, m.z) > m.y + 1.f && covered_at(model, m))
            continue;
        const u32 ix = clampr(u32((m.x - g.bounds.vMin.x) / k_shell_cell), 0u, cx - 1);
        const u32 iz = u32(std::max((m.z - g.bounds.vMin.z) / k_shell_cell, 0.f));
        const float area = 0.5f * Fvector().crossproduct(Fvector().sub(b, a), Fvector().sub(c, a)).magnitude();
        const u32 wa = weld_of(a), wb = weld_of(b), wc = weld_of(c);
        // (welded into a sliver: its zero-length edge would count as open, a pit in the middle of a roof)
        if (wa == wb || wb == wc || wc == wa)
            continue;
        faces.push_back({{wa, wb, wc}, iz * cx + ix, n, area});
    }
    if (faces.empty())
        return;
    const u32 faces_all = u32(faces.size());

    // 2. Islands of faces joined by their edges; only the broad, smooth ones keep their faces.
    const auto edge_key = [](u32 a, u32 b) { return a < b ? (u64(a) << 32) | b : (u64(b) << 32) | a; };
    xr_unordered_map<u64, std::pair<u32, u32>> edge_faces; // first face, uses
    xr_vector<u32> parent(faces.size());
    for (u32 i = 0; i < parent.size(); ++i)
        parent[i] = i;
    const auto root = [&](u32 i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    xr_vector<float> crease(faces.size(), 0.f);
    for (u32 i = 0; i < faces.size(); ++i)
    {
        for (int e = 0; e < 3; ++e)
        {
            const u32 a = faces[i].v[e], b = faces[i].v[(e + 1) % 3];
            auto [it, added] = edge_faces.emplace(edge_key(a, b), std::pair<u32, u32>{i, 0u});
            ++it->second.second;
            if (added)
                continue;
            const u32 j = it->second.first;
            parent[root(i)] = root(j);
            if (faces[i].n.dotproduct(faces[j].n) < 0.94f)
                crease[i] += wv[a].distance_to(wv[b]);
        }
    }
    struct Island
    {
        float area = 0.f, open = 0.f, crease = 0.f;
    };
    xr_unordered_map<u32, Island> islands;
    for (u32 i = 0; i < faces.size(); ++i)
    {
        Island& is = islands[root(i)];
        is.area += faces[i].area;
        is.crease += crease[i];
        for (int e = 0; e < 3; ++e)
            if (edge_faces[edge_key(faces[i].v[e], faces[i].v[(e + 1) % 3])].second == 1)
                is.open += wv[faces[i].v[e]].distance_to(wv[faces[i].v[(e + 1) % 3]]);
    }
    u32 kept_islands = 0;
    for (auto& [r, is] : islands)
    {
        const bool keep = is.area >= k_island_area && is.area >= k_island_width * is.open && is.crease <= k_island_crease * is.area;
        is.area = keep ? 1.f : 0.f;
        kept_islands += keep ? 1 : 0;
    }
    {
        xr_vector<Face> kept;
        for (u32 i = 0; i < faces.size(); ++i)
            if (islands[root(i)].area > 0.f)
                kept.push_back(faces[i]);
        faces.swap(kept);
    }
    if (faces.empty())
        return;

    // Open edges: used by one face only. Their segments go to a 1 m hash for the distance below.
    xr_unordered_map<u64, u32> edge_use;
    for (const Face& fc : faces)
        for (int e = 0; e < 3; ++e)
            ++edge_use[edge_key(fc.v[e], fc.v[(e + 1) % 3])];
    xr_vector<std::pair<u32, u32>> open;
    for (const auto& [key, uses] : edge_use)
        if (uses == 1)
            open.emplace_back(u32(key >> 32), u32(key & 0xFFFFFFFFu));
    // A segment's 1 m cells within `margin` of it: sampled every half metre along it (a long diagonal edge's
    // bounding box held tens of thousands of cells, every query running through far segments).
    const auto hkey = [](int x, int z) { return (u64(u32(x + (1 << 20))) << 32) | u32(z + (1 << 20)); };
    const auto hash_segments = [&](float margin, xr_unordered_map<u64, xr_vector<u32>>& hash) {
        xr_vector<u64> keys;
        for (u32 s = 0; s < open.size(); ++s)
        {
            const Fvector& a = wv[open[s].first];
            const Fvector& b = wv[open[s].second];
            const int steps = std::max(1, iCeil(a.distance_to(b) / 0.5f));
            const float r = margin + 0.25f;
            keys.clear();
            for (int k = 0; k <= steps; ++k)
            {
                const Fvector p = Fvector().lerp(a, b, float(k) / float(steps));
                for (int z = iFloor(p.z - r); z <= iFloor(p.z + r); ++z)
                    for (int x = iFloor(p.x - r); x <= iFloor(p.x + r); ++x)
                        keys.push_back(hkey(x, z));
            }
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            for (const u64 key : keys)
                hash[key].push_back(s);
        }
    };
    // A T-junction (a face's edge along two of its neighbour's, the panels' seams the collision left unwelded)
    // is no open edge: its middle lies on another open edge running the same way. Left open, the snow sank to the
    // roof along every such seam.
    {
        xr_unordered_map<u64, xr_vector<u32>> along;
        hash_segments(0.f, along);
        xr_vector<u8> seam(open.size(), 0);
        for (u32 s = 0; s < open.size(); ++s)
        {
            const Fvector& a = wv[open[s].first];
            const Fvector& b = wv[open[s].second];
            const Fvector mid = Fvector().add(a, b).mul(0.5f);
            const Fvector dir = Fvector().sub(b, a).normalize_safe();
            const auto it = along.find(hkey(iFloor(mid.x), iFloor(mid.z)));
            if (it == along.end())
                continue;
            for (const u32 t : it->second)
            {
                if (t == s)
                    continue;
                const Fvector& c = wv[open[t].first];
                const Fvector& e = wv[open[t].second];
                Fvector q;
                if (seg_dist2(mid, c, e, q) < 1e-4f && _abs(dir.dotproduct(Fvector().sub(e, c).normalize_safe())) > 0.999f)
                {
                    seam[s] = 1;
                    break;
                }
            }
        }
        u32 kept = 0;
        for (u32 s = 0; s < open.size(); ++s)
            if (!seam[s])
                open[kept++] = open[s];
        open.resize(kept);
    }
    xr_unordered_map<u64, xr_vector<u32>> seg_hash;
    hash_segments(k_shell_reach, seg_hash);

    // The distance from p to the nearest open edge (capped at the reach), and the way away from it.
    const auto edge_dist = [&](const Fvector& p, Fvector* away) {
        float best = k_shell_reach * k_shell_reach;
        if (away)
            away->set(0.f, 0.f, 0.f);
        const auto it = seg_hash.find(hkey(iFloor(p.x), iFloor(p.z)));
        if (it != seg_hash.end())
        {
            for (const u32 s : it->second)
            {
                Fvector q;
                const float d2 = seg_dist2(p, wv[open[s].first], wv[open[s].second], q);
                if (d2 < best)
                {
                    best = d2;
                    if (away)
                        away->set(p.x - q.x, 0.f, p.z - q.z);
                }
            }
        }
        return _sqrt(best);
    };

    // 3. Each face cut into triangles no longer than the step near its open edges (the rounding lives there;
    //    a face far from every open edge stays whole: its snow is flat). An edge's points depend on the edge
    //    alone (its count from its length and its distance to an open edge, its points from its two corners),
    //    and a row of the face's own lattice snaps onto them, so faces sharing an edge share its vertices: no
    //    cracks. By cells, in chunks.
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.cell < b.cell; });
    xr_vector<ShellVert> verts;
    xr_vector<u16> indices;
    xr_vector<Fvector> nsum;
    // A vertex's lattice key; corners' and edge points' normals sum over every face round them, whichever chunk
    // drew it (a seam between chunks keeps one normal and one height: no crack along a ridge or a valley).
    xr_vector<u64> vkey;
    xr_unordered_map<u64, Fvector> shared_n;
    xr_unordered_map<u64, u16> local;
    Ground::ShellChunk chunk{};
    const auto chunk_begin = [&] {
        chunk = {};
        chunk.box.invalidate();
        chunk.base_vertex = u32(verts.size());
        chunk.first_index = u32(indices.size());
        local.clear();
    };
    const auto chunk_end = [&] {
        chunk.vcount = u32(verts.size()) - chunk.base_vertex;
        chunk.tris = (u32(indices.size()) - chunk.first_index) / 3;
        if (chunk.tris)
            g.shell_chunks.push_back(chunk);
    };
    chunk_begin();
    u32 cell = faces.front().cell;
    u64 interior = 0;
    u32 dropped = 0, capped = 0;
    for (const Face& fc : faces)
    {
        const Fvector& A = wv[fc.v[0]];
        const Fvector& B = wv[fc.v[1]];
        const Fvector& C = wv[fc.v[2]];
        const auto count = [&](const Fvector& p, const Fvector& q) {
            const Fvector mid = Fvector().add(p, q).mul(0.5f);
            const bool close = std::min(edge_dist(p, nullptr), std::min(edge_dist(q, nullptr), edge_dist(mid, nullptr))) < k_shell_reach;
            if (!close)
                return 1;
            const int want = iCeil(p.distance_to(q) / k_shell_step);
            capped += want > k_shell_max_segs ? 1 : 0;
            return clampr(want, 1, k_shell_max_segs);
        };
        const int nab = count(A, B), nbc = count(B, C), nca = count(C, A);
        const int nn = std::max(nab, std::max(nbc, nca));
        // the vertices this face may add: the whole lattice at most
        if (fc.cell != cell || verts.size() - chunk.base_vertex + size_t((nn + 1) * (nn + 2) / 2) > 65535)
        {
            chunk_end();
            chunk_begin();
            cell = fc.cell;
        }
        if (verts.size() + size_t((nn + 1) * (nn + 2) / 2) > k_shell_max_verts)
        {
            ++dropped;
            continue;
        }
        Fvector fn;
        fn.mknormal(A, B, C);
        const float area = Fvector().crossproduct(Fvector().sub(B, A), Fvector().sub(C, A)).magnitude();
        // a lattice point's key and position: corners by their welded index, edge points by their edge's
        // corners and step, inner points by the face
        const auto edge_point = [&](u32 ia, u32 ib, const Fvector& pa, const Fvector& pb, int ne, float t, Fvector& p) {
            int k = iFloor(t * float(ne) + 0.5f);
            if (k == 0)
            {
                p = pa;
                return (u64(1) << 62) | ia;
            }
            if (k == ne)
            {
                p = pb;
                return (u64(1) << 62) | ib;
            }
            // in the edge's own order: both faces on it compute the same point to the last bit
            const bool flip = ia > ib;
            if (flip)
            {
                std::swap(ia, ib);
                k = ne - k;
            }
            p.lerp(flip ? pb : pa, flip ? pa : pb, float(k) / float(ne));
            return (u64(2) << 62) | (u64(ia & 0xFFFFFFu) << 32) | (u64(ib & 0xFFFFFFu) << 8) | u64(k);
        };
        const auto vertex = [&](int i, int j) -> u16 {
            Fvector p;
            u64 key;
            if (i == 0 && j == 0)
                p = A, key = (u64(1) << 62) | fc.v[0];
            else if (j == 0)
                key = edge_point(fc.v[0], fc.v[1], A, B, nab, float(i) / float(nn), p);
            else if (i == 0)
                key = edge_point(fc.v[2], fc.v[0], C, A, nca, 1.f - float(j) / float(nn), p);
            else if (i + j == nn)
                key = edge_point(fc.v[1], fc.v[2], B, C, nbc, float(j) / float(nn), p);
            else
            {
                p.set(A).mad(Fvector().sub(B, A), float(i) / float(nn)).mad(Fvector().sub(C, A), float(j) / float(nn));
                key = (u64(3) << 62) | ++interior;
            }
            const auto [it, added] = local.emplace(key, u16(verts.size() - chunk.base_vertex));
            if (added)
            {
                verts.push_back({{p.x, p.y, p.z, 0.f}, {0.f, 0.f, 0.f, 0.f}});
                nsum.push_back({0.f, 0.f, 0.f});
                vkey.push_back(key);
                chunk.box.modify(p);
            }
            nsum[chunk.base_vertex + it->second].mad(fn, area);
            if ((key >> 62) != 3)
                shared_n[key].mad(fn, area);
            return it->second;
        };
        // the lattice rows, kept in the face's winding
        xr_vector<u16> row0, row1;
        for (int j = 0; j <= nn; ++j)
        {
            row1.clear();
            for (int i = 0; i + j <= nn; ++i)
                row1.push_back(vertex(i, j));
            if (j)
            {
                for (int i = 0; i + j <= nn; ++i)
                {
                    const u16 t0[3] = {row0[i], row0[i + 1], row1[i]};
                    if (t0[0] != t0[1] && t0[1] != t0[2] && t0[0] != t0[2])
                        indices.insert(indices.end(), {t0[0], t0[1], t0[2]});
                    if (i + 1 < int(row1.size()))
                    {
                        const u16 t1[3] = {row0[i + 1], row1[i + 1], row1[i]};
                        if (t1[0] != t1[1] && t1[1] != t1[2] && t1[0] != t1[2])
                            indices.insert(indices.end(), {t1[0], t1[1], t1[2]});
                    }
                }
            }
            row0.swap(row1);
        }
    }
    chunk_end();

    // 4. Per vertex: the normal, the distance to the nearest open edge and the way away from it; a vertex
    //    under another surface (a lower roof under an eave, a balcony under the roof) is an edge itself.
    std::atomic<u32> covered{0};
    xr_parallel_for(TaskRange<size_t>(0, verts.size(), 1024), [&](const TaskRange<size_t>& range) {
        for (size_t v = range.begin(); v != range.end(); ++v)
        {
            ShellVert& sv = verts[v];
            const auto shared = (vkey[v] >> 62) != 3 ? shared_n.find(vkey[v]) : shared_n.end();
            Fvector n = shared != shared_n.end() ? shared->second : nsum[v];
            n.normalize_safe();
            if (n.y < 0.2f)
                n.set(0.f, 1.f, 0.f);
            const Fvector p{sv.p.x, sv.p.y, sv.p.z};
            Fvector away;
            float dist = edge_dist(p, &away);
            if (away.square_magnitude() > 1e-10f)
                away.normalize();
            // the 1 m top field first: a ray only where something may lie over the vertex; a covered vertex is an
            // edge with no way away (its slope would lean toward a far edge)
            if (field_at(f, f.top, p.x, p.z) > p.y + 1.f && covered_at(model, p))
            {
                dist = 0.f;
                away.set(0.f, 0.f, 0.f);
                ++covered;
            }
            sv.p.w = dist;
            sv.n.set(n.x, n.z, away.x, away.z);
        }
    });

    // 5. The buffers.
    if (!verts.empty() && !indices.empty())
    {
        D3D11_BUFFER_DESC desc{};
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.ByteWidth = UINT(verts.size() * sizeof(ShellVert));
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA vdata{verts.data(), 0, 0};
        D3D11_BUFFER_DESC idesc = desc;
        idesc.ByteWidth = UINT(indices.size() * sizeof(u16));
        idesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA idata{indices.data(), 0, 0};
        if (FAILED(HW.pDevice->CreateBuffer(&desc, &vdata, &g.shell_vb)) || FAILED(HW.pDevice->CreateBuffer(&idesc, &idata, &g.shell_ib)))
        {
            Msg("! [ground] the snow shells (%u vertices) could not be uploaded", u32(verts.size()));
            shells_release();
            return;
        }
        static const VertexElement decl[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};
        g.shell_geom.create(decl, g.shell_vb, g.shell_ib);
    }
    Msg("* [ground] snow shells: %u of %u faces in %u of %u islands, %u open edges, %u vertices (%u under a cover), %u "
        "triangles in %u chunks, %.1f MB, %.0f ms",
        u32(faces.size()), faces_all, kept_islands, u32(islands.size()), u32(open.size()), u32(verts.size()), covered.load(),
        u32(indices.size() / 3), u32(g.shell_chunks.size()),
        (verts.size() * sizeof(ShellVert) + indices.size() * 2) / (1024.f * 1024.f), timer.GetElapsed_sec() * 1000.f);
    if (capped)
        Msg("* [ground] snow shells: %u edges near an open edge cut at %d segments", capped, k_shell_max_segs);
    if (dropped)
        Msg("! [ground] snow shells: %u faces past the %u vertices' budget have no shell (the layer's cover there)", dropped,
            k_shell_max_verts);
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
    g.tri_flags.resize(g.tri_count);
    for (u32 i = 0; i < g.tri_count; ++i)
        g.tri_flags[i] = rows[size_t(i) * 4 + 3];
    build_index(rows);
    window_create();
    Msg("* [ground] collision mesh: %u triangles, %u vertices, %.1f MB, index %u entries, %.0f ms", g.tri_count,
        vcount, (vcount * 12.f + g.tri_count * 16.f) / (1024.f * 1024.f), u32(g.cell_tris.size()),
        timer.GetElapsed_sec() * 1000.f);
}

void da_ground_level_unload()
{
    shells_release();
    release_cdb();
    window_placeholders();
}

// QA (r__snow_window_probe x z dx dz count): the window's texels along a line read back next frame - the raw top, the
// snow base, its taper, shelter and opened top.
ID3D11Texture2D* da_snow_fine_surface();
float da_snow_cap();
Fvector4 da_snow_fine_xf();

namespace
{
struct SProbeReq
{
    float x, z, dx, dz;
    int count;
} g_probe_req{};
bool g_probe_pending = false;

float read_texel(ID3D11Texture2D* src, DXGI_FORMAT fmt, int ax, int az, u32 comp)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1;
    d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &st)) || !st)
        return -99999.f;
    ID3D11DeviceContext* ctx = HW.get_context(HW.IMM_CTX_ID);
    D3D11_BOX box{u32(ax), u32(az), 0, u32(ax) + 1, u32(az) + 1, 1};
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, &box);
    float v = -99999.f;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
    {
        v = static_cast<const float*>(m.pData)[comp];
        ctx->Unmap(st, 0);
    }
    st->Release();
    return v;
}

void window_probe()
{
    g_probe_pending = false;
    if (!g.valid || !g.n || !g.base.surface || !g.top.surface)
    {
        Msg("! [qa] window probe: no window");
        return;
    }
    for (int i = 0; i < g_probe_req.count; ++i)
    {
        const float x = g_probe_req.x + g_probe_req.dx * float(i), z = g_probe_req.z + g_probe_req.dz * float(i);
        const int tx = iFloor(x / g.texel), tz = iFloor(z / g.texel), n = int(g.n);
        const int ax = ((tx % n) + n) % n, az = ((tz % n) + n) % n;
        const float top = read_texel(g.top.surface, DXGI_FORMAT_R32_FLOAT, ax, az, 0);
        float b[4];
        for (u32 c = 0; c < 4; ++c)
            b[c] = read_texel(g.base.surface, DXGI_FORMAT_R32G32B32A32_FLOAT, ax, az, c);
        // the fine depth there (low 16 bits, unorm over the cap)
        float fine = -1.f;
        const Fvector4 fx = da_snow_fine_xf();
        if (ID3D11Texture2D* fs = da_snow_fine_surface(); fs && fx.y > 1.f)
        {
            const int fn = int(fx.y), fxi = iFloor(x * fx.x), fzi = iFloor(z * fx.x);
            const float raw = read_texel(fs, DXGI_FORMAT_R32_UINT, ((fxi % fn) + fn) % fn, ((fzi % fn) + fn) % fn, 0);
            u32 bits;
            std::memcpy(&bits, &raw, 4);
            fine = float(bits & 0xFFFFu) * da_snow_cap() / 65535.f;
        }
        Msg("* [qa] window %.2f %.2f: top %.2f base %.2f taper %.2f shelter %.2f opened %.2f fine depth %.3f", x, z, top, b[0],
            b[1], b[2], b[3], fine);
    }
}
} // namespace

void da_ground_window_probe(pcstr args)
{
    SProbeReq r{0.f, 0.f, 0.25f, 0.f, 8};
    if (args)
        sscanf(args, "%f %f %f %f %d", &r.x, &r.z, &r.dx, &r.dz, &r.count);
    r.count = clampr(r.count, 1, 64);
    g_probe_req = r;
    g_probe_pending = true;
}

void da_ground_frame_prepare(CBackend& cmd_list)
{
    if (g_probe_pending)
        window_probe();
    if (!g.tris || !ensure_shaders())
        return;
    // The snow is the window's only reader: nothing is rasterised while it has nothing to draw, and the
    // first snowy frame rebuilds the whole window before the snow reads it.
    if (!da_snow_depth_active())
    {
        g.valid = false;
        return;
    }
    // (the bake may come before the collision mesh: the shells wait for both)
    da_ground_shells_sync();
    const WindowSize& ws = k_sizes[g.preset];
    if ((ws.n != g.n || ws.texel != g.texel || !g.base.live()) && !window_create())
        return;
    // A new terrain upload (a bake): every texel's cover test changes.
    Fvector4 level_xf, level_y;
    const u32 terrain = da_snow_level_terrain(level_xf, level_y);
    if (terrain != g.terrain_version)
    {
        g.terrain_version = terrain;
        g.valid = false;
    }

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
        clear_rect(cmd_list, g.raw_low, strips[i]);
        raster_rect(cmd_list, strips[i]);
    }
    for (u32 i = 0; i < count; ++i)
        resolve_rect(cmd_list, strips[i], window);
}

void da_ground_shells_sync()
{
    const u32 stamp = da_snow_field_stamp();
    if (stamp == g.shell_field)
        return;
    const SSnowField* f = da_snow_field();
    if (!f || !g.tris)
        return;
    shells_build(*f);
    g.shell_field = stamp;
}

u32 da_ground_shells_render(CBackend& cmd_list, float reach)
{
    if (!g.shell_geom || g.shell_chunks.empty())
        return 0;
    const Fvector& cam = Device.vCameraPosition;
    CFrustum frustum;
    frustum.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    cmd_list.set_Geometry(g.shell_geom);
    u32 drawn = 0;
    for (const Ground::ShellChunk& c : g.shell_chunks)
    {
        // the roof's snow raises a chunk by half a metre at most
        Fbox box = c.box;
        box.vMax.y += 0.6f;
        const float dx = std::max(std::max(box.vMin.x - cam.x, cam.x - box.vMax.x), 0.f);
        const float dz = std::max(std::max(box.vMin.z - cam.z, cam.z - box.vMax.z), 0.f);
        if (dx * dx + dz * dz > reach * reach)
            continue;
        u32 mask = FRUSTUM_P_LRTB | FRUSTUM_P_FAR;
        if (frustum.testAABB(&box.vMin.x, mask) == fcvNone)
            continue;
        cmd_list.Render(D3DPT_TRIANGLELIST, c.base_vertex, 0, c.vcount, c.first_index, c.tris);
        ++drawn;
    }
    return drawn;
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
    shells_release();
    g.raw_solid.destroy();
    g.raw_water.destroy();
    g.raw_low.destroy();
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
