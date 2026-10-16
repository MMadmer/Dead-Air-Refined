// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Volumetric seasonal snow, renderer side (docs/dead-air/SNOW.md, da_snow.h).
//
// Products for every shader (r3\da_snow_sample.h): $user$da_snow_level, the current snow depth of the whole
// level at 1 m with the coarse trails, and $user$da_snow_fine, the current depth of the trail window
// around the camera (its season part a cubic B-spline of the level fields). Both keep the previous
// frame's depth in their high half (exact motion vectors). For the snow layer alone: $user$da_snow_trod,
// how much of the fine window's snow prints compacted, and $user$da_snow_road, the distance to a road's
// edge (the wheel tracks).
//
// Frame, first on the main thread before Calculate() (da_snow_frame_publish, from CRender::OnCameraUpdated:
// the sun cascades recorded on worker threads from then on read its constants):
//   1. the level fields of xrEngine's bake (da_snow_field) go to the GPU once per bake, when snow first
//      shows on the level; the passes of the frame are decided, the windows placed, the constants set;
// then on the immediate context (da_snow_frame_prepare):
//   2. the trail windows scroll: the coarse map (L1, 37.5 cm) clears what it enters, the fine one (L0,
//      4.7 cm) takes L1's trails there;
//   3. the deformers of da_snow_stamps() are stamped (L0 near, L1 further out); snowfall refills;
//   4. L0 is pooled into L1 where prints were made;
//   5. the fine and the level depth are recomposited where anything changed, all of it when what the
//      composites read of the season moved, and once more on the next frame so the previous half settles.
// G-buffer: the clipmap snow grid (da_snow_render_grid). After the rain apply: the full-screen snow layer
// (da_snow_apply_layer). Every pass is skipped while the season has no snow.

#include "stdafx.h"
#include "da_snow.h"
#include "da_ground.h"
#include "da_snow_gpu.h"

#include "xrCDB/Frustum.h"
#include "xrEngine/da_snow_field.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include <unordered_set>

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using namespace da_snow_gpu;

constexpr float k_fine_texel = 96.f / 2048.f;    // L0 texel at every preset (4.7 cm)
constexpr float k_coarse_texel = 384.f / 1024.f; // L1 texel: 8 L0 texels (37.5 cm)
constexpr int k_fine_step = 8;                   // L0 texels per window move
constexpr int k_coarse_step = 8;                 // L1 texels per window move
constexpr int k_tile = 64;                       // L0 texels per composite / pool tile
constexpr int k_tile_l1 = k_tile / 8;            // the same tile in L1 texels
constexpr u32 k_max_stamps = 8192;
constexpr float k_carve_rate = 10.f;   // m/s a print sinks at (partial carving): a leg plunges, nothing cuts at once
constexpr float k_rim_reach = 0.12f;   // m around a print its rim reaches (2 L0 texels and change)
constexpr float k_cap_default = 0.536f;
constexpr float k_grid_min = 0.04f;    // the thinnest snow drawn as geometry (m)
constexpr float k_grid_bias = 8.f / 16777216.f;
constexpr u32 k_cells = 32;            // grid cells per tile side
// The shells' reach by preset (m): past it a roof keeps the layer's cover of snow, a couple of pixels thinner. The
// minimum preset draws no grid and no shells: the layer's cover is all the snow there is.
constexpr float k_shell_reach[5] = {0.f, 160.f, 200.f, 260.f, 320.f};
// The grid's reach in the two near sun cascades (m): the walls of a trail and a drift's lee cast their shadows
// there; the first cascade is 20 m deep, its box a little more, and the second takes the same tiles - the band
// where the two blend (dithered) reads both, and with no snow in the second the trails' shadows broke into noise
// from 15 m.
constexpr float k_shadow_reach = 28.f;
// Within this far of the eye every pixel takes the first cascade (m): the second casts the grid from there out.
constexpr float k_shadow_inner = 12.f;
constexpr u32 k_max_tiles = 320;
constexpr u32 k_max_rings = 6;

struct Preset
{
    u32 l0;        // L0 texels per side
    u32 l1;        // L1 texels per side
    u32 grid_n;    // grid cells per ring side; 0 = no grid (the layer only)
    float grid_s0; // finest grid spacing (m)
    u32 rings;
    float reach;   // grid reach (m)
};
// minimum, low, default, high, extreme (docs/dead-air/SNOW.md, presets)
constexpr Preset k_presets[5] = {
    {1024, 512, 0, 0.f, 0, 0.f},
    {1024, 512, 128, 0.25f, 3, 64.f},
    {1536, 1024, 256, 0.25f, 3, 128.f},
    {2048, 1024, 192, 0.125f, 5, 192.f},
    {2048, 1024, 256, 0.125f, 5, 256.f},
};

constexpr pcstr k_name_fine = "$user$da_snow_fine";
constexpr pcstr k_name_roof = "$user$da_snow_roof"; // the prints on the shells, the near window's (da_snow_stamp.cs)
constexpr pcstr k_name_level = "$user$da_snow_level";
constexpr pcstr k_name_l0 = "$user$da_snow_l0";
constexpr pcstr k_name_l1 = "$user$da_snow_l1";
constexpr pcstr k_name_weights = "$user$da_snow_weights";
constexpr pcstr k_name_factors = "$user$da_snow_factors";
constexpr pcstr k_name_base = "$user$da_snow_base";
constexpr pcstr k_name_trod = "$user$da_snow_trod";
constexpr pcstr k_name_road = "$user$da_snow_road";
constexpr pcstr k_name_terrain = "$user$da_snow_ground";
constexpr pcstr k_name_shelter = "$user$da_snow_shelter";
constexpr pcstr k_name_top = "$user$da_snow_top";
constexpr pcstr k_name_walk = "$user$da_snow_walk";
constexpr pcstr k_name_overhang = "$user$da_snow_overhang";
// Yellow snow (stains_build): the stains by 8 m cells (R32_UINT: first << 12 | count) and the stains themselves
// (RGBA32F: x, z, radius, seed).
constexpr pcstr k_name_stain_cells = "$user$da_snow_stain_cells";
constexpr pcstr k_name_stains = "$user$da_snow_stains";
constexpr float k_stain_cell = 8.f;
constexpr pcstr k_name_ground_top = "$user$da_ground_top"; // da_ground.cpp
constexpr pcstr k_name_ground_base = "$user$da_ground_base"; // da_ground.cpp
constexpr pcstr k_name_ground_shore = "$user$da_ground_shore"; // da_ground.cpp
constexpr pcstr k_name_water_class = "$user$water_class";    // r4_water_field.cpp
// Metres to the road's edge the road texture holds at most (unorm 1).
constexpr float k_road_edge_max = 12.75f;

// A toroidal trail map: the world texel (x, z) lives at (x mod n, z mod n).
struct Map
{
    Texture tex;
    u32 n = 0;
    float texel = 0.f;
    int org_x = 0, org_z = 0;
    int next_x = 0, next_z = 0; // where the frame's publish placed it; the prepare scrolls it there
    bool valid = false;

    Fvector4 xf() const { return {1.f / texel, float(n), float(org_x), float(org_z)}; }
    Rect window() const { return {org_x, org_z, org_x + int(n), org_z + int(n)}; }
};

// What the composites read of the season, quantised to what the stored depth can show: S and Mc to one
// u16 depth step at the largest class factor (A_c up to 2.7, M_c up to 2), the residual shares to 1/256,
// the cap. Any change recomposites everything.
struct SeasonKey
{
    s32 v[10]{};
    float cap = 0.f;
    bool operator==(const SeasonKey& o) const { return !memcmp(v, o.v, sizeof(v)) && cap == o.cap; }
};

class CBlender_da_snow_grid : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: snow grid"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        const bool packed = RImplementation.o.gbuffer_opt;
        constexpr u32 all = D3D_COLOR_WRITE_ENABLE_ALL;
        switch (C.iElement)
        {
        case 0:
            C.r_Pass("da_snow_grid", "da_snow_grid", false, TRUE, TRUE);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
            // Targets: packed = position, albedo, velocity; plain = position, normal, albedo. The albedo
            // under the grid stays for the layer's thin-snow see-through. Per-target masks need independent
            // blend: without it every target takes target 0's mask.
            C.RS.SetRS(XRDX11RS_INDEPENDENTBLEND, TRUE);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE, all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE1, packed ? 0u : all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE2, packed ? all : 0u);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE3, 0u);
            C.r_dx11Texture("s_da_snow_fine", k_name_fine);
            C.r_dx11Texture("s_da_snow_level", k_name_level);
            C.r_dx11Texture("s_da_sn_base", k_name_base);
            C.r_dx11Texture("s_da_ground_base", k_name_ground_base);
            C.r_dx11Texture("s_da_ground_shore", k_name_ground_shore);
            C.r_dx11Texture("s_da_sn_weights", k_name_weights);
            // the frozen water the grid crosses (da_snow_ice.h)
            C.r_dx11Texture("s_water_field", r2_RT_water_field);
            C.r_dx11Texture("s_water_class", k_name_water_class);
            C.r_End();
            break;
        case 1:
            // The albedo under the sky pixels, before the grid can cover any of them.
            C.r_Pass("da_fullscreen", "da_snow_skyfill", false, FALSE, FALSE);
            C.r_Stencil(TRUE, D3DCMP_EQUAL, 0xff, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x00);
            C.r_CullMode(D3DCULL_NONE);
            C.RS.SetRS(XRDX11RS_INDEPENDENTBLEND, TRUE);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE, 0u);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE1, packed ? all : 0u);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE2, packed ? 0u : all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE3, 0u);
            C.r_End();
            break;
        case 2:
            // The snow shells (da_ground.cpp): written like the grid, the albedo under them kept.
            C.r_Pass("da_snow_shell", "da_snow_shell", false, TRUE, TRUE);
            C.r_Stencil(TRUE, D3DCMP_ALWAYS, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
            C.RS.SetRS(XRDX11RS_INDEPENDENTBLEND, TRUE);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE, all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE1, packed ? 0u : all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE2, packed ? all : 0u);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE3, 0u);
            C.r_dx11Texture("s_da_sn_factors", k_name_factors);
            C.r_dx11Texture("s_da_sn_roof", k_name_roof);
            if (!RImplementation.o.msaa)
                C.r_dx11Texture("s_depth_copy", r2_RT_depth_copy);
            C.r_End();
            break;
        case 3:
            // The grid in the near sun cascade: depth only.
            C.r_Pass("da_snow_grid_shadow", "null", FALSE, TRUE, TRUE, FALSE);
            C.r_CullMode(D3DCULL_NONE);
            C.r_dx11Texture("s_da_snow_fine", k_name_fine);
            C.r_dx11Texture("s_da_snow_level", k_name_level);
            C.r_dx11Texture("s_da_sn_base", k_name_base);
            C.r_dx11Texture("s_da_ground_base", k_name_ground_base);
            C.r_dx11Texture("s_da_ground_shore", k_name_ground_shore);
            C.r_dx11Texture("s_water_field", r2_RT_water_field);
            C.r_dx11Texture("s_water_class", k_name_water_class);
            C.r_End();
            break;
        default: break;
        }
    }
};

