// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Async compute paths (da_async.h): the path a frame takes, the passes at the two forks and the
// join, and r__async_verify.

#include "stdafx.h"

#include <atomic>
#include <cstring>
#include <optional>

#include "da_async.h"
#include "da_rt.h"

#include "Layers/xrRender/da_temporal.h"
#include "Layers/xrRenderDX11/dx11GpuTimers.h"
#include "Layers/xrRenderDX11/StateManager/dx11SamplerStateCache.h"

#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
IReader* open_shader(pcstr shader); // ResourceManager.cpp
void da_water_field_update(); // r4_water_field.cpp

namespace
{
enum class DaAsyncPath : u8
{
    Pixel, // today's pixel shaders at today's places (R4 always)
    ComputeInline, // the compute shaders at the fork points on the direct queue (QA)
    ComputeAsync, // the compute shaders as jobs on the compute queue
};

// Jobs of a frame, in fork order: water_ripple at fork A, gtao at fork B.
enum class DaJob : u8
{
    Ripple,
    Gtao,
    Count,
};
constexpr pcstr da_job_names[] = { "water_ripple", "gtao" };

// What the frame's forks did, for the joins and the AO slot.
struct DaAsyncFrame
{
    DaAsyncPath path{ DaAsyncPath::Pixel };
    bool ripple_moved{}; // fork A stepped the ripple in compute (or had no step to take)
    bool ripple_timed{}; // fork A issued its direct-queue timer slot
    bool gtao_done{}; // fork B ran GTAO: the AO slot has nothing left to do
    bool joined{}; // the last join ran: ripple published, schedule compares recorded
    u8 forked{}; // bit per DaJob forked this frame (the cq_* fields)
#if defined(XR_D3D12)
    xrd3d12::AsyncJob* jobs[u32(DaJob::Count)]{}; // forked this frame and not joined yet
#endif
};
DaAsyncFrame g_frame;

// r__async_compute st_async_auto on this device.
bool g_auto_recommended = false;

// A message that is worth one line per session.
#define DA_ASYNC_LOG_ONCE(...) \
    do \
    { \
        static bool told = false; \
        if (!told) \
        { \
            told = true; \
            Msg(__VA_ARGS__); \
        } \
    } while (false)

// One job's recording (async.md 3.4). On the job path the passes record on a pooled context
// switched to compute recording, and fork() turns its list into a job on the compute queue (in
// order on the direct queue without one) and gives the context back. On the inline path, or with no
// context free, they record on the immediate context and fork() has nothing to do.
class DaAsyncBatch
{
    CBackend* m_cmd;
    DaJob m_job;
    u32 m_context{ R_dsgraph_structure::INVALID_CONTEXT_ID };

    // The recording ends without a job; true = there was one.
    bool finish([[maybe_unused]] ID3D11CommandList** list)
    {
#if defined(XR_D3D12)
        if (m_context == R_dsgraph_structure::INVALID_CONTEXT_ID)
            return false;
        // The pooled context goes back without the scene's samplers.
        SSManager.UseSceneBias(m_cmd->context_id, false);
        const HRESULT hr = HW.get_context(m_cmd->context_id)->FinishCommandList(FALSE, list);
        RImplementation.release_context(m_context);
        m_context = R_dsgraph_structure::INVALID_CONTEXT_ID;
        return SUCCEEDED(hr) && *list;
#else
        return false;
#endif
    }

public:
    DaAsyncBatch(DaJob job, CBackend& immediate) : m_cmd(&immediate), m_job(job)
    {
#if defined(XR_D3D12)
        pcstr name = da_job_names[u32(job)];
        if (g_frame.path != DaAsyncPath::ComputeAsync || g_frame.jobs[u32(job)])
            return;
        const u32 id = RImplementation.alloc_context();
        if (id == R_dsgraph_structure::INVALID_CONTEXT_ID)
        {
            DA_ASYNC_LOG_ONCE("! [async] no free context for job %s: its passes run on the direct queue", name);
            return;
        }
        CBackend& cmd = RImplementation.get_context(id).cmd_list;
        ID3D11DeviceContext* deferred = HW.get_context(cmd.context_id);
        if (!xrd3d12::BeginAsyncRecording(deferred))
        {
            // A pooled context released with recorded work: nobody executes a released context's list,
            // so the stray work is dropped and the job records on the clean context.
            ID3D11CommandList* stray = nullptr;
            if (SUCCEEDED(deferred->FinishCommandList(FALSE, &stray)))
                _RELEASE(stray);
            if (!xrd3d12::BeginAsyncRecording(deferred))
            {
                RImplementation.release_context(id);
                DA_ASYNC_LOG_ONCE("! [async] job %s: no compute recording, its passes run on the direct queue", name);
                return;
            }
            DA_ASYNC_LOG_ONCE("! [async] job %s: pooled context %u held stray recorded work, dropped", name, id);
        }
        m_context = id;
        m_cmd = &cmd;
        // The forks lie in the scene stage: the job samples as RCache does there, with the
        // upscaler's texture LOD bias (r2_R_render.cpp, scene_stage_guard).
        SSManager.UseSceneBias(cmd.context_id, da_upscaler_active());
#endif
    }
    DaAsyncBatch(const DaAsyncBatch&) = delete;
    DaAsyncBatch& operator=(const DaAsyncBatch&) = delete;
    ~DaAsyncBatch() { drop(); }

