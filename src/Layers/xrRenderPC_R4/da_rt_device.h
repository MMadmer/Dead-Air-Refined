// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Inside the ray tracing lane: the device part (da_rt_device.cpp) the scene and the passes build on.
// One binding model for every RT pass (dxr.md 5, PLAN.md C8): on DirectX 12 one root signature and
// native sections on the immediate context; on Vulkan compute dispatches of the D3D11 path with the
// layer's bindless set, the tables by device address and the scene as an SRV (VULKAN_RENDERER.md).
// The lane's GPU objects (buffers, acceleration structures, builds) are the same on both
// (da_rt_native.cpp).
#pragma once

#include "da_rt.h"

#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#elif defined(XR_VULKAN)
#include "Layers/xrRenderVK/vk_api.h"
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

#if defined(DA_RT_LANE)
using DaRtAddress = u64; // GPU virtual address (DX12) or device address (Vulkan)
#if defined(XR_D3D12)
using DaRtPipeline = ID3D12PipelineState*;
#else
using DaRtPipeline = ID3D11ComputeShader*;
#endif

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
ID3D12RootSignature* da_rt_root_signature();
#endif
constexpr u32 da_rt_max_inputs = 16;
constexpr u32 da_rt_max_outputs = 8;
constexpr u32 da_rt_max_tables = 4;
constexpr u32 da_rt_max_constants = 4096;

// Compute pipeline of the RT pass shader `file` (under the shader root, e.g. "rt/rt_ao.hlsl") at
// cs_6_5, compiled through da_dxc_compile_native (DXIL, or the SPIR-V container on Vulkan). Created
// with the device or its targets, never on a frame; null on failure (logged once).
DaRtPipeline da_rt_pipeline(pcstr file, pcstr entry, const DaDxcDefine* defines = nullptr, u32 define_count = 0);

