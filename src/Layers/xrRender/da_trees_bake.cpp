// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The billboards' runtime bake (docs/dead-air/TREES.md "The billboards", SEASONS.md): no billboard atlas ships.
//
// The trees' atlas pair (da_trees.h: $user$da_tree_bb and _nm, a cell set's eight views a row of cells - a template's
// own, and the dead look of a template some of whose trees stand dead among living ones): each set drawn from its
// template's own far tiers as the G-buffer has them at the billboards' distance R - the stock wood and its patches, the
// clusters, the day's species tables, the snow, the dead and the red needles -, eight orthographic views along its stock
// facets' normals, four by four samples a texel; resolved into a staging chain (the coverage, the albedo, the normal in
// the bake camera's view space, the hemisphere light, the foliage), its empty texels filled from the coarser levels,
// each level's alpha scaled so the billboard shader's alpha test keeps the cell's coverage (da_tree_bake_cover.cs), the
// albedo's lod tuning and the hemisphere light's per-vertex scale taken back so lod.ps writes what the blobs wrote
// (da_tree_bake_mip.ps); the foliage (the share of a texel that is leaves, their translucency) into the half-size
// foliage atlas da_tree_lod.ps writes the near tiers' material from. Every set at the level's first frame and after a
// device reset; then a few views a frame within about 0.3 ms of the GPU whenever the season's day moved two days and
// the species' table changed, or the snow, the sun or the presets moved.
//
// The stock path's atlas (level_lods): the level's own atlas copied into a texture of the engine's, every stock-path FLOD
// wearing a trees\ texture (reeds, sedge, duckweed, the stock fallen trees and debris, a mod's vegetation) drawn again
// from its parts with their latched cards, one cell set a model, calibrated to Dead Air's bake as the offline pipeline
// did (pipeline\seasons_lods.py: the alpha's quantile map and the colour's linear gains, measured once a level with the
// base cards), bound as the level_lods texture's surface; again whenever the texture latch moves.

#include "stdafx.h"
#include "da_trees_bake.h"
#include "da_trees.h"
#include "da_veg.h"
#include "da_veg_internal.h"
#include "FTreeVisual.h"
#include "FLOD.h"
#include "BufferUtils.h"
#include "ResourceManager.h"
#include "xrRender_console.h"
#include "xrEngine/da_season.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRenderPC_R4/da_async.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

#include <DirectXTex.h>
#include <atomic>
#include <bit>
#include <map>
#include <optional>

extern ENGINE_API float g_fov;

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
using namespace da_veg;

// Samples a texel side (the offline bake's); the samples' targets (a 128-pixel cell at four); the staging's side and
// levels (a 128-pixel cell down to its texel).
constexpr u32 kSS = 4, kSSize = 512, kStage = 1024, kLevels = 8;
// A quad's record (da_tree_bake_quad.vs); the records, cells, instance records and chunks a batch holds at most; the
// factors' levels a cell.
constexpr u32 kQuadRec = 48, kMaxQuads = 8192, kMaxCells = 1024, kMaxInst = 64, kMaxBakeChunks = 8192, kKLevels = 16;
// The clusters' pixels in the trees' bake over a runtime pixel at R (tree_draw_view).
constexpr float kBakeLeafZoom = 16.f;
// The bake camera's distance: the shaders' view vectors near parallel (an orthographic view along the facet).
constexpr float kCamDist = 1000.f;
// lod.vs's L_SCALE (2 x 1.55): the facets' vertex hemisphere light times this, then r__lod_hemi (lod.ps).
constexpr float kLodScale = 3.1f;
// The progressive bakes' GPU time a frame; the days the season moves before a changed look is baked again.
constexpr float kBudgetMs = 0.3f, kDaySteps = 2.f;
// The shader files the bake ships (compiling a missing one is fatal).
constexpr pcstr kCommonFiles[] = {"da_tree_bake.h", "da_tree_bake_quad.vs", "da_tree_bake_resolve.ps", "da_tree_bake_chain.ps",
    "da_tree_bake_cover.cs"};
constexpr pcstr kTreeFiles[] = {"da_tree_bake_mass.ps", "da_tree_bake_wood.ps", "da_tree_bake_mip.ps", "da_tree_bake_mip_top.ps",
    "da_shrub_cluster.vs", "da_tree_stock.vs", "da_tree_patch.vs", "da_shrub_cluster.ps", "da_shrub_wood.ps"};
constexpr pcstr kLodsFiles[] = {"da_tree_bake_lods.ps", "da_tree_bake_stock.vs", "da_tree_bake_stock.ps"};

// The bake's passes: no stencil, both sides, the depth only for the views; the vegetation's texture arrays and the
// samplers bound by name where a shader asks for them.
class CBlender_tree_bake : public IBlender
{
    pcstr m_vs, m_ps;
    bool m_depth;

public:
    CBlender_tree_bake(pcstr vs, pcstr ps, bool depth) : m_vs(vs), m_ps(ps), m_depth(depth) {}
    LPCSTR getComment() override { return "INTERNAL: trees, billboard bake"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_Pass(m_vs, m_ps, FALSE, m_depth ? TRUE : FALSE, m_depth ? TRUE : FALSE);
        C.r_dx11Texture("s_shrub_leaf_d", "da\\shrub_leaf_d");
        C.r_dx11Texture("s_shrub_leaf_n", "da\\shrub_leaf_n");
        C.r_dx11Texture("s_shrub_bark_d", "da\\shrub_bark_d");
        C.r_dx11Texture("s_shrub_bark_n", "da\\shrub_bark_n");
        C.r_dx11Sampler("smp_base");
        C.r_dx11Sampler("smp_linear");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_CullMode(D3DCULL_NONE);
        C.r_End();
    }
};

class CBlender_tree_bake_cs : public IBlender
{
    pcstr m_cs;

public:
    explicit CBlender_tree_bake_cs(pcstr cs) : m_cs(cs) {}
    LPCSTR getComment() override { return "INTERNAL: trees, billboard bake, compute"; }
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

bool ready(const ref_shader& s)
{
    if (!s || !s->E[0] || s->E[0]->passes.empty())
        return false;
    const SPass& p = *s->E[0]->passes[0];
    return (p.vs && p.vs->sh && p.ps && p.ps->sh) || (p.cs && p.cs->sh);
}

template <size_t N>
bool files_present(const pcstr (&files)[N])
{
    for (pcstr f : files)
        if (!da_async_shader_exists(f))
            return false;
    return true;
}

// ======================================================================================================
// GPU objects
// ======================================================================================================
// A texture of the bake's (or the views of one someone else owns: the trees' atlas): a target and a view of each
// level, a view of them all.
struct MipTex
{
    ID3D11Texture2D* tex{};
    xr_vector<ID3D11RenderTargetView*> rtv;
    xr_vector<ID3D11ShaderResourceView*> srv;
    ID3D11ShaderResourceView* all{};
    u32 w{}, h{}, levels{};

    bool create(u32 width, u32 height, u32 mips, DXGI_FORMAT fmt)
    {
        release();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = mips;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &tex)))
            return false;
        return views();
    }

    bool wrap(ID3D11Texture2D* t)
    {
        release();
        if (!t)
            return false;
        tex = t;
        tex->AddRef();
        return views();
    }

    bool views()
    {
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        w = desc.Width;
        h = desc.Height;
        levels = desc.MipLevels;
        rtv.assign(levels, nullptr);
        srv.assign(levels, nullptr);
        for (u32 m = 0; m < levels; ++m)
        {
            D3D11_RENDER_TARGET_VIEW_DESC r{};
            r.Format = desc.Format;
            r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            r.Texture2D.MipSlice = m;
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = desc.Format;
            s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            s.Texture2D.MostDetailedMip = m;
            s.Texture2D.MipLevels = 1;
            if (FAILED(HW.pDevice->CreateRenderTargetView(tex, &r, &rtv[m])) || FAILED(HW.pDevice->CreateShaderResourceView(tex, &s, &srv[m])))
            {
                release();
                return false;
            }
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = desc.Format;
        s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = levels;
        if (FAILED(HW.pDevice->CreateShaderResourceView(tex, &s, &all)))
        {
            release();
            return false;
        }
        return true;
    }

    void release()
    {
        for (ID3D11RenderTargetView*& v : rtv)
            _RELEASE(v);
        for (ID3D11ShaderResourceView*& v : srv)
            _RELEASE(v);
        rtv.clear();
        srv.clear();
        _RELEASE(all);
        _RELEASE(tex);
        w = h = levels = 0;
    }
};

struct DepthTex
{
    ID3D11Texture2D* tex{};
    ID3D11DepthStencilView* dsv{};

    bool create(u32 width, u32 height)
    {
        release();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        return SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &tex)) && SUCCEEDED(HW.pDevice->CreateDepthStencilView(tex, nullptr, &dsv));
    }

    void release()
    {
        _RELEASE(dsv);
        _RELEASE(tex);
    }
};

// A pass's quad (da_tree_bake_quad.vs): its destination's edges in clip space, its source's, four words.
struct QuadRec
{
    float dst[4];
    float src[4];
    u32 info[4];
};
static_assert(sizeof(QuadRec) == kQuadRec);

// A quad over the pixels [x0, x1) x [y0, y1) of a w x h target, reading [s0, s1) x [t0, t1).
QuadRec quad(float x0, float y0, float x1, float y1, float w, float h, float s0, float t0, float s1, float t1, u32 i0 = 0, u32 i1 = 0)
{
    QuadRec q{};
    q.dst[0] = x0 / w * 2.f - 1.f;
    q.dst[1] = 1.f - y0 / h * 2.f;
    q.dst[2] = x1 / w * 2.f - 1.f;
    q.dst[3] = 1.f - y1 / h * 2.f;
    q.src[0] = s0;
    q.src[1] = t0;
    q.src[2] = s1;
    q.src[3] = t1;
    q.info[0] = i0;
    q.info[1] = i1;
    return q;
}

// What every bake shares: the shaders, the samples' targets, the staging and its filled copy (the albedo, the normal,
// the foliage), the buffers, the quads' geometry.
struct Shared
{
    bool tried{}, common{}, trees{}, lods{}, gpu{}, gpu_failed{};
    ref_shader s_mass, s_stock, s_patch, s_resolve, s_chain, s_mip, s_mip_top, s_lods, s_part, s_cover;
    // An empty texture list: set, the backend unbinds the textures it bound and forgets them (unbind).
    ref_texture_list no_tex;
    MipTex ss_d, ss_nm, ss_m, stage_d, stage_nm, stage_m, fill_d, fill_nm, fill_m;
    DepthTex ss_z;
    GpuBuffer quads_b, cells_b, k_b, inst_b, chunks_b, tables_b, lut_b, dummy_b;
    VertexStagingBuffer vb;
    IndexStagingBuffer ib;
    ref_geom geom;
};
Shared g_sh;