    CBackend& cmd() const { return *m_cmd; }
    // The passes record into a job (false: on the immediate context, where timer slots work).
    bool is_job() const { return m_context != R_dsgraph_structure::INVALID_CONTEXT_ID; }

    // The recorded passes become a job that starts after everything the direct queue has so far.
    // false = the recording is lost (no list or no job): the caller undoes what the pass advanced
    // on the CPU and takes its fallback.
    bool fork()
    {
#if defined(XR_D3D12)
        if (!is_job())
            return true;
        pcstr name = da_job_names[u32(m_job)];
        ID3D11CommandList* list = nullptr;
        if (!finish(&list))
        {
            DA_ASYNC_LOG_ONCE("! [async] job %s: its recording could not be closed, the frame falls back", name);
            return false;
        }
        xrd3d12::AsyncJob* job = xrd3d12::AsyncJobFromCommandList(HW.pDevice, name, list);
        _RELEASE(list);
        if (!job)
        {
            DA_ASYNC_LOG_ONCE("! [async] job %s could not be made: the frame falls back", name);
            return false;
        }
        xrd3d12::ForkAsyncCompute(HW.get_context(CHW::IMM_CTX_ID), job);
        g_frame.jobs[u32(m_job)] = job;
        g_frame.forked |= u8(1u << u32(m_job));
#endif
        return true;
    }

