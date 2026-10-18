// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The local lamps as instanced volumes (da_light_volumes.h). The records follow the stock accumulation's constants
// (accum_spot, accum_point): the view-space position and falloff, the colour with the lamp's LOD, the shadow map and
// projector transforms of its atlas rect, and the volume's own transform (light::xform_calc).

#include "stdafx.h"

#include "da_light_volumes.h"
#include "da_vrs.h"
#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRender/du_cone.h"
#include "Layers/xrRender/du_sphere.h"
#include "Layers/xrRender/du_sphere_part.h"
#include "Layers/xrRender/light.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
constexpr u32 kMaxLights = 2048; // records per frame, both light packages
constexpr u32 kProjectorSlots = 14; // t18..t31 (da_light_volume.ps)
constexpr u32 kFirstSlot = 16; // t16: the records

constexpr u32 kPoint = 0, kSpot = 1, kFace = 2, kTypes = 3;
constexpr u32 kShadow = 1, kLmapXform = 2, kLmapShadow = 4, kEnabled = 8;

// r3\da_light_volumes.h, float4 by float4.
struct DaLvLight
{
    Fvector4 pos;
    Fvector4 color;
    Fvector4 dir;
    u32 info[4]; // type | face << 4, flags, -, projector slot
    Fvector4 shadow[4];
    Fvector4 lmap[2];
    Fvector4 xform[3];
    Fvector4 pad[3];
};
static_assert(sizeof(DaLvLight) == 256, "r3\\da_light_volumes.h: DA_LV_STRIDE");
static_assert(offsetof(DaLvLight, xform) == 160, "r3\\da_light_volumes.h: DA_LV_XFORM_OFFSET");

// The lighting: what accum_base.ps reads, added to the accumulator on the G-buffer's pixels (stencil bit 0) the volume
// covers. A volume the eye sees from outside draws its front faces with the depth test as the scene's, which the
// coarse depth culling keeps (element 1): what stands before it fails. One the near plane cuts draws its back faces with
// the test inverted (element 0): what lies behind it fails, the sky too. The rest is the shader's reach test.
constexpr u32 kInside = 0, kOutside = 1;

class CBlender_da_lv : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: light volumes"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > kOutside)
            return;
        const bool inside = C.iElement == kInside;
        C.r_Pass("da_light_volume", "da_light_volume", FALSE, TRUE, FALSE, TRUE, D3DBLEND_ONE, D3DBLEND_ONE);
        C.PassSET_ZB(TRUE, FALSE, inside);
        C.r_CullMode(inside ? D3DCULL_CW : D3DCULL_CCW);
        C.r_Stencil(TRUE, D3DCMP_EQUAL, 0x01, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP);
        C.r_StencilRef(0x01);
        C.r_dx11Texture("s_position", r2_RT_P);
        C.r_dx11Texture("s_depth_copy", r2_RT_depth_copy);
        C.r_dx11Texture("s_normal", r2_RT_N);
        C.r_dx11Texture("s_diffuse", r2_RT_albedo);
        C.r_dx11Texture("s_material", r2_material);
        C.r_dx11Texture("s_smap", r2_RT_smap_depth);
        C.r_dx11Sampler("smp_nofilter");
        C.r_dx11Sampler("smp_material");
        C.r_dx11Sampler("smp_rtlinear");
        C.r_dx11Sampler("smp_smap");
        C.r_End();
    }
};

struct State
{
    ref_shader shader;
    ref_texture default_projector;
    ID3D11Buffer* buf{};
    ID3D11ShaderResourceView* srv{};
    bool failed = false;
    bool warned_lights = false, warned_projectors = false;

    u32 frame = u32(-1);
    u32 used = 0; // the frame's records so far
    xr_vector<DaLvLight> records;
    CTexture* projectors[kProjectorSlots]{};
    u32 projector_count = 0;
} g;