bool shaders()
{
    if (g_sh.tried)
        return g_sh.common;
    g_sh.tried = true;
    if (!files_present(kCommonFiles))
    {
        Msg("! [trees] the billboard bake's shaders are missing: the trees' billboards keep their stock cells, level_lods its own");
        return false;
    }
    STextureList none;
    g_sh.no_tex = ref_texture_list(RImplementation.Resources->_CreateTextureList(none));
    static CBlender_tree_bake b_resolve("da_tree_bake_quad", "da_tree_bake_resolve", false), b_chain("da_tree_bake_quad", "da_tree_bake_chain", false);
    static CBlender_tree_bake_cs b_cover("da_tree_bake_cover");
    g_sh.s_resolve.create(&b_resolve, "r2" DELIMITER "da_tree_bake_resolve");
    g_sh.s_chain.create(&b_chain, "r2" DELIMITER "da_tree_bake_chain");
    g_sh.s_cover.create(&b_cover, "r2" DELIMITER "da_tree_bake_cover");
    g_sh.common = ready(g_sh.s_resolve) && ready(g_sh.s_chain) && ready(g_sh.s_cover);
    if (g_sh.common && files_present(kTreeFiles))
    {
        static CBlender_tree_bake b_mass("da_shrub_cluster", "da_tree_bake_mass", true), b_stock("da_tree_stock", "da_tree_bake_wood", true),
            b_patch("da_tree_patch", "da_tree_bake_wood", true), b_mip("da_tree_bake_quad", "da_tree_bake_mip", false),
            b_mip_top("da_tree_bake_quad", "da_tree_bake_mip_top", false);
        g_sh.s_mass.create(&b_mass, "r2" DELIMITER "da_tree_bake_mass");
        g_sh.s_stock.create(&b_stock, "r2" DELIMITER "da_tree_bake_stock_wood");
        g_sh.s_patch.create(&b_patch, "r2" DELIMITER "da_tree_bake_patch");
        g_sh.s_mip.create(&b_mip, "r2" DELIMITER "da_tree_bake_mip");
        g_sh.s_mip_top.create(&b_mip_top, "r2" DELIMITER "da_tree_bake_mip_top");
        g_sh.trees = ready(g_sh.s_mass) && ready(g_sh.s_stock) && ready(g_sh.s_patch) && ready(g_sh.s_mip) && ready(g_sh.s_mip_top);
    }
    if (g_sh.common && files_present(kLodsFiles))
    {
        static CBlender_tree_bake b_lods("da_tree_bake_quad", "da_tree_bake_lods", false), b_part("da_tree_bake_stock", "da_tree_bake_stock", true);
        g_sh.s_lods.create(&b_lods, "r2" DELIMITER "da_tree_bake_lods");
        g_sh.s_part.create(&b_part, "r2" DELIMITER "da_tree_bake_part");
        g_sh.lods = ready(g_sh.s_lods) && ready(g_sh.s_part);
    }
    if (!g_sh.common || !g_sh.trees || !g_sh.lods)
        Msg("! [trees] the billboard bake's shaders failed (%s%s%s): the stock cells stay where it cannot run", g_sh.common ? "" : "passes ",
            g_sh.trees ? "" : "trees ", g_sh.lods ? "" : "level_lods");
    return g_sh.common;
}