    // Nothing was recorded (the pass had no work): no job, no fork.
    void drop()
    {
        ID3D11CommandList* list = nullptr;
        if (finish(&list))
            _RELEASE(list);
    }
};

#if defined(XR_D3D12)
// GPU time of a job on the compute queue (the last resolved one of that name), for the cq_* fields
// of r__gpu_log; 0 unless the job was forked this frame, so a job that stopped running reads 0.
float da_async_job_ms(DaJob job)
{
    return (g_frame.forked & (1u << u32(job))) ? xrd3d12::AsyncComputeMs(HW.pDevice, da_job_names[u32(job)]) : 0.f;
}
#endif

// A direct-queue timer slot around passes that record on the immediate context (a job's pooled
// context takes no timestamps: its time is the cq_* field). A slot is issued once a frame.
class DaInlineTimer
{
    std::optional<dx11GpuTimerScope> m_scope;

public:
    DaInlineTimer(dx11GpuTimers::Slot slot, bool on, bool& issued)
    {
        if (on && !issued)
        {
            m_scope.emplace(slot);
            issued = true;
        }
    }
};

#if defined(XR_D3D12)
void da_async_join_job(DaJob job)
{
    if (xrd3d12::AsyncJob*& forked = g_frame.jobs[u32(job)])
    {
        xrd3d12::JoinAsyncCompute(HW.get_context(CHW::IMM_CTX_ID), forked);
        forked = nullptr;
    }
}

// The auto rule (async.md 3.2): the adapters that overlap graphics and compute work.
void da_async_auto_decide()
{
    g_auto_recommended = false;
    pcstr reason = "no compute queue";
    const xrd3d12::NativeCaps* caps = xrd3d12::GetNativeCaps(HW.pDevice);
    if (caps && HW.native.asyncCompute)
    {
        if (caps->uma)
            reason = "integrated GPU";
        else
        {
            switch (caps->vendorId)
            {
            // Measured on Ampere (async.md, final test phase): the jobs overlap, but the extra work in
            // flight lowers the clock of a power-limited GPU more than the overlap saves - no NVIDIA
            // generation gains.
            case 0x10DE: reason = "NVIDIA: no measured gain"; break;
            // Every feature level 11_0 AMD part is GCN or RDNA, with async compute engines.
            case 0x1002:
                g_auto_recommended = true;
                reason = "AMD";
                break;
            case 0x8086:
                g_auto_recommended = true;
                reason = "Intel discrete";
                break;
            default: reason = "unknown vendor"; break;
            }
        }
    }
    Msg("* [async] auto: %s (%s)", g_auto_recommended ? "compute queue" : "pixel passes", reason);
}
#endif

// r__async_verify (async.md 3.6), DirectX 12 only. The request is only a flag (the console runs on
// another thread); the next frame starts the port stage: the frame takes the compute path inline,
// and each pass also runs its pixel version first from the same inputs. After its report the
// schedule stage follows: the frame takes the job path, and each pass also runs its compute version
// inline on the immediate context before its job. The outputs are compared on the GPU
// (da_async_compare.cs) and read back at the start of the frame after the stage's last compare.
struct DaAsyncVerify
{
    enum class Stage : u8
    {
        Idle,
        Port,
        Schedule,
        Report,
    };
    Stage stage{ Stage::Idle };
    bool schedule{}; // the stage running or being reported is the schedule stage
    bool gtao{}, ripple{}; // still to compare
    u32 frames{}; // frames of the stage so far

    struct Kept
    {
        shared_str name;
        ID3DTexture2D* copy{};
    };
    struct Result
    {
        u32 output{};
        u32 texels{};
        ID3DTexture2D* staging{};
    };
    xr_vector<Kept> kept;
    xr_vector<Result> results;
    // Schedule stage: kept name -> the job's result, compared after this frame's join.
    xr_vector<std::pair<shared_str, ID3DTexture2D*>> expected;
    // x = 0: differing texels, x = 1: the largest difference as float bits.
    ID3DTexture2D* counter{};
    ID3D11UnorderedAccessView* counter_uav{};
    ref_texture kept_alias[std::size(da_async_verify_outputs)];
    ref_texture live_alias[std::size(da_async_verify_outputs)];
};
DaAsyncVerify g_verify;
std::atomic_bool g_verify_requested{ false };

// Ripple steps come at 30 Hz, so a frame without one is normal; after this many frames the ripple
// is reported as not stepping.
constexpr u32 verify_port_frames = 90;

void da_async_verify_alias(string64& name, char side, int output)
{
    xr_sprintf(name, "$user$async_cmp_%c%d", side, output);
}

// One element per compared output: each binds its own pair of aliases, so the views of every
// comparison of the frame stay alive until the frame ran.
class CBlender_da_async_compare : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: r__async_verify's texel comparison"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement >= int(std::size(da_async_verify_outputs)))
            return;
        string64 kept, live;
        da_async_verify_alias(kept, 'a', C.iElement);
        da_async_verify_alias(live, 'b', C.iElement);
        C.r_ComputePass("da_async_compare");
        C.r_dx11Texture("s_cmp_a", kept);
        C.r_dx11Texture("s_cmp_b", live);
        C.r_End();
    }
};

void da_async_verify_release()
{
    for (auto& k : g_verify.kept)
        _RELEASE(k.copy);
    for (auto& r : g_verify.results)
        _RELEASE(r.staging);
    g_verify.kept.clear();
    g_verify.results.clear();
    g_verify.expected.clear();
    // The aliases go with the textures they pointed at (and before the resource manager does).
    for (auto* alias : { g_verify.kept_alias, g_verify.live_alias })
    {
        for (size_t i = 0; i < std::size(da_async_verify_outputs); ++i)
        {
            if (alias[i])
                alias[i]->surface_set(nullptr);
            alias[i]._set(nullptr);
        }
    }
    _RELEASE(g_verify.counter_uav);
    _RELEASE(g_verify.counter);
    g_verify.stage = DaAsyncVerify::Stage::Idle;
}

