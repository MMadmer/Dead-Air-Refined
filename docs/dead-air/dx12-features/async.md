# Async compute (DirectX 12): GTAO and the ripple solver on the compute queue

Status: implemented (28.09.2026). A1-A7 reached; the functional part of the final test phase passed
(verify port and schedule 0 texels after layer fix S1), and the Perf phase decided A7 and the auto rule
("Final test phase, performance" under "Implementation state"): **the compute cloud march, `r__async_march`,
`r__async_join` and the split joins are removed** - the march stays the pixel pass after the G-buffer
with its early-out (C14), the jobs are GTAO and the water ripple; `r__async_compute` auto (the default)
picks the pixel passes on NVIDIA ("no measured gain": a loss on Ampere) and the compute queue on AMD
and Intel discrete GPUs; the on/off setting stays for the player. The design text below still names the
march job where it was written before those removals. Builds on `infra.md` section 3.3 (compute queue, `crossFence` /
`computeFence`, `AsyncJob`, Fork/Join markers in `Context::Submit`, join at `Present1`, lent-resource
guard, in-order fallback) and 3.1 (`acq` cap, `-dx12_no_async`). This feature adds: recording of
**engine passes** (blenders, `CBackend`, binders) into async jobs, read-shared lending, compute
versions of three passes with results identical to their pixel shaders, an in-engine verifier, the
frame placement, timers and one menu row. `xrRender_R4.dll` keeps today's passes and order.

## Integration amendments (PLAN.md)

Implemented by the `async` lane (milestones A1-A7 of `PLAN.md` 4.1). These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **C12**: the layer part of 3.3 (compute recording mode, typed allocators/lists, refusals,
  `NON_PIXEL` CS inputs, `transitioned`, `AsyncJobFromCommandList`, read-shared lending and the guard
  refinement, `StatAutoJoins`) and layer scenarios A-E (`scenarios_async.cpp`) are built by the
  `infra` lane in milestone I5 together with the jobs. This lane owns `da_async.*`, the three phase
  files, `blender_gtao.cpp`, `r2_rendertarget.cpp`, the shaders of section 6 and `async_check.lua`.
- **Hooks**: the frame placement of 3.4 goes through hook functions `core` pre-places in
  `r2_R_render.cpp` and `r4_rendertarget_phase_combine.cpp` (`PLAN.md` 2.5): `da_async_frame_begin`,
  `da_async_fork_early` (cloud map + jobs; returns whether the ripple and the march moved),
  `da_async_fork_gtao` after `r_rain.sync()`, `da_async_join` at the combine head (with the ripple
  publish), `da_async_ao_done` in the AO slot. The RT scene build runs before `da_async_fork_early`.
- **C13, `r__gpu_log`**: numeric keys only - `ripple` (direct-queue ripple steps), `join`,
  `cq_march`, `cq_ripple`, `cq_gtao` (compute-queue job times), `async` (0 pixel, 1 inline,
  2 async). The timer slots `Ripple`, `JoinWait` and the field registry are `core`'s (K2).
- **C14, cloud-march early-out** (moved here from `vrs.md` 3.7): all full-res depths of the texel's
  2x2 footprint plus a 1-texel border in front of the far plane -> write the sentinel `a = -1`; the
  history blend and `da_clouds.ps` treat the sentinel as "no history". It needs the current frame's
  depth, so it runs on the DirectX 12 Pixel path, where the march comes after the G-buffer; a job at
  fork A marched without it. DirectX 11 has no early-out: PLAN 0.1 ("DX11 frames and FPS unchanged")
  ranks above C14 (final-phase decision F1, PLAN 8.3). A7 measured async `on` against `off +
  early-out`: the march job lost, so the march stays a pixel pass after the G-buffer on every path and
  job A carries the ripple only (the compute march is deleted).
- **C15**: the `dxc` lane later makes its payoff edits in `gtao.h` and `da_clouds_march.h`; the
  compute variants then need a quad-swizzled thread-to-pixel mapping or the non-quad path under
  `DA_COMPUTE`, and `r__async_verify` port must stay at 0.
- **C18**: the GTAO job is recorded only when GTAO is this frame's AO technique (not when RTAO runs).
  The GTAO targets are created when `ssao_mode == gtao || da_rt_needs_ao_targets()` (`rt` stub), R16F
  `rt_ssao_temp` with UAV on R5.
- **C4**: the new compute shaders are SM 5.0-compatible HLSL; on R5 they compile in the session's
  mode (DXIL with DXC), on R4 with FXC.
- **UI**: the `async_compute` row, nodes and strings of section 7 are written by `core` (K3); the
  console commands are registered by `core` (K2) and call this lane's functions.

## Implementation state

**Removed by the Perf phase** ("Final test phase, performance" below): `phase_clouds_march_cs`, its blender and `da_clouds_march.cs`,
the `clouds_march` job and its `cq_march` field, the verify's `clouds0` compare, `r__async_march`,
`r__async_join`, `da_async_join_clouds` and `da_async_join_late` with its hook line in `phase_combine`.
The cloud map stays on the direct queue at fork A, the march runs as the pixel pass at fork B (after the
G-buffer, before the sun) on every path, and one join at the combine head (`da_async_join`) covers the
frame's jobs. The bullets below record A1-A7 as they were built and tested; where they name those parts,
this paragraph wins.

A1-A7 are in the tree: the ripple's measured cost, the shared bodies, the compute versions, the
pixel path's early-out, `da_async` with the `Pixel`, `ComputeInline` and `ComputeAsync` paths, the
fork and join bodies behind `core`'s hooks, `r__async_verify`'s port and schedule stages, the auto
rule and the two A7 placements as runtime switches (A5-A7 untested, see "Final test phase"). Where
the code differs from sections 3-6 below:

- **Members** (`r4_rendertarget.h`, the `[dx12:async]` block `core` pre-placed): the three compute
  passes `phase_clouds_march_cs(CBackend&, bool history)`, `phase_gtao_cs(CBackend&)`,
  `phase_water_ripple_cs(CBackend&)`, `phase_water_ripple_publish()`, their shaders, plus
  `phase_clouds_march_ps(CBackend&, bool history, bool early_out)` (the pixel march; the verify runs it
  with the early-out off at the fork) and `phase_water_ripple_step(CBackend&, const DaRippleStep&,
  int index, bool compute)` (one step of either path; a pixel step keeps today's per-step copy, a
  compute step reads scratch b from its second step on and publishes once). `phase_async_early/gtao`
  are not members: the fork hooks in `da_async.cpp` call the passes (A4).
- **Lane state**: `DaAsyncTargets` (`da_async.h`) behind `m_da_async`, made at the end of the
  CRenderTarget constructor and dropped first in its destructor (`da_async_targets_create/destroy`).
  `DaRippleStep` carries a frame's solve from `da_ripple_prepare` (the CPU half of 3.4: priming clears
  on the immediate context, accumulator, sources, window shift) to the steps.
- **GTAO compute**: `CBlender_gtao::compute` builds the two compute passes as a separate shader
  `s_gtao_cs` (elements 0/1) instead of elements 2-3 of `s_gtao`.
- **Compute shaders exist only with their files**: the march and GTAO compute shaders are created at
  target creation on R5 when `da_clouds_march.cs` / `gtao_render.cs` + `gtao_filter.cs` ship, the
  ripple's on first use (its FFT is size-dependent) when `da_water_ripple.cs` ships
  (`da_async_shader_exists`); without them the pixel passes stay (an install with older content does
  not die on a missing shader).
- **GTAO body and MSAA**: the render half of `gtao.h` is compiled only without MSAA (GTAO is never
  created under MSAA, where `common.h` types `s_position` as `Texture2DMS`); `gtao_render.cs` joins
  `gtao_render.ps` on `Test-Shaders.ps1`'s known list for the MSAA4 permutation.
- **Early-out (C14)**, `da_clouds_march.h` + `da_clouds_march.s`: the depth is the render-size scene
  depth (`rt_Render_Depth`) under a fixed alias `$user$cloud_depth` (`surface_set`, no copy); four
  `GatherRed` cover the texel's 4x4 full-res block (2x2 footprint plus the one-pixel border, clamp at
  the rim); all 16 below 1.0 -> `float4(0, 0, 0, -1)` and no march. The alias exists on DirectX 12
  without MSAA only (`da_async_targets_create`, under `XR_D3D12`): DirectX 11 marches every texel as
  at Gate 0 (F1). The gathers sit in a branch of their own under `da_cloud_early.x` (HLSL's `&&` does
  not short-circuit, so a combined condition ran them with the early-out off). The march unbinds its pixel-shader inputs after the draw (an empty texture list): the depth is a depth
  target again right after, and a binding the runtime drops behind the backend's cache would never be
  bound again. The composite never reaches a sentinel texel from a sky pixel (the border), so its
  bilinear taps are unchanged.
- **Sentinel reads** (`da_clouds_hist.h`, `da_clouds_fetch`): the march's history and the composite
  gather the four alphas first; with no sentinel among them the hardware bilinear sample is used, bit
  for bit as before; otherwise the valid taps are reweighted with the bilinear weights, and all four
  hidden reads as "no history" (march) or "no deck" (composite). After a turn, texels that were
  behind geometry start without history, which the motion fade does anyway. `da_cloud_early`
  (declared here; x = skip, y = the targets may hold sentinels) is set by all three passes from the
  alias (`da_cloud_set_early`): with y = 0 (DirectX 11, MSAA) the read is the single `SampleLevel` of
  Gate 0, no `GatherAlpha`.
- **GTAO filter corner**: the gather corner is `(pix + 1) * inv_size` from the integer pixel. The
  interpolated form `floor(tc * size - 0.5) + 1` rounded to the neighbour's corner on a share of the
  pixels, which is the one visible change on DirectX 11 (PLAN 6.4 known delta).

- **Paths**: `da_async_frame_begin` decides the frame's path: `r__async_path` 1 `Pixel`, 2
  `ComputeInline`, 3 `ComputeAsync` (with `-dx12_no_async` infra's in-order fallback runs the jobs
  on the direct queue); 0 follows `r__async_compute`: `st_async_on` or `st_async_auto` on a
  recommended adapter take `ComputeAsync` when `acq=1`, everything else `Pixel`. R4 is always
  `Pixel`, every hook returns at once there and the commands are inert (`r__async_verify` answers
  "DirectX 12 only").
