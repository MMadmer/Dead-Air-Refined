// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The sun's light in the air as a froxel volume, after DOOM Eternal's light scattering (shaders/r3/da_froxel.h).
// The stock shafts marched every render pixel's ray with 20 to 80 shadow taps of the far cascade alone, full
// resolution, no temporal filter, the same strength for a wall at arm's length as for the horizon. Here a grid over the
// view frustum (the r2_sun_shafts tier sets its density) is injected once a frame after the cascades, every cell with
// the finest cascade that holds it and the cloud shadow, its air thinning with height and drifting with the wind,
// jittered in depth and filtered over frames; integrated front to back; read at every pixel's depth into the
// volumetric light target combine_volumetric adds to the scene, as before.

#include "stdafx.h"

#include "Layers/xrRender/blenders/blender_da_froxel.h"
#include "Layers/xrRender/da_temporal.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// r2_sun_shafts low / medium / high (the Default, High and Extreme presets): render pixels per froxel, slices.
struct FroxelTier
{
    u32 tile;
    u32 slices;
};
constexpr FroxelTier kTiers[] = {{16, 64}, {12, 64}, {8, 96}};

constexpr float kNear = 0.1f;
constexpr float kFar = 200.f;
// The air scatters (and dims) one e-fold of light every 40 m at the eye's height. What it scatters toward the eye
// grows with the sunlit path and levels off by some 80 m of lit air at four times the weather's shaft strength: the
// stock shafts lifted every pixel by the strength, a wall at arm's length as much as the horizon, and the same light
// gathered where the air is deep keeps the frame's brightness (measured 7.1 against 8.1 of 255 at noon).
constexpr float kExtinction = 1.f / 40.f;
constexpr float kLevel = 4.f;
// The air thins by e every 60 m above the eye; the wisps are 14 m across and move the density by up to 60 %.
constexpr float kHeightFalloff = 1.f / 60.f;
constexpr float kNoiseFrequency = 1.f / 14.f;
constexpr float kNoiseShare = 0.6f;
// The medium drifts slower than the wind at 10 m: it is the air near the ground.
constexpr float kDriftShare = 0.6f;
// The history's share (DOOM keeps 0.95; a swaying crown's shadow must not trail).
constexpr float kHistory = 0.9f;
} // namespace

struct DaFroxels
{
    u32 tier = 0, w = 0, h = 0, d = 0;
    ID3DTexture3D* tex[3]{}; // A, B (injection, ping-pong), the integral
    ID3D11UnorderedAccessView* uav[3]{};
    ref_texture named[3];
    ref_shader shader;
    u32 last = 0; // the volume the last injection wrote
    u32 frame = 0; // the frame it did
    Fmatrix prev_viewproj{}; // the previous injection's view-projection, unjittered
    Fmatrix shadow[3]{}; // view -> cascade texture space, this frame
    u32 shadow_frame[3]{};
    Fvector drift{}; // the medium's drift with the wind, wrapped at the noise's period
    u32 phase = 0;
};

namespace
{
void release(DaFroxels& f)
{
    for (u32 i = 0; i < 3; ++i)
    {
        if (f.named[i])
            f.named[i]->surface_set(nullptr);
        f.named[i] = nullptr;
        _RELEASE(f.uav[i]);
        _RELEASE(f.tex[i]);
    }
}

bool allocate(DaFroxels& f, u32 tier)
{
    release(f);
    f.tier = tier;
    f.w = (Device.dwRenderWidth + kTiers[tier].tile - 1) / kTiers[tier].tile;
    f.h = (Device.dwRenderHeight + kTiers[tier].tile - 1) / kTiers[tier].tile;
    f.d = kTiers[tier].slices;
    const pcstr names[3] = {da_froxel_volume_rt[0], da_froxel_volume_rt[1], da_froxel_int_rt};
    for (u32 i = 0; i < 3; ++i)
    {
        D3D_TEXTURE3D_DESC desc{};
        desc.Width = f.w;
        desc.Height = f.h;
        desc.Depth = f.d;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Usage = D3D_USAGE_DEFAULT;
        desc.BindFlags = D3D_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(HW.pDevice->CreateTexture3D(&desc, nullptr, &f.tex[i])) ||
            FAILED(HW.pDevice->CreateUnorderedAccessView(f.tex[i], nullptr, &f.uav[i])))
        {
            Msg("! [froxels] the %ux%ux%u volumes could not be created: no volumetric sunlight", f.w, f.h, f.d);
            release(f);
            return false;
        }
        f.named[i] = RImplementation.Resources->_CreateTexture(names[i]);
        f.named[i]->surface_set(f.tex[i]);
    }
    f.frame = 0;
    Msg("* [froxels] %ux%ux%u, %.0f m", f.w, f.h, f.d, kFar);
    return true;
}