pcstr da_async_verify_stage_name() { return g_verify.schedule ? "schedule" : "port"; }

bool da_async_verify_running()
{
    return g_verify.stage == DaAsyncVerify::Stage::Port || g_verify.stage == DaAsyncVerify::Stage::Schedule;
}

bool da_async_verify_start(CRenderTarget& target, bool schedule)
{
    if (!target.s_async_compare)
    {
        if (!da_async_shader_exists("da_async_compare.cs"))
        {
            Msg("! r__async_verify: da_async_compare.cs is missing");
            return false;
        }
        CBlender_da_async_compare b_compare;
        target.s_async_compare.create(&b_compare, "r2" DELIMITER "da_async_compare");
        if (!target.s_async_compare)
            return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 2;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_UINT;
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &g_verify.counter)) ||
        FAILED(HW.pDevice->CreateUnorderedAccessView(g_verify.counter, &view, &g_verify.counter_uav)))
    {
        Msg("! r__async_verify: no counter texture");
        da_async_verify_release();
        return false;
    }
    for (int i = 0; i < int(std::size(da_async_verify_outputs)); ++i)
    {
        string64 name;
        da_async_verify_alias(name, 'a', i);
        g_verify.kept_alias[i].create(name);
        da_async_verify_alias(name, 'b', i);
        g_verify.live_alias[i].create(name);
    }
    g_verify.gtao = g_verify.ripple = true;
    g_verify.frames = 0;
    g_verify.schedule = schedule;
    g_verify.stage = schedule ? DaAsyncVerify::Stage::Schedule : DaAsyncVerify::Stage::Port;
    Msg(schedule ? "* [async] verify: schedule stage, frame %u (compute passes inline and as jobs from the same inputs)" :
                   "* [async] verify: port stage, frame %u (pixel and compute passes from the same inputs)",
        Device.dwFrame);
    return true;
}

void da_async_verify_report()
{
    auto* ctx = HW.get_context(CHW::IMM_CTX_ID);
    pcstr stage = da_async_verify_stage_name();
    bool compared[std::size(da_async_verify_outputs)]{};
    for (const auto& r : g_verify.results)
    {
        pcstr name = da_async_verify_outputs[r.output];
        compared[r.output] = true;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(ctx->Map(r.staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            Msg("! [async] verify %s %s: the result could not be read back", stage, name);
            continue;
        }
        u32 values[2];
        std::memcpy(values, mapped.pData, sizeof(values));
        ctx->Unmap(r.staging, 0);
        float largest;
        std::memcpy(&largest, &values[1], sizeof(largest));
        Msg("%c [async] verify %s %s: %u of %u texels differ, max %g", values[0] ? '!' : '*', stage, name, values[0],
            r.texels, largest);
    }
    for (size_t i = 0; i < std::size(da_async_verify_outputs); ++i)
    {
        if (!compared[i])
            Msg("* [async] verify %s %s: not compared, the pass did not run in compute", stage, da_async_verify_outputs[i]);
    }
    da_async_verify_release();
}

const DaAsyncVerify::Kept* da_async_verify_find(pcstr name)
{
    for (const auto& k : g_verify.kept)
    {
        if (!xr_strcmp(k.name, name))
            return &k;
    }
    return nullptr;
}

// The target and viewport the frame had before a fork. u_setrt leaves the viewport alone, and the
// G-buffer after fork A draws with whatever viewport it finds (the frame start's display size, set
// again by rmNormal) - the cloud map's would clip it to its own square.
void da_async_restore_scene_state(CBackend& cmd_list, CRenderTarget& target)
{
    target.u_setrt_scene_base(cmd_list);
    RImplementation.rmNormal(cmd_list);
}

// This frame's path (async.md 3.2).
DaAsyncPath da_async_decide()
{
#if defined(XR_D3D12)
    switch (ps_r__async_path)
    {
    case 1: return DaAsyncPath::Pixel;
    case 2: return DaAsyncPath::ComputeInline;
    // Forced jobs: with -dx12_no_async they run in order on the direct queue (the layer's fallback).
    case 3: return DaAsyncPath::ComputeAsync;
    default: break;
    }
    // The setting needs a compute queue; auto takes it where the adapter gains from it.
    if (HW.native.asyncCompute && (ps_r__async_compute == 2 || (ps_r__async_compute == 1 && g_auto_recommended)))
        return DaAsyncPath::ComputeAsync;
#endif
    return DaAsyncPath::Pixel;
}

// Everything the combine reads from the jobs: their joins (the last forked first - one compute
// queue runs them in order, so its wait covers the others), the ripple step's publish, the
// schedule stage's compares, and the end of a verify stage.
void da_async_join_all(CBackend& cmd_list)
{
    if (g_frame.joined)
        return;
    g_frame.joined = true;
#if defined(XR_D3D12)
    da_async_join_job(DaJob::Gtao);
    da_async_join_job(DaJob::Ripple);
#endif
    // The readers of the new ripple step all come after this point (water, puddles, combine_2).
    RImplementation.Target->phase_water_ripple_publish();
    for (const auto& [name, live] : g_verify.expected)
        da_async_verify_compare(cmd_list, name.c_str(), live);
    g_verify.expected.clear();
    if (da_async_verify_running() && !g_verify.gtao &&
        (!g_verify.ripple || g_verify.frames >= verify_port_frames))
    {
        g_verify.stage = DaAsyncVerify::Stage::Report;
    }
}

} // namespace