// The targets and buffers, made once a level when a bake first needs them.
bool gpu()
{
    if (g_sh.gpu || g_sh.gpu_failed)
        return g_sh.gpu;
    if (!shaders())
        return false;
    const DXGI_FORMAT rgba = DXGI_FORMAT_R8G8B8A8_UNORM, rg = DXGI_FORMAT_R8G8_UNORM;
    bool ok = g_sh.ss_d.create(kSSize, kSSize, 1, rgba) && g_sh.ss_nm.create(kSSize, kSSize, 1, rgba) && g_sh.ss_m.create(kSSize, kSSize, 1, rg) &&
        g_sh.ss_z.create(kSSize, kSSize) && g_sh.stage_d.create(kStage, kStage, kLevels, rgba) &&
        g_sh.stage_nm.create(kStage, kStage, kLevels, rgba) && g_sh.stage_m.create(kStage, kStage, kLevels, rg) &&
        g_sh.fill_d.create(kStage, kStage, kLevels, rgba) && g_sh.fill_nm.create(kStage, kStage, kLevels, rgba) &&
        g_sh.fill_m.create(kStage, kStage, kLevels, rg);
    ok = ok && make_buffer(g_sh.quads_b, kMaxQuads * kQuadRec, nullptr) && make_buffer(g_sh.cells_b, kMaxCells * 16, nullptr) &&
        make_buffer(g_sh.k_b, kMaxCells * kKLevels * 4, nullptr, true) && make_buffer(g_sh.inst_b, kMaxInst * kInstRec, nullptr) &&
        make_buffer(g_sh.chunks_b, kMaxBakeChunks * kChunkRec, nullptr) && make_buffer(g_sh.tables_b, kTabTotal * 16, nullptr) &&
        make_buffer(g_sh.lut_b, 256 * 4, nullptr) && make_buffer(g_sh.dummy_b, 64, nullptr);
    if (ok)
    {
        // The quads' geometry: 64 quads an instance, SV_VertexID only (the vertex stream is a placeholder).
        xr_vector<u16> indices;
        for (u16 e = 0; e < 64; ++e)
            for (const u16 i : {0, 1, 2, 2, 1, 3})
                indices.push_back(u16(e * 4 + i));
        static const VertexElement decl[] = {{0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
        g_sh.vb.Create(256 * sizeof(float));
        ZeroMemory(g_sh.vb.Map(), 256 * sizeof(float));
        g_sh.vb.Unmap(true);
        g_sh.ib.Create(indices.size() * sizeof(u16));
        CopyMemory(g_sh.ib.Map(), indices.data(), indices.size() * sizeof(u16));
        g_sh.ib.Unmap(true);
        g_sh.geom.create(decl, g_sh.vb, g_sh.ib);
        ok = bool(g_sh.geom);
    }
    if (!ok)
        Msg("! [trees] the billboard bake's targets could not be made: the stock cells stay");
    g_sh.gpu = ok;
    g_sh.gpu_failed = !ok;
    return ok;
}

void release_gpu()
{
    for (MipTex* t : {&g_sh.ss_d, &g_sh.ss_nm, &g_sh.ss_m, &g_sh.stage_d, &g_sh.stage_nm, &g_sh.stage_m, &g_sh.fill_d, &g_sh.fill_nm, &g_sh.fill_m})
        t->release();
    g_sh.ss_z.release();
    for (GpuBuffer* b : {&g_sh.quads_b, &g_sh.cells_b, &g_sh.k_b, &g_sh.inst_b, &g_sh.chunks_b, &g_sh.tables_b, &g_sh.lut_b, &g_sh.dummy_b})
        b->release();
    if (g_sh.geom)
        g_sh.geom.destroy();
    if (g_sh.ib.IsValid())
        g_sh.ib.Release();
    if (g_sh.vb.IsValid())
        g_sh.vb.Release();
    g_sh.gpu = g_sh.gpu_failed = false;
}

void release_shaders()
{
    for (ref_shader* s : {&g_sh.s_mass, &g_sh.s_stock, &g_sh.s_patch, &g_sh.s_resolve, &g_sh.s_chain, &g_sh.s_mip, &g_sh.s_mip_top, &g_sh.s_lods,
             &g_sh.s_part, &g_sh.s_cover})
        s->destroy();
    g_sh.no_tex = ref_texture_list{};
    g_sh.tried = g_sh.common = g_sh.trees = g_sh.lods = false;
}

void upload(CBackend& cmd, const GpuBuffer& b, u32 offset, const void* data, u32 bytes)
{
    if (!bytes || !b.buf || offset + bytes > b.bytes)
        return;
    const D3D11_BOX box{offset, 0, 0, offset + bytes, 1, 1};
    HW.get_context(cmd.context_id)->UpdateSubresource(b.buf, 0, &box, data, 0, 0);
}

// Every shader resource slot the bake binds back to none: the elements' textures through the backend (it forgets them,
// so the next element binds its own again - an element of the same list would otherwise find its slots empty), the
// views bound by hand through the state cache (a target bound over a view makes the device drop the view without the
// cache knowing).
void unbind(CBackend& cmd)
{
    if (g_sh.no_tex)
        cmd.set_Textures(g_sh.no_tex);
    for (u32 slot = 0; slot < 8; ++slot)
    {
        cmd.SRVSManager.SetPSResource(slot, nullptr);
        cmd.SRVSManager.SetVSResource(slot, nullptr);
        cmd.SRVSManager.SetCSResource(slot, nullptr);
    }
    cmd.SRVSManager.Apply(cmd.context_id);
}

// count quads from record `first` with the shader element and the pixel shader's resources already set.
void draw_quads_own(CBackend& cmd, u32 first, u32 count)
{
    if (!count)
        return;
    cmd.set_c("da_tree_bake_q", float(count), float(first), 0.f, 0.f);
    cmd.SRVSManager.SetVSResource(0, g_sh.quads_b.srv);
    cmd.SRVSManager.Apply(cmd.context_id);
    cmd.set_Geometry(g_sh.geom);
    cmd.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, 256, 0, 128, (count + 63) / 64);
}

// ======================================================================================================
// The views
// ======================================================================================================
// An orthographic view along a facet: the camera kCamDist out on the facet's near side looking along its normal (its up
// toward the facet's uv v = 0), the projection mapping the facet's four corners onto their texels of the region drawn (a
// least-squares affine map from the view plane), the depth over the object's sphere.
struct ViewXf
{
    Fmatrix V, P;
    Fvector eye, fwd;
    float depth{};
};

double det3(const double M[3][3])
{
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
        M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
}

bool solve3(const double M[3][3], const double r[3], double out[3])
{
    const double det = det3(M);
    if (std::abs(det) < 1e-18)
        return false;
    for (u32 c = 0; c < 3; ++c)
    {
        double A[3][3];
        for (u32 i = 0; i < 3; ++i)
            for (u32 j = 0; j < 3; ++j)
                A[i][j] = j == c ? r[i] : M[i][j];
        out[c] = det3(A) / det;
    }
    return true;
}

// corner/uv: the facet's quad and each corner's uv; cell: the uv edges of the region the view fills (u0 v0 u1 v1);
// sphere: the object's (the depth range).
bool view_xf(const Fvector* corner, const Fvector2* uv, const Fvector& normal, const Fvector4& cell, const Fsphere& sphere, ViewXf& out)
{
    Fvector N = normal;
    if (N.square_magnitude() < 1e-8f)
        return false;
    N.normalize();
    // Up: from the two corners at the region's bottom (largest v) to the two at its top.
    u32 order[4] = {0, 1, 2, 3};
    std::sort(std::begin(order), std::end(order), [uv](u32 a, u32 b) { return uv[a].y < uv[b].y; });
    Fvector up;
    up.add(corner[order[0]], corner[order[1]]).sub(corner[order[2]]).sub(corner[order[3]]);
    up.mad(N, -up.dotproduct(N));
    if (up.square_magnitude() < 1e-8f)
        up.set(0.f, 1.f, 0.f);
    up.normalize();
    Fvector centre{};
    for (u32 k = 0; k < 4; ++k)
        centre.add(corner[k]);
    centre.mul(0.25f);
    out.fwd = N;
    out.eye.mad(centre, N, -kCamDist);
    out.V.build_camera_dir(out.eye, N, up);
    double M[3][3]{}, rx[3]{}, ry[3]{};
    const float cw = std::max(cell.z - cell.x, 1e-9f), ch = std::max(cell.w - cell.y, 1e-9f);
    for (u32 k = 0; k < 4; ++k)
    {
        Fvector q;
        out.V.transform_tiny(q, corner[k]);
        const double row[3] = {q.x, q.y, 1.0};
        const double nx = (uv[k].x - cell.x) / cw * 2.0 - 1.0, ny = 1.0 - (uv[k].y - cell.y) / ch * 2.0;
        for (u32 i = 0; i < 3; ++i)
        {
            for (u32 j = 0; j < 3; ++j)
                M[i][j] += row[i] * row[j];
            rx[i] += row[i] * nx;
            ry[i] += row[i] * ny;
        }
    }
    double a[3], b[3];
    if (!solve3(M, rx, a) || !solve3(M, ry, b))
        return false;
    Fvector c;
    out.V.transform_tiny(c, sphere.P);
    out.depth = c.z;
    const float zn = c.z - sphere.R * 1.25f - 1.f, zf = c.z + sphere.R * 1.25f + 1.f;
    Fmatrix& P = out.P;
    P.identity();
    P._11 = float(a[0]);
    P._21 = float(a[1]);
    P._41 = float(a[2]);
    P._12 = float(b[0]);
    P._22 = float(b[1]);
    P._42 = float(b[2]);
    P._33 = 1.f / (zf - zn);
    P._43 = -zn / (zf - zn);
    return true;
}

// Round a frame's bakes (made at the first work): the GPU timer, the transforms saved; the targets, viewport and state
// cache back to the scene's after.
struct Restore
{
    CBackend& cmd;
    Fmatrix W, V, P;
    explicit Restore(CBackend& c) : cmd(c), W(c.xforms.get_W()), V(c.xforms.get_V()), P(c.xforms.get_P()) { unbind(cmd); }
    ~Restore()
    {
        unbind(cmd);
        cmd.set_xform_world(W);
        cmd.set_xform_view(V);
        cmd.set_xform_project(P);
        cmd.set_CullMode(CULL_CCW);
        RImplementation.Target->u_setrt_scene_base(cmd);
        RImplementation.rmNormal(cmd);
    }
};
struct Work
{
    CBackend& cmd;
    std::optional<dx11GpuTimerScope> timer;
    std::optional<Restore> restore;
    explicit Work(CBackend& c) : cmd(c) {}
    void begin()
    {
        if (restore)
            return;
        timer.emplace(dx11GpuTimers::TreesBake);
        restore.emplace(cmd);
    }
};

// ======================================================================================================
// The staging's chain, the coverage factors, the resolve
// ======================================================================================================
// The levels of the staging's region [0, w) x [0, h) (level 0 texels): down to `levels`, then filled from the top.
void chain(CBackend& cmd, u32 w, u32 h, u32 levels)
{
    xr_vector<QuadRec> q;
    for (u32 m = 0; m < levels; ++m)
    {
        const float lw = float(std::max(kStage >> m, 1u));
        const float wm = float(std::max((w + (1u << m) - 1) >> m, 1u)), hm = float(std::max((h + (1u << m) - 1) >> m, 1u));
        q.push_back(quad(0.f, 0.f, wm, hm, lw, lw, 0.f, 0.f, wm, hm));
    }
    upload(cmd, g_sh.quads_b, 0, q.data(), u32(q.size() * sizeof(QuadRec)));
    // (The views a pass reads, from slot 0: the level, its normal, the filled level above, its normal, the foliage of the
    // level, of the level above - da_tree_bake_chain.ps.)
    const auto pass = [&](u32 m, ID3D11RenderTargetView* rd, ID3D11RenderTargetView* rn, ID3D11RenderTargetView* rm,
                          std::initializer_list<ID3D11ShaderResourceView*> srv, float mode) {
        const u32 size = std::max(kStage >> m, 1u);
        RImplementation.Target->u_setrt(cmd, size, size, rd, rn, rm, nullptr);
        cmd.SetViewport({0.f, 0.f, float(size), float(size), 0.f, 1.f});
        cmd.set_Element(g_sh.s_chain->E[0]);
        cmd.set_c("da_tree_bake_m", float(m), mode, 0.f, 0.f);
        u32 slot = 0;
        for (ID3D11ShaderResourceView* v : srv)
            cmd.SRVSManager.SetPSResource(slot++, v);
        draw_quads_own(cmd, m, 1);
        unbind(cmd);
    };
    for (u32 m = 1; m < levels; ++m)
        pass(m, g_sh.stage_d.rtv[m], g_sh.stage_nm.rtv[m], g_sh.stage_m.rtv[m],
            {g_sh.stage_d.srv[m - 1], g_sh.stage_nm.srv[m - 1], nullptr, nullptr, g_sh.stage_m.srv[m - 1]}, 0.f);
    const u32 top = levels - 1;
    pass(top, g_sh.fill_d.rtv[top], g_sh.fill_nm.rtv[top], g_sh.fill_m.rtv[top],
        {g_sh.stage_d.srv[top], g_sh.stage_nm.srv[top], nullptr, nullptr, g_sh.stage_m.srv[top]}, 2.f);
    for (u32 m = top; m-- > 0;)
        pass(m, g_sh.fill_d.rtv[m], g_sh.fill_nm.rtv[m], g_sh.fill_m.rtv[m],
            {g_sh.stage_d.srv[m], g_sh.stage_nm.srv[m], g_sh.fill_d.srv[m + 1], g_sh.fill_nm.srv[m + 1], g_sh.stage_m.srv[m],
                g_sh.fill_m.srv[m + 1]},
            1.f);
}

// Each cell's alpha factor of each level (da_tree_bake_cover.cs); cells as staging rects (x, y, w, h at level 0).
void cover(CBackend& cmd, const xr_vector<u32>& cells, u32 levels, bool stock)
{
    const u32 n = u32(cells.size() / 4);
    if (!n)
        return;
    upload(cmd, g_sh.cells_b, 0, cells.data(), u32(cells.size() * 4));
    SPass& S = *g_sh.s_cover->E[0]->passes[0];
    cmd.set_States(S.state);
    cmd.set_CS(S.cs);
    cmd.set_Constants(S.constants);
    cmd.set_c("da_tree_bake_c", float(ps_r__aref_quality) / 255.f, float(levels), stock ? 1.f : 0.f, 0.f);
    cmd.SRVSManager.SetCSResource(0, g_sh.stage_d.all);
    cmd.SRVSManager.SetCSResource(1, g_sh.cells_b.srv);
    cmd.SRVSManager.Apply(cmd.context_id);
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    const UINT initial = ~0u;
    ctx->CSSetUnorderedAccessViews(0, 1, &g_sh.k_b.uav, &initial);
    cmd.Compute(levels, n, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
    unbind(cmd);
    cmd.clear_CS_resources();
}

// One view's samples (ss a side, the view's region w x h texels) resolved into the staging at (x, y).
void resolve(CBackend& cmd, u32 x, u32 y, u32 w, u32 h, u32 ss, float mode, u32 lut_n, const float* gain)
{
    const QuadRec q = quad(float(x), float(y), float(x + w), float(y + h), float(kStage), float(kStage), 0.f, 0.f, float(w * ss), float(h * ss));
    upload(cmd, g_sh.quads_b, 0, &q, sizeof(q));
    RImplementation.Target->u_setrt(cmd, kStage, kStage, g_sh.stage_d.rtv[0], g_sh.stage_nm.rtv[0], g_sh.stage_m.rtv[0], nullptr);
    cmd.SetViewport({0.f, 0.f, float(kStage), float(kStage), 0.f, 1.f});
    cmd.set_Element(g_sh.s_resolve->E[0]);
    cmd.set_c("da_tree_bake_r", float(ss), mode, float(lut_n), 0.f);
    cmd.set_c("da_tree_bake_g", gain[0], gain[1], gain[2], 0.f);
    cmd.SRVSManager.SetPSResource(0, g_sh.ss_d.srv[0]);
    cmd.SRVSManager.SetPSResource(1, g_sh.ss_nm.srv[0]);
    cmd.SRVSManager.SetPSResource(2, g_sh.lut_b.srv);
    cmd.SRVSManager.SetPSResource(3, g_sh.ss_m.srv[0]);
    draw_quads_own(cmd, 0, 1);
    unbind(cmd);
}

// The samples' targets bound and cleared for a view of w x h texels at ss a side.
void begin_view(CBackend& cmd, u32 w, u32 h, u32 ss)
{
    RImplementation.Target->u_setrt(cmd, kSSize, kSSize, g_sh.ss_d.rtv[0], g_sh.ss_nm.rtv[0], g_sh.ss_m.rtv[0], g_sh.ss_z.dsv);
    for (ID3D11RenderTargetView* v : {g_sh.ss_d.rtv[0], g_sh.ss_nm.rtv[0], g_sh.ss_m.rtv[0]})
        cmd.ClearRT(v, Fcolor{0.f, 0.f, 0.f, 0.f});
    cmd.ClearZB(g_sh.ss_z.dsv, 1.f, 0);
    cmd.SetViewport({0.f, 0.f, float(w * ss), float(h * ss), 0.f, 1.f});
}

void clear_stage(CBackend& cmd)
{
    RImplementation.Target->u_setrt(cmd, kStage, kStage, g_sh.stage_d.rtv[0], g_sh.stage_nm.rtv[0], g_sh.stage_m.rtv[0], nullptr);
    for (ID3D11RenderTargetView* v : {g_sh.stage_d.rtv[0], g_sh.stage_nm.rtv[0], g_sh.stage_m.rtv[0]})
        cmd.ClearRT(v, Fcolor{0.f, 0.f, 0.f, 0.f});
}

// The levels a cell of `size` texels has (down to its texel), the staging's at most.
u32 level_count(u32 size)
{
    u32 n = 1;
    while ((1u << n) <= size && n < kLevels)
        ++n;
    return n;
}

float day_gap(float a, float b)
{
    const float d = std::abs(a - b);
    return std::min(d, 365.f - d);
}

u64 hash_floats(const float* v, u32 n, float quantum, u64 h = kFnvOffset)
{
    for (u32 i = 0; i < n; ++i)
    {
        const s32 q = s32(std::lround(v[i] / quantum));
        h = fnv(&q, sizeof(q), h);
    }
    return h;
}

constexpr float kOne[3] = {1.f, 1.f, 1.f};

// ======================================================================================================
// The trees' atlas
// ======================================================================================================
struct TmplState
{
    bool baked{};
    u64 look{}, season{};
    float day{};
};

constexpr u32 kLookParts = 10;

struct TreeBake
{
    bool attached{};
    ref_rt rt_d, rt_nm;
    u64 order_d{}, order_nm{};
    MipTex d, nm;
    // The foliage atlas (half the pair's size, its levels the pair's from the second down) and its texture by the name
    // the trees' billboard shader samples (da_tree_lod.ps).
    MipTex mtl;
    ref_texture mtl_ref;
    DaTreeBbAtlas info{};
    // Each cell set's state (a template's, then the dead looks', da_trees.h).
    xr_vector<TmplState> st;
    // The batch in the works: its cell sets (a row of the staging each), its views done; each set's API view, chunk
    // ranges (drStock8, drPatch, drCluster), views, hemisphere light's scale and season.
    struct Item
    {
        u32 set{};
        DaTreeBbTemplate tb{};
        u32 first[3]{}, count[3]{};
        ViewXf view[8]{};
        bool ok[8]{};
        float hscale{};
        u64 season{};
    };
    xr_vector<Item> batch;
    u32 views_done{};
    // The next bakes all at once (the level's first frame, a device reset, r__trees_bb_rebake, a jump of the season).
    bool full{true};
    // The season's day the last frame saw (a jump past kDaySteps from it re-bakes everything at once).
    float seen_day{-1e9f};
    // Nothing was stale at this day and look (the scan waits for either to move).
    bool idle{};
    float idle_day{-1.f};
    u64 idle_look{};
    // The look the cells are baked for: hashed (the presets, the snow, the sun), its parts in their steps (tree_look);
    // its parts for the stats.
    u64 look{};
    s32 look_q[kLookParts]{};
    bool look_set{};
    float snow{}, sun_e{}, sun_share{};
    // The figures.
    u32 baked{}, views{}, rebakes{}, full_bakes{};
    float full_ms{}, gpu_ms{}, view_cost{0.04f};
    // The views of the last frames (the GPU timer's frame is a few back) and the frame the estimate waits for after a
    // full bake or a composition.
    u32 recent[4]{}, recent_at{}, calm{};
    pcstr reason{"the level's first frame"};
};
TreeBake g_tb;

void tree_release()
{
    if (g_tb.mtl_ref)
    {
        g_tb.mtl_ref->surface_set(nullptr);
        g_tb.mtl_ref = ref_texture{};
    }
    g_tb.mtl.release();
    g_tb.d.release();
    g_tb.nm.release();
    g_tb.rt_d.destroy();
    g_tb.rt_nm.destroy();
    g_tb.st.clear();
    g_tb.batch.clear();
    g_tb.views_done = 0;
    g_tb.attached = false;
    g_tb.full = true;
    g_tb.idle = false;
}

// The atlas pair's levels (a device reset or a new level made them again: everything bakes again).
bool tree_attach(const DaTreeBbAtlas& info)
{
    if (g_tb.attached && g_tb.rt_d && g_tb.rt_nm && g_tb.rt_d->pSurface == g_tb.d.tex && g_tb.rt_nm->pSurface == g_tb.nm.tex &&
        g_tb.rt_d->_order == g_tb.order_d && g_tb.rt_nm->_order == g_tb.order_nm && g_tb.info.width == info.width &&
        g_tb.info.height == info.height && g_tb.info.cell == info.cell && g_tb.st.size() == info.sets)
        return true;
    const bool again = g_tb.attached;
    tree_release();
    g_tb.rt_d.create(info.albedo, info.width, info.height, D3DFMT_A8R8G8B8);
    g_tb.rt_nm.create(info.normal, info.width, info.height, D3DFMT_A8R8G8B8);
    if (!g_tb.rt_d || !g_tb.rt_nm || !g_tb.rt_d->pSurface || !g_tb.rt_nm->pSurface || !g_tb.d.wrap(g_tb.rt_d->pSurface) ||
        !g_tb.nm.wrap(g_tb.rt_nm->pSurface) || g_tb.d.levels < level_count(info.cell) || g_tb.d.w != info.width)
    {
        tree_release();
        return false;
    }
    // (Without it the billboards write lod.ps's material: no leaves.)
    if (info.material && g_tb.mtl.create(info.width / 2, info.height / 2, g_tb.d.levels - 1, DXGI_FORMAT_R8G8_UNORM))
    {
        g_tb.mtl_ref = RImplementation.Resources->_CreateTexture(info.material);
        g_tb.mtl_ref->surface_set(g_tb.mtl.tex);
    }
    g_tb.order_d = g_tb.rt_d->_order;
    g_tb.order_nm = g_tb.rt_nm->_order;
    g_tb.info = info;
    g_tb.st.assign(info.sets, TmplState{});
    g_tb.attached = true;
    g_tb.full = true;
    g_tb.reason = again ? "the atlas made again (a device reset)" : "the level's first frame";
    return true;
}

// The render pixels a metre at a metre of view depth the billboards are baked for: the field of view setting's, not
// the frame's (a scope, the health's narrowing and the adrenaline's widening would bake every template again).
float bake_px_per_m()
{
    const float render_h = float(std::max(Device.dwRenderHeight ? Device.dwRenderHeight : Device.dwHeight, 1u));
    return render_h / (2.f * std::tan(deg2rad(std::clamp(g_fov, 5.f, 179.f)) * 0.5f));
}

// The look every cell shares: the alpha reference, lod.ps's tuning, the billboards' pixels a metre at R, the pixel
// floor, the snow. Each in its steps, a step taken only three quarters of one past where it stands (a value hovering at
// a step's edge does not bake everything again and back); a step bakes again. The sun is no part of it: the billboard
// shader takes the parts' sunlit side under the frame's sun (da_tree_lod.ps; passing clouds moved the sun's share
// across steps and kept the whole atlas baking). Its elevation and share are kept for the stats.
u64 tree_look(const DaTreeBbAtlas& info)
{
    const Latch& L = latch();
    float elev = 0.f, share = 0.f;
    if (g_pGamePersistent)
    {
        const CEnvDescriptor& env = g_pGamePersistent->Environment().CurrentEnv;
        const float l_sun = std::max(0.2126f * env.sun_color.x + 0.7152f * env.sun_color.y + 0.0722f * env.sun_color.z, 0.f);
        const float l_sky = std::max(0.2126f * env.hemi_color.x + 0.7152f * env.hemi_color.y + 0.0722f * env.hemi_color.z, 0.f);
        share = l_sun / std::max(l_sun + l_sky, 1e-4f);
        elev = rad2deg(std::asin(std::clamp(L.sun.y, -1.f, 1.f)));
    }
    g_tb.snow = L.snow;
    g_tb.sun_e = elev;
    g_tb.sun_share = share;
    const float px_R = bake_px_per_m() / std::max(info.R, 1.f);
    const float v[] = {float(ps_r__aref_quality), ps_r__lod_hemi * 100.f, ps_r__lod_sat * 100.f, ps_r__lod_bright * 100.f,
        std::log2(std::max(px_R, 1e-3f)) * 20.f, info.kpx * 100.f, L.snow * 10.f, 0.f, 0.f, float(info.cell)};
    static_assert(std::size(v) == kLookParts);
    for (u32 i = 0; i < kLookParts; ++i)
        if (!g_tb.look_set || std::abs(v[i] - float(g_tb.look_q[i])) > 0.75f)
            g_tb.look_q[i] = s32(std::lround(v[i]));
    g_tb.look_set = true;
    return fnv(g_tb.look_q, sizeof(g_tb.look_q), kFnvOffset);
}

// A species' season rows at the day (the tables the frame filled), quantised.
u64 tree_season(u32 species)
{
    const Fvector4* t = tables();
    return species < kSpecies ? hash_floats(&t[kTabSeas + species * kSeasF4].x, kSeasF4 * 4, 1.f / 256.f) : 0;
}

// A cell set's batch item (record `row`): its template's record in the set's look, its chunks (the far tiers at R: the
// stock wood's coarsest rings, its patches as the stock has them - the far list, not the near one's tessellation -, the
// clusters the pixel floor keeps, unthinned: the view's clusters run on to R and dissolve into the billboard, so it
// shows what they show - baked from the blobs, a poplar's billboard stood as flat daubs with the sky between them),
// its views. False: no item; `later` with it: its chunks do not fit the batch's room beside the items before it (it
// waits for the next batch whole - a cut one baked half a tree).
bool tree_item(const Population& P, u32 set, u32 row, TreeBake::Item& it, xr_vector<u32>& recs, xr_vector<ChunkRec>& chunks, bool& later)
{
    later = false;
    if (!da_trees_bb_template(set, it.tb) || it.tb.tmpl >= P.templates.size())
        return false;
    const Template& T = P.templates[it.tb.tmpl];
    const DaTreeBbTemplate& tb = it.tb;
    const float px = bake_px_per_m() / std::max(g_tb.info.R, 1.f);
    const float keep_c = floor_keep(2.f * T.cluster_r, px, std::max(g_tb.info.kpx, 0.5f) * 2.f);
    const u32 firsts[3] = {T.first[kKindStock] + T.stock_lod_first[2], T.first[kKindPatch] + T.count[kKindPatch], T.first[kKindCluster]};
    const u32 counts[3] = {T.stock_lod_count[2], T.patch_far, kept(T.count[kKindCluster], keep_c)};
    const float keeps[3] = {1.f, 1.f, keep_c};
    size_t need = 0;
    for (const u32 c : counts)
        need += (c + kChunk - 1) / kChunk;
    if (chunks.size() + need > kMaxBakeChunks)
    {
        later = !chunks.empty();
        return false;
    }
    it.set = set;
    // The record: the template on itself (identity), no wind and no sway, its look's flags, hemi and sun 1 (the
    // hemisphere light's per-placement part goes through the facets' vertex light).
    u32 rec[kInstRec / 4]{};
    const float rows[12] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    CopyMemory(rec, rows, sizeof(rows));
    const u32 flags = (tb.dead ? kInstDead : 0u) | kInstTree | (tb.red ? kInstRed : 0u);
    // (A young tree's spring lead is 0.6 .. 1 of its youth's share, da_trees.cpp build: their mean.)
    rec[12] = (tb.species & 0xFFu) | (flags << 8) | (u8f(tb.youth * 0.8f) << 24);
    rec[13] = T.axis_first;
    rec[14] = u8f(1.f) | (u8f(1.f) << 8);
    rec[15] = f16(1.f) | (0x5bd1u << 16);
    rec[16] = h2(1.f, 0.f);
    rec[17] = h2(0.f, 0.f);
    rec[18] = h2(1.f, 0.f);
    rec[19] = h2(0.f, 0.f);
    rec[20] = h2(0.f, 0.f);
    rec[21] = h2(0.f, 0.f);
    rec[22] = h2(T.height, T.climb_top);
    rec[23] = (T.aux_first & 0xFFFFFFu) | (u8f(tb.youth) << 24);
    // (No axis records: the bake's chunks are rigid.)
    rec[24] = ~0u;
    rec[25] = T.axis_count;
    recs.insert(recs.end(), std::begin(rec), std::end(rec));
    for (u32 k = 0; k < 3; ++k)
    {
        it.first[k] = u32(chunks.size());
        for (u32 e = 0; e < counts[k]; e += kChunk)
            chunks.push_back(ChunkRec{row, firsts[k] + e, std::min(kChunk, counts[k] - e) | (255u << 16) | kChunkRigid, h2(keeps[k], 0.f)});
        it.count[k] = u32(chunks.size()) - it.first[k];
    }
    // The views: each facet's quad onto its cell inside the gutter (the view fills the whole cell, the gutter the trees
    // beyond the facet's edges).
    const float gu = float(g_tb.info.gutter) / float(g_tb.info.width), gv = float(g_tb.info.gutter) / float(g_tb.info.height);
    Fsphere sphere;
    sphere.set(T.centre, std::max(T.radius, 0.5f));
    for (u32 f = 0; f < 8; ++f)
    {
        const DaTreeBbView& v = tb.view[f];
        const Fvector4 cell{v.rect.x - gu, v.rect.y - gv, v.rect.z + gu, v.rect.w + gv};
        it.ok[f] = view_xf(v.corner, v.uv, v.normal, cell, sphere, it.view[f]);
    }
    // The hemisphere light over what lod.vs and lod.ps scale the atlas's by: the facets' vertex light (the template's mean
    // ratio of its trees' records' light to it) x 3.1 x r__lod_hemi.
    it.hscale = tb.hemi / (kLodScale * std::max(ps_r__lod_hemi, 0.05f));
    it.season = tree_season(tb.species);
    return true;
}

// The template's far tiers into view f of its item.
void tree_draw_view(CBackend& cmd, const Population& P, const TreeBake::Item& it, u32 f)
{
    const ViewXf& vx = it.view[f];
    const u32 cell = g_tb.info.cell;
    begin_view(cmd, cell, cell, kSS);
    cmd.set_xform_world(Fidentity);
    cmd.set_xform_view(vx.V);
    cmd.set_xform_project(vx.P);
    // The camera's metres a pixel at a metre of depth, so that at the view's depth a pixel is a runtime pixel at R: the
    // far tiers' sizes on the screen, their wire rule and their leaves' clumps decide as they do at the switch.
    const Latch& L = latch();
    const float m_px = std::max(g_tb.info.R, 1.f) / std::max(bake_px_per_m(), 1.f) / std::max(vx.depth, 1.f);
    static constexpr u32 kDraws[3] = {drStock8, drPatch, drCluster};
    const ref_shader* progs[3] = {&g_sh.s_stock, &g_sh.s_patch, &g_sh.s_mass};
    for (u32 k = 0; k < 3; ++k)
    {
        if (!it.count[k])
            continue;
        const u32 d = kDraws[k];
        cmd.set_Element((*progs[k])->E[0]);
        cmd.set_c("da_shrub_draw", float(chunk_verts(d) / kChunk), float(kDrawKind[d]), float(d), 0.f);
        cmd.set_c("da_shrub_chunk0", float(it.first[k]), 0.f, 0.f, 0.f);
        // (The clusters as if sixteen times nearer: their leaves resolved into cells - a few pixels across at R a part
        // is its mean blot, a disc as wide as the root of its cover, and a thin crown's discs stood apart as dots and
        // dashes with the sky between them - and clipped against the dither, the bake's samples averaging both into the
        // crown's true cover; the wood keeps a runtime pixel at R.)
        cmd.set_c("da_shrub_cam", vx.eye.x, vx.eye.y, vx.eye.z, k == 2 ? m_px / kBakeLeafZoom : m_px);
        cmd.set_c("da_shrub_fwd", vx.fwd.x, vx.fwd.y, vx.fwd.z, 0.f);
        cmd.set_c("da_shrub_time", 0.f, 0.f, 0.f, 0.f);
        // (The sun straight ahead of the camera: the leaves' translucency at its full value, the billboard's shader turns
        // it to the frame's sun, and the parts' sunlit side too.)
        cmd.set_c("da_shrub_sun", vx.fwd.x, vx.fwd.y, vx.fwd.z, L.snow);
        Fvector up;
        vx.V.transform_dir(up, Fvector{0.f, 1.f, 0.f});
        cmd.set_c("da_shrub_up", up.x, up.y, up.z, 0.f);
        cmd.set_c("da_shrub_bend_cap", 0.f, 0.f, 0.f, 0.f);
        // (No wind motor bends a baked tree.)
        cmd.set_c("da_wm_info", 0.f, 0.f, 0.f, 0.f);
        cmd.set_c("da_wm_info_prev", 0.f, 0.f, 0.f, 0.f);
        cmd.SRVSManager.SetVSResource(0, P.elems_b.srv);
        cmd.SRVSManager.SetVSResource(1, P.axes_b.srv);
        cmd.SRVSManager.SetVSResource(2, g_sh.inst_b.srv);
        cmd.SRVSManager.SetVSResource(3, g_sh.chunks_b.srv);
        cmd.SRVSManager.SetVSResource(4, g_sh.tables_b.srv);
        cmd.SRVSManager.SetVSResource(5, g_sh.dummy_b.srv);
        cmd.SRVSManager.SetVSResource(6, P.aux_b.srv ? P.aux_b.srv : g_sh.dummy_b.srv);
        cmd.SRVSManager.SetPSResource(4, g_sh.tables_b.srv);
        draw_chunks(cmd, d, it.count[k]);
        unbind(cmd);
    }
}

// The batch's cells into the atlas pair, level by level (da_tree_bake_mip.ps), after the chain and the factors.
void tree_finish(CBackend& cmd)
{
    const u32 cell = g_tb.info.cell, n = u32(g_tb.batch.size());
    const u32 levels = std::min(level_count(cell), g_tb.d.levels);
    chain(cmd, 8 * cell, n * cell, levels);
    xr_vector<u32> cells;
    for (u32 r = 0; r < n; ++r)
        for (u32 f = 0; f < 8; ++f)
            cells.insert(cells.end(), {f * cell, r * cell, cell, cell});
    cover(cmd, cells, levels, false);
    xr_vector<QuadRec> q;
    const float W = float(g_tb.info.width), H = float(g_tb.info.height);
    for (u32 m = 0; m < levels; ++m)
    {
        const float wm = float(std::max(g_tb.info.width >> m, 1u)), hm = float(std::max(g_tb.info.height >> m, 1u));
        const u32 cm = std::max(cell >> m, 1u);
        for (u32 r = 0; r < n; ++r)
            for (u32 f = 0; f < 8; ++f)
            {
                const DaTreeBbView& v = g_tb.batch[r].tb.view[f];
                const u32 ax = u32(std::lround(v.rect.x * W)) - g_tb.info.gutter, ay = u32(std::lround(v.rect.y * H)) - g_tb.info.gutter;
                const float x0 = float(ax >> m), y0 = float(ay >> m), sx = float((f * cell) >> m), sy = float((r * cell) >> m);
                q.push_back(quad(x0, y0, x0 + float(cm), y0 + float(cm), wm, hm, sx, sy, sx + float(cm), sy + float(cm), r * 8 + f,
                    std::bit_cast<u32>(g_tb.batch[r].hscale)));
            }
    }
    upload(cmd, g_sh.quads_b, 0, q.data(), u32(q.size() * sizeof(QuadRec)));
    for (u32 m = 0; m < levels; ++m)
    {
        const u32 wm = std::max(g_tb.info.width >> m, 1u), hm = std::max(g_tb.info.height >> m, 1u);
        // (The foliage atlas's level a level of the pair's down: none for the top.)
        ID3D11RenderTargetView* rm = m && m - 1 < g_tb.mtl.levels ? g_tb.mtl.rtv[m - 1] : nullptr;
        RImplementation.Target->u_setrt(cmd, wm, hm, g_tb.d.rtv[m], g_tb.nm.rtv[m], rm, nullptr);
        cmd.SetViewport({0.f, 0.f, float(wm), float(hm), 0.f, 1.f});
        cmd.set_Element((rm ? g_sh.s_mip : g_sh.s_mip_top)->E[0]);
        cmd.set_c("da_tree_bake_m", float(m), 0.f, float(levels), 0.f);
        cmd.set_c("da_tree_bake_t", ps_r__lod_sat, ps_r__lod_bright, 0.f, 0.f);
        cmd.SRVSManager.SetPSResource(0, g_sh.fill_d.all);
        cmd.SRVSManager.SetPSResource(1, g_sh.fill_nm.all);
        cmd.SRVSManager.SetPSResource(2, g_sh.k_b.srv);
        cmd.SRVSManager.SetPSResource(3, g_sh.fill_m.all);
        draw_quads_own(cmd, m * n * 8, n * 8);
        unbind(cmd);
    }
    const float day = da_season_day();
    for (const TreeBake::Item& it : g_tb.batch)
    {
        da_trees_bb_mark(it.set, false);
        g_tb.st[it.set] = TmplState{true, g_tb.look, it.season, day};
        ++g_tb.baked;
    }
    g_tb.batch.clear();
    g_tb.views_done = 0;
}

// The stale cell sets, the stalest first: never baked or asked for (the atlas's dirty flag), then the ones the presets,
// the snow or the sun moved, then the ones whose species' table changed over two days of the season.
void tree_stale(const Population& P, xr_vector<u32>& out, float& top)
{
    out.clear();
    top = 0.f;
    const float day = da_season_day();
    xr_vector<std::pair<float, u32>> ranked;
    for (u32 t = 0; t < g_tb.st.size(); ++t)
    {
        DaTreeBbTemplate tb;
        if (!da_trees_bb_template(t, tb) || tb.tmpl >= P.templates.size())
            continue;
        const TmplState& s = g_tb.st[t];
        float rank = -1.f;
        if (!s.baked || tb.dirty)
            rank = 1000.f;
        else if (s.look != g_tb.look)
            rank = 500.f;
        else if (const float gap = day_gap(s.day, day); gap >= kDaySteps && s.season != tree_season(tb.species))
            rank = gap;
        if (rank >= 0.f)
            ranked.emplace_back(rank, t);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [r, t] : ranked)
        out.push_back(t);
    top = ranked.empty() ? 0.f : ranked.front().first;
}

// This frame's work on the trees' atlas: everything when a full bake is due, else views within the budget.
u32 tree_frame(Work& work)
{
    CBackend& cmd = work.cmd;
    Population* P = population(kPopTrees);
    DaTreeBbAtlas info;
    if (!P || !P->active || !P->detached || !g_sh.trees || !da_trees_bb_atlas(info) || !info.templates)
    {
        if (g_tb.attached && (!P || !P->loaded))
            tree_release();
        return 0;
    }
    if (!tree_attach(info))
        return 0;
    g_tb.info = info;
    const u64 look = tree_look(info);
    const float day = da_season_day();
    g_tb.look = look;
    // (A jump of the season - a save of another day, a time skip, the QA pins - takes every cell set at once: within
    // the progressive budget a January forest stood seconds in July's billboards.)
    if (g_tb.seen_day > -1e8f && day_gap(g_tb.seen_day, day) > kDaySteps && !g_tb.full)
    {
        g_tb.full = true;
        g_tb.reason = "a jump of the season";
    }
    g_tb.seen_day = day;
    if (g_tb.full)
    {
        g_tb.batch.clear();
        g_tb.views_done = 0;
    }
    else if (g_tb.batch.empty() && g_tb.idle && g_tb.idle_look == look && day_gap(g_tb.idle_day, day) < 0.01f)
        return 0;
    g_tb.idle = false;
    const bool full = g_tb.full;
    u32 budget = full ? ~0u : u32(std::clamp(kBudgetMs / std::max(g_tb.view_cost, 1e-3f), 1.f, 64.f));
    const u32 rows = std::min(kStage / info.cell, kMaxInst);
    const u64 t0 = CPU::QPC();
    u32 views = 0;
    xr_vector<u32> stale, recs;
    xr_vector<ChunkRec> chunks;
    bool tables_up = false;
    while (budget)
    {
        if (g_tb.batch.empty())
        {
            float top;
            tree_stale(*P, stale, top);
            if (stale.empty())
            {
                g_tb.idle = true;
                g_tb.idle_day = day;
                g_tb.idle_look = look;
                break;
            }
            if (!full)
                g_tb.reason = top >= 1000.f ? "a template asked for" : (top >= 500.f ? "the presets or the snow" : "the season's day");
            work.begin();
            // A full bake fills the staging's rows; a progressive one a template at a time.
            const u32 n = full ? std::min(u32(stale.size()), rows) : 1u;
            recs.clear();
            chunks.clear();
            for (u32 i = 0; i < stale.size() && g_tb.batch.size() < n; ++i)
            {
                TreeBake::Item it;
                bool later = false;
                if (tree_item(*P, stale[i], u32(g_tb.batch.size()), it, recs, chunks, later))
                    g_tb.batch.push_back(it);
                else if (later)
                    break;
                else
                    g_tb.st[stale[i]] = TmplState{true, look, 0, day};
            }
            if (g_tb.batch.empty())
                continue;
            upload(cmd, g_sh.inst_b, 0, recs.data(), u32(recs.size() * 4));
            upload(cmd, g_sh.chunks_b, 0, chunks.data(), u32(chunks.size() * sizeof(ChunkRec)));
            clear_stage(cmd);
            g_tb.views_done = 0;
        }
        work.begin();
        if (!tables_up)
        {
            upload(cmd, g_sh.tables_b, 0, tables(), kTabTotal * 16);
            tables_up = true;
        }
        const u32 total = u32(g_tb.batch.size()) * 8;
        while (g_tb.views_done < total && budget)
        {
            const u32 r = g_tb.views_done / 8, f = g_tb.views_done % 8;
            const TreeBake::Item& it = g_tb.batch[r];
            if (it.ok[f])
            {
                tree_draw_view(cmd, *P, it, f);
                resolve(cmd, f * info.cell, r * info.cell, info.cell, info.cell, kSS, 0.f, 0, kOne);
                ++views;
                --budget;
            }
            ++g_tb.views_done;
        }
        if (g_tb.views_done >= total)
            tree_finish(cmd);
    }
    if (full)
    {
        g_tb.full = false;
        if (views)
        {
            g_tb.full_ms = float(CPU::QPC() - t0) * 1000.f / float(CPU::qpc_freq);
            ++g_tb.full_bakes;
            Msg("* [trees] billboards baked (%s): %u cell sets (%u templates, %u in the dead look of their dead trees), %u views in %.0f ms "
                "on the CPU, %ux%u atlas of %u px cells%s, day %.1f",
                g_tb.reason, u32(std::count_if(g_tb.st.begin(), g_tb.st.end(), [](const TmplState& s) { return s.baked; })), info.templates,
                info.sets - info.templates, views, g_tb.full_ms, info.width, info.height, info.cell,
                g_tb.mtl.tex ? ", the foliage atlas half that" : ", no foliage atlas (lod.ps's material)", day);
        }
    }
    else if (views)
        ++g_tb.rebakes;
    g_tb.views += views;
    return views;
}

// ======================================================================================================
// The stock path's atlas (level_lods)
// ======================================================================================================
// A model's cell set: its first placement's FLOD (the facets' quads and uv), its parts with their cards.
struct LodsPart
{
    const FTreeVisual* visual{};
    shared_str card;
    bool cut{};
};
struct LodsSet
{
    const FLOD* flod{};
    xr_vector<LodsPart> parts;
    u32 cell[8][4]{}; // atlas pixels: x, y, w, h (w 0: not drawn again)
    // A placement on the stock path; the populations that replace its others (bits by population index).
    bool stock{};
    u8 pops{};
};
struct LodsAtlas
{
    ref_texture tex;
    xr_vector<LodsSet> sets;
    u32 placements{}, replaced{};
    // Any set on the stock path; the populations replacing any; theirs the copy holds drawn again.
    bool stock{};
    u8 pops{}, drawn{};
    MipTex composed;
    const DaSeasonLatch* latch{};
    // The copy's view as the texture made it (held: no view made after it can take its address).
    ID3D11ShaderResourceView* bound{};
    bool calibrated{}, failed{};
    float gain[3]{1.f, 1.f, 1.f};
    xr_vector<float> lut; // its coverages, then its alphas
    u32 lut_n{}, cells{}, models{}, composes{};
    float last_ms{};
};
struct LodsBake
{
    bool init{};
    xr_vector<LodsAtlas> atlases;
    u32 candidates{};
};
LodsBake g_lb;
bool g_rebake_lods{};

// The base texture of a level shader string ("shader/texture,texture2").
void base_texture(pcstr shader, string_path& out)
{
    out[0] = 0;
    pcstr slash = shader ? strchr(shader, '/') : nullptr;
    if (!slash)
        return;
    xr_strcpy(out, slash + 1);
    if (pstr comma = strchr(out, ','))
        *comma = 0;
}

// A part its shader cuts out of its card (the uber blender's "_aref" pixel shader: foliage, never a trunk).
bool cut_out(const FTreeVisual* v)
{
    if (!v->shader || !v->shader->E[0] || v->shader->E[0]->passes.empty())
        return false;
    const ref_ps& ps = v->shader->E[0]->passes[0]->ps;
    return ps && ps->cName.c_str() && strstr(ps->cName.c_str(), "_aref");
}

// The census's FLODs wearing a trees\ texture, by atlas and model (the facets' uv rects): the stock path's, and the
// populations' (the stock path's again while their stock models stand: r__shrubs / r__trees 0, a load that failed).
void lods_init()
{
    g_lb.init = true;
    if (!g_sh.lods)
        return;
    Census& C = census();
    std::map<std::pair<const CTexture*, xr_vector<s32>>, std::pair<u32, u32>> sets;
    string_path name;
    for (const Candidate& c : C.candidates)
    {
        const u32 pop = c.taken == ~0u ? 0u : (c.taken < kPops ? 1u << c.taken : ~0u);
        if (pop == ~0u || !c.flod || c.flod->Type != MT_LOD)
            continue;
        if (!pop)
            ++g_lb.candidates;
        bool veg = false;
        for (const u32 pi : c.parts)
        {
            base_texture(C.parts[pi].shader, name);
            veg = veg || !_strnicmp(name, "trees\\", 6);
        }
        if (!veg)
            continue;
        const FLOD* flod = static_cast<const FLOD*>(c.flod);
        // The atlas its facets draw with: the stock billboard shader's s_base (level_lods; not its _nm).
        CTexture* atlas = nullptr;
        if (flod->shader && flod->shader->E[SE_R1_LMODELS])
            for (const ref_pass& pass : flod->shader->E[SE_R1_LMODELS]->passes)
            {
                if (!pass || !pass->T)
                    continue;
                for (const auto& [stage, t] : *pass->T)
                {
                    pcstr n = t ? t->cName.c_str() : nullptr;
                    const size_t len = n ? xr_strlen(n) : 0;
                    if (len && !(len > 3 && !xr_stricmp(n + len - 3, "_nm")))
                        atlas = &*t;
                }
            }
        if (!atlas)
            continue;
        xr_vector<s32> key;
        for (const FLOD::_face& F : flod->facets)
            for (const FLOD::_vertex& v : F.v)
                key.insert(key.end(), {s32(std::lround(v.t.x * 10000.f)), s32(std::lround(v.t.y * 10000.f))});
        const auto [it, fresh] = sets.try_emplace({atlas, key}, std::pair<u32, u32>{});
        if (fresh)
        {
            u32 a = 0;
            while (a < g_lb.atlases.size() && &*g_lb.atlases[a].tex != atlas)
                ++a;
            if (a == g_lb.atlases.size())
            {
                g_lb.atlases.emplace_back();
                g_lb.atlases.back().tex = atlas;
            }
            LodsSet s;
            s.flod = flod;
            for (const u32 pi : c.parts)
            {
                base_texture(C.parts[pi].shader, name);
                if (name[0])
                    s.parts.push_back(LodsPart{C.parts[pi].visual, name, cut_out(C.parts[pi].visual)});
            }
            it->second = {a, u32(g_lb.atlases[a].sets.size())};
            g_lb.atlases[a].sets.push_back(std::move(s));
        }
        LodsAtlas& A = g_lb.atlases[it->second.first];
        LodsSet& S = A.sets[it->second.second];
        if (pop)
        {
            S.pops |= u8(pop);
            A.pops |= u8(pop);
            ++A.replaced;
        }
        else
        {
            S.stock = A.stock = true;
            ++A.placements;
        }
    }
    for (const LodsAtlas& A : g_lb.atlases)
        Msg("* [lods] %s: %u stock-path vegetation placements of %u models to draw again (of %u stock-path FLODs of tree parts); %u "
            "placements of %u models the shrubs and trees replace, drawn again while their stock models stand",
            A.tex->cName.c_str(), A.placements, u32(std::count_if(A.sets.begin(), A.sets.end(), [](const LodsSet& S) { return S.stock; })),
            g_lb.candidates, A.replaced, u32(std::count_if(A.sets.begin(), A.sets.end(), [](const LodsSet& S) { return S.pops != 0; })));
}

// A texture loaded from its file as it is (the base set: a latched look of it is another file); its view (and texture).
ID3D11ShaderResourceView* load_file(pcstr folder, pcstr name, ID3D11Texture2D** tex_out = nullptr)
{
    string_path fn;
    if (!FS.exist(fn, folder, name, ".dds"))
        return nullptr;
    IReader* r = FS.r_open(fn);
    if (!r)
        return nullptr;
    xr_vector<u8> bytes(static_cast<const u8*>(r->pointer()), static_cast<const u8*>(r->pointer()) + r->length());
    FS.r_close(r);
    // (Legacy D3DX wrote the header's size with the magic: 128.)
    if (bytes.size() >= 8 && !memcmp(bytes.data(), "DDS ", 4))
    {
        u32 size;
        memcpy(&size, bytes.data() + 4, 4);
        if (size == 128)
        {
            size = 124;
            memcpy(bytes.data() + 4, &size, 4);
        }
    }
    DirectX::TexMetadata meta;
    DirectX::ScratchImage image;
    if (FAILED(DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_PERMISSIVE, &meta, image)))
        return nullptr;
    ID3D11Resource* res = nullptr;
    if (FAILED(DirectX::CreateTexture(HW.pDevice, image.GetImages(), image.GetImageCount(), meta, &res)))
        return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(HW.pDevice->CreateShaderResourceView(res, nullptr, &srv)))
        srv = nullptr;
    if (tex_out && srv)
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex_out));
    res->Release();
    return srv;
}

