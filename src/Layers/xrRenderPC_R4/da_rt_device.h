// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Inside the ray tracing lane: the device part (da_rt_device.cpp) the scene and the passes build on.
// One root signature for every RT pass (dxr.md 5, PLAN.md C8), pipelines through the layer's caches,
// and native sections on the immediate context that bind a pass's inputs, outputs and constants.
#pragma once

#include "da_rt.h"

#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
struct DaDxcDefine;

// The device can run RT passes (tier, DXC, root signature, the probe pipeline and its dispatch).
bool da_rt_device_ready();
// Why it cannot; "" when it can.
pcstr da_rt_device_reason();
// The scene holds acceleration structures the passes may trace (da_rt_scene.cpp).
bool da_rt_scene_ready();
// The pass pipelines (da_rt_passes.cpp), built once per device by the first target creation with an
// effect switched on (renderer start, or the restart an RT row asks for); a device that never traces
// compiles none.
void da_rt_passes_create();
void da_rt_passes_destroy();
// Every pipeline of the effect exists.
bool da_rt_pass_ready(RtEffect effect);
// The render targets of the effect exist (r4_rendertarget_phase_rt.cpp).
bool da_rt_targets_ready(RtEffect effect);
// Frees the scene and the level's walk (device destroy, level unload, every effect switched off).
void da_rt_scene_release();

// What r__rt_quality buys (dxr.md 10; the final test phase tunes the numbers against the budget
// table, PLAN.md 4.6 R6). Latched with the frame.
struct DaRtTier
{
    bool sun_jitter; // soft shadows from the sun disc
    bool sun_filter; // temporal + a-trous (Low: the raw hard shadow)
    bool ao_half; // occlusion traced at half resolution
    float ao_tmax; // occlusion ray length, metres
    float ao_frames; // temporal cap
    float refl_tmax; // reflection reach, metres (also capped by the fog's end)
    bool refl_foliage; // reflection rays see (alpha-tested) crowns
    bool refl_sun_rays; // off-screen reflection hits trace their sun shadow
    float rigid_radius; // moving rigid objects in the scene, metres around the camera
    float skinned_radius; // characters in the scene
    u32 skinned_max; // nearest characters pre-skinned per frame
    u32 blas_per_frame; // new moving-object structures built per frame
    u32 refl_steps; // screen-space steps of a reflection ray before its traced hit, at most
};
const DaRtTier& da_rt_tier(); // this frame's (latched)
const DaRtTier& da_rt_tier_of(u32 quality);

#if defined(XR_D3D12)
// Root parameters of the RT root signature. Registers: b0 constants, t0 space2 the scene, t0-t15
// inputs, u0-u7 outputs, t0 space100 / space101 the bindless textures / raw buffers, t0-t3 space3
// the geometry and instance tables (static, per frame), u0 space3 a raw output buffer; static
// samplers s0 linear wrap (anisotropic 4), s1 point clamp, s2 linear clamp, s3 comparison
// (rt/rt_common.hlsli).
enum DaRtRoot : u32
{
    DaRtRootConstants,
    DaRtRootScene,
    DaRtRootInputs,
    DaRtRootOutputs,
    DaRtRootTextures,
    DaRtRootBuffers,
    DaRtRootTable0,
    DaRtRootTable1,
    DaRtRootTable2,
    DaRtRootTable3,
    DaRtRootRawOutput,
    DaRtRootCount
};
constexpr u32 da_rt_max_inputs = 16;
constexpr u32 da_rt_max_outputs = 8;
constexpr u32 da_rt_max_tables = 4;
constexpr u32 da_rt_max_constants = 4096;

ID3D12RootSignature* da_rt_root_signature();
// Compute pipeline of the RT pass shader `file` (under the shader root, e.g. "rt/rt_ao.hlsl") at
// cs_6_5, compiled through da_dxc_compile_native. Created with the device or its targets, never on a
// frame; null on failure (logged once).
ID3D12PipelineState* da_rt_pipeline(pcstr file, pcstr entry, const DaDxcDefine* defines = nullptr, u32 define_count = 0);

struct DaRtDispatch
{
    ID3D12PipelineState* pipeline{};
    const void* constants{}; // b0, up to da_rt_max_constants bytes
    u32 constants_size{};
    ID3D11ShaderResourceView* scene{}; // acceleration structure view; null = the empty scene
    ID3D11ShaderResourceView* const* inputs{}; // t0.. (NON_PIXEL_SHADER_RESOURCE)
    u32 input_count{};
    ID3D11UnorderedAccessView* const* outputs{}; // u0.. (UNORDERED_ACCESS)
    u32 output_count{};
    // t0-t3 space3 as root SRVs (structured or raw buffers); 0 = a stand-in the pass does not read.
    D3D12_GPU_VIRTUAL_ADDRESS tables[da_rt_max_tables]{};
    D3D12_GPU_VIRTUAL_ADDRESS raw_output{}; // u0 space3 (RWByteAddressBuffer); 0 = stand-in
    u32 groups_x{1}, groups_y{1}, groups_z{1};
};