- **Jobs (A5)**, `DaAsyncBatch` in `da_async.cpp`: on `ComputeAsync` a job's passes record on a
  pooled context (`alloc_context`, whose `reset()` invalidates its `CBackend`) switched to compute
  recording (`xrd3d12::BeginAsyncRecording`); `fork()` is `FinishCommandList` ->
  `AsyncJobFromCommandList` -> `ForkAsyncCompute` -> `release_context`. No free context or a refused
  recording: the passes record on `RCache` (one log line each). A job is recorded only for a pass
  with work this frame (`da_clouds_march_due`, `da_water_ripple_due` - the accumulator test of
  `da_ripple_prepare` without moving it); a pass that recorded nothing is dropped without a fork.
  Job names `clouds_march`, `water_ripple`, `gtao` are the `cq_*` fields' keys
  (`xrd3d12::AsyncComputeMs`, the last resolved time of that name, shown only while the job was
  forked this frame - a per-frame bit in `DaAsyncFrame::forked` - so a job that stopped running,
  e.g. the march under `r__async_march 1`, reads 0). The pooled context records with the scene
  stage's samplers (`SSManager.UseSceneBias(ctx, da_upscaler_active())`, as `RCache` has them at
  the forks; reset before the context goes back), so the march's LOD-1 taps of the shape volume
  take the upscaler's mip bias on every path. A job that cannot be made (`FinishCommandList` or
  `AsyncJobFromCommandList` fails; `fork()` returns `false`, log once) falls back: the march drops
  its history (`da_clouds_history_drop`, the lost job carried the history copy) and runs as the
  pixel pass at fork B; GTAO leaves the AO slot to the pixel pass; the ripple publishes nothing and
  puts the field's window centre back (`da_water_ripple_lost`), the step is skipped.
- **Auto rule (A6)**, decided when the targets are made (`da_async_auto_decide`, one line
  `* [async] auto: compute queue|pixel passes (<reason>)`): a compute queue (`acq=1`), not UMA, and
  AMD or Intel discrete -> `ComputeAsync`; NVIDIA (every generation, `NVIDIA: no measured gain` since the
  Perf phase - the first version took Turing and newer), unknown vendors and integrated GPUs -> `Pixel`.
  On this machine (RTX 3070 Laptop) the default R5 frame is therefore the pixel path; `r__async_compute
  st_async_on` forces the jobs where a compute queue exists, `-dx12_no_async` removes the queue (pixel
  path; only `r__async_path 3` still records jobs, which then run in order on the direct queue).
- **Fork A** (`da_async_fork_early`, compute paths): `da_water_field_update`, the cloud map, the
  compute march (with history), the compute ripple steps, then the scene target and viewport the
  frame had (`u_setrt_scene_base` + `rmNormal`): `u_setrt` leaves the viewport alone and the
  G-buffer on the immediate context draws with the viewport it finds, so without the restore the
  cloud map's square viewport clipped the G-buffer (seen in the captures of the first A4 runs;
  the restore went in after the gate run, it changes nothing the port compares). A pass whose
  compute version is missing (shader file or UAV) returns `false` (the ripple `-1`) and runs as
  its pixel pass at fork B, where the pixel path runs it (the march with its early-out), so the
  data flow stays the pixel path's.
- **Fork B** (`da_async_fork_gtao`): those pixel fallbacks, then GTAO in compute when GTAO is the
  frame's AO technique (`gtao_is_frame_ao()`, the test of `phase_combine`'s AO slot) and RTAO does
  not run this frame (`!da_rt_live(RtEffect::AO)`, the `rt` lane's per-frame latch, C18); `true`
  makes `da_async_ao_done()` skip the AO slot. A pass that falls back to its pixel version logs
  once (`! [async] ... has no compute version here`).
- **Join**: `da_async_join_all` joins the frame's jobs last-forked first (infra's join of a job
  covers every job forked before it on the one compute queue, so `-dxstats` counts one join), then
  publishes the ripple (the one copy into `water_ripple0`) and records the schedule stage's
  compares. `r__async_join 0` (default): at the combine head (`da_async_join`, `JoinWait` timer;
  on the inline path that is the 8 MB publish copy, 0.05 ms at 1024^2). `r__async_join 1` (A7,
  split per consumer): the march job at the head of `phase_clouds_composite`
  (`da_async_join_clouds`, inside the `clouds` timer) and the rest right before `combine_1`
  (`da_async_join_late`, a `[dx12:async] hook` line in `phase_combine`; not timed - a timer slot
  is issued once a frame and `JoinWait` is taken by the head); the AO slot, the render-target
  clears and the sky between them read nothing the jobs write.
- **March placement (A7, C14)**: `r__async_march 1` keeps the march out of job A: it runs as the
  pixel pass with its early-out at fork B (after the G-buffer, before the sun) and job A carries the
  ripple only; a verify frame still marches in compute so all four outputs are compared.
- **Timers**: on `ComputeInline` and for every pass that runs on the immediate context the march
  and the ripple keep their slots (`march`, `ripple`, each issued at most once a frame), so pixel and
  inline read side by side; on `ComputeAsync` their time is `cq_march` / `cq_ripple` / `cq_gtao`;
  `async` is registered as a `[gpu]` field (0/1/2, R5 only).
- **`r__async_verify`, port stage**: the request is a flag; the next frame starts the stage and
  runs on `ComputeInline`. Each pass runs its pixel version first from the same inputs (the march
  with history off and without the early-out, GTAO render + filter at the render-size viewport,
  the ripple's step 0 - its state and step clock are put back afterwards), keeps the outputs
  (`da_async_verify_keep`), then its compute version into the same targets and compares on the
  GPU (`da_async_compare.cs`, one blender element per output with its own pair of
  `$user$async_cmp_*` aliases, a 2x1 R32_UINT counter, one staging copy per output). The results
  are read back at the next frame's start: `* [async] verify port <name>: N of M texels differ,
  max D` (`!` when N > 0); a pass that did not run in compute is listed as not compared. The ripple
  waits up to 90 frames for a frame with a step.
- **`r__async_verify`, schedule stage (A5)**: the frame after the port report starts it on
  `ComputeAsync`. Each pass first runs its compute version inline on `RCache` from the same inputs
  (history and step clock not advanced), keeps it, then records its job as usual; the ripple job
  keeps its first step itself (`ripple_job`, a copy recorded in the job) because its later steps
  overwrite scratch b. After the join each kept inline result is compared with the job's
  (`da_async_verify_expect`): `* [async] verify schedule <name>: N of M texels differ, max D`.
  A compare unbinds the colour targets for its dispatch and binds them again after it: with split
  joins the schedule compares run right before `combine_1`, which draws into what the combine bound.
  Without a compute queue (or a free pooled context) the "job" runs in order or inline and the
  stage compares the same work twice.
- **`precise` on the stage outputs** (R5 only): the pixel and the compute stage of one body are
  bit-identical only if the driver compiles both the same way. DXC marks every float operation
  `fast`, and the driver contracted the two pipelines differently: the first port run
  (`async-a4-verify`) found clouds0 346 of 518400 texels (max 2^-11, a half-float rounding flip),
  gtao_0 2493 of 2073600 (max 0.28: a last-bit tap coordinate picks the neighbouring
  point-sampled texel) and ssao_temp 2775; the ripple was already equal. The wrappers now store
  through `DA_STAGE_EXACT` (`precise` under `GRIZZLY_DXC`, empty for FXC, defined in `gtao.h` and
  `da_clouds_march.h`); DXC then emits no `fast` flag in the body (0 of 223 float operations of
  `gtao_render.ps`). DirectX 11 and the DirectX 12 FXC fallback compile the same instructions as
  before. The cost is measured at A7 (the first runs, under machine load, showed no change of
  `ao`/`scene` or `march`/`scene` beyond the noise). The `dxc` lane's payoff edits (C15) live
  inside that precise chain and must keep the port at 0 (told in its inbox).
- **GTAO without a VRS scope**: the `vrs` lane measured the `AoRaw` scope out (V5, blockiness
  +29 %), so `phase_gtao` is the one pixel GTAO again and C29 no longer applies.

### Final test phase (A5-A7, PLAN.md 0.1b / 0.1a)

The A5-A7 code is untested; these runs are the gates, taken once in the final test phase, R5 on
`qa_pedro` unless named (`Invoke-Dx12Stand`, parity-free, `-KeepShaderCache`):

1. **A5 + A7 in one session**, `async-a5-verify`: `-Scenario async_check -Weather af3_bright_storm
   -ExtraArgs '-r5 -always_active -dxstats' -PreCommands 'r__gpu_log 300'`, globals
   `qa_fps_ms = 8000` and `qa_async_phases` = off (`r__async_compute st_async_off`), on
   (`st_async_on`, `r__async_join 0`, `r__async_march 0`), on + `r__async_march 1`, on +
   `r__async_join 1` (about 60 s after the load). Pass: `* [async] auto: compute queue (NVIDIA
   Turing or newer)`; `verify port` 0 differing texels for `clouds0`, `gtao_0`, `ssao_temp`,
   `ripple`; `verify schedule` exactly 0 for all four; `-dxstats` 3 forks a frame (2 without a
   ripple step), 1 join (2 in the split phase), 0 by the guard; no `not joined`, `joined early`,
   `compute recording`, `no compute version`, `no free context`, `could not be made` or `could not
   be closed` line; `[gpu]` lines with `async=2`, `cq_march`/`cq_gtao` > 0 and `join` < 0.3 ms in
   the on phases (`cq_march` 0 in the `r__async_march 1` phase: no march job), `async=0` in off.
2. **A5 robustness**, `async-a5-gbv`: run 1 with `-ExtraArgs '-r5 -always_active -dxdebug
   -dx12gbv'`, `-PreCommands 'vid_mode 1280x720 (60Hz)','r__gpu_log 300'`, globals
   `qa_verify_ms = 30000` and one phase (on). Pass: no `D3D12 ERROR` / `CORRUPTION` line, verify as
   in 1, the capture renders.
3. **A5 fallback**, `async-a5-fallback`: `-ExtraArgs '-r5 -always_active -dx12_no_async'
   -PreCommands 'r__async_path 3'`, one phase. Pass: caps `acq=0`, `[async] auto: pixel passes (no
   compute queue)`, the jobs in order (`-dxstats` forks = joins, nothing async), verify port and
   schedule 0.
4. **A6**: R4 inertness (`r__async_verify` answers "DirectX 12 only", `r__async_compute` accepted),
   DX11 frames and FPS, upscalers with async on, motion vectors with async on (no AO lag or cloud
   smear on the turn), the Video row (under the renderer, three values, greyed on DX11, applies
   without a restart: the next `[gpu]` line shows the new `async=`) are all taken from the final
   gate's own runs (PLAN.md 6.4 R4 parity/perf/options_tab, 6.6 defaults = auto = on here:
   `upscale_check`, `motion_check`, `options_tab`); no extra run unless one of them fails there.