// A texture the engine holds, loaded; its view.
ID3D11ShaderResourceView* engine_srv(CTexture* t)
{
    if (!t)
        return nullptr;
    if (!t->flags.bLoaded)
        t->Load();
    else if (t->ds_pending())
        t->ds_finish();
    return t->get_SRView();
}

// The slot a cell takes in the staging (the power of two its larger side fits).
u32 slot_size(const u32* cell)
{
    u32 s = 1;
    while (s < std::max(cell[2], cell[3]))
        s <<= 1;
    return s;
}

// The cells of a batch in the staging, in rows of the largest slot first (every slot on its own size's grid, so the
// chain's levels keep each to itself); `todo` sorted by slot, larger first.
struct LodsSlot
{
    u32 set{}, view{}, x{}, y{};
};
bool lods_pack(const LodsAtlas& A, const xr_vector<std::pair<u32, u32>>& todo, u32& at, xr_vector<LodsSlot>& out, u32& used_h, u32& max_size)
{
    out.clear();
    used_h = 0;
    max_size = 1;
    u32 x = 0, y = 0, row = 0;
    while (at < todo.size())
    {
        const auto [si, f] = todo[at];
        const u32 s = slot_size(A.sets[si].cell[f]);
        if (x + s > kStage)
        {
            x = 0;
            y += row;
            row = 0;
        }
        if (y + s > kStage || out.size() >= kMaxCells)
            break;
        if (!row)
            row = s;
        out.push_back(LodsSlot{si, f, x, y});
        x += s;
        used_h = std::max(used_h, y + s);
        max_size = std::max(max_size, s);
        ++at;
    }
    return !out.empty();
}