class CBlender_da_snow_layer : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: snow layer"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        const bool packed = RImplementation.o.gbuffer_opt;
        constexpr u32 all = D3D_COLOR_WRITE_ENABLE_ALL;
        constexpr u32 r = D3DCOLORWRITEENABLE_RED, g = D3DCOLORWRITEENABLE_GREEN, a = D3DCOLORWRITEENABLE_ALPHA;
        switch (C.iElement)
        {
        case 0:
            C.r_Pass("da_fullscreen", "da_snow_layer", false, FALSE, FALSE);
            C.r_Stencil(TRUE, D3DCMP_EQUAL, 0x01, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
            C.r_ColorWriteEnable(true, true, true, true);
            C.r_dx11Texture("s_position", r2_RT_P);
            C.r_dx11Texture("s_normal", r2_RT_N);
            C.r_dx11Texture("s_diffuse", r2_RT_albedo);
            C.r_dx11Texture("s_da_snow_fine", k_name_fine);
            C.r_dx11Texture("s_da_snow_level", k_name_level);
            C.r_dx11Texture("s_da_sn_weights", k_name_weights);
            C.r_dx11Texture("s_da_sn_factors", k_name_factors);
            C.r_dx11Texture("s_da_sn_base", k_name_base);
            C.r_dx11Texture("s_da_ground_base", k_name_ground_base);
            C.r_dx11Texture("s_da_ground_shore", k_name_ground_shore);
            C.r_dx11Texture("s_da_sn_trod", k_name_trod);
            C.r_dx11Texture("s_da_sn_road", k_name_road);
            C.r_dx11Texture("s_da_ground_top", k_name_ground_top);
            C.r_dx11Texture("s_da_sn_top", k_name_top);
            C.r_dx11Texture("s_da_sn_roof", k_name_roof);
            C.r_dx11Texture("s_da_sn_stain_cells", k_name_stain_cells);
            C.r_dx11Texture("s_da_sn_stains", k_name_stains);
            C.r_dx11Texture("s_water_field", r2_RT_water_field);
            C.r_dx11Texture("s_water_class", k_name_water_class);
            // The hardware depth tells first-person pixels apart (no single-sampled copy under MSAA).
            if (!RImplementation.o.msaa)
                C.r_dx11Texture("s_depth_copy", r2_RT_depth_copy);
            C.r_End();
            break;
        case 1:
            C.r_Pass("da_fullscreen", "da_snow_apply", false, FALSE, FALSE);
            C.r_Stencil(TRUE, D3DCMP_EQUAL, 0x01, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP);
            C.r_StencilRef(0x01);
            C.r_CullMode(D3DCULL_NONE);
            // packed: position (normal xy and the material word; the depth stays), albedo
            // plain: position (the material only), normal and the sky term, albedo
            // (independent blend: else the albedo would take the position's mask and keep its old blue)
            C.RS.SetRS(XRDX11RS_INDEPENDENTBLEND, TRUE);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE, packed ? (r | g | a) : a);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE1, all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE2, packed ? 0u : all);
            C.RS.SetRS(D3DRS_COLORWRITEENABLE3, 0u);
            C.r_dx11Texture("s_da_sn_tmp0", r2_RT_accum);
            C.r_dx11Texture("s_da_sn_tmp1", RImplementation.o.msaa ? r2_RT_generic1_r : r2_RT_generic1);
            C.r_End();
            break;
        default: break;
        }
    }
};

struct State
{
    u32 preset = 4;
    u32 preset_pending = 4; // da_snow_apply_preset, taken at the next frame

    // passes
    Compute cs_clear, cs_stamp, cs_scroll, cs_refill, cs_pool, cs_composite, cs_level;
    CBlender_da_snow_grid b_grid;
    CBlender_da_snow_layer b_layer;
    ref_shader s_grid, s_layer;
    bool shaders_tried = false;
    bool shaders_ok = false;

    // the level's bake
    u32 field_stamp = u32(-1);
    bool field = false;
    bool uploaded = false;
    u32 dim_x = 0, dim_z = 0;
    float texel = 1.f;
    Fvector2 origin{};
    float ymin = 0.f, yrange = 1.f;
    Texture weights, factors, base, level, road, terrain, shelter, top, walk, overhang;
    Texture stain_cells, stains;
    xr_vector<Fvector4> stain_list; // for r__snow_stats
    Fvector4 stain_xf{}; // xy the stain grid's origin, z its cell (m), w cells per row (0: none)
    u32 terrain_version = 0; // counts the terrain uploads (da_snow_level_terrain)
    bool level_fresh = true;

    // trails
    Map l0, l1;
    Texture fine, trod, roof;
    bool fine_fresh = true;
    float refill_acc = 0.f;
    bool trails_live = false;

    // frame history
    SeasonKey season{};
    bool season_moved_prev = true;
    bool refilled_prev = false;
    bool l1_changed_prev = false;
    Rect level_rect_prev{};
    xr_vector<u32> dirty, dirty_prev, tiles;
    xr_vector<Fvector4> stamps;
    Buffer stamp_buf, tile_buf;

    // this frame
    u32 publish_frame = u32(-1);
    const SSnowFrame* frame = nullptr;
    float cap = k_cap_default;
    float share_open = 0.5f, share_road = 0.9f;
    float rho_open = 250.f; // kg/m3 of the open snow now: how far a planted foot sinks into it
    bool layer_on = false;
    bool depth_on = false;
    Fvector4 fine_xf{}, level_xf{}, misc{};

    // grid
    ID3D11Buffer* grid_vb = nullptr;
    ID3D11Buffer* grid_ib = nullptr;
    ref_geom grid_geom;
    ref_geom quad_geom;

    // statistics of the last frame (r__snow_stats)
    u32 stat_stamps_near = 0, stat_stamps_far = 0, stat_tiles = 0, stat_level_texels = 0, stat_grid_tiles = 0;
    u32 stat_shell_chunks = 0;
    bool stat_full = false;
};
State g;

bool ensure_shaders()
{
    if (g.shaders_tried)
        return g.shaders_ok;
    g.shaders_tried = true;
    const std::pair<pcstr, pcstr> fields[] = {{"s_da_sn_weights", k_name_weights}, {"s_da_sn_factors", k_name_factors}};
    const std::pair<pcstr, pcstr> bases[] = {{"s_da_sn_base", k_name_base}, {"s_da_ground_base", k_name_ground_base}};
    // the shelter of covers: the window's (in its base) and the level's
    const std::pair<pcstr, pcstr> shelter = {"s_da_sn_shelter", k_name_shelter};
    bool ok = g.cs_clear.create("da_snow_clear", {});
    ok = ok && g.cs_stamp.create("da_snow_stamp", {fields[0], fields[1], bases[0], bases[1], shelter, {"s_da_sn_top", k_name_top}, {"s_da_sn_terrain", k_name_terrain}});
    ok = ok && g.cs_scroll.create("da_snow_scroll", {{"s_da_sn_l1", k_name_l1}});
    ok = ok && g.cs_refill.create("da_snow_refill", {});
    ok = ok && g.cs_pool.create("da_snow_pool", {{"s_da_sn_l0", k_name_l0}, fields[0], fields[1], bases[1], shelter});
    ok = ok && g.cs_composite.create("da_snow_composite", {{"s_da_sn_l0", k_name_l0}, fields[0], fields[1], bases[1], shelter});
    ok = ok && g.cs_level.create("da_snow_level", {{"s_da_sn_l1", k_name_l1}, fields[0], fields[1], shelter});
    for (pcstr file : {"da_snow_grid.vs", "da_snow_grid.ps", "da_snow_skyfill.ps", "da_snow_layer.ps", "da_snow_apply.ps",
             "da_fullscreen.vs", "da_snow_shell.vs", "da_snow_shell.ps", "da_snow_grid_shadow.vs"})
        ok = ok && shader_exists(file);
    if (ok)
    {
        g.s_grid.create(&g.b_grid, "r2" DELIMITER "da_snow_grid");
        g.s_layer.create(&g.b_layer, "r2" DELIMITER "da_snow_layer");
        ok = g.s_grid && g.s_grid->E[0] && g.s_grid->E[1] && g.s_grid->E[2] && g.s_grid->E[3] && g.s_layer &&
            g.s_layer->E[0] && g.s_layer->E[1];
    }
    if (!ok)
        Msg("! [snow] the snow shaders are missing from the content: no snow");
    g.shaders_ok = ok;
    return ok;
}

// The names every consumer binds resolve from the first frame on, as empty textures.
void placeholders()
{
    if (!g.fine.named)
        g.fine.placeholder(k_name_fine, DXGI_FORMAT_R32_UINT);
    if (!g.level.named)
        g.level.placeholder(k_name_level, DXGI_FORMAT_R32_UINT);
    if (!g.weights.named)
        g.weights.placeholder(k_name_weights, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!g.factors.named)
        g.factors.placeholder(k_name_factors, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!g.base.named)
        g.base.placeholder(k_name_base, DXGI_FORMAT_R16_UNORM);
    if (!g.l0.tex.named)
        g.l0.tex.placeholder(k_name_l0, DXGI_FORMAT_R32_UINT);
    if (!g.l1.tex.named)
        g.l1.tex.placeholder(k_name_l1, DXGI_FORMAT_R32_UINT);
    if (!g.trod.named)
        g.trod.placeholder(k_name_trod, DXGI_FORMAT_R8_UNORM);
    if (!g.roof.named)
        g.roof.placeholder(k_name_roof, DXGI_FORMAT_R32_UINT);
    if (!g.road.named)
        g.road.placeholder(k_name_road, DXGI_FORMAT_R8_UNORM);
    if (!g.terrain.named)
        g.terrain.placeholder(k_name_terrain, DXGI_FORMAT_R16_UNORM);
    if (!g.shelter.named)
        g.shelter.placeholder(k_name_shelter, DXGI_FORMAT_R8_UNORM);
    if (!g.top.named)
        g.top.placeholder(k_name_top, DXGI_FORMAT_R16_UNORM);
    if (!g.walk.named)
        g.walk.placeholder(k_name_walk, DXGI_FORMAT_R16_UNORM);
    if (!g.overhang.named)
        g.overhang.placeholder(k_name_overhang, DXGI_FORMAT_R8_UNORM);
    if (!g.stain_cells.named)
        g.stain_cells.placeholder(k_name_stain_cells, DXGI_FORMAT_R32_UINT);
    if (!g.stains.named)
        g.stains.placeholder(k_name_stains, DXGI_FORMAT_R32G32B32A32_FLOAT);
}