bool da_async_shader_exists(pcstr name)
{
    IReader* file = open_shader(name);
    const bool exists = !!file;
    FS.r_close(file);
    return exists;
}

void da_async_targets_create(CRenderTarget& target)
{
    auto* state = xr_new<DaAsyncTargets>();
    target.m_da_async = state;
#if defined(XR_D3D12)
    // The early-out reads the depth the G-buffer wrote; an MSAA depth is multisampled. DirectX 12
    // only: without the alias DirectX 11 marches every texel, as at Gate 0 (PLAN 0.1, C14).
    if (!RImplementation.o.msaa && target.rt_Render_Depth && target.rt_Render_Depth->pSurface)
    {
        state->cloud_depth.create(da_cloud_depth_name);
        state->cloud_depth->surface_set(target.rt_Render_Depth->pSurface);
    }
    da_async_auto_decide();
    dx11GpuTimers::RegisterField("async", [] { return float(da_async_path_code()); });
    dx11GpuTimers::RegisterField("cq_ripple", [] { return da_async_job_ms(DaJob::Ripple); });
    dx11GpuTimers::RegisterField("cq_gtao", [] { return da_async_job_ms(DaJob::Gtao); });
#endif
}

void da_async_targets_destroy(CRenderTarget& target)
{
    DaAsyncTargets* state = target.m_da_async;
    if (!state)
        return;
    // A verify in flight dies with the targets it compares.
    da_async_verify_release();
    // The alias may outlive the target in a shader's texture list: it must not keep the depth.
    if (state->cloud_depth)
        state->cloud_depth->surface_set(nullptr);
    xr_delete(state);
    target.m_da_async = nullptr;
}

void da_async_frame_begin()
{
    g_frame = {};
    if (g_verify.stage == DaAsyncVerify::Stage::Report)
    {
        // The port stage's report is followed by the schedule stage.
        const bool schedule_next = !g_verify.schedule;
        da_async_verify_report();
        if (schedule_next)
            da_async_verify_start(*RImplementation.Target, true);
    }
    if (g_verify_requested.exchange(false) && g_verify.stage == DaAsyncVerify::Stage::Idle)
        da_async_verify_start(*RImplementation.Target, false);
    g_frame.path = da_async_decide();
    // The port stage compares the pixel and the compute passes at the fork points (inline path),
    // the schedule stage the inline and the job runs of the compute passes (job path).
    if (da_async_verify_running())
    {
        g_frame.path = g_verify.schedule ? DaAsyncPath::ComputeAsync : DaAsyncPath::ComputeInline;
        ++g_verify.frames;
    }
}