5. **A7 perf**, one `Invoke-Dx12Perf` batch, `qa_pedro`, `-Pairs 3`, 2560x1440 Extreme TAA
   (PLAN.md 0.5): A = `r__async_compute st_async_off` (the pixel path with the early-out: C14's
   "off + early-out"), B = `st_async_on` with the placements run 1 chose. Gate: `frame` at least
   0.8 ms lower in B at equal clocks, `join` <= 0.2 ms, `cq_march`/`cq_gtao` within 1.5x of A's
   `march`/`ao`. `foliage` (>= 0.5 ms) comes from the final gate's per-feature table (6.7).

Decisions the final phase takes from these numbers (the losing code is deleted, not left behind a
switch):
- **C14 march placement** (run 1, on vs on + `r__async_march 1`, `frame` means and FPS): if the
  pixel march after the G-buffer wins beyond the phase noise, job A carries the ripple only - delete
  the fork-A march branch, `phase_clouds_march_cs`, `CBlender_da_clouds_march_cs`,
  `da_clouds_march.cs`, the UAV of `rt_clouds[0]`, the verify's `clouds0` and `cq_march`;
  otherwise delete `r__async_march` and the `march_pixel` branch.
- **Join split** (run 1, on vs on + `r__async_join 1`): only if `join` > 0.2 ms at the head and the
  split phase's `frame` is lower beyond the noise, keep the split alone (the head `da_async_join`
  becomes a no-op and goes); otherwise delete `r__async_join`, `da_async_join_clouds`,
  `da_async_join_late` and its hook line in `phase_combine`.
- **Auto rule**: if B does not beat A on this adapter (Ampere), the NVIDIA branch of the rule
  resolves to pixel (PLAN.md 7.2: auto resolves to off where it does not gain; the row stays).