void trails_release()
{
    g.fine.placeholder(k_name_fine, DXGI_FORMAT_R32_UINT);
    g.trod.placeholder(k_name_trod, DXGI_FORMAT_R8_UNORM);
    g.roof.placeholder(k_name_roof, DXGI_FORMAT_R32_UINT);
    g.l0.tex.placeholder(k_name_l0, DXGI_FORMAT_R32_UINT);
    g.l1.tex.placeholder(k_name_l1, DXGI_FORMAT_R32_UINT);
    g.l0.n = g.l1.n = 0;
    g.l0.valid = g.l1.valid = false;
    g.trails_live = false;
    g.fine_fresh = true;
    g.refill_acc = 0.f;
    g.dirty.clear();
    g.dirty_prev.clear();
}

void level_release()
{
    g.weights.placeholder(k_name_weights, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.factors.placeholder(k_name_factors, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.base.placeholder(k_name_base, DXGI_FORMAT_R16_UNORM);
    g.level.placeholder(k_name_level, DXGI_FORMAT_R32_UINT);
    g.road.placeholder(k_name_road, DXGI_FORMAT_R8_UNORM);
    g.terrain.placeholder(k_name_terrain, DXGI_FORMAT_R16_UNORM);
    g.shelter.placeholder(k_name_shelter, DXGI_FORMAT_R8_UNORM);
    g.top.placeholder(k_name_top, DXGI_FORMAT_R16_UNORM);
    g.walk.placeholder(k_name_walk, DXGI_FORMAT_R16_UNORM);
    g.overhang.placeholder(k_name_overhang, DXGI_FORMAT_R8_UNORM);
    g.stain_cells.placeholder(k_name_stain_cells, DXGI_FORMAT_R32_UINT);
    g.stains.placeholder(k_name_stains, DXGI_FORMAT_R32G32B32A32_FLOAT);
    g.stain_xf.set(0.f, 0.f, 0.f, 0.f);
    g.uploaded = false;
    g.level_fresh = true;
    g.season_moved_prev = true;
    g.refilled_prev = false;
    g.l1_changed_prev = false;
    g.level_rect_prev = {};
}

void sync_field()
{
    const u32 stamp = da_snow_field_stamp();
    if (stamp == g.field_stamp)
        return;
    g.field_stamp = stamp;
    level_release();
    trails_release();
    const SSnowField* f = da_snow_field();
    const size_t cells = f ? size_t(f->dim_x) * f->dim_z : 0;
    g.field = f && cells && f->weights.size() == cells && f->factors.size() == cells && f->top.size() == cells &&
        f->texel > 0.f;
    if (!g.field)
        return;
    g.dim_x = f->dim_x;
    g.dim_z = f->dim_z;
    g.texel = f->texel;
    g.origin = f->origin;
}

// Felzenszwalb and Huttenlocher's squared distance transform of n samples of f (stride apart) in place:
// each sample becomes the least (q - p)^2 + f(p). v and z are scratch of n and n + 1 entries.
void distance_1d(float* f, size_t stride, int n, xr_vector<float>& d, xr_vector<int>& v, xr_vector<float>& z)
{
    const auto at = [&](int i) { return f[size_t(i) * stride]; };
    // where the parabolas of samples q and p meet
    const auto meet = [&](int q, int p) { return ((at(q) + float(q) * q) - (at(p) + float(p) * p)) / float(2 * (q - p)); };
    int k = 0;
    v[0] = 0;
    z[0] = -flt_max;
    z[1] = flt_max;
    for (int q = 1; q < n; ++q)
    {
        float s = meet(q, v[k]);
        while (s <= z[k])
            s = meet(q, v[--k]);
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = flt_max;
    }
    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < float(q))
            ++k;
        const float dq = float(q - v[k]);
        d[q] = dq * dq + at(v[k]);
    }
    for (int q = 0; q < n; ++q)
        f[size_t(q) * stride] = d[q];
}

// The road texture: metres from a road texel's centre to the road's edge (half a texel short of the
// nearest texel outside the road), unorm over k_road_edge_max; 0 off the roads. The snow layer lays the
// wheel tracks along its iso-lines.
void road_edges(const SSnowField& f, xr_vector<u8>& out)
{
    const int w = int(f.dim_x), h = int(f.dim_z);
    const size_t cells = size_t(w) * h;
    // squared distances in texels; far enough for every texel the texture can hold
    const float far2 = _sqr(k_road_edge_max / f.texel + 2.f);
    xr_vector<float> sq(cells);
    for (size_t i = 0; i < cells; ++i)
        sq[i] = ((f.weights[i] >> 16) & 0xFFu) >= 128u ? far2 : 0.f;
    const int n = std::max(w, h);
    xr_vector<float> d(n);
    xr_vector<int> v(n);
    xr_vector<float> z(size_t(n) + 1);
    for (int x = 0; x < w; ++x)
        distance_1d(&sq[x], size_t(w), h, d, v, z);
    for (int y = 0; y < h; ++y)
        distance_1d(&sq[size_t(y) * w], 1, w, d, v, z);
    out.resize(cells);
    for (size_t i = 0; i < cells; ++i)
    {
        const float metres = std::max(_sqrt(std::min(sq[i], far2)) - 0.5f, 0.f) * f.texel;
        out[i] = u8(clampr(metres / k_road_edge_max, 0.f, 1.f) * 255.f + 0.5f);
    }
}

// The level's textures, once per bake: weights and factors as baked, the snow base (the topmost surface
// opened with a 3 x 3 square at 1 m, the terrain under a cover), the shelter of the covers (their mask
// eroded by a texel: walls and posts drop out), the terrain and the topmost surface themselves, the road
// edges, the depth texture the composite fills.
// Yellow snow: where a man or a dog relieved himself - rarely, at the foot of a tree or a stump in the open, more
// rarely at a static object's or a wall's foot, never amid the houses (by the overhangs' density round it). A stain
// stands a little off what it was made against, some 20 to 60 cm across its core (the photo pack: a darker amber core
// melted into the snow, a pale lemon halo, a scatter of drops; at a tree's foot, a bench's leg, a path's edge).
void stains_build(const SSnowField& f)
{
    const u32 w = f.dim_x, h = f.dim_z;
    const size_t cells = size_t(w) * h;
    if (f.cover.size() != cells)
        return;
    // the overhangs' summed-area table: how built-up the 41 x 41 m round a point is
    xr_vector<u32> sat((size_t(w) + 1) * (h + 1), 0);
    for (u32 z = 0; z < h; ++z)
        for (u32 x = 0; x < w; ++x)
            sat[size_t(z + 1) * (w + 1) + x + 1] = (f.cover[size_t(z) * w + x] == 2 ? 1u : 0u) +
                sat[size_t(z) * (w + 1) + x + 1] + sat[size_t(z + 1) * (w + 1) + x] - sat[size_t(z) * (w + 1) + x];
    const auto built = [&](float wx, float wz) {
        const int cx = int((wx - f.origin.x) / f.texel), cz = int((wz - f.origin.y) / f.texel);
        const int x0 = clampr(cx - 20, 0, int(w)), x1 = clampr(cx + 21, 0, int(w));
        const int z0 = clampr(cz - 20, 0, int(h)), z1 = clampr(cz + 21, 0, int(h));
        return sat[size_t(z1) * (w + 1) + x1] - sat[size_t(z0) * (w + 1) + x1] - sat[size_t(z1) * (w + 1) + x0] +
            sat[size_t(z0) * (w + 1) + x0];
    };
    const auto hash = [](float a, float b, u32 salt) {
        u32 v = u32(int(std::floor(a * 4.f))) * 73856093u ^ u32(int(std::floor(b * 4.f))) * 19349663u ^ salt * 83492791u;
        v ^= v >> 13;
        v *= 0x5bd1e995u;
        v ^= v >> 15;
        return float(v & 0xFFFFFFu) / 16777216.f;
    };
    xr_vector<Fvector4> list;
    // at least 6 m apart (a tree's several visuals share a root; a group of blocks gave a cluster of stains)
    std::unordered_set<u64> taken;
    const auto add = [&](float x, float z, u32 salt) {
        const int kx = int(std::floor(x / 6.f)), kz = int(std::floor(z / 6.f));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (taken.count((u64(u32(kx + dx)) << 32) | u32(kz + dz)))
                    return;
        taken.insert((u64(u32(kx)) << 32) | u32(kz));
        const float r = 0.16f + 0.32f * hash(x, z, salt + 7u);
        list.push_back({x, z, r, hash(x, z, salt + 11u)});
    };
    // trees and stumps: one in twenty, in the open
    if (g_pGamePersistent)
        for (const Fvector& p : g_pGamePersistent->Environment().wind_veg_trees)
        {
            if (hash(p.x, p.z, 1u) > 0.05f || built(p.x, p.z) > 60u)
                continue;
            const float a = hash(p.x, p.z, 2u) * PI_MUL_2, d = 0.35f + 0.55f * hash(p.x, p.z, 3u);
            add(p.x + std::sin(a) * d, p.z + std::cos(a) * d, 4u);
        }
    // the foot of a static object or a wall: an edge texel of a cover, one in a hundred (an object) or two hundred
    // (a building) of the 4 m cells along it, away from the built-up middle
    static const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (u32 z = 1; z + 1 < h; ++z)
        for (u32 x = 1; x + 1 < w; ++x)
        {
            const u8 c = f.cover[size_t(z) * w + x];
            if (!c)
                continue;
            for (const auto& d : nb)
            {
                if (f.cover[size_t(int(z) + d[1]) * w + size_t(int(x) + d[0])])
                    continue;
                const float cx = f.origin.x + (float(x) + 0.5f) * f.texel, cz = f.origin.y + (float(z) + 0.5f) * f.texel;
                const u32 side = u32(d[0] + 2) * 3u + u32(d[1] + 2);
                if (hash(std::floor(cx / 4.f), std::floor(cz / 4.f), 5u + side) > (c == 1 ? 0.01f : 0.005f))
                    continue;
                if (built(cx, cz) > (c == 1 ? 60u : 250u))
                    continue;
                const float off = 0.5f * f.texel + 0.25f + 0.4f * hash(cx, cz, 6u);
                const float along = (hash(cx, cz, 8u) - 0.5f) * f.texel;
                add(cx + float(d[0]) * off + float(d[1]) * along, cz + float(d[1]) * off + float(d[0]) * along, 9u);
            }
        }
    // by 8 m cells
    const u32 gw = u32(std::ceil(float(w) * f.texel / k_stain_cell)) + 1;
    const u32 gh = u32(std::ceil(float(h) * f.texel / k_stain_cell)) + 1;
    xr_vector<xr_vector<u32>> bins(size_t(gw) * gh);
    for (u32 i = 0; i < u32(list.size()); ++i)
    {
        const int bx = int((list[i].x - f.origin.x) / k_stain_cell), bz = int((list[i].y - f.origin.y) / k_stain_cell);
        if (bx >= 0 && bz >= 0 && bx < int(gw) && bz < int(gh))
            bins[size_t(bz) * gw + bx].push_back(i);
    }
    xr_vector<u32> grid(size_t(gw) * gh, 0);
    xr_vector<Fvector4> sorted;
    for (size_t b = 0; b < bins.size(); ++b)
    {
        const u32 n = std::min<u32>(u32(bins[b].size()), 8u);
        if (sorted.size() + n > 16384u)
            break;
        grid[b] = (u32(sorted.size()) << 12) | n;
        for (u32 k = 0; k < n; ++k)
            sorted.push_back(list[bins[b][k]]);
    }
    if (sorted.empty())
        sorted.push_back({0.f, 0.f, 0.f, 0.f});
    const u32 sw = u32(sorted.size());
    g.stain_list = list;
    if (g.stain_cells.create(k_name_stain_cells, gw, gh, DXGI_FORMAT_R32_UINT, false, grid.data(), gw * 4) &&
        g.stains.create(k_name_stains, sw, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false, sorted.data(), sw * 16))
        g.stain_xf.set(f.origin.x, f.origin.y, k_stain_cell, float(gw));
    Msg("* [snow] yellow snow: %u stain(s) (%u tree positions)", u32(list.size()),
        g_pGamePersistent ? u32(g_pGamePersistent->Environment().wind_veg_trees.size()) : 0u);
}