void release_all()
{
    _RELEASE(g.srv);
    _RELEASE(g.buf);
    g.shader.destroy();
    g.default_projector.destroy();
    g.failed = false;
}

bool ensure_resources()
{
    if (g.failed)
        return false;
    if (g.shader)
        return true;
    static CBlender_da_lv blender;
    g.shader.create(&blender, "r2" DELIMITER "da_light_volume");
    // The stock spot's projector where a lamp names none (s_accum_spot).
    g.default_projector.create("lights" DELIMITER "lights_spot01");

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = kMaxLights * sizeof(DaLvLight);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    view.BufferEx.NumElements = desc.ByteWidth / 4;
    if (!g.shader || !g.shader->E[kInside] || !g.shader->E[kOutside] || FAILED(HW.pDevice->CreateBuffer(&desc, nullptr, &g.buf)) ||
        FAILED(HW.pDevice->CreateShaderResourceView(g.buf, &view, &g.srv)))
    {
        Msg("! [light volumes] the shader or the lamp buffer could not be made: the local lamps light nothing");
        release_all();
        g.failed = true;
        return false;
    }
    return true;
}

// Fmatrix's columns as the shader's rows (the stock m_lmap rows, the m_shadow constant).
void rows(const Fmatrix& M, Fvector4* out, u32 count)
{
    for (u32 i = 0; i < count; ++i)
        out[i].set(M.m[0][i], M.m[1][i], M.m[2][i], M.m[3][i]);
}

// accum_spot: the shadow map rect of the atlas and the projector's whole frame, both from view space.
void shadow_xforms(light* L, Fmatrix& shadow, Fmatrix& lmap)
{
    const float smapsize = float(RImplementation.o.smapsize);
    const float texel = .5f / smapsize;
    const float range = ps_r2_ls_depth_scale;
    const float bias = ps_r2_ls_depth_bias;
    const auto frame = [&](float dim, float sx, float sy, Fmatrix& out) {
        const Fmatrix adjust = {
            dim / 2.f, 0.0f, 0.0f, 0.0f,
            0.0f, -dim / 2.f, 0.0f, 0.0f,
            0.0f, 0.0f, range, 0.0f,
            dim / 2.f + sx + texel, dim / 2.f + sy + texel, bias, 1.0f
        };
        Fmatrix project;
        project.mul(adjust, L->X.S.project);
        out.mul(L->X.S.view, Device.mInvView);
        out.mulA_44(project);
    };
    frame(float(L->X.S.size - 2) / smapsize, float(L->X.S.posX + 1) / smapsize, float(L->X.S.posY + 1) / smapsize, shadow);
    frame(1.f, 0.f, 0.f, lmap);
}

u32 projector_slot(light* L)
{
    CTexture* texture = L->projector ? L->projector._get() : g.default_projector._get();
    if (!texture)
        return 0;
    for (u32 i = 0; i < g.projector_count; ++i)
    {
        if (g.projectors[i] == texture)
            return i;
    }
    if (g.projector_count == kProjectorSlots)
    {
        if (!g.warned_projectors)
        {
            g.warned_projectors = true;
            Msg("! [light volumes] more than %u projector textures in one batch: the rest take the first", kProjectorSlots);
        }
        return 0;
    }
    g.projectors[g.projector_count] = texture;
    return g.projector_count++;
}

// The near plane cuts the volume, or soon may: the radius of the sphere about the lamp that holds its mesh (du_cone
// reaches 1 / cos of the half cone along its slant, du_sphere and the cube face's cap the range), with a margin.
bool near_cut(light* L, u32 type)
{
    const float reach = type == kSpot ? L->range / std::max(_cos(L->cone * .5f), .1f) : L->range;
    Fvector p;
    Device.mView.transform_tiny(p, L->position);
    return p.z - reach <= VIEWPORT_NEAR * 4.f;
}