- **`precise`** (`DA_STAGE_EXACT`): its cost is the R5 `march`/`ao` rise against the Gate-0 R5
  perf reference in 6.7 while R4 `march`/`ao` stay within +-0.05 ms (FXC compiles no `precise`,
  and R4 has no early-out, F1); it stays (identical results are this design's requirement), the
  number is recorded here.

### Final test phase results, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- Run 1 (`final-async-verify`, then `final-async-verify-b` after the layer fix): `* [async] auto: compute queue
  (NVIDIA Turing or newer)`; `verify port` and `verify schedule` 0 of 518400 / 1048576 / 2073600 / 2073600 texels for
  `clouds0`, `ripple`, `gtao_0`, `ssao_temp`, max 0; no `not joined` / `joined early` / `compute recording` / `no
  compute version` / `no free context` / `could not be made` / `could not be closed` line. The first run joined the
  march job early on 47-150 of 300 frames: the sun cascade's deferred list samples the cloud map the job reads
  read-shared, and the layer demanded the list's exact entry state (infra S1, fixed in `ExecuteCommandList`); the
  re-run has **0 by the guard** in every window, 2.0-2.6 forks and 1 join a frame (split phase ~1.8). The phases'
  FPS are not a measurement (Perf phase).
- A4-A6 coverage from the other runs: jobs forked with GD, mesh grass, DirectStorage, VRS and all three RT effects on
  (`final-on-*`, `final-robust-*`), `r__caps ... acq=1`; R4 inert (`caps api=11 ... acq=0`), the Video row 50/50 on
  R4 and R5 (`final-r4-options`, `final-r5-options`).
- Found with `-dxdebug` (`final-robust-pedro`): every job refused by `BeginAsyncRecording` because pooled context 3
  held stray GPU markers of the light shadow batches (engine backend, stand-fixes S4, fixed). `DaAsyncBatch` now
  drops a pooled context's stray recorded work and records the job on it (one log line), instead of running every
  later job on the direct queue (stand-fixes S5).
- Look-neutral with the other features (`final-on-nods-pedro` against the DXC-only control + Gate 0): calibrated
  "equal".

### Final test phase, performance (Perf phase, 28.09.2026)

A7 batch `perf-async-pedro` (`Invoke-Dx12Perf`, qa_pedro, 2560x1440 Extreme TAA, `-dxstats`, 2 pairs after the
warm-up; `qa\perf-async-pedro.summary.txt`), means of two runs:

| Variant | FPS | frame ms | busy clock | frame at 1300 MHz |
|---|---|---|---|---|
| off (`st_async_off`: pixel path with the early-out) | 54.3 | 18.18 | 1291 MHz | 18.04 ms |
| on (march, GTAO, ripple as jobs) | 50.0 | 19.58 | 1228 MHz | 18.50 ms |
| on + `r__async_march 1` (march as the pixel pass) | 52.1 | 18.74 | 1289 MHz | 18.59 ms |
| on + `r__async_join 1` (split joins) | 51.6 | 19.01 | 1188 MHz | 17.37 ms |
| masked (`-dx12_no_async -dx12_no_rt`) | 55.7 | 17.64 | 1268 MHz | 17.20 ms |

The only pair inside the 3 % clock rule, off -> on + pixel march, is -3.9 % (`sun` +1.40 ms and `cq_gtao` 6.5 ms
while GTAO leaves `combine`/`ao`, -1.6 ms): the jobs do overlap, but the extra work in flight lowers the clock of this
power-limited GPU (1291 -> 1228 MHz) more than the overlap saves; every "on" variant is slower than off. Decisions
(the losing code deleted, PLAN 7.2):
- **C14 march placement**: the pixel march after the G-buffer (with its early-out) beats the job - the compute march
  and `r__async_march` are removed; job A carries the ripple only.
- **Join split**: no gain - `r__async_join`, `da_async_join_clouds`, `da_async_join_late` and the hook line are removed;
  one join at the combine head.
- **Auto rule**: auto resolves to the pixel passes on NVIDIA (`* [async] auto: pixel passes (NVIDIA: no measured
  gain)`), stays the compute queue on AMD and Intel discrete (not measurable here); the row keeps off/auto/on.
- **`precise`** stays (identical results are this design's requirement); its cost was not measured separately.
- Open (data, not code): the Video row's hint `st_da_x64_video_hint_async_compute` (eng and rus) still names the
  clouds among the async work.

One pass with everything on and async forced on (`final-onepass-foliage`, RTAO replacing GTAO, so the ripple is the
only job): `cq_ripple` 1.04 ms on the compute queue, `join` 0.035 ms, `-dxstats` 182 forks and 182 joins per 300
frames (one per frame with a ripple step), 0 by the guard.

### Gate results (A1, A4)

- **A1, the ripple's cost** (`Ripple` slot around today's pixel ripple, 1920x1080 Extreme = 1024^2
  field, `render_check` parity runs with `r__gpu_log 100` of the K2 code with the Gate-0 overlay,
  `core-k2-renderb-<pedro|foliage|trees>[-r4]`, plus `async-a4-g0gtao-trees-r4`; no run of its
  own - the numbers were already in these logs, PLAN 0.1a): one step costs **0.25 ms on R5**
  (median 0.251, 0.243-0.261; 16 of 25 logged frames had a step) and **0.26 ms on R4** (median
  0.261, 0.244-0.29; one 0.90 ms frame of a slow load took three steps). Per frame on average
  0.16 ms (R5) / 0.21 ms (R4): a step is due in about two frames of three at these frame rates.
  In the storm of `async_check` (rain drops into the field) the pixel step reads 0.40 ms and the
  compute step 0.35 ms (medians, `async-a4-verify`, R5, one session). Scripts and output in
  `_work\dx12-features-20260928\async`: `a1-ripple.py` / `a1-ripple.txt`, `a4-gpu.py` /
  `a4-gpu.txt`.
- **A4, build and static checks**: `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` EXIT 0 (15:28), no
  warning from the touched files; `Test-Shaders.ps1` on the six changed shaders with FXC, DXC SM 6.6
  and SM 6.0: 0 failures (`a4-shaders.txt`); `Check-ShaderLinkage.py` FXC and DXC 6.6: 512
  pipelines, 0 refused; layer tests 77 passed, 0 failed (floor 68).
- **A4, the gate** (`async_check`, R5, qa_pedro, storm, `r__async_path 2`, run
  `async-a4-verify-b`): `verify port clouds0: 0 of 518400 texels differ`, `ripple: 0 of 1048576`,
  `gtao_0: 0 of 2073600`, `ssao_temp: 0 of 2073600`. PASS. (The first run, `async-a4-verify`,
  without `precise`, is the measurement above.)
- **A4 on DirectX 11**: `async-a4-g0gtao-trees-r4` (tree binaries, Gate-0 overlay + the async
  shaders with the two Gate-0 GTAO pixel shaders, parity recipe, plus `r__async_path 2`,
  `r__async_compute st_async_on`, `r__async_verify` in `qa_render_cmds`): `r__async_verify` answers
  "DirectX 12 only"; the frame is equal to Gate 0 with the Gate-0 snapshot control
  `core-k2-ctl-trees-r4` as fourth reference (within 99.94 %, PSNR 54.5 dB, PASS); against the
  three quiet references alone one 1077 px cluster in a distant crown fails the cluster limit - the
  snapshot's own control differs from them at the same spot (its largest cluster, 697 px). An
  earlier attempt of this run (moved to `qa\_superseded_async`) had the `dxc` lane's 15:00 payoff
  headers mixed into the Gate-0 overlay and compiled no march.

### Gate results (A2, A3)

Runs under `_work\dx12-features-20260928\qa`, binaries of one lane snapshot (`async\snap-a3r`, built
12:49 from the tree), overlay `async\ovl-a3` = the Gate-0 overlay plus exactly the async shaders (the
tree's overlay also carries other lanes' shader edits); parity recipe of the Gate-0 references
(`render_check -Tga`, SMAA, `wind_freeze 1`, 1920x1080), compared with the three references plus a
same-period control of the Gate-0 snapshot (PLAN 0.6).

- Static: `Test-Shaders.ps1` on the ten async shaders with FXC, DXC SM 6.6 and DXC SM 6.0: 0 failures
  (the MSAA4 permutation of `gtao_render.ps/.cs` is on the known list); `Check-ShaderLinkage.py` FXC,
  DXC 6.6, DXC 6.0: 472 pipelines, 0 refused; layer tests 50/50 (floor 45 at the time).
- A2, PSNR against Gate 0 (gate >= 45 dB): R5 qa_pedro 52.2 dB, foliage 47.1 dB, trees 51.2 dB
  (`async-a3-render-<save>`); R4 qa_pedro 52.7 dB, foliage 46.6 dB, trees 50.1 dB
  (`async-a3-render-<save>-r4`).
- Attribution: the same binaries with the two Gate-0 GTAO pixel shaders are **equal** to Gate 0
  (calibrated): R5 foliage 99.79 % within, 55.1 dB (`async-a3-g0gtao-foliage-b`), trees 99.97 %,
  63.4 dB (`async-a3-g0gtao-trees`); R4 foliage 99.84 %, 55.2 dB (`async-a3-g0gtao-foliage-r4-b`),
  trees 99.94 %, 54.3 dB (`async-a3-g0gtao-trees-r4`). On top of that the Gate-0 ripple shader
  (`async-a3-g0rip-foliage-r4`) or the Gate-0 cloud shaders (`async-a3-g0cld-foliage-r4`) change
  nothing (both equal), and the lane binaries with the whole Gate-0 overlay are equal on R5 and R4
  (`async-a3-g0ovl-foliage[-r4]`): the whole delta of A2/A3 is the GTAO filter corner. The first
  foliage runs of the GTAO attribution failed under machine load (the R4 one ran at 8 FPS, game clock
  16 s later than the references) and passed when repeated.
- A3, the early-out hides no visible pixel: every `g0gtao` run above carries the early-out and the
  sentinel reads and is equal to Gate 0; the differing pixels of the GTAO-corner runs sit on geometry
  edges, none in the sky.
- A3, `march` timer (R4, `r__gpu_log 100`, parity runs, one binary with a temporary
  `-async_no_early_out` switch that was removed afterwards; `async-a3-eo-<on|off>-<save>-r4`):
  qa_pedro (interior) 1.156 -> 0.126 ms, trees 1.061 -> 0.368 ms, foliage (open sky above bushes)
  1.222 -> 0.954 ms. Measured on R4 before the final phase made the early-out DirectX 12 only (F1);
  they stand for the saving of the R5 pixel path (same shader, same half-resolution target).

## 1. Goal

1. Take the GPU-bound passes that touch nothing the raster work around them touches off the direct
   queue and run them on the compute queue while the direct queue is busy with geometry:
   - **fork A** at the start of the frame: the volumetric cloud march and the ripple solver's steps,
     overlapping the G-buffer (level, grass, HUD, wallmarks, rain);
   - **fork B** right after the rain pass (the last writer of the G-buffer): GTAO render + filter,
     overlapping the sun cascades, their accumulation, emissive and local lights;
   - **one join** at the head of `phase_combine`, where the first consumer of any of them runs.
2. Identical output: each compute shader runs the same per-pixel function as its pixel shader, and
   `r__async_verify` compares the two (and inline against async) bit for bit inside a running frame.
3. DX11 intact: R4 runs the pixel shaders where it runs them today; the only R4-visible change is that
   those pixel shaders compute their coordinates from `SV_Position` (section 3.6).
4. Measurable: per-job GPU time on the compute queue, the direct queue's wait at the join, frame time
   A/B at equal clocks.

### Candidates

Costs: R5, 2560x1440 Extreme, TAA, save `qa_pedro`, `r__gpu_log` means of runs `q4-r5a..c`
(`_work/dx12-perf-20260927/qa`, 1.28-1.35 GHz, 89-96 W): frame 16.2 ms, scene 3.0, sun 4.55,
lights 0.45, march 1.9, combine 5.3 of which ao 1.57.

| Pass | GPU | Reads | First consumer | Verdict |
|---|---|---|---|---|
| Cloud march `phase_clouds_march` | 1.87-1.98 ms | cloud map, its own history, two noise volumes | clouds composite in `phase_combine` | **async, fork A** |
| GTAO `phase_gtao` | 1.57 ms | `rt_Position` (+`rt_Normal` unpacked), blue noise | `combine_1` (`s_occ`) | **async, fork B** |
| Ripple solver `phase_water_ripple` | untimed today; estimated 0.3-0.5 ms per 30 Hz step at 1024^2 (Extreme), 0-3 steps a frame | its state, the water field | `water.ps`, `da_puddle_refl.ps`, `combine_2` | **async, fork A** (FFT and propagate are already compute) |
| Cloud map `phase_cloud_map` | about 0.1 ms | nothing of the frame | sun accumulation, march, composite | stays a pixel pass on the direct queue; in async mode it moves to the frame start so fork A can read it (a join before the sun for 0.1 ms is not worth a sync point) |
| Bloom / luminance | 0.05 ms | the finished HDR scene | combine_2, right after | no: nothing left to overlap with |
| Visor drops | small, only with a visor | own state | combine_2 | no: below the fork/join overhead |
| HDAO compute (`s_hdao_cs`) | - | - | - | not in the presets since GTAO replaced it; untouched |

Same-queue compute is **not** an alternative: on this driver a dispatch next to graphics work in one
list gains nothing (probe below: within -0.09..+0.12 ms of the pixel shaders). Without a compute
queue the renderer keeps the pixel shaders.

## 2. Current code

### 2.1 Frame (`src/Layers/xrRender_R2/r2_R_render.cpp`, `CRender::Render`)

| Line | What | Relevance |
|---|---|---|
| 276 | `GpuTimers.FrameBegin()` | the `frame` timer spans the direct queue |
| 316-319 | `q_sync_point.Wait/End` (an event query, `dx11HW.cpp:1169`: its `End` submits, `d12_context_ops.cpp:2439-2443`) | the queue is flushed right before fork A |
| 321 | `r_main.sync()` | fork A goes after it |
| 326/333 | `phase_scene_prepare` (clears `rt_Position` when AO is on) | |
| 357-402 | G-buffer part 0 (parallel grass on DX12, `parallel_details` `:33-140`) | overlaps job A |
| 406-533 | occlusion, light visibility | |
| 535-570 | part 1 (split scene), HUD UI, wallmarks | |
| 601 | `r_rain.sync()`: rain patch draws into `rt_Position`/`rt_Normal`+`rt_Color` (`r3_rendertarget_draw_rain.cpp:310-314`) | last G-buffer writer: fork B goes after it |
| 608-609 | `da_water_field_update()`, `phase_water_ripple()` | moves to fork A |
| 612 | `phase_visor_drops()` | stays |
| 618-622 | `phase_cloud_map()`, `phase_clouds_march()` (timer `CloudsMarch`) | map to frame start, march to fork A |
| 625-644 | sun: `r_sun.sync()` executes the cascade lists (smap + accumulate, `render_phase_sun.cpp:467-534`), HUD shadow, `accum_direct_blend` | overlaps job B |
| 645-664 | emissive (`phase_accumulator`, writes the accumulator only) | overlaps job B |
| 676-693 | local lights | overlaps job B |
| 709 | `phase_combine()` (`r4_rendertarget_phase_combine.cpp:131`): AO block `:149-173` (GTAO `:150-156`), sky + clouds composite `:196-201`, `combine_1` `:238`, puddles `:405`, forward `:408` | join at its head |
| 727-740 | `GpuTimers.FrameEnd`, `r__gpu_log` line | new fields |

Nothing between `r_rain.sync()` and `phase_combine` writes `rt_Position` or `rt_Normal` (writers:
`r3_rendertarget_phase_scene.cpp:24-124` and the rain pass only).

### 2.2 The three passes

- **Cloud march** (`r4_rendertarget_phase_clouds.cpp:62-104`): full-screen quad of
  `da_clouds_march.ps` into `rt_clouds[0]` (render/2, RGBA16F, `r2_rendertarget.cpp:880`), then
  `CopyResource(rt_clouds[1], rt_clouds[0])` (history, `:98`). Inputs by `da_clouds_march.s`:
  `$user$cloud_map`, `$user$clouds1`, `da\da_cloud_shape`, `da\da_cloud_detail`; every fetch is
  `SampleLevel` (`da_clouds_vol.h:77,86`, `da_clouds.h:140,161`), so it is compute-legal. It reads
  `I.tc0` (interpolated) for the view ray and `pos2d` for the jitter hash. Its only consumer is
  `da_clouds.ps` through `phase_clouds_composite` (`phase_combine:201`).
- **GTAO** (`r4_rendertarget_phase_gtao.cpp:11-56`, blender `blender_gtao.cpp`): `gtao_render.ps`
  -> `rt_gtao` (G16R16F, `r2_rendertarget.cpp:816`), `gtao_filter.ps` (16 `GatherRed` + 16
  `GatherGreen`) -> `rt_ssao_temp` (R16F at `:820`; G16R16F without UAV when the blur branch `:800`
  created it first). Inputs `s_position`, `s_normal`, `$user$blue_noise`. Consumer: `combine_1`
  (`USE_GTAO`, `r4_shaders.cpp:383`). Active on High/Extreme (`ssao_mode_by_preset`,
  `xrRender_console.cpp:1230`), never under MSAA.
- **Ripple solver** (`r4_rendertarget_phase_water_ripple.cpp:120-313`): 0-3 fixed 30 Hz steps; per
  step the real-space pixel pass `da_water_ripple.ps` (reads `$user$water_ripple0` and
  `$user$water_field`, writes FFT scratch a), four FFT passes and the propagate pass on compute
  (`da_ripple_compute` `:75-91`), then `CopyResource(water_ripple0, FFT b)` (`:301`). Readers of
  `water_ripple0`: `deffer_impl_flat.ps` (G-buffer, via `da_puddles.h`) and `rain_patch_normal.ps`
  (rain) run **before** the solver and read the previous step; `water.ps`, `da_puddle_refl.ps`,
  `combine_2_*.ps` (`da_water_under.h`, `blender_combine.cpp:263`) run in `phase_combine` and read
  the new one. Priming clears (`:163-171`) are RTV clears.

### 2.3 Engine and layer facts used here

- Compute passes already exist: C++ blenders with `r_ComputePass` (`Blender_Recorder_R3.cpp:189`),
  `CBackend::Compute` (`dx11R_Backend_Runtime.h:324`), UAVs bound with `CSSetUnorderedAccessViews`
  (`phase_hdao`, the ripple FFT). Targets get UAVs with `CRT::CreateUAV` (`dx11SH_RT.cpp:134`).
  Binders take the context's `CBackend` (`r2.cpp:99-121`), so a pass recorded on a pooled context
  gets `pos_decompression_params2`, `eye_position`, the cloud and fog constants exactly as on `RCache`.
- Pooled contexts: `alloc_context/release_context` (`D3DXRenderBase.h:80-118`), 8 parallel
  (`Engine.h:20-28`). Sun (3) and rain (1) are allocated in `Calculate` (`r2_R_calculate.cpp:174-199`),
  the grass takes 3 during the G-buffer: at both fork points at least 4 are free.
- Layer: deferred contexts record DIRECT lists from DIRECT allocators (`Context::AcquireAllocator`
  / `OpenList`, `d12_context_ops.cpp:1300-1383`); CS inputs are required in
  `PIXEL|NON_PIXEL_SHADER_RESOURCE` (`EnsureShaderInputs`, `d12_context_state.cpp:537-561`), UAVs in
  `UNORDERED_ACCESS` (`:564-579`); the deferred tracker harvests first/last states
  (`StateTracker::Require/Harvest`, `d12_context_ops.cpp:776-893`); `ExecuteCommandList` patches them
  exactly (`:2510-2573`); queries are refused on deferred contexts (`:2346-2353`, `:2390-2396`).
- GPU timers are immediate-only (`dx11GpuTimers.h:17-19`); no slot covers the ripple.

### 2.4 Probe on this machine

Offline probe written for this design (session scratchpad `async_probe/probe.cpp` + `probe.hlsl`,
not in the tree; its checks become layer scenarios A-E). Stand-ins: 3 cascades of 2048^2 D32
(16 instances of a 512x512-quad grid with vertex "wind"), the same alpha-tested, the cascades each
followed by a 16-tap `SampleCmp` accumulation at 2560x1440, a 3-target RGBA16F G-buffer; a 40-step
3D-noise march at 1280x720 and a GTAO-like 24-tap pass + 8x8 gather filter at 2560x1440, each once
as full-screen pixel shaders (today) and once as compute from the **same** HLSL function. 150
iterations per case, all work queued behind a CPU gate, GPU timestamps, three rounds, stand and
ninja locks held; GPU at 1374 MHz / 85 W average (power-limited like the game). Debug layer and
GPU-based validation silent, including the compute queue reading textures kept in
`PIXEL|NON_PIXEL_SHADER_RESOURCE`.

Saved = (graphics + pixel-shader passes on the direct queue) - (graphics on direct || compute on the
compute queue, fork before, join after), ms, rounds 1/2/3:

| Direct-queue work (alone) | + march (PS 1.20-1.27) | + GTAO (PS 2.46-2.49) | + both |
|---|---|---|---|
| cascades depth-only (2.73-2.80) | 0.64 / 0.70 / 0.66 (54%) | 1.08 / 1.10 / 1.03 (43%) | 1.57 / 1.58 / 1.71 |
| cascades alpha-tested (3.63-3.76) | 0.46 / 0.53 / 0.45 (39%) | 0.60 / 0.62 / 0.73 (26%) | 0.74 / 0.75 / 0.64 |
| cascades + accumulation (3.79-3.95) | 0.78 / 0.67 / 0.69 (57%) | 1.04 / 1.05 / 1.08 (43%) | 1.45 / 1.65 / 1.69 |
| G-buffer (2.09-2.16) | 0.63 / 0.65 / 0.65 (52%) | 0.87 / 0.93 / 0.83 (35%) | 1.27 / 0.99 / 0.93 |

Also measured: the compute versions alone on the compute queue cost the same as the pixel shaders
(march 1.28-1.33 vs 1.20-1.27, GTAO 2.49-2.54 vs 2.46-2.49); compute recorded right after or right
before the graphics work in the same direct list (no barrier between) lands within -0.09..+0.12 ms
of the pixel shaders, i.e. one queue overlaps nothing; a fence wait between them on the same queue
costs up to 0.35 ms more. Under contention the compute span stretches 1.2x behind the G-buffer,
1.35x behind cascades + accumulation and 1.7x behind alpha-tested casters, so the windows below must
be long enough (they are, section 10).

## 3. Design

### 3.1 Frame with async on

```
direct : [cloud map] F_A [G-buffer, occlusion, wallmarks, rain] F_B [visor][sun cascades + accum][emissive][lights] J [combine ...][present]
compute:                 [clouds_march][water_ripple]               [gtao render][gtao filter] ................^
```

`F_A`, `F_B` are forks (infra: `crossFence` signal on the direct queue, `Wait` on the compute
queue), `J` is the join (`Wait` on `computeFence` for the last job). All of it is still handed to
the queues in the frame's one `Submit` at `Present1`; the markers split that submission into three
direct batches and three compute ones (+4-6 `ExecuteCommandLists`, +5-7 fence operations, 60-100 us
of render thread at Present). Two forks with no direct work between them (the two jobs of F_A) share
one `crossFence` value: `Submit` signals only when the direct batch before the marker is not empty.

### 3.2 Paths

`da_async_path()` (new `xrRenderPC_R4/da_async.cpp`, both modules) is decided once at the top of
`CRender::Render`:

| Path | Where the passes run | When |
|---|---|---|
| `Pixel` | today's pixel shaders, today's places | R4 always; R5 with `r__async_compute st_async_off`; `auto` where not recommended; `acq=0` (a single pass whose UAV or compute shader is missing falls back alone, section 8) |
| `ComputeAsync` | compute shaders in jobs at F_A/F_B, joined at J | R5, `acq=1`, `st_async_on`, or `st_async_auto` when recommended |
| `ComputeInline` | the same compute shaders at F_A/F_B on `RCache` (direct queue) | QA only: `r__async_path 2` (R5) |

"Recommended" (auto): `acq=1` and the adapter overlaps graphics and compute - NVIDIA with mesh
shader tier 1 (Turing and newer; `NativeCaps::meshShaderTier`), any AMD (every FL 11_0 AMD part is
GCN or RDNA with async compute engines), Intel discrete (non-UMA, `D3D12_FEATURE_ARCHITECTURE1`).
Not recommended: NVIDIA Maxwell/Pascal, integrated GPUs, unknown vendors. (As built after the Perf
phase: no NVIDIA adapter is recommended - "NVIDIA: no measured gain"; AMD and Intel discrete stay.) `r__async_path` (runtime,
not saved): 0 follow the setting, 1 force `Pixel`, 2 force `ComputeInline`, 3 force `ComputeAsync`
(with `-dx12_no_async` this runs the jobs in order on the direct queue: infra's fallback).

### 3.3 Layer: D3D11-API recording into async jobs (extends infra 3.3)

Infra's jobs record native commands through a `NativeScope`. The passes here are engine passes: a
blender, `set_Element`, `set_c`, binders, `CSSetShaderResources`, `Dispatch`. They are recorded
through the D3D11 API on a pooled deferred context switched to compute recording, and the finished
command list becomes the job.

```cpp
// d12_api.h (async.md)
// Switches a deferred context to compute recording until its next FinishCommandList: the D3D12 list
// is COMPUTE (DIRECT when the device has no compute queue). Recorded: compute shaders and
// dispatches, copies, UAV clears, every state setter, Map/Unmap. Dropped with one log line: draws,
// RTV/DSV clears, resolves, queries. Shader reads use NON_PIXEL_SHADER_RESOURCE inside the list.
bool BeginAsyncRecording(ID3D11DeviceContext* deferred);
// Wraps a list finished by such a context into a job (infra's AsyncJob). Every tracked resource the
// list touched becomes an AsyncUse (entry = first state, exit = last state); one the list only
// reads is lent read-shared. The job holds a reference on each until it is joined.
AsyncJob* AsyncJobFromCommandList(ID3D11Device* device, const char* name, ID3D11CommandList* list);
```

Then infra's `ForkAsyncCompute(immediate, job)`, `JoinAsyncCompute(immediate, job)` and
`AsyncComputeMs(device, name)` apply unchanged.

**Context in compute recording** (`d12_layer.h` `Context`, `d12_context_ops.cpp`):
- `bool m_recordCompute`, set by `BeginAsyncRecording` (refused with a log line while a list is open),
  cleared by `FinishCommandList`. `AllocatorSlot m_allocators[2][kFramesInFlight + 1]` and
  `m_spareLists[2]` indexed by list type (allocators and lists are typed); `AcquireAllocator` and
  `OpenList` (`:1300-1383`) take the type; `CommandList` gets `D3D12_COMMAND_LIST_TYPE type`.
- `PrepareDraw`, `ClearRenderTargetView`, `ClearDepthStencilView`, `ResolveSubresource`,
  `Begin/End` (queries) return early with `LogOnce` while `m_recordCompute`. `OMSet*`, `RSSet*`,
  `IASet*` only store state (nothing reaches the list before a draw), so `set_Element`/`set_States`
  on a compute recording cost nothing and need no special casing in the engine.
- `EnsureShaderInputs` (`d12_context_state.cpp:537`) requires `NON_PIXEL_SHADER_RESOURCE` for CS
  inputs of a compute recording (`kStateShaderRead` otherwise), so every barrier the list records is
  between compute-legal states (`NON_PIXEL_SHADER_RESOURCE`, `UNORDERED_ACCESS`, `COPY_SOURCE`,
  `COPY_DEST`); the tracker `LogOnce`s and skips a transition that would involve a graphics state.
- `StateTracker::Local` gets `bool transitioned` (a barrier was pushed for it in this list), carried
  into `CommandList::Requirement::transitioned` by `Harvest` (`:858`).

**Job from the list** (`d12_async.cpp`, infra's file): for each requirement,
- `!transitioned` and `first` read-only -> **read-shared**: entry = `first | kStateShaderRead`
  (widened: the direct queue keeps reading it in pixel shaders while the job runs; the probe and GBV
  accept compute reads of a texture in `PIXEL|NON_PIXEL`), exit = entry;
- otherwise -> **exclusive**: entry = `first` (exact, the list's first barrier starts from it),
  exit = `last`.
With no compute queue the list is DIRECT and infra's fallback executes it in order at the fork.

**Lending** (refines infra's guard, `Resource::lentTo` + new `bool lentShared`):
- Fork: exclusive -> exact `RequireTracked(entry)`; read-shared -> non-exact (merges into the current
  read state, no barrier when already satisfied).
- Guard in `Context::Require/RequireAll` and in `ExecuteCommandList`'s requirement patch: an exclusive
  lent resource joins on any access (infra); a read-shared one joins only when the direct queue needs
  a state its current one does not satisfy (a write or a non-read state). The G-buffer terrain and
  the rain pass read `water_ripple0` while job A reads it, and the sun reads `$user$cloud_map`: no join.
- Joins inserted by the guard log once with the resource's debug name and count in `-dxstats`
  (`StatAutoJoins`); the frame stays correct.

### 3.4 Engine

`da_async.h/.cpp` (new, `xrRenderPC_R4`, both modules; the job calls are `#if defined(XR_D3D12)`):

```cpp
enum class DaAsyncPath : u8 { Pixel, ComputeInline, ComputeAsync };
DaAsyncPath da_async_path();          // this frame's path (3.2), set by da_async_frame_begin()
void da_async_frame_begin();          // top of CRender::Render
// One job's recording: ComputeAsync -> a pooled context (alloc_context) in BeginAsyncRecording;
// ComputeInline -> RCache. fork() finishes the list, makes the job, forks it, releases the context.
class DaAsyncBatch
{
public:
    DaAsyncBatch(pcstr job_name);
    CBackend& cmd();
    void fork();
};
void da_async_join();                 // head of phase_combine: joins this frame's last job
```

On `ComputeAsync` the batch takes `alloc_context()`, calls `xrd3d12::BeginAsyncRecording` on
`HW.get_context(id)` and `Invalidate()`s that context's `CBackend` (the layer cleared the deferred
context's D3D11 state at its last `FinishCommandList(FALSE)`); `fork()` is `FinishCommandList` ->
`AsyncJobFromCommandList` -> `ForkAsyncCompute` -> `release_context`. If `alloc_context()` finds no
free context the batch records on `RCache` (inline) and logs once.

`CRender::Render` (R4 builds keep only the `Pixel` branches):

```cpp
r_main.sync();                                   // :321
da_async_frame_begin();
const bool async = da_async_path() != DaAsyncPath::Pixel;
if (async)
{
    // Nothing below reads the frame's geometry: the march and the ripple steps start here and
    // overlap the G-buffer. The cloud map stays on the graphics queue (the sun reads it too).
    da_water_field_update();
    Target->phase_cloud_map();
    Target->phase_async_early();                 // jobs "clouds_march", "water_ripple"
}
...
r_rain.sync();                                   // :601
if (async)
    Target->phase_async_gtao();                  // job "gtao": the G-buffer is final here
if (!async)
{
    da_water_field_update();
    Target->phase_water_ripple();
}
Target->phase_visor_drops();
if (!async)
{
    Target->phase_cloud_map();
    dx11GpuTimerScope gpu_march(dx11GpuTimers::CloudsMarch);
    Target->phase_clouds_march();
}
```

`phase_combine` head: `da_async_join()` (bracketed by the `JoinWait` timer), then
`phase_water_ripple_publish()`; the AO block runs `phase_gtao(RCache)` only on the `Pixel` path.

New `CRenderTarget` members (`r4_rendertarget.h`):
- `phase_clouds_march_cs(CBackend&, bool history)`: `set_Element(s_clouds_march_cs)`,
  `da_cloud_set_camera(cmd)` and the four constants as today plus `da_cloud_target` (w, h, 1/w, 1/h of
  `rt_clouds[0]`), UAV `rt_clouds[0]`, `Compute(ceil(w/8), ceil(h/8))`, then (history) the
  `CopyResource` into `rt_clouds[1]` on the same context. The camera/frame statics advance only when
  `history` is set. `da_cloud_set_camera` and `da_cloud_draw_quad` take the `CBackend`.
- `phase_gtao_cs(CBackend&)`: element 0 (`gtao_render.cs`, UAV `rt_gtao`), element 1 (`gtao_filter.cs`,
  reads `$user$gtao_0`, UAV `rt_ssao_temp`), groups `ceil(render/8)`.
- `phase_water_ripple_cs(CBackend&)`: the accumulator, window, rain and constants exactly as
  `phase_water_ripple` (the CPU half is factored into `da_ripple_prepare()` shared by both); priming
  clears stay RTV clears on `RCache` (they precede the fork in queue order); per step the real-space
  pass is `da_water_ripple.cs` (element 0 reads `water_ripple0`, element 1 reads FFT b for steps 2-3),
  then the existing FFT/propagate dispatches through `da_ripple_compute(cmd, ...)`; **no** per-step
  copy. `m_ripple_publish = steps > 0`.
- `phase_water_ripple_publish()`: `CopyResource(water_ripple0, FFT b)` on `RCache` when
  `m_ripple_publish`. Readers of the new step are all in `phase_combine` (2.2), so the data flow is
  today's: G-buffer and rain read the previous step, combine reads the new one.
- `phase_async_early()` / `phase_async_gtao()`: one `DaAsyncBatch` per job (`clouds_march` when the
  cloud tier is >= 2, `water_ripple` when steps > 0, `gtao` when `s_gtao_cs` exists); no job, no fork.

Targets (`r2_rendertarget.cpp`, R5 only, `#if defined(XR_D3D12)`): `CRT::CreateUAV` on `rt_clouds[0]`
(`:880`), `rt_gtao` (`:816`), and in GTAO mode `rt_ssao_temp` is created R16F with `CreateUAV` before
the blur branch can create it (`:800` skipped in GTAO mode: the blur path never runs there). The FFT
scratch pair already has UAVs (`:344-345`). R4 targets are unchanged.

### 3.5 Resource hand-off

| Job (fork) | Resource | Access in the job | Entry (direct, at fork) | Exit (direct adopts at join) | Lent |
|---|---|---|---|---|---|
| clouds_march (A) | `$user$cloud_map` | SRV | its read state + `PIXEL|NON_PIXEL` | same | shared (sun reads it) |
| | `da_cloud_shape`, `da_cloud_detail` | SRV | same | same | shared |
| | `$user$clouds1` | SRV, then copy dest | `NON_PIXEL` (from last frame's `COPY_DEST`) | `COPY_DEST` | exclusive |
| | `$user$clouds0` | UAV, then copy source | `UNORDERED_ACCESS` (from `PIXEL|NON_PIXEL`) | `COPY_SOURCE` | exclusive |
| water_ripple (A) | `$user$water_ripple0` | SRV (step 1) | `PIXEL|NON_PIXEL` (from last publish's `COPY_DEST`) | same | shared (G-buffer, rain read it) |
| | `$user$water_field` | SRV | same | same | shared |
| | FFT a, FFT b | UAV / SRV ping-pong | `UNORDERED_ACCESS` | FFT a `NON_PIXEL`, FFT b `UNORDERED_ACCESS` | exclusive |
| gtao (B) | `$user$position` (`rt_Position`), `rt_Normal` | SRV | `PIXEL|NON_PIXEL` (from `RENDER_TARGET`) | same | shared (sun and lights read it) |
| | `$user$blue_noise` | SRV | same | same | shared |
| | `$user$gtao_0` | UAV, then SRV | `UNORDERED_ACCESS` | `NON_PIXEL` | exclusive |
| | `$user$ssao_temp` | UAV | `UNORDERED_ACCESS` | `UNORDERED_ACCESS` | exclusive |

Constant buffers are dynamic (upload memory, `GENERIC_READ`, never tracked); a DISCARD by the direct
queue renames, so the job keeps its version. Everything the job needs from the frame (descriptor
chunks, upload pages, dynamic versions, the allocator slot) is frame-tagged and covered by the frame
fence through infra's join-before-Present invariant.

### 3.6 Identical results

- One body per pass in a header, called by both stages with the **integer pixel**:
  `gtao_render_pixel(uint2)`, `gtao_filter_pixel(uint2)` (`gtao.h`), `da_clouds_march_pixel(uint2)`
  (`da_clouds_march.h`), `da_water_ripple_pixel(uint2)` (`da_water_ripple.h`). The pixel shaders
  pass `uint2(SV_Position.xy)`, the compute shaders `SV_DispatchThreadID.xy` (bounds-checked).
  Inside, `tc = (float2(pix) + 0.5) * inv_size` and the jitter hash's `pos2d = float2(pix) + 0.5`:
  the same float operations in both stages. `Sample` becomes `SampleLevel(..., 0)` (every input has
  one mip, so the value is the same).
- This is the one change R4 sees: today's `tc0` comes from the rasterizer's interpolation, which can
  differ from the exact value by an ulp. For the march and GTAO render that is an ulp of the view
  ray; for the GTAO filter the gather corner `floor(tc * size - 0.5) + 1` can round to the
  neighbouring corner. The new corner is `(float2(pix) + 1) * inv_size`, the value the formula
  intends ("texel-corner-snapped", `gtao_filter.ps` comment). No visible change; R4 frames are
  compared in 9.3.
- The pixel shaders keep their full input signatures (`TEXCOORD0`, `SV_Position`) so the D3D12
  linkage rule holds (`Check-ShaderLinkage.py`).
- **`r__async_verify`** (runtime command, R5): the next frame that has all three passes runs each
  twice from the same inputs and compares the outputs on the GPU (`da_async_compare.cs`: differing
  texel count + largest difference, `InterlockedAdd/Max` into a 2x1 R32_UINT texture, read back
  through a staging copy - a QA frame may stall). Stage **port**: pixel shader into the real targets,
  copied into snapshot textures, then the compute shader inline into the real targets, compare.
  Stage **schedule** (next frame): compute inline at the fork points (before the fork, into the real
  targets, snapshot), then the async jobs into the real targets, compare after the join. History copy
  and ripple publish run once, after the second run; the ripple is compared on its first step.
  Log: `* [async] verify port clouds0: 0 of 921600 texels differ, max 0` for `clouds0`, `gtao_0`,
  `ssao_temp`, `ripple`; the same with `schedule`. Snapshots (about 40 MB at 1440p Extreme) are
  created for the verify and released after it.

### 3.7 Timers and diagnostics

- `dx11GpuTimers`: new direct slots `Ripple` (the `Pixel` path's ripple steps, so both paths are
  comparable) and `JoinWait` (a stamp in the direct list before the join and one after it: the
  queue's wait). Compute-queue times come from infra's per-job stamps: `AsyncComputeMs(device,
  "clouds_march" | "water_ripple" | "gtao")`, converted with the compute queue's own
  `GetTimestampFrequency` (both queues stamp at 1 GHz here, probe); durations only, so the two
  queues' clocks never have to be correlated. A `cq_*` field reads 0 on a frame that did not fork
  that job (the layer keeps the last resolved time per name and never clears it).
- `r__gpu_log` line (`r2_R_render.cpp:732`) gains, through `core`'s field registry,
  ` ripple=%.3f join=%.3f cq_march=%.3f cq_ripple=%.3f cq_gtao=%.3f async=%d` (`async` 0 pixel,
  1 inline, 2 async; numeric so `analyze.ps1` reads it). On the async path `march` and `ao` of the
  direct slots read about 0 and `combine` loses the AO.
- `-dxstats`: forks, joins, guard joins per frame (infra's counters + `StatAutoJoins`).

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRenderDX12/d12_api.h` | `BeginAsyncRecording`, `AsyncJobFromCommandList` |
| `src/Layers/xrRenderDX12/d12_layer.h` | `Context::m_recordCompute`, typed allocator slots and spare lists, `CommandList::type`, `StateTracker::Local::transitioned`, `CommandList::Requirement::transitioned`, `Resource::lentShared`, `Device::StatAutoJoins` |
| `d12_context_ops.cpp` | typed `AcquireAllocator`/`OpenList`/`ReturnList`, `FinishCommandList` (type, clear the mode), harvest flag, refusals of clears/resolves/queries, guard refinement in `Require`/`RequireAll`/`ExecuteCommandList` |
| `d12_context_state.cpp` | `EnsureShaderInputs` state for compute recordings, `PrepareDraw` refusal |
| `d12_async.cpp` (infra's new file) | `BeginAsyncRecording`, `AsyncJobFromCommandList`, shared/exclusive fork entry |
| `src/Layers/xrRenderPC_R4/da_async.h/.cpp` (new), `sources.cmake` | paths, auto rule, `DaAsyncBatch`, join, verify state machine and compare |
| `src/Layers/xrRender_R2/r2_R_render.cpp` | `da_async_frame_begin`, fork A (`:321`), fork B (`:601`), `Pixel`-only blocks (`:606-623`), `r__gpu_log` fields |
| `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp` | join + ripple publish at the head (`:131`), GTAO only on `Pixel` (`:150`) |
| `r4_rendertarget_phase_clouds.cpp` | `phase_clouds_march_cs`, `CBackend&` helpers, `da_cloud_target`, CS blender |
| `r4_rendertarget_phase_gtao.cpp`, `src/Layers/xrRender/blenders/blender_gtao.cpp` | `phase_gtao_cs`; elements 2-3 (`r_ComputePass("gtao_render")`, `("gtao_filter")`) |
| `r4_rendertarget_phase_water_ripple.cpp` | `da_ripple_prepare`, `phase_water_ripple_cs`, `phase_water_ripple_publish`, `da_ripple_compute(CBackend&, ...)`, real-space CS blender (2 elements) |
| `r4_rendertarget.h` | the members above, `s_clouds_march_cs`, `s_gtao_cs`, `s_water_ripple_cs`, `s_async_compare`, `m_ripple_publish` |
| `src/Layers/xrRender_R2/r2_rendertarget.cpp` | R5 UAV flags (`:816`, `:820`, `:880`), GTAO-mode `rt_ssao_temp`, CS shaders created with the pixel ones |
| `src/Layers/xrRenderDX11/dx11GpuTimers.h/.cpp` | `Ripple`, `JoinWait` slots and names |
| `src/Layers/xrRender/xrRender_console.cpp` | `r__async_compute` (token, saved), `r__async_path` (runtime), `r__async_verify` (command); both modules; not in `_preset` or `xrRender_sync_preset_derived` |
| `packaging/.../shaders/r3/` | section 6 |
| `packaging/.../configs/dead_air_x64_video_options.ltx`, `configs/ui/ui_mm_opt.xml`, `ui_mm_opt_16.xml`, `scripts/ui_mm_opt_video.script`, `configs/text/rus|eng/dead_air_x64_video.xml` | section 7 |
| `tools/qa/d3d12layer/scenarios_async.cpp` (new), `scenarios_api.cpp` table | scenarios A-E (9.1) |
| `tools/qa/water/async_check.lua` (new) | stand scenario (9.3) |
| `docs/dead-air/DX12_RENDERER.md` | section 2 rows for the three console commands; section 4 a pointer here |

New engine files (`da_async.*`, `scenarios_async.cpp`) start with the Grizzly/MIT header; shaders
follow the shader folder's convention (no header).

## 5. Data and resources

- No new render targets. UAV flags on three R5 targets (render/2 RGBA16F, render G16R16F, render
  R16F); `ALLOW_UNORDERED_ACCESS` next to `ALLOW_RENDER_TARGET` is legal and costs no memory.
- Up to three jobs, two forks, one join a frame; one pooled context held for the few microseconds of
  each job's recording (released right after `FinishCommandList`).
- Verify only: snapshot textures of the four outputs (about 40 MB at 1440p Extreme, 1024^2 ripple),
  a 2x1 R32_UINT UAV texture and its staging copy; released after the verify.
- Settings: `r__async_compute` in user.ltx (default `st_async_auto`).

## 6. Shaders (`packaging/dead-air-x64/compatibility/gamedata/shaders/r3`, SM 5.0 HLSL; DXC on R5 per `dxc.md`)

| File | New/changed | Content |
|---|---|---|
| `gtao.h` | new | `gtao_view_pos`, `gtao_reference_normal`, `gtao_compute` (moved verbatim), `gtao_render_pixel(uint2)`, `gtao_filter_pixel(uint2)` |
| `gtao_render.ps`, `gtao_filter.ps` | changed | `#include "gtao.h"`, `main` calls the pixel function with `uint2(hpos.xy)` |
| `gtao_render.cs`, `gtao_filter.cs` | new | `[numthreads(8, 8, 1)]`, bounds check against `pos_decompression_params2.xy`, `RWTexture2D<float2> / <float>` at `u0` |
| `da_clouds_march.h` | new | the march body of `da_clouds_march.ps` as `da_clouds_march_pixel(uint2)`; `uniform float4 da_cloud_target`; the DirectX 12 pixel path's early-out (`da_cloud_early.x`, `s_cloud_depth`, outside `DA_COMPUTE`) |
| `da_clouds_march.ps` | changed | wrapper; `da_clouds_march.s` binds `$user$cloud_depth` and `smp_nofilter` for the early-out |
| `da_clouds_hist.h` | new | `uniform float4 da_cloud_early`; `da_clouds_fetch`: bilinear read of the march target that leaves early-out sentinels out, the plain `SampleLevel` when `da_cloud_early.y` = 0 (DirectX 11, MSAA) |
| `da_clouds.ps` | changed | the composite reads the march through `da_clouds_fetch` |
| `da_clouds_march.cs` | new | `[numthreads(8, 8, 1)]`, `RWTexture2D<float4>` at `u0` |
| `da_water_ripple.h` | new | the step body of `da_water_ripple.ps` as `da_water_ripple_pixel(uint2)` (`tc` from `da_ripple_sim.z` texels) |
| `da_water_ripple.ps` | changed | wrapper |
| `da_water_ripple.cs` | new | `[numthreads(16, 16, 1)]`, `RWTexture2D<float2>` at `u0` (scratch a) |
| `da_async_compare.cs` | new | two `Texture2D<float4>` compared by bits, count and largest difference into `RWTexture2D<uint>` |

The compute blenders bind by name like the ripple FFT blender (`r_dx11Texture`, `r_dx11Sampler`); the
march needs `smp_linear`/`smp_rtlinear` on the CS stage (`dx11SamplerStateCache.cpp:256` binds CS
samplers). `Test-Shaders.ps1` covers the new files; `r4_shaders.cpp` names them on R5.

## 7. UI and presets

- Not a quality setting (it changes no pixel) and not in any preset ladder; not in the RTX group.
  One row in `[group_screen]` of `dead_air_x64_video_options.ltx`, after `renderer`:
  `async_compute = list, r__async_compute`, caption `ui_mm_da_async_compute`.
- Nodes `list_async_compute` in `ui_mm_opt.xml` and `ui_mm_opt_16.xml` (copy of `list_framegen`,
  `list_length="3"`, `options_item entry="r__async_compute" group="mm_opt_video" depend="runtime"`):
  it applies at the next frame, no restart.
- Values: `st_async_off` "Выкл" / "Off", `st_async_auto` "Авто" / "Auto", `st_async_on` "Вкл" / "On".
- `ui_mm_opt_video.script` `InitStates` (`:92-127`): on `caps.api ~= 12` the row is greyed with
  `st_da_x64_video_hint_async_compute_dx11`; on `caps.acq ~= 1` with
  `st_da_x64_video_hint_async_compute_unsupported`; the rules block (`:198-206`) greys it while the
  renderer combo is not DirectX 12, as frame generation does.
- Strings (`dead_air_x64_video.xml`, cp1251 for rus): caption "Асинхронные вычисления" / "Async compute";
  hint "DirectX 12: облака, затенение GTAO и рябь воды считаются параллельно с тенями и G-буфером.
  Картинка та же, кадр короче. «Авто» включает там, где видеокарта от этого выигрывает." / "DirectX 12:
  clouds, GTAO shading and water ripples are computed alongside the shadows and the G-buffer. Same
  image, shorter frame. Auto turns it on where the graphics card gains from it."; DX11 hint "Только
  для DirectX 12." / "DirectX 12 only."; unsupported "Видеокарта или драйвер не дают отдельную
  очередь вычислений." / "The graphics card or driver has no separate compute queue.".
  `tools/qa/Check-OptionHints.py` must pass. Open since the Perf phase: the clouds are no job any more,
  the hint still names them.

## 8. Fallback

| Situation | Behaviour |
|---|---|
| DirectX 11 (R4) | `Pixel` path, today's order; the row is shown greyed; the saved value is kept for DX12 |
| No compute queue, `-dx12_no_async` (`acq=0`) | `Pixel` path for on and auto; `r__async_path 3` runs the jobs in order on the direct queue (infra fallback) for tests |
| Adapter not recommended, auto | `Pixel` |
| Missing UAV (format support) or a compute shader that failed to compile | that pass uses its pixel shader, log once; the others stay async |
| GTAO off (Low/Medium/Default presets, MSAA), cloud tier < 2, no ripple step this frame | the job is not recorded; with no job there is no fork |
| No free pooled context | the batch runs inline on `RCache`, log once |
| A recorded job that cannot be made (`FinishCommandList` / `AsyncJobFromCommandList` fails) | log once; the march drops its history and runs as the pixel pass at fork B, GTAO runs in the AO slot, the ripple skips the step (no publish, window centre put back) |
| A direct-queue access of an exclusive lent resource (future code) | the guard joins there, log once with the name; correct frame, less overlap |
| A fork without a join (early return, exception) | joined at `Present1` (infra) |

## 9. Test plan

### 9.1 Layer tests (`tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance`, then `-Gbv`)

The reference D3D11 device runs the same work with `ExecuteCommandList`; results must be equal and
the debug layer (and GBV) silent. Pass: all existing 36 scenarios + infra's + these:

- **A** `async D3D11 recording`: a deferred context in async recording runs a CS reading texture S
  (which the immediate reads with a PS between fork and join) and writing UAV T; join; a PS reads T.
- **B** `async recording refusals`: a draw and an RTV clear on a compute recording are dropped with
  one log line each; the list still executes; results equal the reference without them.
- **C** `read-shared lending`: an immediate PS read of S during the job inserts no join (layer counter
  0); an immediate render into S does (counter 1); both results equal the reference.
- **D** `compute-list hand-off`: UAV -> SRV -> `COPY_SOURCE` and SRV -> `COPY_DEST` inside one job
  (the march's history), exit states adopted, next frame's exact entry, two frames equal.
- **E** `no compute queue`: A with the device created without the compute queue: same results.

### 9.2 Build and static checks

`. locked.ps1; Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` (0 errors, no new warnings);
`tools\qa\Test-Shaders.ps1 -Shader <each new or changed file>` (every permutation compiles);
`python tools\qa\Check-ShaderLinkage.py` (no new mismatch); `python tools\qa\Check-OptionHints.py`.

### 9.3 Stand (`D:\Games\Dead Air\_qa\lightdiag`, hidden desktop, save `qa_pedro`)

New scenario `tools\qa\water\async_check.lua`: after 150 frames sets `qa_water_weather`
(`level.set_weather(..., true)`, as `rain.lua`), waits 6 s, prints caps and `r__async_compute`, runs
`r__async_verify`, waits 3 s; then `r__async_compute st_async_off`, 2 s, `r__capture 1 2`, FPS over
10 s (`[qa] async off FPS x`); the same with `st_async_on` (`[qa] async on FPS y`);
`DA_WATER_PROBE_DONE`. Storm weather gives the march clouds and the ripple rain drops.

| # | Command (under `Enter-Lock 'stand'`; `Invoke-Stand` where it fits) | Pass |
|---|---|---|
| 1 | `tools\qa\Run-WaterQa.ps1 -Name async-verify -Scenario async_check -Save qa_pedro -Weather af3_bright_storm -ExtraArgs '-r5 -always_active -dxstats' -PreCommands 'r__gpu_log 300' -KeepShaderCache -Out <out>` | `[async] verify port` 0 differing texels for `clouds0`, `gtao_0`, `ssao_temp`, `ripple` (tolerated if the driver contracts differently: <= 0.01% and <= 2^-10 relative, noted); `[async] verify schedule` exactly 0 for all four; `-dxstats` 3 jobs a frame (2 in frames without a ripple step) at 2 fork points, 1 join, 0 guard joins; no "not joined" / "lent" line; `[gpu]` lines show `async=on`, `march`/`gtao` compute times > 0, `join` < 0.3 ms |
| 2 | 1 with `-ExtraArgs '-r5 -always_active -dxdebug -dx12gbv'`, `-Name async-gbv`, `-Seconds 200`, `-PreCommands 'vid_mode 1280x720 (60Hz)','r__gpu_log 300'` (GBV is slow) | no `D3D12 ERROR`/`CORRUPTION` line in the log; verify as in 1; the capture renders |
| 3 | `Invoke-Stand -Name async-r4 -Scenario async_check -ExtraArgs '-always_active'`, plus `Invoke-Stand -Name async-r4-base -Scenario render_check -ExtraArgs '-always_active'` with the previous DLL (`-Dll`) and the same with the new one | caps `api=11`, `r__async_compute` accepted and inert, verify prints "DirectX 12 only"; the two `render_check` captures side by side show no difference (sky deck, AO in corners, ripples) |
| 4 | `Invoke-Stand -Name async-upscale -Scenario upscale_check -ExtraArgs '-r5 -always_active' -PreCommands 'r__async_compute st_async_on'` | every step as in its last baseline run (FSR/DLSS/FG steps run, frames clean, no new log errors) |
| 5 | `Invoke-Stand -Name async-motion -Scenario motion_check -ExtraArgs '-r5 -always_active' -PreCommands 'r__async_compute st_async_on' -Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__motion_vectors 1' }"` | no AO lag or cloud smear under the turn compared with the same run at `st_async_off` (an AO from the previous frame would trail edges) |
| 6 | `Invoke-Stand -Name async-fallback -Scenario async_check -ExtraArgs '-r5 -always_active -dx12_no_async' -PreCommands 'r__async_path 3'` | caps `acq=0`; jobs in order; verify port and schedule 0 differences |
| 7 | `tools\qa\Run-WaterQa.ps1 -Name async-options-r5 -Scenario options_tab -Save qa_pedro -KeepShaderCache -ExtraArgs '-r5 -always_active'`, then the same without `-r5` (`async-options-r4`) | the walk passes (every Video row has a hint); the row sits under the renderer, three values, greyed on DX11 with the DX11 hint; switching it in the menu applies without a restart (the next `[gpu]` line shows the new `async=`) |

### 9.4 Performance (ninja.lock held for the whole A/B; 2560x1440 Extreme, TAA)

The setting is a runtime toggle, so one DLL, alternating runs (`<scratchpad>` = the session
scratchpad that holds `locked.ps1`, `ab_run.ps1`, `analyze.ps1`; results land in
`_work\dx12-perf-20260927\qa`):

```powershell
. <scratchpad>\locked.ps1
$n = Enter-Lock 'ninja'
try {
    $base = @('vid_mode 2560x1440 (60Hz)', 'rs_v_sync off', 'r__aa st_aa_taa', 'r__fsr st_fsr_off', 'r__dlss st_dlss_off', 'r__framegen st_fg_off', 'r__gpu_log 300')
    foreach ($save in 'qa_pedro', 'foliage') { foreach ($i in 1..3) {
        & <scratchpad>\ab_run.ps1 -Save $save -Prefix "async-$save" -Runs "r5off$i" -Pre ($base + 'r__async_compute st_async_off')
        & <scratchpad>\ab_run.ps1 -Save $save -Prefix "async-$save" -Runs "r5on$i"  -Pre ($base + 'r__async_compute st_async_on') } }
} finally { Exit-Lock $n }
& <scratchpad>\analyze.ps1 -Names (foreach ($save in 'qa_pedro', 'foliage') { 1..3 | % { "async-$save-r5off$_"; "async-$save-r5on$_" } })
```

Pass: mean `frame` on `qa_pedro` at least **0.8 ms** lower with async on (FPS at least +5%), on
`foliage` at least 0.5 ms lower; busy clocks of on/off within 3% (else the gain is reported
normalised by clock); `join` <= 0.2 ms mean; the compute times `march`/`gtao` within 1.5x of their
direct-queue times at off (contention, not a broken queue). A second A/B at 1920x1080 High (GTAO on,
cloud tier 2): on is not slower than off by more than 0.1 ms. An R4 A/B (old vs new DLL, `-Dll`)
shows `ao`, `march` and `clouds` within +-0.05 ms (the shared-body change costs nothing; the C14
early-out and the sentinel gather are DirectX 12 only, F1), FPS +-1 % (PLAN.md 6.4).

## 10. Performance expectation

- Work moved off the direct queue on `qa_pedro` 1440p Extreme: march 1.9 ms + GTAO 1.57 ms + ripple
  about 0.1-0.2 ms on average (0.3-0.5 ms in a frame with a step).
- Windows: job A (march + ripple, about 2.0 ms; 2.4-3.4 ms stretched by contention, 1.2-1.7x) in
  the G-buffer window (scene 3.0 ms + occlusion, wallmarks, rain about 0.4 ms); job B (GTAO; 2.1-2.7
  ms stretched) in sun + emissive + lights (about 5.1 ms). One compute queue runs them in order, so
  the real bound is A + B = 4.5-6.1 ms against 8.5 ms of direct work from F_A to J: the join wait
  should be near 0 even when A spills past F_B.
- Hidden share from the probe: march behind the G-buffer 51-52%, but the real G-buffer carries
  alpha-tested grass and trees (probe: 39%); GTAO behind cascades + accumulation 43%, 26% where the
  casters are alpha-tested. The real cascades carry a large share of alpha-tested vegetation, so the
  estimate takes the lower half of each range.
- Expected saving: march 0.7-1.0 ms, GTAO 0.4-0.7 ms, ripple about 0.1 ms: **1.2-1.8 ms of 16.2 ms,
  frame 14.4-15.0 ms, 62 -> 66-69 FPS (+8-12%)** on the RTX 3070 Laptop, less where the laptop's
  power limit pulls the clock down under the denser load (visible in the A/B clocks). The pass
  criterion (0.8 ms) is set below the expected range on purpose.
- Lower presets gain less (GTAO off below High, cloud tier 2 on High, ripple 512^2): High about
  0.8-1.2 ms. CPU: 60-100 us more at `Present` (the split submission) and a few microseconds of
  recording; no effect on CPU-bound frames.

## 11. Risks

- **Infra dependency**: nothing here works before infra step 8 (queue, fences, jobs, markers, Present
  join, guard). This feature's layer part is an extension of that code, built in the same files.
- **Driver overlap is not guaranteed**: some adapters serialize the compute queue (probe numbers are
  NVIDIA Ampere). Mitigated by the auto rule, the join timer, the A/B; the setting can be turned off.
- **Power-limited laptops**: denser load lowers the boost; the A/B reports clocks, the probe already
  ran at the power limit and still gained.
- **State sharing across queues**: read-shared textures stay in `PIXEL|NON_PIXEL` while the compute
  queue reads them (probe with GBV silent; infra's rule). Graphics states never appear in a job's
  barriers (compute recording uses `NON_PIXEL` and refuses graphics transitions).
- **Silent reads of an old AO/clouds frame** if a join were missed: impossible by construction (the
  consumer comes after the join in queue order), caught by verify stage schedule and by scenario 5.
- **Shared-body change on DX11**: ulp-level differences and the GTAO filter corner; checked by 9.3/3
  and the R4 GPU timers.
- **Pooled context exhaustion**: at both fork points 4+ of 8 contexts are free; fallback inline.
- **Future passes touching lent resources**: the guard joins and logs; never corruption.
- **Ripple publish moved to the combine head**: only valid while no pass between the fork and the
  combine reads the new step; the readers are listed in 2.2 and a new reader would read the old step
  (as the G-buffer does today), never garbage.

## 12. Implementation order

1. Timers first, measured on today's code: `Ripple` and `JoinWait` slots, `r__gpu_log` fields;
   `render_check` on R5 and R4 to get the ripple's real cost.
2. Shared bodies: `gtao.h`, `da_clouds_march.h`, `da_water_ripple.h`, wrappers in the three pixel
   shaders, `da_cloud_target` set by the pixel path; `Test-Shaders.ps1`, linkage check; R4 and R5
   `render_check` shots against the baseline.
3. Compute shaders and blenders (`gtao_render.cs`, `gtao_filter.cs`, `da_clouds_march.cs`,
   `da_water_ripple.cs`, `da_async_compare.cs`), R5 UAV flags and the GTAO-mode `rt_ssao_temp`,
   `phase_*_cs`, `da_ripple_prepare`, `phase_water_ripple_publish`, `CBackend&` helpers.
4. `da_async.*` with the `Pixel` and `ComputeInline` paths, the reordered `CRender::Render`, the join
   point (a no-op inline), console commands; `r__async_verify` stage port. Stand run 1 with
   `r__async_path 2`: port differences 0.
5. Layer (built by the `infra` lane in I5, PLAN C12): compute recording mode (typed allocators/lists,
   refusals, `NON_PIXEL` inputs, `transitioned`), `AsyncJobFromCommandList`, read-shared lending and
   the guard refinement, `StatAutoJoins`; layer scenarios A-E with and without GBV.
6. `ComputeAsync` path in `DaAsyncBatch` and the join; verify stage schedule; stand runs 1, 2, 6.
7. Auto rule, `r__async_compute` row, strings, script; stand runs 3, 4, 5, 7.
8. Performance A/B (9.4), including async `on` against `off` with the cloud-march early-out (PLAN
   C14; the march leaves job A if it loses); if `join` > 0.2 ms, split the join per consumer (the compute queue runs
   in order, so a job's join implies the earlier ones): `clouds_march` right before
   `phase_clouds_composite` (`phase_combine:201`), `gtao` + ripple publish right before `combine_1`
   (`:238`), and measure again.
9. DX12_RENDERER.md rows; final full layer suite and stand matrix.