bool upload_level()
{
    const SSnowField* f = da_snow_field();
    if (!f)
        return false;
    CTimer timer;
    timer.Start();
    const u32 w = g.dim_x, h = g.dim_z;
    const size_t cells = size_t(w) * h;

    xr_vector<float> top(cells), ground(cells);
    float lo = flt_max, hi = -flt_max;
    const bool has_ground = f->ground.size() == cells;
    const bool has_solid = f->solid.size() == cells;
    const auto valid = [](float y) { return y > -1e4f && y < 1e5f; };
    for (size_t i = 0; i < cells; ++i)
    {
        float y = f->top[i];
        if (!valid(y) && has_ground)
            y = f->ground[i];
        if (!valid(y))
            y = flt_max;
        top[i] = y;
        ground[i] = has_ground && valid(f->ground[i]) ? f->ground[i] : y;
        for (const float v : {y, ground[i]})
            if (v != flt_max)
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
    }
    if (lo > hi)
    {
        lo = 0.f;
        hi = 1.f;
    }
    for (size_t i = 0; i < cells; ++i)
    {
        if (top[i] == flt_max)
            top[i] = lo;
        if (ground[i] == flt_max)
            ground[i] = top[i];
    }
    // Covers: the bake's (da_snow_cover_at: a solid top a cover's height over the terrain, no deck the AI walks), else
    // the height alone. The ground's snow lies on the terrain there, and the cover keeps the sky's snow off it.
    const bool baked = f->cover.size() == cells && f->shelter.size() == cells;
    xr_vector<u8> cover(cells, 0);
    for (size_t i = 0; i < cells; ++i)
        cover[i] = baked ? f->cover[i] : ((!has_solid || f->solid[i]) && top[i] - ground[i] > da_snow_cover_over ? 1 : 0);
    xr_vector<float> snow_base(cells);
    for (size_t i = 0; i < cells; ++i)
        snow_base[i] = cover[i] ? ground[i] : top[i];
    // opening: erosion then dilation, each separable over 3 texels
    xr_vector<float> tmp(cells);
    const auto pass = [&](const xr_vector<float>& src, xr_vector<float>& dst, bool horizontal, bool erode) {
        for (u32 z = 0; z < h; ++z)
            for (u32 x = 0; x < w; ++x)
            {
                float v = src[size_t(z) * w + x];
                for (int d = -1; d <= 1; d += 2)
                {
                    const int sx = int(x) + (horizontal ? d : 0), sz = int(z) + (horizontal ? 0 : d);
                    if (sx < 0 || sz < 0 || sx >= int(w) || sz >= int(h))
                        continue;
                    const float s = src[size_t(sz) * w + sx];
                    v = erode ? std::min(v, s) : std::max(v, s);
                }
                dst[size_t(z) * w + x] = v;
            }
    };
    pass(snow_base, tmp, true, true);
    pass(tmp, snow_base, false, true);
    pass(snow_base, tmp, true, false);
    pass(tmp, snow_base, false, false);
    // the shelter: the cover mask eroded by one texel (a 3 x 3 minimum)
    xr_vector<u8> shelter = baked ? f->shelter : xr_vector<u8>(cells, 0);
    for (u32 z = 1; !baked && z + 1 < h; ++z)
        for (u32 x = 1; x + 1 < w; ++x)
        {
            u8 v = 255;
            for (int dz = -1; dz <= 1 && v; ++dz)
                for (int dx = -1; dx <= 1 && v; ++dx)
                    if (!cover[size_t(z + dz) * w + x + dx])
                        v = 0;
            shelter[size_t(z) * w + x] = v;
        }

    g.ymin = lo - 1.f;
    g.yrange = std::max(hi - lo + 2.f, 1.f);
    const auto encode = [](float y) { return u16(std::max(clampr((y - g.ymin) / g.yrange, 0.f, 1.f) * 65535.f + 0.5f, 1.f)); };
    xr_vector<u16> base(cells), terrain(cells), top16(cells), walk(cells, 0);
    // The covers' kinds (da_snow_cover_at) for the window's resolve - 255 an overhang (it shelters the ground under
    // it), 128 a solid object -, dilated by a texel: an eave's texels at the 1 m cell's edge are the roof's.
    xr_vector<u8> overhang(cells, 0);
    for (u32 z = 0; z < h; ++z)
        for (u32 x = 0; x < w; ++x)
        {
            u8 v = 0;
            for (int dz = -1; dz <= 1 && v < 255; ++dz)
                for (int dx = -1; dx <= 1 && v < 255; ++dx)
                {
                    const int sx = int(x) + dx, sz = int(z) + dz;
                    if (sx < 0 || sz < 0 || sx >= int(w) || sz >= int(h))
                        continue;
                    const size_t j = size_t(sz) * w + sx;
                    const u8 kind = baked ? f->cover[j] : u8(cover[j] != 0 ? 2 : 0);
                    v = std::max<u8>(v, kind == 2 ? 255 : (kind == 1 ? 128 : 0));
                }
            overhang[size_t(z) * w + x] = v;
        }
    // 0 is reserved: no collision under the texel (the backdrop beyond the playable area, where top is
    // the hole-filled ground). The layer still paints snow there; the grid does not stand on it.
    for (size_t i = 0; i < cells; ++i)
    {
        base[i] = (has_solid && !f->solid[i]) ? u16(0) : encode(snow_base[i]);
        terrain[i] = encode(ground[i]);
        top16[i] = encode(top[i]);
        // the highest surface the AI walks on (0: none), for the window's cover test (da_ground_resolve.cs)
        if (f->walk.size() == cells && f->walk[i] > -1e4f)
            walk[i] = encode(f->walk[i]);
    }

    xr_vector<u8> road;
    road_edges(*f, road);

    const bool ok = g.weights.create(k_name_weights, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false, f->weights.data(), w * 4) &&
        g.factors.create(k_name_factors, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false, f->factors.data(), w * 4) &&
        g.base.create(k_name_base, w, h, DXGI_FORMAT_R16_UNORM, false, base.data(), w * 2) &&
        g.road.create(k_name_road, w, h, DXGI_FORMAT_R8_UNORM, false, road.data(), w) &&
        g.terrain.create(k_name_terrain, w, h, DXGI_FORMAT_R16_UNORM, false, terrain.data(), w * 2) &&
        g.shelter.create(k_name_shelter, w, h, DXGI_FORMAT_R8_UNORM, false, shelter.data(), w) &&
        g.top.create(k_name_top, w, h, DXGI_FORMAT_R16_UNORM, false, top16.data(), w * 2) &&
        g.walk.create(k_name_walk, w, h, DXGI_FORMAT_R16_UNORM, false, walk.data(), w * 2) &&
        g.overhang.create(k_name_overhang, w, h, DXGI_FORMAT_R8_UNORM, false, overhang.data(), w) &&
        g.level.create(k_name_level, w, h, DXGI_FORMAT_R32_UINT, true);
    if (!ok)
    {
        Msg("! [snow] the %ux%u level fields could not be uploaded", w, h);
        level_release();
        g.field = false;
        return false;
    }
    g.uploaded = true;
    g.level_fresh = true;
    ++g.terrain_version;
    stains_build(*f);
    Msg("* [snow] level fields %ux%u at %.2f m, base %.1f..%.1f m, %.1f MB, %.0f ms", w, h, g.texel, g.ymin,
        g.ymin + g.yrange, cells * 23.f / (1024.f * 1024.f), timer.GetElapsed_sec() * 1000.f);
    // the snow on the covers, from the same bake
    da_ground_shells_sync();
    return true;
}