// The RT targets behind CRenderTarget::m_da_rt (r4_rendertarget_phase_rt.cpp), created with the render
// targets for the effects switched on (the rows restart the renderer, dxr.md 4.1).
struct DaRtTargets
{
    // Sun: the trace (visibility, hit distance), the temporal pair, the mask the sun passes read.
    ref_rt sun_raw, sun_hist[2], sun;
    // Occlusion at the trace resolution: the trace (visibility, view z) and the temporal pair.
    ref_rt ao_raw, ao_hist[2];
    bool ao_half{};
    u32 ao_width{}, ao_height{};
    // Reflections: the requests and the traced result.
    ref_rt refl_req, refl;
    ref_shader s_puddle_req, s_water_req, s_water_req_b;
    // r__rt_debug, made on first use.
    ref_rt debug;
    ref_shader s_debug;
    // Engine textures the passes read.
    ref_texture t_env0, t_env1, t_sky0, t_sky1, t_tonemap;
    // Which history of each pair this frame writes, and the frame that wrote the other one.
    u32 sun_current{}, ao_current{};
    u32 sun_frame{u32(-2)}, ao_frame{u32(-2)};
    u32 width{}, height{};
};
DaRtTargets* da_rt_targets();
// The combine's private resources the RT passes use (a friend of CRenderTarget).
struct DaRtAccess
{
    static ref_geom& quad(CRenderTarget& t) { return t.g_combine; }
    static ref_shader& gtao(CRenderTarget& t) { return t.s_gtao; }
    static ref_rt& gtao_target(CRenderTarget& t) { return t.rt_gtao; }
    static ref_rt& ao_target(CRenderTarget& t) { return t.rt_ssao_temp; }
    static bool puddle_pass(CRenderTarget& t) { return !!t.s_puddle_refl; }
};
// The mirror requests of this frame (water, puddles) into refl_req; false when nothing asked.
bool da_rt_reflection_requests(CBackend& cmd_list, CRenderTarget& target, DaRtTargets& rt);
// r__rt_debug's view over the frame.
void da_rt_debug_blit(CBackend& cmd_list, CRenderTarget& target, DaRtTargets& rt);

// What the passes of this frame trace against (valid while da_rt_scene_ready()): the acceleration
// structure and the tables of rt/rt_geometry.hlsli.
struct DaRtSceneBinding
{
    ID3D11ShaderResourceView* tlas{};
    D3D12_GPU_VIRTUAL_ADDRESS tables[da_rt_max_tables]{}; // geometry, instances, per-frame geometry, per-frame instances
    u32 static_instances{};
    u32 static_texture_base{};
    // The vegetation tables (da_veg_rt_tables): bindless slot (~0u none), a species' profile rows, the season block's
    // first float4, a species' season rows.
    u32 veg_tables{~0u}, veg_prof{}, veg_seas{}, veg_rows{};
};
const DaRtSceneBinding& da_rt_scene_binding();
class DaRtSection;
// Engine vertex and index buffers the layer tracks (DirectStorage writes the level's own) into the read
// state before a native pass reads them: BLAS builds, the alpha test, hit shading.
void da_rt_require_geometry(DaRtSection& section);
// The pre-skinning pipeline (rt/rt_skin.hlsl), null when it failed.
ID3D12PipelineState* da_rt_skin_pipeline();

// A native section of RT passes on the immediate context: the RT root signature and the bindless
// tables are set once, every dispatch binds its own tables and constants. No D3D11 call on the
// immediate context while it is open (GPU timer scopes go around it).
class DaRtSection
{
public:
    DaRtSection();
    ~DaRtSection();
    DaRtSection(const DaRtSection&) = delete;
    DaRtSection& operator=(const DaRtSection&) = delete;

    explicit operator bool() const { return m_open; }
    bool dispatch(const DaRtDispatch& d);
    // A UAV written by one dispatch and read or written as a UAV by the next; null = every UAV.
    void uav_barrier(ID3D11Resource* resource = nullptr);
    // A native resource of this lane between two states (the layer does not track them).
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    // UAV barrier on a native resource (acceleration structures, scratch).
    void native_uav_barrier(ID3D12Resource* resource);
    const xrd3d12::NativeScope& scope() const { return m_scope; }
    ID3D12GraphicsCommandList4* list4() const { return m_scope.list4; }
    ID3D11DeviceContext* context() const { return m_context; }

private:
    ID3D11DeviceContext* m_context{};
    xrd3d12::NativeScope m_scope{};
    bool m_open{};
};
#endif
} // namespace xray::render::RENDER_NAMESPACE