bool da_async_fork_early(CBackend& cmd_list)
{
    if (g_frame.path == DaAsyncPath::Pixel)
        return false;
    CRenderTarget& target = *RImplementation.Target;
    const bool verify = da_async_verify_running();

    // Nothing below reads the frame's geometry. The water field and the cloud map stay on the
    // direct queue (the sun and the march read the map), then the ripple steps. The march waits for
    // the G-buffer: its early-out skips what the geometry hides (PLAN.md C14). A job is recorded only
    // for a pass with work this frame.
    da_water_field_update();
    target.phase_cloud_map();
    {
        const bool check = verify && g_verify.ripple;
        // Without a step the solve only moves its clock, on the immediate context.
        std::optional<DaAsyncBatch> job;
        if (da_water_ripple_due(target))
            job.emplace(DaJob::Ripple, cmd_list);
        DaInlineTimer timer(dx11GpuTimers::Ripple, !job || !job->is_job(), g_frame.ripple_timed);
        const int steps = target.phase_water_ripple_cs(job ? job->cmd() : cmd_list, check);
        g_frame.ripple_moved = steps >= 0;
        // A lost job's steps never ran: nothing to publish, the field keeps its window.
        if (job && steps > 0 && !job->fork())
            da_water_ripple_lost(target);
        if (steps < 0)
            DA_ASYNC_LOG_ONCE("! [async] the ripple step has no compute version here (shader or UAV): pixel pass");
        // Compared on its first step, or without a compute step there is nothing to compare.
        if (check && steps != 0)
            g_verify.ripple = false;
    }
    da_async_restore_scene_state(cmd_list, target);
    return true;
}

void da_async_fork_gtao(CBackend& cmd_list)
{
    if (g_frame.path == DaAsyncPath::Pixel)
        return;
    CRenderTarget& target = *RImplementation.Target;
    const bool verify = da_async_verify_running();

    // The march, and a pass without its compute version, run as the pixel passes where the pixel
    // path runs them: after the G-buffer, before the sun.
    if (!g_frame.ripple_moved)
    {
        DaInlineTimer timer(dx11GpuTimers::Ripple, true, g_frame.ripple_timed);
        target.phase_water_ripple();
    }
    {
        dx11GpuTimerScope timer(dx11GpuTimers::CloudsMarch);
        target.phase_clouds_march();
    }
    // GTAO only when it is this frame's AO technique: not under RTAO (PLAN C18).
    if (target.gtao_is_frame_ao() && !da_rt_live(RtEffect::AO))
    {
        DaAsyncBatch job(DaJob::Gtao, cmd_list);
        if (!target.phase_gtao_cs(job.cmd(), verify && g_verify.gtao))
            DA_ASYNC_LOG_ONCE("! [async] GTAO has no compute version here (shader or UAV): pixel pass in the combine");
        // A lost job leaves the AO slot to the pixel pass.
        else
            g_frame.gtao_done = job.fork();
    }
    if (verify)
        g_verify.gtao = false;
    da_async_restore_scene_state(cmd_list, target);
}

void da_async_join(CBackend& cmd_list)
{
    if (g_frame.path == DaAsyncPath::Pixel)
        return;
    // The direct queue's wait for the jobs: timed only where there are jobs to wait for.
    dx11GpuTimerScope gpu(dx11GpuTimers::JoinWait);
    da_async_join_all(cmd_list);
}

bool da_async_ao_done() { return g_frame.gtao_done; }

bool da_async_auto_recommended() { return g_auto_recommended; }

int da_async_path_code()
{
    switch (g_frame.path)
    {
    case DaAsyncPath::ComputeInline: return 1;
    case DaAsyncPath::ComputeAsync: return 2;
    default: return 0;
    }
}

void da_async_verify_request()
{
#if defined(XR_D3D12)
    g_verify_requested = true;
    Msg("* [async] verify requested");
#else
    Msg("! r__async_verify: DirectX 12 only");
#endif
}

bool da_async_verify_schedule() { return g_verify.stage == DaAsyncVerify::Stage::Schedule; }