bool trails_create()
{
    const Preset& p = k_presets[g.preset];
    const bool ok = g.l0.tex.create(k_name_l0, p.l0, p.l0, DXGI_FORMAT_R32_UINT, true) &&
        g.l1.tex.create(k_name_l1, p.l1, p.l1, DXGI_FORMAT_R32_UINT, true) &&
        g.fine.create(k_name_fine, p.l0, p.l0, DXGI_FORMAT_R32_UINT, true) &&
        g.trod.create(k_name_trod, p.l0, p.l0, DXGI_FORMAT_R8_UNORM, true) &&
        g.roof.create(k_name_roof, p.l0, p.l0, DXGI_FORMAT_R32_UINT, true);
    if (!ok)
    {
        Msg("! [snow] the trail maps (%u / %u) could not be created", p.l0, p.l1);
        trails_release();
        return false;
    }
    g.l0.n = p.l0;
    g.l0.texel = k_fine_texel;
    g.l1.n = p.l1;
    g.l1.texel = k_coarse_texel;
    g.l0.valid = g.l1.valid = false;
    g.fine_fresh = true;
    g.trails_live = true;
    return true;
}

void grid_create()
{
    if (g.grid_geom)
        return;
    constexpr u32 side = k_cells + 1;
    xr_vector<Fvector2> verts(side * side);
    for (u32 j = 0; j < side; ++j)
        for (u32 i = 0; i < side; ++i)
            verts[j * side + i].set(float(i), float(j));
    xr_vector<u16> indices;
    indices.reserve(k_cells * k_cells * 6 + (k_cells / 2) * (k_cells / 2) * 6);
    // the full cells, then cells of every other vertex (the far shadow cascade's: its texels are coarser)
    for (u32 step = 1; step <= 2; ++step)
        for (u32 j = 0; j < k_cells; j += step)
            for (u32 i = 0; i < k_cells; i += step)
            {
                const u16 v00 = u16(j * side + i), v10 = u16(v00 + step), v01 = u16(v00 + step * side), v11 = u16(v01 + step);
                // the diagonal alternates, so no slope gets a uniform grain
                if (((i + j) / step) & 1)
                    indices.insert(indices.end(), {v00, v01, v10, v10, v01, v11});
                else
                    indices.insert(indices.end(), {v00, v01, v11, v00, v11, v10});
            }
    D3D11_BUFFER_DESC desc{};
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.ByteWidth = UINT(verts.size() * sizeof(Fvector2));
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA vdata{verts.data(), 0, 0};
    D3D11_BUFFER_DESC idesc = desc;
    idesc.ByteWidth = UINT(indices.size() * sizeof(u16));
    idesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA idata{indices.data(), 0, 0};
    if (FAILED(HW.pDevice->CreateBuffer(&desc, &vdata, &g.grid_vb)) || FAILED(HW.pDevice->CreateBuffer(&idesc, &idata, &g.grid_ib)))
    {
        release(g.grid_vb);
        release(g.grid_ib);
        return;
    }
    static const VertexElement decl[] = {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()};
    g.grid_geom.create(decl, g.grid_vb, g.grid_ib);
}

void quad(CBackend& cmd)
{
    if (!g.quad_geom)
        g.quad_geom.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    // da_fullscreen.vs: xy clip position, zw texture coordinate
    u32 offset = 0;
    FVF::TL* pv = static_cast<FVF::TL*>(RImplementation.Vertex.Lock(4, g.quad_geom->vb_stride, offset));
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f);
    pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f);
    pv++;
    RImplementation.Vertex.Unlock(4, g.quad_geom->vb_stride);
    cmd.set_Geometry(g.quad_geom);
    cmd.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
}

u32 pack_tile(int x, int z) { return u32(x + 32768) | (u32(z + 32768) << 16); }

// L0 tiles a world rectangle (metres) touches, inside the L0 window.
void add_tiles(const Fvector2& lo, const Fvector2& hi, xr_vector<u32>& out)
{
    const Rect w = g.l0.window();
    const int x0 = std::max(int(std::floor(lo.x / k_fine_texel)), w.x0), x1 = std::min(int(std::floor(hi.x / k_fine_texel)), w.x1 - 1);
    const int z0 = std::max(int(std::floor(lo.y / k_fine_texel)), w.z0), z1 = std::min(int(std::floor(hi.y / k_fine_texel)), w.z1 - 1);
    if (x0 > x1 || z0 > z1)
        return;
    for (int tz = floor_div(z0, k_tile); tz <= floor_div(z1, k_tile); ++tz)
        for (int tx = floor_div(x0, k_tile); tx <= floor_div(x1, k_tile); ++tx)
            out.push_back(pack_tile(tx, tz));
}

void add_rect_tiles(const Rect& r, xr_vector<u32>& out)
{
    if (r.empty())
        return;
    for (int tz = floor_div(r.z0, k_tile); tz <= floor_div(r.z1 - 1, k_tile); ++tz)
        for (int tx = floor_div(r.x0, k_tile); tx <= floor_div(r.x1 - 1, k_tile); ++tx)
            out.push_back(pack_tile(tx, tz));
}