struct DaRtDispatch
{
    DaRtPipeline pipeline{};
    const void* constants{}; // b0, up to da_rt_max_constants bytes
    u32 constants_size{};
    ID3D11ShaderResourceView* scene{}; // acceleration structure view; null = the empty scene
    ID3D11ShaderResourceView* const* inputs{}; // t0.. (NON_PIXEL_SHADER_RESOURCE)
    u32 input_count{};
    ID3D11UnorderedAccessView* const* outputs{}; // u0.. (UNORDERED_ACCESS)
    u32 output_count{};
    // t0-t3 space3 (structured or raw buffers; Vulkan: their addresses in the push constants); 0 = a
    // stand-in the pass does not read.
    DaRtAddress tables[da_rt_max_tables]{};
    DaRtAddress raw_output{}; // u0 space3 (RWByteAddressBuffer); 0 = stand-in
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
    DaRtAddress tables[da_rt_max_tables]{}; // geometry, instances, per-frame geometry, per-frame instances
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
DaRtPipeline da_rt_skin_pipeline();

// ---- the lane's GPU objects (da_rt_native.cpp) --------------------------------------------------

// Opaque handles: a native buffer (an ID3D12Resource, or an xrVulkan RT buffer), an acceleration
// structure, the compacted sizes of a batch of builds.
struct DaRtBuffer;
struct DaRtAccel;
struct DaRtSizeQuery;

constexpr u32 da_rt_bindless_invalid = ~0u;
// One mesh of a bottom-level build: float3 positions, 16-bit indices.
struct DaRtTriangles
{
    DaRtAddress vb{};
    u32 stride{}, vertex_count{};
    DaRtAddress ib{};
    u32 index_count{};
    bool alpha{}; // any-hit candidates reach the query loops (each once); else opaque
};
// D3D12_RAYTRACING_INSTANCE_DESC, whose layout VkAccelerationStructureInstanceKHR shares.
struct DaRtInstanceDesc
{
    float transform[3][4];
    u32 id : 24;
    u32 mask : 8;
    u32 contribution : 24;
    u32 flags : 8;
    DaRtAddress accel;
};
static_assert(sizeof(DaRtInstanceDesc) == 64);
constexpr u32 da_rt_instance_cull_disable = 0x1; // TRIANGLE_CULL_DISABLE / TRIANGLE_FACING_CULL_DISABLE
constexpr u32 da_rt_instance_force_non_opaque = 0x8;
// Build flags (the values D3D12 and Vulkan share).
enum DaRtBuildFlags : u32
{
    DaRtBuildAllowUpdate = 0x1,
    DaRtBuildCompaction = 0x2,
    DaRtBuildFastTrace = 0x4,
    DaRtBuildFastBuild = 0x8,
};
struct DaRtSizes
{
    u64 result{}, build{}, update{}; // the structure, the scratch of a build, of an update
};
// A bottom level over `count` meshes, or (geometry null) a top level of `count` instances.
bool da_rt_sizes(const DaRtTriangles* geometry, u32 count, u32 flags, DaRtSizes& out);

// Device-local, shader-writable buffers (scratch, pre-skinned vertices).
DaRtBuffer* da_rt_buffer_create(u64 size, pcstr name);
DaRtAddress da_rt_address(const DaRtBuffer* buffer);
// Its first `size` bytes as a bindless raw buffer; da_rt_bindless_invalid when the range is full.
u32 da_rt_bindless_raw(DaRtBuffer* buffer, u64 size);
DaRtAccel* da_rt_accel_create(u64 size, bool top, pcstr name);
DaRtAddress da_rt_address(const DaRtAccel* accel);
// The view a pass binds the scene with (a new reference; released before the structure).
ID3D11ShaderResourceView* da_rt_accel_view(DaRtAccel* accel);
// Released once the GPU finished the work recorded so far; the handle is cleared.
void da_rt_release(DaRtBuffer*& buffer);
void da_rt_release(DaRtAccel*& accel);
void da_rt_release(DaRtSizeQuery*& query);
// Compacted sizes of `count` builds (DaRtBuild::sizes), read after the work completed; 0 = unknown.
DaRtSizeQuery* da_rt_size_query(u32 count);
bool da_rt_read_sizes(DaRtSizeQuery* query, u64* sizes, u32 count);

// An engine buffer's address (0: none, a dynamic buffer); `tracked`: the layer tracks its state
// (DirectStorage writes it) and native reads must require it.
DaRtAddress da_rt_engine_address(ID3D11Buffer* buffer, bool* tracked = nullptr);
// The device's bindless range (xrD3D12 / xrVulkan): contiguous, da_rt_bindless_invalid when full;
// a freed range comes back after the frames in flight.
u32 da_rt_bindless_alloc(u32 count);
void da_rt_bindless_texture(u32 index, ID3D11ShaderResourceView* view);
void da_rt_bindless_buffer(u32 index, ID3D11Buffer* buffer);
void da_rt_bindless_free(u32 index, u32 count);
// Grows when textures were written, created or destroyed (bindless tables refresh then).
u64 da_rt_texture_generation();
bool da_rt_video_memory(u64& budget, u64& usage);
// Submits the immediate context's work and waits for the GPU.
void da_rt_flush_wait();

struct DaRtBuild
{
    DaRtAccel* dst{};
    DaRtAccel* src{}; // the structure an update refits
    const DaRtTriangles* geometry{}; // null: a top level of `count` instances at `instances`
    u32 count{};
    DaRtAddress instances{};
    u32 flags{};
    bool update{};
    DaRtAddress scratch{};
    DaRtSizeQuery* sizes{}; // the compacted size goes to its entry `size_index`
    u32 size_index{};
};
struct DaRtUpload
{
    void* cpu{};
    DaRtAddress address{};
};

// A section of RT work on the immediate context. DirectX 12: a native section (the RT root signature
// and the bindless tables are set once, every dispatch binds its own tables and constants; no D3D11
// call on the immediate context while it is open, GPU timer scopes go around it). Vulkan: D3D11-path
// dispatches; the context's compute bindings come back when it closes.
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
    ID3D11DeviceContext* context() const { return m_context; }

    // The lane's native work (da_rt_native.cpp).
    // A device-local buffer filled with `data`, then readable by the passes and the builds.
    DaRtBuffer* upload_buffer(const void* data, u64 size, pcstr name);
    // This frame's upload memory by address (instances, tables, palettes).
    DaRtUpload upload(u64 size, u64 alignment);
    void build(const DaRtBuild& b);
    void compact(DaRtAccel* dst, DaRtAccel* src);
    // Every structure and scratch write before what follows (DX12: a UAV barrier, on `accel` alone
    // when set).
    void accel_barrier(DaRtAccel* accel = nullptr);
    // The next builds reuse this scratch.
    void scratch_barrier(DaRtBuffer* scratch);
    // A buffer a dispatch wrote through raw_output, read next by builds and passes.
    void written(DaRtBuffer* buffer);
    // The compacted sizes of the builds so far into `query` (after an accel_barrier).
    void copy_sizes(DaRtSizeQuery* query, u32 count);
    // Resources the passes read through the bindless tables, into their read state.
    void require_read(ID3D11Resource* const* resources, u32 count);

private:
    ID3D11DeviceContext* m_context{};
    bool m_open{};
#if defined(XR_D3D12)
    xrd3d12::NativeScope m_scope{};
#else
    // The compute bindings of the context before the section (put back when it closes).
    ID3D11ComputeShader* m_shader{};
    ID3D11ShaderResourceView* m_srvs[da_rt_max_inputs + 1]{};
    ID3D11UnorderedAccessView* m_uavs[da_rt_max_outputs]{};
    ID3D11Buffer* m_cb{};
    ID3D11SamplerState* m_samplers[4]{};
    ID3D11RenderTargetView* m_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* m_dsv{};
#endif
};
#endif
} // namespace xray::render::RENDER_NAMESPACE