ID3DTexture2D* da_async_verify_kept(pcstr name)
{
    const DaAsyncVerify::Kept* kept = da_async_verify_find(name);
    return kept ? kept->copy : nullptr;
}

void da_async_verify_expect(pcstr name, ID3DTexture2D* live)
{
    if (da_async_verify_schedule() && live)
        g_verify.expected.emplace_back(name, live);
}

void da_async_verify_keep(CBackend& cmd_list, pcstr name, ID3DTexture2D* source)
{
    if (!da_async_verify_running() || !source)
        return;
    const DaAsyncVerify::Kept* kept = da_async_verify_find(name);
    if (!kept)
    {
        D3D11_TEXTURE2D_DESC desc;
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        ID3DTexture2D* copy = nullptr;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &copy)) || !copy)
        {
            Msg("! [async] verify: no copy of %s", name);
            return;
        }
        kept = &g_verify.kept.emplace_back(DaAsyncVerify::Kept{ name, copy });
    }
    HW.get_context(cmd_list.context_id)->CopyResource(kept->copy, source);
}

void da_async_verify_restore(CBackend& cmd_list, pcstr name, ID3DTexture2D* target)
{
    if (!da_async_verify_running() || !target)
        return;
    if (const DaAsyncVerify::Kept* kept = da_async_verify_find(name))
        HW.get_context(cmd_list.context_id)->CopyResource(target, kept->copy);
}

void da_async_verify_compare(CBackend& cmd_list, pcstr name, ID3DTexture2D* live)
{
    if (!da_async_verify_running() || !live)
        return;
    const auto output = u32(std::find_if(std::begin(da_async_verify_outputs), std::end(da_async_verify_outputs),
        [name](pcstr o) { return !xr_strcmp(o, name); }) - std::begin(da_async_verify_outputs));
    const DaAsyncVerify::Kept* kept = da_async_verify_find(name);
    CRenderTarget& target = *RImplementation.Target;
    if (output >= std::size(da_async_verify_outputs) || !kept || !target.s_async_compare || !g_verify.counter_uav)
        return;

    D3D11_TEXTURE2D_DESC desc;
    live->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = 2;
    staging_desc.Height = 1;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_R32_UINT;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3DTexture2D* staging = nullptr;
    if (FAILED(HW.pDevice->CreateTexture2D(&staging_desc, nullptr, &staging)) || !staging)
    {
        Msg("! [async] verify: no read-back texture for %s", name);
        return;
    }

    auto* ctx = HW.get_context(cmd_list.context_id);
    constexpr UINT zero[4]{};
    ctx->ClearUnorderedAccessViewUint(g_verify.counter_uav, zero);
    g_verify.kept_alias[output]->surface_set(kept->copy);
    g_verify.live_alias[output]->surface_set(live);
    // Nothing compared may still be bound for output, and the backend must bind the aliases anew.
    // The targets come back afterwards: the combine draws on into the targets it bound.
    ID3DRenderTargetView* bound[4];
    for (u32 slot = 0; slot < 4; ++slot)
    {
        bound[slot] = cmd_list.get_RT(slot);
        cmd_list.set_RT(nullptr, slot);
    }
    cmd_list.clear_CS_resources();

    SPass& P = *target.s_async_compare->E[output]->passes[0];
    cmd_list.set_States(P.state);
    cmd_list.set_CS(P.cs);
    cmd_list.set_Constants(P.constants);
    cmd_list.set_Textures(P.T);
    cmd_list.set_c("da_cmp_size", float(desc.Width), float(desc.Height), 0.f, 0.f);
    UINT initial = 1;
    ctx->CSSetUnorderedAccessViews(0, 1, &g_verify.counter_uav, &initial);
    cmd_list.Compute((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &none, &initial);
    cmd_list.clear_CS_resources();
    for (u32 slot = 0; slot < 4; ++slot)
        cmd_list.set_RT(bound[slot], slot);

    ctx->CopyResource(staging, g_verify.counter);
    g_verify.results.push_back({ output, desc.Width * desc.Height, staging });
}
} // namespace xray::render::RENDER_NAMESPACE