void unique(xr_vector<u32>& v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// The frame's constants every snow pass reads (r3\da_snow_sample.h, da_snow_common.h).
void set_common(CBackend& cmd)
{
    const SSnowFrame& fr = *g.frame;
    cmd.set_c("da_snow_fine_xf", g.fine_xf);
    cmd.set_c("da_snow_level_xf", g.level_xf);
    cmd.set_c("da_snow_misc", g.misc);
    cmd.set_c("da_snow_S", fr.S);
    cmd.set_c("da_snow_Mc", fr.Mc);
    cmd.set_c("da_snow_S_prev", fr.S_prev);
    cmd.set_c("da_snow_Mc_prev", fr.Mc_prev);
    cmd.set_c("da_sn_lvl", g.ymin, g.yrange, 0.f, 0.f);
    Fvector4 win, win2;
    da_ground_window(win, win2);
    win2.z = float(ps_r__dbg_snow);
    cmd.set_c("da_sn_win", win);
    cmd.set_c("da_sn_win2", win2);
}

void set_map(CBackend& cmd, pcstr name, const Map& m) { cmd.set_c(name, m.xf()); }

void clear_map(CBackend& cmd, const Map& m, const Rect& r, bool untouched)
{
    dispatch(cmd, g.cs_clear, u32(r.width() + 7) / 8, u32(r.height() + 7) / 8, 1, {}, {m.tex.uav}, [&] {
        cmd.set_c("da_sn_rect", float(r.x0), float(r.z0), float(r.width()), float(r.height()));
        cmd.set_c("da_sn_clear", float(m.n), untouched ? 1.f : 0.f, 0.f, 0.f);
    });
}

// Where a window goes this frame: centred on the camera, in whole steps.
void place_window(Map& m, int step)
{
    const Fvector& cam = Device.vCameraPosition;
    const int half = int(m.n) / 2;
    m.next_x = floor_div(int(std::floor(cam.x / m.texel)) - half, step) * step;
    m.next_z = floor_div(int(std::floor(cam.z / m.texel)) - half, step) * step;
}

// Moves a window where the publish placed it; the rectangles it newly covers.
u32 move_window(Map& m, Rect out[2])
{
    const u32 count = window_strips(m.org_x, m.org_z, m.next_x, m.next_z, int(m.n), m.valid, out);
    m.org_x = m.next_x;
    m.org_z = m.next_z;
    m.valid = true;
    return count;
}

SeasonKey season_key(const SSnowFrame& fr)
{
    const float depth_step = g.cap / 65535.f / 2.7f;
    const auto q = [](float v, float step) { return s32(std::floor(v / step + 0.5f)); };
    SeasonKey k;
    const Fvector4* vectors[2] = {&fr.S, &fr.Mc};
    for (u32 i = 0; i < 2; ++i)
    {
        k.v[i * 4 + 0] = q(vectors[i]->x, depth_step);
        k.v[i * 4 + 1] = q(vectors[i]->y, depth_step);
        k.v[i * 4 + 2] = q(vectors[i]->z, depth_step);
        k.v[i * 4 + 3] = q(vectors[i]->w, depth_step);
    }
    k.v[8] = q(g.share_open, 1.f / 256.f);
    k.v[9] = q(g.share_road, 1.f / 256.f);
    k.cap = g.cap;
    return k;
}

void update_trails(CBackend& cmd, Rect fresh[2], u32& fresh_count, bool& l1_changed, bool& refilled)
{
    const SSnowFrame& fr = *g.frame;
    const float dt = clampr(Device.fTimeDelta, 0.f, 0.1f);

    // 2. windows
    Rect r1[2];
    const u32 n1 = move_window(g.l1, r1);
    for (u32 i = 0; i < n1; ++i)
        clear_map(cmd, g.l1, r1[i], true);
    fresh_count = move_window(g.l0, fresh);
    for (u32 i = 0; i < fresh_count; ++i)
    {
        const Rect& r = fresh[i];
        dispatch(cmd, g.cs_clear, u32(r.width() + 7) / 8, u32(r.height() + 7) / 8, 1, {}, {g.roof.uav}, [&] {
            cmd.set_c("da_sn_rect", float(r.x0), float(r.z0), float(r.width()), float(r.height()));
            cmd.set_c("da_sn_clear", float(g.l0.n), 0.f, 0.f, 0.f);
        });
        dispatch(cmd, g.cs_scroll, u32(r.width() + 7) / 8, u32(r.height() + 7) / 8, 1, {}, {g.l0.tex.uav}, [&] {
            set_common(cmd);
            cmd.set_c("da_sn_rect", float(r.x0), float(r.z0), float(r.width()), float(r.height()));
            set_map(cmd, "da_sn_map", g.l0);
            set_map(cmd, "da_sn_map1", g.l1);
        });
    }
    l1_changed = n1 > 0;

    // 3. stamps: the ones reaching into L0 first, then the ones only L1 holds
    const xr_vector<SSnowStamp>& src = da_snow_stamps();
    g.stamps.clear();
    g.dirty.clear();
    const Rect w0 = g.l0.window(), w1 = g.l1.window();
    // a stamp is the capsule from xz0 to xz
    const auto overlaps = [](const Rect& w, float texel, const SSnowStamp& s) {
        return std::max(s.xz.x, s.xz0.x) + s.radius >= w.x0 * texel && std::min(s.xz.x, s.xz0.x) - s.radius < w.x1 * texel &&
            std::max(s.xz.y, s.xz0.y) + s.radius >= w.z0 * texel && std::min(s.xz.y, s.xz0.y) - s.radius < w.z1 * texel;
    };
    const auto push = [](const SSnowStamp& s) {
        g.stamps.push_back({s.xz.x, s.xz.y, s.foot_y, s.radius});
        g.stamps.push_back({s.sharp, float(s.kind), s.sink, s.pack});
        g.stamps.push_back({s.xz0.x, s.xz0.y, s.foot_y0, s.fall});
    };
    u32 near_count = 0;
    for (const SSnowStamp& s : src)
    {
        if (near_count >= k_max_stamps)
            break;
        // a negative sink: a swinging foot, the grass push only
        if (s.radius <= 0.f || s.sink < 0.f || !overlaps(w0, k_fine_texel, s))
            continue;
        push(s);
        ++near_count;
        const float reach = s.radius + k_rim_reach;
        add_tiles({std::min(s.xz.x, s.xz0.x) - reach, std::min(s.xz.y, s.xz0.y) - reach},
            {std::max(s.xz.x, s.xz0.x) + reach, std::max(s.xz.y, s.xz0.y) + reach}, g.dirty);
    }
    u32 far_count = 0;
    for (const SSnowStamp& s : src)
    {
        if (near_count + far_count >= k_max_stamps)
            break;
        if (s.radius <= 0.f || s.sink < 0.f || overlaps(w0, k_fine_texel, s) || !overlaps(w1, k_coarse_texel, s))
            continue;
        push(s);
        ++far_count;
    }
    unique(g.dirty);
    g.stat_stamps_near = near_count;
    g.stat_stamps_far = far_count;
    const u32 total = near_count + far_count;
    if (total && g.stamp_buf.reserve(total * 3, sizeof(Fvector4), false))
    {
        g.stamp_buf.upload(cmd, g.stamps.data(), total * 3);
        const auto stamp = [&](const Map& m, u32 first, u32 count, float min_radius) {
            const bool in_l0 = &m == &g.l0;
            dispatch(cmd, g.cs_stamp, std::min(count, 1024u), (count + 1023) / 1024, 1, {{8, g.stamp_buf.srv}}, {m.tex.uav, g.roof.uav}, [&] {
                set_common(cmd);
                set_map(cmd, "da_sn_map", m);
                cmd.set_c("da_sn_map2", m.texel, k_carve_rate * dt, g.rho_open, min_radius);
                cmd.set_c("da_sn_stamp", float(first), float(count), in_l0 ? 1.f : 0.f, 0.f);
            });
        };
        if (near_count)
            stamp(g.l0, 0, near_count, 0.f);
        if (far_count)
        {
            stamp(g.l1, near_count, far_count, 0.75f * k_coarse_texel);
            l1_changed = true;
        }
    }

    // refill from the snow that fell, in whole unorm steps
    refilled = false;
    const float unit = g.cap / 65535.f;
    g.refill_acc += std::max(fr.fall.y, 0.f) * dt;
    if (g.refill_acc >= 4.f * unit)
    {
        const float units = std::min(std::floor(g.refill_acc / unit), 65535.f);
        g.refill_acc -= units * unit;
        for (const Map* m : {&g.l0, &g.l1})
        {
            dispatch(cmd, g.cs_refill, (m->n + 7) / 8, (m->n + 7) / 8, 1, {}, {m->tex.uav},
                [&] { cmd.set_c("da_sn_refill", units, float(m->n), 0.f, 0.f); });
        }
        dispatch(cmd, g.cs_refill, (g.l0.n + 7) / 8, (g.l0.n + 7) / 8, 1, {}, {g.roof.uav},
            [&] { cmd.set_c("da_sn_refill", units, float(g.l0.n), 1.f, 0.f); });
        refilled = true;
        l1_changed = true;
    }

    // 4. pool the stamped tiles down into L1
    if (!g.dirty.empty() && g.tile_buf.reserve(u32(g.dirty.size()), sizeof(u32), false))
    {
        g.tile_buf.upload(cmd, g.dirty.data(), u32(g.dirty.size()));
        dispatch(cmd, g.cs_pool, u32(g.dirty.size()), 1, 1, {{8, g.tile_buf.srv}}, {g.l1.tex.uav}, [&] {
            set_common(cmd);
            set_map(cmd, "da_sn_map", g.l0);
            set_map(cmd, "da_sn_map1", g.l1);
        });
        l1_changed = true;
    }
}

void composite_fine(CBackend& cmd, bool full, const Rect fresh[2], u32 fresh_count)
{
    g.tiles.clear();
    if (full || g.fine_fresh)
        add_rect_tiles(g.l0.window(), g.tiles);
    else
    {
        g.tiles.insert(g.tiles.end(), g.dirty.begin(), g.dirty.end());
        g.tiles.insert(g.tiles.end(), g.dirty_prev.begin(), g.dirty_prev.end());
        for (u32 i = 0; i < fresh_count; ++i)
            add_rect_tiles(fresh[i], g.tiles);
        unique(g.tiles);
    }
    g.stat_tiles = u32(g.tiles.size());
    if (g.tiles.empty() || !g.tile_buf.reserve(u32(g.tiles.size()), sizeof(u32), false))
        return;
    g.tile_buf.upload(cmd, g.tiles.data(), u32(g.tiles.size()));
    Rect f[2]{};
    for (u32 i = 0; i < fresh_count && i < 2; ++i)
        f[i] = fresh[i];
    dispatch(cmd, g.cs_composite, 8, 8, u32(g.tiles.size()), {{8, g.tile_buf.srv}}, {g.fine.uav, g.trod.uav}, [&] {
        set_common(cmd);
        set_map(cmd, "da_sn_map", g.l0);
        cmd.set_c("da_sn_comp", g.share_open, g.share_road, g.fine_fresh ? 1.f : 0.f, 0.f);
        cmd.set_c("da_sn_fresh0", float(f[0].x0), float(f[0].z0), float(f[0].x1), float(f[0].z1));
        cmd.set_c("da_sn_fresh1", float(f[1].x0), float(f[1].z0), float(f[1].x1), float(f[1].z1));
    });
    g.fine_fresh = false;
}

void composite_level(CBackend& cmd, bool full, bool l1_changed)
{
    Rect r{};
    const Rect all{0, 0, int(g.dim_x), int(g.dim_z)};
    // The L1 window in level texels, one texel of margin for the bilinear reads.
    const Rect w1 = g.l1.window();
    const float inv = 1.f / g.texel;
    const Rect around{int(std::floor((w1.x0 * k_coarse_texel - g.origin.x) * inv)) - 1,
        int(std::floor((w1.z0 * k_coarse_texel - g.origin.y) * inv)) - 1,
        int(std::ceil((w1.x1 * k_coarse_texel - g.origin.x) * inv)) + 1,
        int(std::ceil((w1.z1 * k_coarse_texel - g.origin.y) * inv)) + 1};
    if (full || g.level_fresh)
        r = all;
    else if (l1_changed || g.l1_changed_prev)
    {
        r = around;
        if (!g.level_rect_prev.empty())
        {
            r.x0 = std::min(r.x0, g.level_rect_prev.x0);
            r.z0 = std::min(r.z0, g.level_rect_prev.z0);
            r.x1 = std::max(r.x1, g.level_rect_prev.x1);
            r.z1 = std::max(r.z1, g.level_rect_prev.z1);
        }
    }
    g.level_rect_prev = around;
    r = {std::max(r.x0, 0), std::max(r.z0, 0), std::min(r.x1, all.x1), std::min(r.z1, all.z1)};
    g.stat_level_texels = r.empty() ? 0 : u32(r.width() * r.height());
    if (r.empty())
        return;
    dispatch(cmd, g.cs_level, u32(r.width() + 7) / 8, u32(r.height() + 7) / 8, 1, {}, {g.level.uav}, [&] {
        set_common(cmd);
        set_map(cmd, "da_sn_map1", g.l1);
        cmd.set_c("da_sn_rect", float(r.x0), float(r.z0), float(r.width()), float(r.height()));
        cmd.set_c("da_sn_comp", g.share_open, g.share_road, g.level_fresh ? 1.f : 0.f, g.l1.valid ? 1.f : 0.f);
    });
    g.level_fresh = false;
}

void publish_off()
{
    g.layer_on = false;
    g.depth_on = false;
    g.misc.set(g.cap, 0.f, 0.f, 1.f);
}

// The grid's rings and visible tiles of this frame; false when there is nothing to draw.
struct GridFrame
{
    Fvector4 rings[k_max_rings]{};
    Fvector4 holes[k_max_rings]{};
    Fvector4 tiles[k_max_tiles]{};
    u32 count = 0;
};

// The grid's tiles of this frame: in the view and the reach, or (shadow) all round the camera within the near
// sun cascade's reach, on the same rings, so the cascade holds the very surface the G-buffer draws.
// The tiles of the grid: in the view (and the grid's reach), or for a shadow cascade within `shadow_reach` of the
// eye and in the cascade's box (its sides: casters lie anywhere along the light), none wholly inside `inner`.
bool grid_frame(GridFrame& out, float shadow_reach = 0.f, const Fmatrix* cascade = nullptr, float inner = 0.f)
{
    const Preset& p = k_presets[g.preset];
    if (!p.grid_n || !p.rings)
        return false;
    const bool shadow = shadow_reach > 0.f;
    const float reach = shadow ? std::min(shadow_reach, p.reach) : p.reach;
    const Fvector& cam = Device.vCameraPosition;
    CFrustum frustum;
    Fmatrix vp = shadow ? *cascade : Device.mFullTransform;
    frustum.CreateFromMatrix(vp, shadow ? FRUSTUM_P_LRTB : FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    const float y0 = g.ymin - 1.f, y1 = g.ymin + g.yrange + g.cap + 1.f;
    const u32 per_side = p.grid_n / k_cells;
    float prev_cx = 0.f, prev_cz = 0.f, prev_half = 0.f;
    for (u32 k = 0; k < p.rings && k < k_max_rings; ++k)
    {
        const float s = p.grid_s0 * float(1u << k);
        const float snap = 2.f * s;
        const float cx = std::floor(cam.x / snap) * snap, cz = std::floor(cam.z / snap) * snap;
        const float half = float(p.grid_n / 2) * s;
        // Geomorph over the outer band; the last ring has nothing coarser to meet.
        const float band = std::max(4.f * s, 0.15f * half);
        const bool last = k + 1 == p.rings;
        out.rings[k].set(cx, cz, last ? 1e9f : half - band, 1.f / band);
        out.holes[k].set(prev_cx, prev_cz, k ? prev_half : 0.f, 0.f);
        for (u32 tz = 0; tz < per_side; ++tz)
        {
            for (u32 tx = 0; tx < per_side; ++tx)
            {
                if (out.count >= k_max_tiles)
                    break;
                const float ox = cx - half + float(tx * k_cells) * s, oz = cz - half + float(tz * k_cells) * s;
                const float ex = ox + float(k_cells) * s, ez = oz + float(k_cells) * s;
                // entirely inside the finer ring
                if (k && ox >= prev_cx - prev_half && ex <= prev_cx + prev_half && oz >= prev_cz - prev_half &&
                    ez <= prev_cz + prev_half)
                    continue;
                // beyond the reach
                const float dx = std::max(std::max(ox - cam.x, cam.x - ex), 0.f);
                const float dz = std::max(std::max(oz - cam.z, cam.z - ez), 0.f);
                if (dx * dx + dz * dz > reach * reach)
                    continue;
                const float fx = std::max(cam.x - ox, ex - cam.x), fz = std::max(cam.z - oz, ez - cam.z);
                if (fx * fx + fz * fz < inner * inner)
                    continue;
                Fbox box;
                box.vMin.set(ox, y0, oz);
                box.vMax.set(ex, y1, ez);
                u32 mask = shadow ? FRUSTUM_P_LRTB : FRUSTUM_P_LRTB | FRUSTUM_P_FAR;
                if (frustum.testAABB(&box.vMin.x, mask) == fcvNone)
                    continue;
                out.tiles[out.count++].set(ox, oz, s, float(k));
            }
        }
        prev_cx = cx;
        prev_cz = cz;
        prev_half = half;
    }
    return out.count > 0;
}
} // namespace

u32 da_snow_level_terrain(Fvector4& xf, Fvector4& lvl)
{
    if (!g.uploaded || !g.terrain.live())
    {
        xf.set(0.f, 0.f, 0.f, 0.f);
        lvl.set(0.f, 1.f, 0.f, 0.f);
        return 0;
    }
    xf.set(1.f / g.texel, 1.f / g.texel, -g.origin.x / g.texel, -g.origin.y / g.texel);
    lvl.set(g.ymin, g.yrange, 0.f, 0.f);
    return g.terrain_version;
}

void da_snow_level_load()
{
    placeholders();
    g.field_stamp = u32(-1);
    g.publish_frame = u32(-1);
    sync_field();
}

void da_snow_level_unload()
{
    level_release();
    trails_release();
    publish_off();
    g.field = false;
    g.field_stamp = u32(-1);
    g.publish_frame = u32(-1);
    g.frame = nullptr;
}

bool da_snow_active() { return g.layer_on; }

bool da_snow_depth_active() { return g.depth_on; }

void da_snow_frame_publish()
{
    if (!g_pGameLevel || g.publish_frame == Device.dwFrame)
        return;
    g.publish_frame = Device.dwFrame;
    placeholders();
    g.frame = &da_snow_frame();
    const SSnowFrame& fr = *g.frame;
    g.cap = fr.dirt.w > 0.f ? fr.dirt.w : k_cap_default;
    if (g.preset_pending != g.preset)
    {
        // the trail maps take the new sizes below
        g.preset = g.preset_pending;
        if (g.trails_live)
            trails_release();
    }
    if (!ensure_shaders())
    {
        publish_off();
        return;
    }
    sync_field();
    // The fields go up with the bake, right after the level load, so snow arriving later (the QA year,
    // a season change) costs no frame.
    if (g.field && !g.uploaded)
        upload_level();
    // A level-mean depth of 0 skips every snow pass (da_snow_field.h); the cover keys of the onset and
    // melt days ride on a small positive mean (the layer's dusting term).
    if (!g.field || !g.uploaded || fr.mean_depth <= 0.f)
    {
        // The season melted the snow: the trails go with it, and the depth starts over when it returns.
        if (g.trails_live)
            trails_release();
        g.level_fresh = true;
        publish_off();
        return;
    }
    if (!g.trails_live && !trails_create())
    {
        publish_off();
        return;
    }
    g.layer_on = true;
    g.depth_on = true;

    // Compacted residual of a print: the open snow's density against packed snow's (~550 kg/m3).
    const float slush = std::max(1.f - (fr.state.x + fr.state.y + fr.state.z + fr.state.w), 0.f);
    const float rho = 80.f * fr.state.x + 250.f * fr.state.y + 350.f * fr.state.z + 425.f * fr.state.w + 600.f * slush;
    g.share_open = clampr(rho / 550.f, 0.15f, 1.f);
    g.rho_open = rho;
    g.share_road = clampr((rho + (500.f - rho) * clampr(fr.dirt.z, 0.f, 1.f)) / 550.f, 0.15f, 1.f);
    g.level_xf.set(1.f / g.texel, 1.f / g.texel, -g.origin.x / g.texel, -g.origin.y / g.texel);

    // The windows' places of this frame; the fine window's constants follow the new L0 window.
    place_window(g.l1, k_coarse_step);
    place_window(g.l0, k_fine_step);
    const float half = float(g.l0.n) * 0.5f;
    g.fine_xf.set(1.f / k_fine_texel, float(g.l0.n), (float(g.l0.next_x) + half) * k_fine_texel,
        (float(g.l0.next_z) + half) * k_fine_texel);
    const float valid_half = (half - 2.f) * k_fine_texel;
    g.misc.set(g.cap, valid_half, 1.f, 0.1f * valid_half);
}

void da_snow_frame_prepare(CBackend& cmd_list)
{
    // A frame the camera update did not reach publishes here (a no-op otherwise).
    da_snow_frame_publish();
    if (!g.depth_on || !g.frame)
        return;
    const SSnowFrame& fr = *g.frame;

    PIX_EVENT_CTX(cmd_list, DA_snow_prepare);
    const SeasonKey key = season_key(fr);
    const bool season_moved = !(key == g.season);
    g.season = key;

    // The windows move first.
    Rect fresh[2];
    u32 fresh_count = 0;
    bool l1_changed = false, refilled = false;
    update_trails(cmd_list, fresh, fresh_count, l1_changed, refilled);

    // 5. the depth where it changed; all of it when the season moved, and once more to settle
    const bool full = season_moved || g.season_moved_prev || refilled || g.refilled_prev;
    g.stat_full = full;
    composite_fine(cmd_list, full, fresh, fresh_count);
    composite_level(cmd_list, season_moved || g.season_moved_prev, l1_changed);

    g.season_moved_prev = season_moved;
    g.refilled_prev = refilled;
    g.l1_changed_prev = l1_changed;
    g.dirty_prev.swap(g.dirty);
}

void da_snow_render_grid(CBackend& cmd_list)
{
    if (!g.depth_on || !g.shaders_ok || !g.frame)
        return;
    GridFrame grid;
    if (!grid_frame(grid))
    {
        g.stat_grid_tiles = 0;
        return;
    }
    grid_create();
    if (!g.grid_geom)
        return;
    g.stat_grid_tiles = grid.count;
    PIX_EVENT_CTX(cmd_list, DA_snow_grid);

    // the albedo under the sky pixels the grid may cover
    cmd_list.set_Element(g.s_grid->E[1]);
    quad(cmd_list);

    cmd_list.set_Element(g.s_grid->E[0]);
    set_common(cmd_list);
    const Preset& p = k_presets[g.preset];
    cmd_list.set_c("da_sn_grid", p.reach, 0.1f * p.reach, k_grid_min, k_grid_bias);
    R_constant* rings = cmd_list.get_c("da_sn_rings")._get();
    R_constant* holes = cmd_list.get_c("da_sn_holes")._get();
    R_constant* tiles = cmd_list.get_c("da_sn_tiles")._get();
    for (u32 k = 0; k < k_max_rings; ++k)
    {
        cmd_list.set_ca(rings, k, grid.rings[k]);
        cmd_list.set_ca(holes, k, grid.holes[k]);
    }
    for (u32 i = 0; i < grid.count; ++i)
        cmd_list.set_ca(tiles, i, grid.tiles[i]);
    cmd_list.set_Geometry(g.grid_geom);
    constexpr u32 side = k_cells + 1;
    cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, side * side, 0, k_cells * k_cells * 2, grid.count);

    // the snow on the covers: roofs, decks, vehicles, crates
    g.stat_shell_chunks = 0;
    if (!ps_r__snow_shells)
        return;
    // the scene so far, for the shells' test of what lies under them
    const bool depth = RImplementation.Target && RImplementation.Target->copy_depth(cmd_list);
    cmd_list.set_Element(g.s_grid->E[2]);
    set_common(cmd_list);
    cmd_list.set_c("da_sn_shell", k_shell_reach[g.preset], 0.f, k_grid_bias, 0.f);
    cmd_list.set_c("da_sn_shell2", depth ? 1.f : 0.f, 0.f, Device.mProject._33, Device.mProject._43);
    g.stat_shell_chunks = da_ground_shells_render(cmd_list, k_shell_reach[g.preset]);
}