void dispatch(CBackend& cmd_list, ShaderElement* element, ID3D11UnorderedAccessView* out, u32 gx, u32 gy, u32 gz,
    const auto& constants)
{
    SPass& P = *element->passes[0];
    cmd_list.set_States(P.state);
    cmd_list.set_CS(P.cs);
    cmd_list.set_Constants(P.constants);
    cmd_list.set_Textures(P.T);
    constants();
    auto* ctx = HW.get_context(cmd_list.context_id);
    u32 initial = 1;
    ctx->CSSetUnorderedAccessViews(0, 1, &out, &initial);
    cmd_list.Compute(gx, gy, gz);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
    cmd_list.clear_CS_resources();
}
} // namespace

void CRenderTarget::froxels_create()
{
    froxels_destroy();
    m_froxels = xr_new<DaFroxels>();
    CBlender_da_froxel blender;
    m_froxels->shader.create(&blender, "r2" DELIMITER "da_froxel");
}

void CRenderTarget::froxels_destroy()
{
    if (!m_froxels)
        return;
    release(*m_froxels);
    xr_delete(m_froxels);
}

void CRenderTarget::froxels_note_cascade(u32 cascade, const Fmatrix& m_shadow)
{
    if (m_froxels && cascade < 3)
    {
        m_froxels->shadow[cascade] = m_shadow;
        m_froxels->shadow_frame[cascade] = Device.dwFrame;
    }
}