// One view of a cell set drawn from its parts (their cards by name), resolved into the staging at its slot.
void lods_view(CBackend& cmd, const LodsAtlas& A, const LodsSlot& slot, const xr_map<shared_str, ID3D11ShaderResourceView*>& cards, float mode)
{
    const LodsSet& S = A.sets[slot.set];
    const u32* c = S.cell[slot.view];
    const u32 ss = std::clamp(kSSize / std::max({c[2], c[3], 1u}), 1u, kSS);
    const FLOD::_face& F = S.flod->facets[slot.view];
    Fvector corner[4];
    Fvector2 uv[4];
    for (u32 k = 0; k < 4; ++k)
    {
        corner[k] = F.v[k].v;
        uv[k] = F.v[k].t;
    }
    const float W = float(A.composed.w), H = float(A.composed.h);
    const Fvector4 cell{float(c[0]) / W, float(c[1]) / H, float(c[0] + c[2]) / W, float(c[1] + c[3]) / H};
    ViewXf vx;
    if (!view_xf(corner, uv, F.N, cell, S.flod->vis.sphere, vx))
        return;
    begin_view(cmd, c[2], c[3], ss);
    cmd.set_xform_view(vx.V);
    cmd.set_xform_project(vx.P);
    for (const LodsPart& part : S.parts)
    {
        FTreeVisualInstancedDraw w;
        const auto it = cards.find(part.card);
        if (it == cards.end() || !it->second || !part.visual->rm_geom || !part.visual->window(0, w))
            continue;
        cmd.set_xform_world(part.visual->world());
        cmd.set_Element(g_sh.s_part->E[0]);
        cmd.set_c("da_tree_bake_uv", 1.f / float(FTreeVisual_quant), 0.f, 0.f, 0.f);
        cmd.set_c("da_tree_bake_part", part.cut ? 1.f : 0.f, 0.f, 0.f, 0.f);
        cmd.SRVSManager.SetPSResource(0, it->second);
        cmd.SRVSManager.Apply(cmd.context_id);
        cmd.set_Geometry(&*part.visual->rm_geom);
        cmd.Render(D3DPT_TRIANGLELIST, w.base_vertex, 0, w.vertex_count, w.start_index, w.primitive_count);
        unbind(cmd);
    }
    cmd.set_xform_world(Fidentity);
    resolve(cmd, slot.x, slot.y, c[2], c[3], ss, mode, A.lut_n, A.gain);
}