bool da_snow_casts(u32 cascade)
{
    return cascade <= 1 && ps_r__snow_shadow && g.depth_on && g.shaders_ok && g.frame && g.grid_geom && k_presets[g.preset].grid_n;
}

void da_snow_render_cascade(CBackend& cmd_list, u32 cascade, const Fmatrix& vp)
{
    if (!da_snow_casts(cascade))
        return;
    // The second cascade only where the first one hands over (the eye's nearest metres are always the first's),
    // in cells of every other vertex.
    const bool outer = cascade > 0;
    GridFrame grid;
    if (!grid_frame(grid, k_shadow_reach, &vp, outer ? k_shadow_inner : 0.f))
        return;
    cmd_list.set_xform_world(Fidentity);
    cmd_list.set_Element(g.s_grid->E[3]);
    set_common(cmd_list);
    const Preset& p = k_presets[g.preset];
    cmd_list.set_c("da_sn_grid", p.reach, 0.1f * p.reach, k_grid_min, 0.f);
    R_constant* rings = cmd_list.get_c("da_sn_rings")._get();
    R_constant* holes = cmd_list.get_c("da_sn_holes")._get();
    R_constant* tiles = cmd_list.get_c("da_sn_tiles")._get();
    for (u32 k = 0; k < k_max_rings; ++k)
    {
        cmd_list.set_ca(rings, k, grid.rings[k]);
        cmd_list.set_ca(holes, k, grid.holes[k]);
    }
    for (u32 i = 0; i < grid.count; ++i)
        cmd_list.set_ca(tiles, i, grid.tiles[i]);
    cmd_list.set_Geometry(g.grid_geom);
    constexpr u32 side = k_cells + 1;
    constexpr u32 half_cells = (k_cells / 2) * (k_cells / 2);
    if (outer)
        cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, side * side, k_cells * k_cells * 6, half_cells * 2, grid.count);
    else
        cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, side * side, 0, k_cells * k_cells * 2, grid.count);
    cmd_list.set_CullMode(CULL_CCW);
}

