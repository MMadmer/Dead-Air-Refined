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
#include "Layers/xrRenderPC_R4/da_particle_light.h"
#include "Layers/xrRender/light.h"
#include "xrCDB/ISpatial.h"
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
    ID3DTexture3D* tex[3]{}; // the air this frame (density, sun visibility), its copy for the next one, the glow
    ID3D11UnorderedAccessView* uav[3]{};
    ref_texture named[3];
    ref_shader shader;
    u32 frame = 0; // the frame the injection last ran
    Fmatrix prev_viewproj{}; // the previous injection's view-projection, unjittered
    Fmatrix shadow[3]{}; // view -> cascade texture space, this frame
    u32 shadow_frame[3]{};
    Fvector drift{}; // the medium's drift with the wind, wrapped at the noise's period
    u32 phase = 0;
};

namespace
{
enum : u32
{
    kCur,
    kHist,
    kInt,
};

DaFroxelFrame g_frame;

// The player's own torch, the volume's one local light (DOOM Eternal injects its lights into the froxels). From the
// eye its beam needs no shadow: what an object hides of it lies behind that object from the camera too (in the third
// person its shadows in the air are missing).
struct DaTorch
{
    u32 frame = 0; // the frame it was taken on
    Fvector pos{}, dir{}, colour{};
    float range = 0.f;
    float cos_half = 1.f; // of the cone
};
DaTorch g_torch;
// The beam's in-scatter per unit of the torch's light, by the weather: the air of the volume holds the day's haze,
// which would show a torch's beam even in clear air; a beam stands out in a fog and a rain, and never hides what it
// lights. Fog reaching 1000 m leaves 1.5 % of it, 100 m 15 %, a full rain 12 % more (look constants over
// r__froxel_torch); under 3 % (clear air) the beam is not drawn at all.
constexpr float kTorchFog = 15.f, kTorchRain = 0.12f, kTorchMin = 0.03f, kTorchMax = 0.3f;

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
    const pcstr names[3] = {da_froxel_cur_rt, da_froxel_hist_rt, da_froxel_int_rt};
    for (u32 i = 0; i < 3; ++i)
    {
        D3D_TEXTURE3D_DESC desc{};
        desc.Width = f.w;
        desc.Height = f.h;
        desc.Depth = f.d;
        desc.MipLevels = 1;
        desc.Format = i == kInt ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
        desc.Usage = D3D_USAGE_DEFAULT;
        desc.BindFlags = D3D_BIND_SHADER_RESOURCE | (i == kHist ? 0u : u32(D3D11_BIND_UNORDERED_ACCESS));
        if (FAILED(HW.pDevice->CreateTexture3D(&desc, nullptr, &f.tex[i])) ||
            (i != kHist && FAILED(HW.pDevice->CreateUnorderedAccessView(f.tex[i], nullptr, &f.uav[i]))))
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

const DaFroxelFrame& da_froxel_frame() { return g_frame; }

void da_froxel_capture_torch()
{
    g_torch.frame = 0;
    if (ps_r__froxel_torch <= 0.f || !g_pGamePersistent)
        return;
    // The lights whose reach takes in the camera's surroundings (the third-person camera stands metres behind the
    // actor); the actor's torch is the one never demoted from its shadow (CTorch::UpdateCL), its widest-reaching spot.
    thread_local xr_vector<ISpatial*> around;
    g_pGamePersistent->SpatialSpace.q_sphere(around, 0, STYPE_LIGHTSOURCE, Device.vCameraPosition, 10.f);
    const light* best = nullptr;
    for (ISpatial* s : around)
    {
        const light* L = (const light*)s->dcast_Light();
        if (L && L->flags.bActive && L->flags.bNeverDemote && L->flags.type == IRender_Light::SPOT &&
            (!best || L->range > best->range))
            best = L;
    }
    if (!best || best->range <= EPS_L)
        return;
    g_torch.frame = Device.dwFrame;
    g_torch.pos = best->position;
    g_torch.dir = best->direction;
    g_torch.colour.set(best->color.r, best->color.g, best->color.b);
    g_torch.range = best->range;
    g_torch.cos_half = _cos(best->cone * 0.5f);
}

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

bool CRenderTarget::froxels_ran() const { return g_frame.frame == Device.dwFrame; }

bool CRenderTarget::phase_froxels(CBackend& cmd_list)
{
    if (!m_froxels || !m_froxels->shader || !RImplementation.o.advancedpp || !ps_r_sun_shafts ||
        RImplementation.o.oldshadowcascades)
        return false;
    DaFroxels& f = *m_froxels;

    // The cascades the sun accumulated this frame, from the near one up without a gap: none in a frame without the sun's
    // pass (a moonless night), where only the torch lights the air.
    u32 cascades = 0;
    while (cascades < 3 && f.shadow_frame[cascades] == Device.dwFrame)
        ++cascades;

    // The glow wants the weather's shaft strength and the sun's cascades; the lit particles, while some are about, want
    // the sun's visibility while there is a sun.
    const auto& env = g_pGamePersistent->Environment();
    const float strength = (env.CurrentEnv.m_fSunShaftsIntensity + ps_r2_sun_shafts_value) * env.cloud_sun_visibility *
        kLevel * ps_r__froxel_strength;
    const bool glow = cascades && strength > 0.0001f;
    light* sun = (light*)RImplementation.Lights.sun._get();
    Fvector colour{sun->color.r, sun->color.g, sun->color.b};
    const float length = colour.magnitude();
    // The player's torch, taken before the cascades were recorded (da_froxel_capture_torch), at the weather's strength.
    const float torch_k = ps_r__froxel_torch *
        std::min(kTorchFog / std::max(env.CurrentEnv.fog_distance, 1.f) + kTorchRain * env.CurrentEnv.rain_density, kTorchMax);
    const bool torch = g_torch.frame == Device.dwFrame && torch_k >= kTorchMin;
    const bool scatter = glow || torch;
    if (!scatter && !(da_particle_light_wanted() && length > 0.01f))
        return false;

    const u32 tier = std::min<u32>(ps_r_sun_shafts, std::size(kTiers)) - 1;
    if ((!f.tex[0] || f.tier != tier || f.w != (Device.dwRenderWidth + kTiers[tier].tile - 1) / kTiers[tier].tile ||
            f.h != (Device.dwRenderHeight + kTiers[tier].tile - 1) / kTiers[tier].tile) &&
        !allocate(f, tier))
        return false;

    if (!cascades && !torch)
        return false;
    PIX_EVENT_CTX(cmd_list, phase_froxels);

    // The sun's hue at the stock shafts' brightness (they lit the air with a unit-length colour).
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

    g_frame.frame = Device.dwFrame;
    g_frame.grid.set(float(f.w), float(f.h), float(f.d), 0.f);
    g_frame.range.set(kNear, kFar - kNear, 1.f / (kFar - kNear), kFar);
    g_frame.ray.set(1.f / Device.mProject._11, 1.f / Device.mProject._22, 0.f, 0.f);
    const auto common = [&]
    {
        cmd_list.set_c("da_froxel_grid", g_frame.grid);
        cmd_list.set_c("da_froxel_range", g_frame.range);
        cmd_list.set_c("da_froxel_ray", g_frame.ray);
    };

    // Nothing of the scene's may stay bound for output while the volumes are written.
    for (u32 i = 0; i < 4; ++i)
        cmd_list.set_RT(nullptr, i);
    cmd_list.set_ZB(nullptr);
    if (cascades)
        rt_smap_depth->set_slice_read(-1);

    dispatch(cmd_list, &*f.shader->E[0], f.uav[kCur], (f.w + 7) / 8, (f.h + 7) / 8, f.d, [&]
    {
        common();
        cmd_list.set_c("m_froxel_v2w", Device.mInvView);
        cmd_list.set_c("m_froxel_reproj", reproject);
        cmd_list.set_c("m_froxel_shadow0", f.shadow[0]);
        cmd_list.set_c("m_froxel_shadow1", f.shadow[1]);
        cmd_list.set_c("m_froxel_shadow2", f.shadow[2]);
        // Without a glow only the sun's visibility is read: the wisps' noise is skipped.
        cmd_list.set_c("da_froxel_media", kExtinction, kHeightFalloff, scatter ? kNoiseShare : 0.f, 0.f);
        cmd_list.set_c("da_froxel_wind", f.drift.x, 0.f, f.drift.z, kNoiseFrequency);
        cmd_list.set_c("da_froxel_frame", jitter, float(cascades), history ? kHistory : 0.f, 0.f);
    });
    if (cascades)
        rt_smap_depth->set_slice_read(SE_SUN_FAR);
    // The next frame's history: the injection cannot read the volume it writes.
    HW.get_context(cmd_list.context_id)->CopyResource(f.tex[kHist], f.tex[kCur]);
    f.frame = Device.dwFrame;
    f.prev_viewproj = Device.mFullTransform;
    if (!scatter)
        return true;

    dispatch(cmd_list, &*f.shader->E[1], f.uav[kInt], (f.w + 7) / 8, (f.h + 7) / 8, 1, [&]
    {
        common();
        const float sun_k = glow ? strength : 0.f;
        cmd_list.set_c("da_froxel_sun", colour.x * sun_k, colour.y * sun_k, colour.z * sun_k, 0.f);
        // The torch in view space: position and 1 / range^2 (0: none), direction and the cone's cosine, its light at
        // the beam's strength and the cosine where the cone's edge has faded in.
        Fvector pos{}, dir{};
        Device.mView.transform_tiny(pos, g_torch.pos);
        Device.mView.transform_dir(dir, g_torch.dir);
        dir.normalize_safe();
        const float k = torch_k;
        cmd_list.set_c("da_froxel_torch_pos", pos.x, pos.y, pos.z, torch ? 1.f / (g_torch.range * g_torch.range) : 0.f);
        cmd_list.set_c("da_froxel_torch_dir", dir.x, dir.y, dir.z, g_torch.cos_half);
        cmd_list.set_c("da_froxel_torch_col", g_torch.colour.x * k, g_torch.colour.y * k, g_torch.colour.z * k,
            g_torch.cos_half + (1.f - g_torch.cos_half) * 0.6f);
    });

    // Every pixel's share, into the volumetric light target.
    phase_vol_accumulator(cmd_list);
    cmd_list.SetViewport({0.f, 0.f, float(Device.dwRenderWidth), float(Device.dwRenderHeight), 0.f, 1.f});
    cmd_list.set_Element(f.shader->E[2]);
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