// The level's cards: the latched ones the engine holds, or the base set's files (the calibration).
void lods_cards(const LodsAtlas& A, bool base, xr_map<shared_str, ID3D11ShaderResourceView*>& out, xr_vector<ID3D11ShaderResourceView*>& owned)
{
    const DaSeasonLatch* latch = texture_season();
    for (const LodsSet& S : A.sets)
        for (const LodsPart& p : S.parts)
        {
            if (out.find(p.card) != out.end())
                continue;
            string_path scratch;
            ID3D11ShaderResourceView* v = nullptr;
            if (base && texture_season_overlay(p.card.c_str(), latch, scratch))
            {
                v = load_file("$game_textures$", p.card.c_str());
                if (v)
                    owned.push_back(v);
            }
            if (!v)
                v = engine_srv(RImplementation.Resources->_CreateTexture(p.card.c_str()));
            out.emplace(p.card, v);
        }
}

// The calibration (once a level): the vegetation cells drawn with the base cards against Dead Air's own cells - the
// alpha's quantile map from the coverage and the colour's linear gains over the texels both cover (seasons_lods.py
// calibration()).
void lods_calibrate(CBackend& cmd, LodsAtlas& A, const xr_vector<std::pair<u32, u32>>& todo)
{
    A.calibrated = true;
    xr_map<shared_str, ID3D11ShaderResourceView*> cards;
    xr_vector<ID3D11ShaderResourceView*> owned;
    lods_cards(A, true, cards, owned);
    // CPU copies of the staging's level 0 (the cells drawn) and the filled copy's (Dead Air's cells packed beside them).
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = kStage;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *cpu_mine = nullptr, *cpu_da = nullptr;
    const bool made = SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &cpu_mine)) && SUCCEEDED(HW.pDevice->CreateTexture2D(&desc, nullptr, &cpu_da));
    ID3D11DeviceContext* ctx = HW.get_context(cmd.context_id);
    constexpr u32 kBins = 4096;
    xr_vector<u64> h_mine(kBins, 0), h_da(kBins, 0);
    double sum_mine[3]{}, sum_da[3]{};
    u64 n_gain = 0, n_all = 0;
    const auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    u32 at = 0;
    xr_vector<LodsSlot> slots;
    u32 used_h = 0, max_size = 0;
    while (made && lods_pack(A, todo, at, slots, used_h, max_size))
    {
        clear_stage(cmd);
        for (const LodsSlot& s : slots)
            lods_view(cmd, A, s, cards, 1.f);
        // Dead Air's cells beside them, from the copy of the base atlas (mode 2).
        xr_vector<QuadRec> q;
        for (const LodsSlot& s : slots)
        {
            const u32* c = A.sets[s.set].cell[s.view];
            q.push_back(quad(float(s.x), float(s.y), float(s.x + c[2]), float(s.y + c[3]), float(kStage), float(kStage), float(c[0]),
                float(c[1]), float(c[0] + c[2]), float(c[1] + c[3])));
        }
        upload(cmd, g_sh.quads_b, 0, q.data(), u32(q.size() * sizeof(QuadRec)));
        RImplementation.Target->u_setrt(cmd, kStage, kStage, g_sh.fill_d.rtv[0], nullptr, nullptr, nullptr);
        cmd.ClearRT(g_sh.fill_d.rtv[0], Fcolor{0.f, 0.f, 0.f, 0.f});
        cmd.SetViewport({0.f, 0.f, float(kStage), float(kStage), 0.f, 1.f});
        cmd.set_Element(g_sh.s_lods->E[0]);
        cmd.set_c("da_tree_bake_m", 0.f, 2.f, 1.f, 0.f);
        cmd.SRVSManager.SetPSResource(0, A.composed.all);
        draw_quads_own(cmd, 0, u32(q.size()));
        unbind(cmd);
        ctx->CopySubresourceRegion(cpu_mine, 0, 0, 0, 0, g_sh.stage_d.tex, 0, nullptr);
        ctx->CopySubresourceRegion(cpu_da, 0, 0, 0, 0, g_sh.fill_d.tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE mm{}, md{};
        if (FAILED(ctx->Map(cpu_mine, 0, D3D11_MAP_READ, 0, &mm)))
            break;
        if (FAILED(ctx->Map(cpu_da, 0, D3D11_MAP_READ, 0, &md)))
        {
            ctx->Unmap(cpu_mine, 0);
            break;
        }
        for (const LodsSlot& s : slots)
        {
            const u32* c = A.sets[s.set].cell[s.view];
            for (u32 y = 0; y < c[3]; ++y)
            {
                const u8* rm = static_cast<const u8*>(mm.pData) + size_t(s.y + y) * mm.RowPitch + size_t(s.x) * 4;
                const u8* rd = static_cast<const u8*>(md.pData) + size_t(s.y + y) * md.RowPitch + size_t(s.x) * 4;
                for (u32 x = 0; x < c[2]; ++x, rm += 4, rd += 4)
                {
                    ++h_mine[u32(rm[3]) * (kBins - 1) / 255u];
                    ++h_da[u32(rd[3]) * (kBins - 1) / 255u];
                    ++n_all;
                    // (Both opaque past 0.6, as the offline bake's.)
                    if (rm[3] > 153 && rd[3] > 153)
                    {
                        for (u32 k = 0; k < 3; ++k)
                        {
                            sum_mine[k] += lin(float(rm[k]) / 255.f);
                            sum_da[k] += lin(float(rd[k]) / 255.f);
                        }
                        ++n_gain;
                    }
                }
            }
        }
        ctx->Unmap(cpu_mine, 0);
        ctx->Unmap(cpu_da, 0);
    }
    _RELEASE(cpu_mine);
    _RELEASE(cpu_da);
    for (ID3D11ShaderResourceView* v : owned)
        v->Release();
    if (!n_all)
        return;
    // The quantiles 0 .. 100 % of both, the coverages made unique (their first), the alphas non-decreasing.
    const auto quantiles = [n_all](const xr_vector<u64>& h, float* out) {
        u64 acc = 0;
        u32 b = 0;
        for (u32 q = 0; q <= 100; ++q)
        {
            const u64 rank = u64(std::llround(double(q) / 100.0 * double(n_all - 1))) + 1;
            while (b < kBins - 1 && acc + h[b] < rank)
                acc += h[b++];
            out[q] = float(b) / float(kBins - 1);
        }
    };
    float xs[101], ys[101];
    quantiles(h_mine, xs);
    quantiles(h_da, ys);
    xr_vector<float> ux, uy;
    for (u32 q = 0; q <= 100; ++q)
        if (ux.empty() || xs[q] > ux.back())
        {
            ux.push_back(xs[q]);
            uy.push_back(uy.empty() ? ys[q] : std::max(uy.back(), ys[q]));
        }
    A.lut_n = u32(ux.size());
    A.lut.assign(ux.begin(), ux.end());
    A.lut.insert(A.lut.end(), uy.begin(), uy.end());
    if (n_gain >= 200)
        for (u32 k = 0; k < 3; ++k)
            A.gain[k] = std::clamp(float(sum_da[k] / std::max(sum_mine[k], 1e-4)), 0.25f, 2.f);
    Msg("* [lods] %s calibrated against Dead Air's bake: gains %.2f %.2f %.2f over %llu texels, an alpha map of %u points (coverage "
        "%.2f %.2f %.2f -> alpha %.2f %.2f %.2f at 25, 50, 75 %%)",
        A.tex->cName.c_str(), A.gain[0], A.gain[1], A.gain[2], (unsigned long long)n_gain, A.lut_n, xs[25], xs[50], xs[75], ys[25], ys[50],
        ys[75]);
}