void da_snow_apply_layer(CBackend& cmd_list)
{
    if (!g.layer_on || !g.shaders_ok || !g.frame)
        return;
    CRenderTarget* T = RImplementation.Target;
    if (!T)
        return;
    const SSnowFrame& fr = *g.frame;
    PIX_EVENT_CTX(cmd_list, DA_snow_layer);

    const auto& env = g_pGamePersistent->Environment();
    Fvector sun = env.CurrentEnv.sun_dir;
    sun.invert();
    sun.normalize_safe();
    // How strongly the sun shines through: its colour's luminance, nothing while it is down. An overcast
    // weather's dim sun makes no glints.
    const Fvector& sun_rgb = env.CurrentEnv.sun_color;
    const float sun_lum = 0.2126f * sun_rgb.x + 0.7152f * sun_rgb.y + 0.0722f * sun_rgb.z;
    const float sun_strength = sun.y > 0.02f ? clampr((sun_lum - 0.08f) * 2.5f, 0.f, 1.f) : 0.f;
    const float wind_speed = env.WindSpeedMs();
    const float metres_per_px = 2.f * std::tan(deg2rad(Device.fFOV) * 0.5f) / float(std::max(Device.dwRenderHeight, 1u));
    const float covered = std::max(std::max(fr.cover.x, fr.cover.y), std::max(fr.cover.z, fr.cover.w));

    const auto constants = [&] {
        set_common(cmd_list);
        cmd_list.set_c("da_sn_v2w", Device.mInvView);
        cmd_list.set_c("da_sn_w2v", Device.mView);
        cmd_list.set_c("da_sn_sun", sun.x, sun.y, sun.z, sun_strength);
        cmd_list.set_c("da_sn_wind", std::sin(env.eff_wind_dir), std::cos(env.eff_wind_dir), clampr(0.3f * wind_speed, 0.f, 1.f), 0.f);
        // dusting from the cover keys only while the depth model has (almost) nothing to draw
        // the glints' strength (r__snow_glint) and reach
        cmd_list.set_c("da_sn_layer", covered > 0.f ? clampr(1.f - fr.mean_depth / 0.01f, 0.f, 1.f) : 0.f, 0.3f, ps_r__snow_glint, metres_per_px);
        cmd_list.set_c("da_sn_layer2", clampr(fr.fall.x / 0.005f, 0.f, 1.f), 30.f, 150.f, r2_hud_depth_limit);
        cmd_list.set_c("da_sn_stain", g.stain_xf);
        cmd_list.set_c("da_snow_cover", fr.cover);
        cmd_list.set_c("da_snow_state", fr.state);
        cmd_list.set_c("da_snow_surf", fr.surf);
        cmd_list.set_c("da_snow_dirt", fr.dirt);
        // the keys are DATA's; a frame without them falls back to settled snow
        const auto albedo = [](const Fvector4& a) {
            return (a.x + a.y + a.z > 0.05f) ? a : Fvector4{0.84f, 0.85f, 0.87f, 0.f};
        };
        cmd_list.set_c("da_snow_albedo_open", albedo(fr.albedo_open));
        cmd_list.set_c("da_snow_albedo_road", albedo(fr.albedo_road));
        cmd_list.set_c("da_snow_albedo_trees", albedo(fr.albedo_trees));
    };

    // The first-person view is rasterised into the top slice of the depth range: the layer reads the depth
    // from a copy, since the depth stays bound for the stencil test.
    T->copy_depth(cmd_list);

    // pass A: the G-buffer -> scratch (accumulator, generic 1), at the render size whatever the rain left
    T->u_setrt(cmd_list, T->rt_Accumulator, T->rt_Generic_1_r, T->rt_MSAADepth);
    RImplementation.rmNormal(cmd_list);
    cmd_list.set_Element(g.s_layer->E[0]);
    constants();
    quad(cmd_list);

    // pass B: the scratch -> the G-buffer, where pass A changed a pixel
    if (RImplementation.o.gbuffer_opt)
        T->u_setrt(cmd_list, T->rt_Position, T->rt_Color, T->rt_MSAADepth);
    else
        T->u_setrt(cmd_list, T->rt_Position, T->rt_Normal, T->rt_Color, T->rt_MSAADepth);
    cmd_list.set_Element(g.s_layer->E[1]);
    quad(cmd_list);
}

void da_snow_apply_preset(u32 preset)
{
    const u32 p = std::min(preset, 4u);
    da_ground_apply_preset(p);
    g.preset_pending = p;
}

Fvector4 da_snow_fine_xf() { return g.fine_xf; }
// QA: the fine depth texture (r__snow_window_probe).
ID3D11Texture2D* da_snow_fine_surface() { return g.fine.surface; }

Fvector4 da_snow_level_xf() { return g.level_xf; }

float da_snow_cap() { return g.cap; }

Fvector4 da_snow_misc() { return g.misc; }

void da_snow_destroy()
{
    g.fine.destroy();
    g.trod.destroy();
    g.roof.destroy();
    g.terrain.destroy();
    g.shelter.destroy();
    g.top.destroy();
    g.walk.destroy();
    g.overhang.destroy();
    g.stain_cells.destroy();
    g.stains.destroy();
    g.level.destroy();
    g.road.destroy();
    g.weights.destroy();
    g.factors.destroy();
    g.base.destroy();
    g.l0.tex.destroy();
    g.l1.tex.destroy();
    g.l0 = {};
    g.l1 = {};
    g.stamp_buf.destroy();
    g.tile_buf.destroy();
    g.grid_geom = nullptr;
    g.quad_geom = nullptr;
    release(g.grid_vb);
    release(g.grid_ib);
    for (Compute* c : {&g.cs_clear, &g.cs_stamp, &g.cs_scroll, &g.cs_refill, &g.cs_pool, &g.cs_composite, &g.cs_level})
        c->destroy();
    g.s_grid = nullptr;
    g.s_layer = nullptr;
    g.shaders_tried = false;
    g.shaders_ok = false;
    g.field_stamp = u32(-1);
    g.field = false;
    g.uploaded = false;
    g.trails_live = false;
    g.publish_frame = u32(-1);
    g.frame = nullptr;
    publish_off();
    da_ground_destroy();
}

void da_snow_stats()
{
    const Preset& p = k_presets[g.preset];
    Msg("* [snow] preset %u: layer %s, depth %s, field %s %ux%u @ %.2f m (uploaded %s), cap %.3f m", g.preset,
        g.layer_on ? "on" : "off", g.depth_on ? "on" : "off", g.field ? "yes" : "no", g.dim_x, g.dim_z, g.texel,
        g.uploaded ? "yes" : "no", g.cap);
    Msg("* [snow] trails: L0 %u @ %.4f m at (%d, %d), L1 %u @ %.3f m at (%d, %d), live %s", g.l0.n, g.l0.texel, g.l0.org_x,
        g.l0.org_z, g.l1.n, g.l1.texel, g.l1.org_x, g.l1.org_z, g.trails_live ? "yes" : "no");
    Msg("* [snow] last frame: stamps %u near / %u far, fine tiles %u (%s), level texels %u, grid tiles %u of %u (n %u, s0 %.3f, "
        "reach %.0f m)",
        g.stat_stamps_near, g.stat_stamps_far, g.stat_tiles, g.stat_full ? "full" : "dirty", g.stat_level_texels,
        g.stat_grid_tiles, p.rings * (p.grid_n / k_cells) * (p.grid_n / k_cells), p.grid_n, p.grid_s0, p.reach);
    Msg("* [snow] shells: %u chunks drawn (reach %.0f m)", g.stat_shell_chunks, k_shell_reach[g.preset]);
    // (the last frame's near stamps as uploaded: where, how high, how big, of what kind)
    for (u32 i = 0; i < g.stat_stamps_near && i < 16 && size_t(i) * 3 + 1 < g.stamps.size(); ++i)
        Msg("* [snow]   stamp %u: %.2f %.2f y %.2f r %.2f kind %.0f", i, g.stamps[i * 3].x, g.stamps[i * 3].y, g.stamps[i * 3].z,
            g.stamps[i * 3].w, g.stamps[i * 3 + 1].y);
    if (g.frame)
    {
        const SSnowFrame& fr = *g.frame;
        Msg("* [snow] frame: mean depth %.3f m, cover %.2f/%.2f/%.2f/%.2f, S %.3f/%.3f/%.3f/%.3f, Mc %.3f/%.3f/%.3f/%.3f, "
            "fall %.4f m/h, refill %.2e m/s, state fresh %.2f settled %.2f old %.2f wet %.2f",
            fr.mean_depth, fr.cover.x, fr.cover.y, fr.cover.z, fr.cover.w, fr.S.x, fr.S.y, fr.S.z, fr.S.w, fr.Mc.x, fr.Mc.y,
            fr.Mc.z, fr.Mc.w, fr.fall.x, fr.fall.y, fr.state.x, fr.state.y, fr.state.z, fr.state.w);
    }
    Fvector4 win, win2;
    const bool window = da_ground_window(win, win2);
    Msg("* [snow] ground window: %s, centre (%.1f, %.1f), half %.1f m, %u texels @ %.3f m", window ? "valid" : "none", win.x,
        win.y, win.z, u32(win2.x), win2.y > 0.f ? 1.f / win2.y : 0.f);
    // the yellow snow nearest the camera
    {
        const Fvector& eye = Device.vCameraPosition;
        xr_vector<std::pair<float, Fvector4>> near_st;
        for (const Fvector4& st : g.stain_list)
            near_st.emplace_back(_sqr(st.x - eye.x) + _sqr(st.y - eye.z), st);
        std::sort(near_st.begin(), near_st.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < std::min<size_t>(near_st.size(), 8); ++k)
            Msg("* [snow]   stain %u: %.2f %.2f r %.2f, %.0f m", u32(k), near_st[k].second.x, near_st[k].second.y,
                near_st[k].second.z, std::sqrt(near_st[k].first));
    }
}
} // namespace xray::render::RENDER_NAMESPACE