DaLvLight make_record(light* L, u32 type, u32 flags, bool lod)
{
    DaLvLight r{};
    Fvector p;
    Device.mView.transform_tiny(p, L->position);
    const float R = L->range * .95f;
    r.pos.set(p.x, p.y, p.z, 1.f / (R * R));

    Fvector c{ L->color.r, L->color.g, L->color.b };
    if (lod)
        c.mul(L->get_LOD());
    r.color.set(c.x, c.y, c.z, u_diffuse2s(c));

    Fvector d;
    Device.mView.transform_dir(d, L->direction);
    d.normalize_safe();
    r.dir.set(d.x, d.y, d.z, _cos(L->cone * .5f));

    u32 face = 0;
    if (type == kFace && L->omnipart_owner)
    {
        for (u32 f = 0; f < 6; ++f)
        {
            if (L->omnipart_owner->omnipart[f] == L)
                face = f;
        }
    }
    r.info[0] = type | (face << 4);
    r.info[1] = flags | kEnabled;
    r.info[3] = (flags & (kLmapXform | kLmapShadow)) ? projector_slot(L) : 0;

    if (type != kPoint)
    {
        Fmatrix shadow, lmap;
        shadow_xforms(L, shadow, lmap);
        // An unshadowed spot projects through its own frame: the stock shader then read m_shadow = m_lmap.
        rows((flags & kShadow) ? shadow : lmap, r.shadow, 4);
        rows(lmap, r.lmap, 2);
    }

    L->xform_calc();
    rows(L->m_xform, r.xform, 3);
    return r;
}
} // namespace