// The base atlas into the copy, every level as it is; the cells in the atlas's pixels.
bool lods_base(CBackend& cmd, LodsAtlas& A)
{
    CTexture* tex = &*A.tex;
    // Dead Air's bake: the texture's file when a look of it is latched or the texture holds a copy of ours, else its surface.
    string_path scratch;
    ID3D11Texture2D* base_tex = nullptr;
    ID3D11ShaderResourceView* base = nullptr;
    bool owned = false;
    const bool ours = A.bound && engine_srv(tex) == A.bound;
    if (ours || texture_season_overlay(tex->cName.c_str(), texture_season(), scratch))
    {
        base = load_file("$level$", tex->cName.c_str(), &base_tex);
        owned = bool(base);
    }
    if (!base && !ours)
    {
        base = engine_srv(tex);
        if (ID3DBaseTexture* surf = tex->surface_get())
        {
            surf->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&base_tex));
            surf->Release();
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    if (base_tex)
        base_tex->GetDesc(&desc);
    const bool ok = base && base_tex && desc.Width && A.composed.create(desc.Width, desc.Height, std::max(desc.MipLevels, 1u), DXGI_FORMAT_R8G8B8A8_UNORM);
    if (ok)
    {
        xr_vector<QuadRec> q;
        for (u32 m = 0; m < A.composed.levels; ++m)
        {
            const float wm = float(std::max(A.composed.w >> m, 1u)), hm = float(std::max(A.composed.h >> m, 1u));
            q.push_back(quad(0.f, 0.f, wm, hm, wm, hm, 0.f, 0.f, wm, hm));
        }
        upload(cmd, g_sh.quads_b, 0, q.data(), u32(q.size() * sizeof(QuadRec)));
        for (u32 m = 0; m < A.composed.levels; ++m)
        {
            const u32 wm = std::max(A.composed.w >> m, 1u), hm = std::max(A.composed.h >> m, 1u);
            RImplementation.Target->u_setrt(cmd, wm, hm, A.composed.rtv[m], nullptr, nullptr, nullptr);
            cmd.SetViewport({0.f, 0.f, float(wm), float(hm), 0.f, 1.f});
            cmd.set_Element(g_sh.s_lods->E[0]);
            cmd.set_c("da_tree_bake_m", float(m), 0.f, 1.f, 0.f);
            cmd.SRVSManager.SetPSResource(0, base);
            draw_quads_own(cmd, m, 1);
            unbind(cmd);
        }
    }
    _RELEASE(base_tex);
    if (owned)
        _RELEASE(base);
    if (!ok)
        return false;
    // The cells in the atlas's pixels (the offline bake's rounding of the facets' uv edges).
    for (LodsSet& S : A.sets)
        for (u32 f = 0; f < 8; ++f)
        {
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (const FLOD::_vertex& v : S.flod->facets[f].v)
            {
                u0 = std::min(u0, v.t.x);
                v0 = std::min(v0, v.t.y);
                u1 = std::max(u1, v.t.x);
                v1 = std::max(v1, v.t.y);
            }
            const s32 x0 = s32(std::floor(u0 * float(A.composed.w) + 0.01f)), x1 = s32(std::ceil(u1 * float(A.composed.w) - 0.01f));
            const s32 y0 = s32(std::floor(v0 * float(A.composed.h) + 0.01f)), y1 = s32(std::ceil(v1 * float(A.composed.h) - 0.01f));
            const bool inside = x0 >= 0 && y0 >= 0 && x1 > x0 && y1 > y0 && u32(x1) <= A.composed.w && u32(y1) <= A.composed.h &&
                u32(x1 - x0) <= kSSize && u32(y1 - y0) <= kSSize;
            S.cell[f][0] = inside ? u32(x0) : 0;
            S.cell[f][1] = inside ? u32(y0) : 0;
            S.cell[f][2] = inside ? u32(x1 - x0) : 0;
            S.cell[f][3] = inside ? u32(y1 - y0) : 0;
        }
    return true;
}

// The level's copy of an atlas: made from the base atlas, calibrated once, the cells of its models the stock path draws
// (stand: the populations whose stock models stand) drawn again with the latched cards, bound as the texture's surface.
void lods_compose(CBackend& cmd, LodsAtlas& A, u8 stand)
{
    const u64 t0 = CPU::QPC();
    CTexture* tex = &*A.tex;
    if (!A.composed.tex && !lods_base(cmd, A))
    {
        A.failed = true;
        Msg("! [lods] %s: the base atlas could not be read, its stock billboards keep it", tex->cName.c_str());
        return;
    }
    xr_vector<std::pair<u32, u32>> todo;
    A.models = 0;
    for (u32 si = 0; si < A.sets.size(); ++si)
    {
        const LodsSet& S = A.sets[si];
        if (!S.stock && !(S.pops & stand))
            continue;
        ++A.models;
        for (u32 f = 0; f < 8; ++f)
            if (S.cell[f][2] && S.cell[f][3])
                todo.emplace_back(si, f);
    }
    std::stable_sort(todo.begin(), todo.end(),
        [&A](const auto& a, const auto& b) { return slot_size(A.sets[a.first].cell[a.second]) > slot_size(A.sets[b.first].cell[b.second]); });
    if (!A.calibrated && !todo.empty())
        lods_calibrate(cmd, A, todo);
    upload(cmd, g_sh.lut_b, 0, A.lut.data(), u32(A.lut.size() * 4));
    xr_map<shared_str, ID3D11ShaderResourceView*> cards;
    xr_vector<ID3D11ShaderResourceView*> owned;
    lods_cards(A, false, cards, owned);
    u32 at = 0;
    xr_vector<LodsSlot> slots;
    u32 used_h = 0, max_size = 0;
    A.cells = 0;
    while (lods_pack(A, todo, at, slots, used_h, max_size))
    {
        clear_stage(cmd);
        for (const LodsSlot& s : slots)
            lods_view(cmd, A, s, cards, 2.f);
        const u32 levels = level_count(max_size);
        chain(cmd, kStage, used_h, levels);
        xr_vector<u32> cells;
        for (const LodsSlot& s : slots)
        {
            const u32* c = A.sets[s.set].cell[s.view];
            cells.insert(cells.end(), {s.x, s.y, c[2], c[3]});
        }
        cover(cmd, cells, levels, true);
        // Each level a cell still covers a texel of, into the copy (mode 1).
        xr_vector<QuadRec> q;
        xr_vector<u32> first(levels + 1, 0);
        for (u32 m = 0; m < levels; ++m)
        {
            first[m] = u32(q.size());
            const float wm = float(std::max(A.composed.w >> m, 1u)), hm = float(std::max(A.composed.h >> m, 1u));
            for (u32 i = 0; i < slots.size(); ++i)
            {
                const LodsSlot& s = slots[i];
                const u32* c = A.sets[s.set].cell[s.view];
                if (m >= A.composed.levels || !(c[2] >> m) || !(c[3] >> m))
                    continue;
                const u32 X0 = c[0] >> m, Y0 = c[1] >> m, X1 = (c[0] + c[2] + (1u << m) - 1) >> m, Y1 = (c[1] + c[3] + (1u << m) - 1) >> m;
                const auto su = [m](u32 X, u32 org, u32 slot) { return (float(X << m) - float(org) + float(slot)) / float(kStage); };
                q.push_back(quad(float(X0), float(Y0), float(X1), float(Y1), wm, hm, su(X0, c[0], s.x), su(Y0, c[1], s.y), su(X1, c[0], s.x),
                    su(Y1, c[1], s.y), i));
            }
        }
        first[levels] = u32(q.size());
        upload(cmd, g_sh.quads_b, 0, q.data(), u32(q.size() * sizeof(QuadRec)));
        for (u32 m = 0; m < levels && m < A.composed.levels; ++m)
        {
            if (first[m + 1] == first[m])
                continue;
            const u32 wm = std::max(A.composed.w >> m, 1u), hm = std::max(A.composed.h >> m, 1u);
            RImplementation.Target->u_setrt(cmd, wm, hm, A.composed.rtv[m], nullptr, nullptr, nullptr);
            cmd.SetViewport({0.f, 0.f, float(wm), float(hm), 0.f, 1.f});
            cmd.set_Element(g_sh.s_lods->E[0]);
            cmd.set_c("da_tree_bake_m", float(m), 1.f, float(levels), 0.f);
            cmd.SRVSManager.SetPSResource(1, g_sh.fill_d.all);
            cmd.SRVSManager.SetPSResource(2, g_sh.k_b.srv);
            draw_quads_own(cmd, first[m], first[m + 1] - first[m]);
            unbind(cmd);
        }
        A.cells += u32(slots.size());
    }
    for (ID3D11ShaderResourceView* v : owned)
        v->Release();
    // The copy is the texture now: the stock billboards (details\lod, render_lods) draw with it.
    tex->surface_set(A.composed.tex);
    _RELEASE(A.bound);
    A.bound = tex->get_SRView();
    if (A.bound)
        A.bound->AddRef();
    A.latch = texture_season();
    A.drawn = A.pops & stand;
    ++A.composes;
    A.last_ms = float(CPU::QPC() - t0) * 1000.f / float(CPU::qpc_freq);
    Msg("* [lods] %s composed: %u vegetation cells of %u models drawn again with the latched cards (%s), %ux%u in %u levels, %.0f ms",
        tex->cName.c_str(), A.cells, A.models, A.latch && A.latch->name ? A.latch->name : "the base set", A.composed.w, A.composed.h,
        A.composed.levels, A.last_ms);
}