bool CRenderTarget::phase_froxels(CBackend& cmd_list)
{
    if (!m_froxels || !m_froxels->shader || !RImplementation.o.advancedpp || !ps_r_sun_shafts ||
        RImplementation.o.oldshadowcascades || !need_to_render_sunshafts())
        return false;
    DaFroxels& f = *m_froxels;
    const u32 tier = std::min<u32>(ps_r_sun_shafts, std::size(kTiers)) - 1;
    if ((!f.tex[0] || f.tier != tier || f.w != (Device.dwRenderWidth + kTiers[tier].tile - 1) / kTiers[tier].tile ||
            f.h != (Device.dwRenderHeight + kTiers[tier].tile - 1) / kTiers[tier].tile) &&
        !allocate(f, tier))
        return false;
    PIX_EVENT_CTX(cmd_list, phase_froxels);

    // The cascades the sun accumulated this frame, from the near one up without a gap.
    u32 cascades = 0;
    while (cascades < 3 && f.shadow_frame[cascades] == Device.dwFrame)
        ++cascades;
    if (!cascades)
        return false;

    const auto& env = g_pGamePersistent->Environment();
    const float strength =
        (env.CurrentEnv.m_fSunShaftsIntensity + ps_r2_sun_shafts_value) * env.cloud_sun_visibility * kLevel * ps_r__froxel_strength;
    // The sun's hue at the stock shafts' brightness (they lit the air with a unit-length colour).
    light* sun = (light*)RImplementation.Lights.sun._get();
    Fvector colour{sun->color.r, sun->color.g, sun->color.b};
    const float length = colour.magnitude();
    colour = length > EPS_S ? colour.div(length) : Fvector{0.577f, 0.577f, 0.577f};

    // The medium drifts with the wind; the noise's lattice repeats every 256 cells, and so does the drift.
    const float dt = std::min(Device.fTimeDelta, 0.1f);
    const float period = 256.f / kNoiseFrequency;
    f.drift.x = fmodf(f.drift.x - _sin(env.eff_wind_dir) * env.WindSpeedMs() * kDriftShare * dt, period);
    f.drift.z = fmodf(f.drift.z - _cos(env.eff_wind_dir) * env.WindSpeedMs() * kDriftShare * dt, period);

    const bool history = f.frame && f.frame + 1 == Device.dwFrame && !da_history_reset();
    Fmatrix reproject;
    reproject.mul(f.prev_viewproj, Device.mInvView);
    // A 16-phase depth jitter, bit-reversed so consecutive frames sample far apart in the slice (DOOM).
    f.phase = (f.phase + 1) & 15;
    const u32 bits = ((f.phase & 1) << 3) | ((f.phase & 2) << 1) | ((f.phase & 4) >> 1) | ((f.phase & 8) >> 3);
    const float jitter = (float(bits) + 0.5f) / 16.f;

    const auto common = [&]
    {
        cmd_list.set_c("da_froxel_grid", float(f.w), float(f.h), float(f.d), 0.f);
        cmd_list.set_c("da_froxel_range", kNear, kFar - kNear, 1.f / (kFar - kNear), kFar);
        cmd_list.set_c("da_froxel_ray", 1.f / Device.mProject._11, 1.f / Device.mProject._22, 0.f, 0.f);
    };

    // Nothing of the scene's may stay bound for output while the volumes are written.
    for (u32 i = 0; i < 4; ++i)
        cmd_list.set_RT(nullptr, i);
    cmd_list.set_ZB(nullptr);
    rt_smap_depth->set_slice_read(-1);

    const u32 write = f.last ^ 1;
    dispatch(cmd_list, &*f.shader->E[write], f.uav[write], (f.w + 7) / 8, (f.h + 7) / 8, f.d, [&]
    {
        common();
        cmd_list.set_c("m_froxel_v2w", Device.mInvView);
        cmd_list.set_c("m_froxel_reproj", reproject);
        cmd_list.set_c("m_froxel_shadow0", f.shadow[0]);
        cmd_list.set_c("m_froxel_shadow1", f.shadow[1]);
        cmd_list.set_c("m_froxel_shadow2", f.shadow[2]);
        cmd_list.set_c("da_froxel_media", kExtinction, kHeightFalloff, kNoiseShare, 0.f);
        cmd_list.set_c("da_froxel_wind", f.drift.x, 0.f, f.drift.z, kNoiseFrequency);
        cmd_list.set_c("da_froxel_sun", colour.x * strength, colour.y * strength, colour.z * strength, history ? 1.f : 0.f);
        cmd_list.set_c("da_froxel_frame", jitter, float(cascades), kHistory, 0.f);
    });
    dispatch(cmd_list, &*f.shader->E[2 + write], f.uav[2], (f.w + 7) / 8, (f.h + 7) / 8, 1, common);
    rt_smap_depth->set_slice_read(SE_SUN_FAR);

    f.last = write;
    f.frame = Device.dwFrame;
    f.prev_viewproj = Device.mFullTransform;

    // Every pixel's share, into the volumetric light target.
    phase_vol_accumulator(cmd_list);
    cmd_list.SetViewport({0.f, 0.f, float(Device.dwRenderWidth), float(Device.dwRenderHeight), 0.f, 1.f});
    cmd_list.set_Element(f.shader->E[4]);
    common();
    u32 offset = 0;
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, offset);
    pv->set(-1.f, 1.f, 0.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(-1.f, -1.f, 0.f, 0.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, 1.f, 1.f, 1.f, 0u, 0.f, 0.f); pv++;
    pv->set(1.f, -1.f, 1.f, 0.f, 0u, 0.f, 0.f); pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
    cmd_list.set_Geometry(g_combine);
    cmd_list.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);
    return true;
}
} // namespace xray::render::RENDER_NAMESPACE