void da_lv_draw(CBackend& cmd_list, const xr_vector<light*>& shadowed, const xr_vector<light*>* unshadowed)
{
    if (shadowed.empty() && (!unshadowed || unshadowed->empty()))
        return;
    if (!ensure_resources())
        return;
    if (g.frame != Device.dwFrame)
    {
        g.frame = Device.dwFrame;
        g.used = 0;
    }

    // The records grouped by volume - the spheres, the cones, the cube faces - and by side of the near plane.
    static xr_vector<DaLvLight> groups[kTypes][2];
    for (auto& type : groups)
    {
        for (auto& side : type)
            side.clear();
    }
    g.projector_count = 0;
    const auto add = [](light* L, u32 type, u32 flags, bool lod) {
        groups[type][near_cut(L, type) ? kInside : kOutside].push_back(make_record(L, type, flags, lod));
    };
    const u32 smapsize = u32(RImplementation.o.smapsize);
    for (light* L : shadowed)
    {
        const bool face = L->flags.type == IRender_Light::OMNIPART;
        // The whole atlas is the projector's frame too (the stock "fullsize" shader); a point lamp's faces project
        // nothing.
        const u32 lmap = face ? 0 : (L->X.S.size == smapsize ? kLmapShadow : kLmapXform);
        add(L, face ? kFace : kSpot, kShadow | lmap, true);
    }
    if (unshadowed)
    {
        for (light* L : *unshadowed)
        {
            if (L->flags.type == IRender_Light::POINT)
                add(L, kPoint, 0, false);
            else
            {
                const bool face = L->flags.type == IRender_Light::OMNIPART;
                add(L, face ? kFace : kSpot, face ? 0 : kLmapShadow, true);
            }
        }
    }

    g.records.clear();
    u32 first[kTypes][2], count[kTypes][2];
    for (u32 t = 0; t < kTypes; ++t)
    {
        for (u32 side = 0; side < 2; ++side)
        {
            first[t][side] = g.used + u32(g.records.size());
            count[t][side] = u32(groups[t][side].size());
            g.records.insert(g.records.end(), groups[t][side].begin(), groups[t][side].end());
        }
    }
    if (g.used + g.records.size() > kMaxLights)
    {
        if (!g.warned_lights)
        {
            g.warned_lights = true;
            Msg("! [light volumes] more than %u local lamps in one frame: the rest light nothing", kMaxLights);
        }
        return;
    }
    RImplementation.Stats.l_visible += u32(g.records.size());

    // Into the frame's own region of the buffer, before anything of the draws is bound.
    ID3D11DeviceContext* ctx = HW.get_context(cmd_list.context_id);
    {
        const D3D11_BOX box{ UINT(g.used * sizeof(DaLvLight)), 0, 0,
            UINT((g.used + g.records.size()) * sizeof(DaLvLight)), 1, 1 };
        ctx->UpdateSubresource(g.buf, 0, &box, g.records.data(), 0, 0);
    }
    g.used += u32(g.records.size());

    // The world's axes in view space, for the cube faces of the point lamps.
    Fvector axes[3];
    for (u32 a = 0; a < 3; ++a)
    {
        const Fvector axis{ a == 0 ? 1.f : 0.f, a == 1 ? 1.f : 0.f, a == 2 ? 1.f : 0.f };
        Device.mView.transform_dir(axes[a], axis);
    }
    const auto bind = [&](u32 side) {
        cmd_list.set_Element(g.shader->E[side]);
        cmd_list.set_xform_world(Fidentity);
        cmd_list.set_xform_view(Device.mView);
        cmd_list.set_xform_project(da_jitter_project(Device.mProject));
        for (u32 a = 0; a < 3; ++a)
            cmd_list.set_ca("da_lv_axes", a, axes[a].x, axes[a].y, axes[a].z, 0.f);
    };
    // The volume passes before leave their lamp's rect.
    cmd_list.set_Scissor(nullptr);

    ID3D11ShaderResourceView* views[2 + kProjectorSlots]{ g.srv, nullptr };
    for (u32 i = 0; i < g.projector_count; ++i)
    {
        CTexture* texture = g.projectors[i];
        // A projector nobody bound yet is loaded on first use, as the stock pass did through its shader.
        if (ID3DBaseTexture* surface = texture->surface_get())
            surface->Release();
        views[2 + i] = texture->get_SRView();
    }
    ctx->PSSetShaderResources(kFirstSlot, 2 + g.projector_count, views);
    ctx->VSSetShaderResources(kFirstSlot, 1, &g.srv);

    {
        const CRenderTarget& T = *RImplementation.Target;
        SGeometry* geoms[kTypes] = { T.volume_point()._get(), T.volume_spot()._get(), T.volume_omnipart()._get() };
        constexpr u32 vertices[kTypes] = { DU_SPHERE_NUMVERTEX, DU_CONE_NUMVERTEX, DU_SPHERE_PART_NUMVERTEX };
        constexpr u32 faces[kTypes] = { DU_SPHERE_NUMFACES, DU_CONE_NUMFACES, DU_SPHERE_PART_NUMFACES };

        da_vrs_scope vrs(cmd_list, VrsPass::Lights);
        GpuTimers.Begin(dx11GpuTimers::LightVolumes);
        for (u32 side : { kOutside, kInside })
        {
            bool bound = false;
            for (u32 t = 0; t < kTypes; ++t)
            {
                if (!count[t][side])
                    continue;
                if (!bound)
                {
                    bind(side);
                    bound = true;
                }
                cmd_list.set_c("da_lv", float(first[t][side]), 0.f, 0.f, 0.f);
                cmd_list.set_Geometry(geoms[t]);
                cmd_list.RenderInstanced(D3DPT_TRIANGLELIST, 0, 0, vertices[t], 0, faces[t], count[t][side]);
            }
        }
        GpuTimers.End(dx11GpuTimers::LightVolumes);
    }

    ID3D11ShaderResourceView* none[2 + kProjectorSlots]{};
    ctx->PSSetShaderResources(kFirstSlot, 2 + kProjectorSlots, none);
    ctx->VSSetShaderResources(kFirstSlot, 1, none);
}

void da_lv_reset()
{
    release_all();
    g.records.clear();
    g.projector_count = 0;
    g.frame = u32(-1);
    g.used = 0;
}
} // namespace xray::render::RENDER_NAMESPACE