// The populations whose stock models stand (r__shrubs / r__trees 0, off, failed): their models are the stock path's.
u8 lods_standing()
{
    u8 m = 0;
    for (u32 p = 0; p < kPops; ++p)
        if (const Population* P = population(p); !P || !P->detached)
            m |= u8(1u << p);
    return m;
}

// An atlas whose copy is due: it has cells the stock path draws, and no copy holds them for the texture latch as the
// texture's surface (none yet, a reload, a season sync, a population's stock models back, r__trees_bb_rebake).
bool lods_due(const LodsAtlas& A, u8 stand)
{
    if (A.failed || (!A.stock && !(A.pops & stand)))
        return false;
    return g_rebake_lods || !A.composed.tex || engine_srv(&*A.tex) != A.bound || texture_season() != A.latch || (A.pops & stand & ~A.drawn);
}

// This frame's work on the stock path's atlases.
bool lods_frame(Work& work)
{
    if (!g_sh.lods || !g_pGameLevel)
        return false;
    if (!g_lb.init)
        lods_init();
    const u8 stand = lods_standing();
    bool any = false;
    for (LodsAtlas& A : g_lb.atlases)
    {
        if (!lods_due(A, stand))
            continue;
        if (g_rebake_lods)
        {
            // (The calibration again needs Dead Air's cells: the copy is made again from the base atlas.)
            A.composed.release();
            A.calibrated = false;
        }
        work.begin();
        // (The staging is shared: a template the trees' bake had in the works starts its views again next frame.)
        g_tb.batch.clear();
        g_tb.views_done = 0;
        lods_compose(work.cmd, A, stand);
        any = true;
    }
    g_rebake_lods = false;
    return any;
}

void lods_release()
{
    for (LodsAtlas& A : g_lb.atlases)
    {
        // (The texture leaves with the level; should it stay, it loads its file again rather than keep a copy of ours.)
        if (A.tex && A.bound && A.tex->get_SRView() == A.bound)
            A.tex->Unload();
        _RELEASE(A.bound);
        A.composed.release();
    }
    g_lb.atlases.clear();
    g_lb.init = false;
    g_lb.candidates = 0;
}

// ======================================================================================================
// QA: the dump
// ======================================================================================================
void level_name(string_path& out)
{
    out[0] = 0;
    FS_Path* path = nullptr;
    if (!FS.get_path("$level$", &path) || !path->m_Add)
        return;
    xr_strcpy(out, path->m_Add);
    size_t n = xr_strlen(out);
    while (n && (out[n - 1] == '\\' || out[n - 1] == '/'))
        out[--n] = 0;
    if (pcstr last = strrchr(out, '\\'))
        memmove(out, last + 1, xr_strlen(last + 1) + 1);
}

void dump_texture(CBackend& cmd, ID3D11Resource* res, pcstr name)
{
    if (!res)
        return;
    DirectX::ScratchImage image;
    if (FAILED(DirectX::CaptureTexture(HW.pDevice, HW.get_context(cmd.context_id), res, image)))
    {
        Msg("! [trees] dump %s: the capture failed", name);
        return;
    }
    string_path dds, png;
    string256 file;
    xr_sprintf(file, "%s.dds", name);
    FS.update_path(dds, "$screenshots$", file);
    xr_sprintf(file, "%s.png", name);
    FS.update_path(png, "$screenshots$", file);
    VerifyPath(dds);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dds, -1, w, MAX_PATH);
    const bool ok_dds = SUCCEEDED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, w));
    MultiByteToWideChar(CP_ACP, 0, png, -1, w, MAX_PATH);
    // (The PNG of the top level as RGBA: WIC writes no two-channel image - the foliage atlas's dump failed.)
    DirectX::ScratchImage rgba;
    const DirectX::Image* top = image.GetImage(0, 0, 0);
    if (image.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        SUCCEEDED(DirectX::Convert(*top, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, rgba)))
        top = rgba.GetImage(0, 0, 0);
    const bool ok_png = SUCCEEDED(DirectX::SaveToWICFile(*top, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), w));
    Msg("* [trees] dump: %s, %zu levels -> %s%s", name, image.GetMetadata().mipLevels, dds, ok_dds && ok_png ? "" : " (FAILED)");
}

std::atomic<bool> g_dump{}, g_rebake{};

void dump(CBackend& cmd)
{
    string_path lvl;
    level_name(lvl);
    string256 name;
    if (g_tb.attached)
    {
        xr_sprintf(name, "trees_bb_%s", lvl);
        dump_texture(cmd, g_tb.d.tex, name);
        xr_sprintf(name, "trees_bb_nm_%s", lvl);
        dump_texture(cmd, g_tb.nm.tex, name);
        xr_sprintf(name, "trees_bb_mtl_%s", lvl);
        dump_texture(cmd, g_tb.mtl.tex, name);
    }
    else
        Msg("* [trees] dump: no trees' billboard atlas on this level");
    for (u32 i = 0; i < g_lb.atlases.size(); ++i)
        if (g_lb.atlases[i].composed.tex)
        {
            if (i)
                xr_sprintf(name, "level_lods_%s_%u", lvl, i);
            else
                xr_sprintf(name, "level_lods_%s", lvl);
            dump_texture(cmd, g_lb.atlases[i].composed.tex, name);
        }
    if (g_lb.atlases.empty())
        Msg("* [trees] dump: no stock-path vegetation cells on this level (level_lods as it ships)");
}
} // namespace

// ======================================================================================================
// Interface
// ======================================================================================================
bool da_trees_bake_available() { return shaders() && g_sh.trees && gpu(); }

bool da_trees_bake_owns_atlas() { return g_sh.trees && g_sh.gpu; }

void da_trees_bake_level_load()
{
    tree_release();
    lods_release();
    g_tb.baked = g_tb.views = g_tb.rebakes = g_tb.full_bakes = 0;
    g_tb.full_ms = g_tb.gpu_ms = 0.f;
    g_tb.look = 0;
    g_tb.look_set = false;
    g_tb.reason = "the level's first frame";
    if (shaders() && g_pGameLevel)
        lods_init();
}

void da_trees_bake_level_unload()
{
    tree_release();
    lods_release();
    release_gpu();
    release_shaders();
}

void da_trees_bake_destroy() { da_trees_bake_level_unload(); }

void da_trees_bake_frame(CBackend& cmd)
{
    if (!g_pGameLevel || !g_sh.common)
        return;
    if (g_rebake.exchange(false))
    {
        g_tb.full = true;
        g_tb.reason = "r__trees_bb_rebake";
        for (TmplState& s : g_tb.st)
            s.baked = false;
        g_rebake_lods = true;
    }
    // (The GPU's last bake frame, a few frames old: the cost of a view for the progressive budget - not a frame a full
    // bake or a level_lods composition went into.)
    if (const float ms = GpuTimers.ms(dx11GpuTimers::TreesBake); ms > 0.f)
    {
        g_tb.gpu_ms = ms;
        u32 sum = 0, n = 0;
        for (const u32 v : g_tb.recent)
            if (v)
            {
                sum += v;
                ++n;
            }
        if (n && Device.dwFrame > g_tb.calm)
            g_tb.view_cost = std::clamp(0.7f * g_tb.view_cost + 0.3f * ms / (float(sum) / float(n)), 0.002f, 2.f);
    }
    const Population* P = population(kPopTrees);
    const bool trees = g_sh.trees && P && P->active && P->detached;
    const u8 stand = lods_standing();
    const bool lods_work = g_sh.lods &&
        (!g_lb.init || std::any_of(g_lb.atlases.begin(), g_lb.atlases.end(), [stand](const LodsAtlas& A) { return lods_due(A, stand); }));
    if (!lods_work)
        g_rebake_lods = false;
    if (!trees && !g_tb.attached && !lods_work && !g_dump)
        return;
    if (!gpu())
        return;
    u32 views = 0;
    bool lods = false;
    const bool full = g_tb.full;
    {
        Work work(cmd);
        views = tree_frame(work);
        lods = lods_frame(work);
        if (g_dump.exchange(false))
        {
            work.begin();
            dump(cmd);
        }
    }
    if ((full && views) || lods)
        g_tb.calm = Device.dwFrame + 6;
    g_tb.recent[g_tb.recent_at++ & 3] = full || lods ? 0 : views;
}

void da_trees_bake_stats()
{
    if (!g_sh.common)
    {
        Msg("* [trees] billboard bake: off (its shaders are missing or failed)");
        return;
    }
    if (g_tb.attached)
        Msg("* [trees] billboard bake: %u cell sets baked (%u views; %u sets, %u of them dead looks), %u full bakes (the last %.0f ms on "
            "the CPU), %u progressive steps; the last for %s; %.3f ms a view on the GPU, %.1f ms a frame; the GPU's last bake frame %.2f "
            "ms; the look: snow %.2f, the sun at %.0f deg with %.2f of the light; %s",
            g_tb.baked, g_tb.views, g_tb.info.sets, g_tb.info.sets - g_tb.info.templates, g_tb.full_bakes, g_tb.full_ms, g_tb.rebakes,
            g_tb.reason, g_tb.view_cost, kBudgetMs, g_tb.gpu_ms, g_tb.snow, g_tb.sun_e, g_tb.sun_share,
            g_tb.mtl.tex ? "the foliage atlas on" : "no foliage atlas");
    else
        Msg("* [trees] billboard bake: no trees' atlas on this level%s", g_sh.trees ? "" : " (its tree shaders failed)");
    for (const LodsAtlas& A : g_lb.atlases)
        Msg("* [lods] %s: %u cells of %u models drawn again (%u stock-path placements, %u the populations replace: %s), composed %u "
            "times (the last %.0f ms), %ux%u; gains %.2f %.2f %.2f, an alpha map of %u points%s",
            A.tex->cName.c_str(), A.cells, A.models, A.placements, A.replaced, A.drawn ? "their stock models stand" : "not drawn",
            A.composes, A.last_ms, A.composed.w, A.composed.h, A.gain[0], A.gain[1], A.gain[2], A.lut_n,
            A.failed ? " (FAILED: the base atlas stays)" : "");
    if (g_lb.init && g_lb.atlases.empty())
        Msg("* [lods] no stock-path vegetation on this level: level_lods as it ships (%u stock-path FLODs of tree parts)", g_lb.candidates);
}

void da_trees_bake_dump()
{
    g_dump = true;
    Msg("* [trees] dump: next frame, into $screenshots$");
}

void da_trees_bake_rebake()
{
    g_rebake = true;
    Msg("* [trees] every billboard bakes again next frame");
}
} // namespace xray::render::RENDER_NAMESPACE
