# DirectX 12 features: integration plan

Status: binding plan for the autonomous implementation of the eight designs in this folder
(28.09.2026). Inputs: `infra.md`, `dxc.md`, `async.md`, `vrs.md`, `gpudriven.md`, `mesh.md`,
`dstorage.md`, `dxr.md`. Where a feature doc and this plan disagree, **this plan wins**; every
feature doc carries an "Integration amendments" section that repeats what changed for it and points
here by contradiction id (`C<n>`, section 1).

Scope is everything in the eight designs, including the phases the designs call "later": ray traced
dynamic and skinned objects (`dxr.md` phases 4-5), the GPU-driven main view (`gpudriven.md` phase 3,
design first), the DirectStorage geometry path (`dstorage.md` 3.8), both Hi-Z variants, the DXC
payoff edits, the infra copy queue. Things with a measured "not worth it" verdict stay rejected and
are listed in 7.3 with the reason; nothing is parked "for later".

## 0. Rules for every lane

### 0.1 Project rules (repeated so a lane agent needs nothing else)

- Nobody asks the user anything or waits for a permission: decide within these rules and write the
  decision and its reason into the lane status file (0.7) and the feature doc.
- No commits, no git worktrees, no `git checkout/stash/reset/clean`, no reformatting of whole files.
  The tree carries other uncommitted work (1.5.0, options pages): never revert or touch a file
  outside your lane's ownership (2.2).
- Every **new** engine source file starts with `// Copyright (c) 2026 Grizzly` and
  `// Released under the MIT licence, see License.txt`. Not in ported files, build files, `.def`
  files or modified upstream files. Shaders follow the shader folder's convention (no header).
- Code comments in English, brief, only where the code is not self-explanatory. Language sugar where
  it costs nothing. Raw pointers are tested by implicit bool conversion, not against `nullptr`.
- Never hardcode the names of other mods: eligibility comes from compiled signatures, shader flags,
  data files.
- Graphics features that change the look go through the quality presets
  (`xrRender_sync_preset_derived`), never behind a default-off console knob. **Exception: ray
  tracing**, which lives in its own **"RTX"** category of Options -> Video and never in a preset,
  `rspec_*.ltx` or `xrRender_sync_preset_derived`. Rows that the GPU/API cannot run are shown but
  greyed with the reason, like frame generation. Console commands are only parameters of live
  features or QA diagnostics. A feature that fails its gate is **removed** from the tree (with the
  measured numbers written into its doc), not left behind a switch.
- Plant shadows sway in every shadow technique: wind stays in the vertex/mesh shaders of every shadow
  pass; vegetation is never in a shadow ray's instance mask and never frozen into a cached map.
- DirectX 11 (`xrRender_R4.dll`) stays intact: its frames equal the Gate-0 baseline (0.6) and its
  FPS stays within +-1 %. No DX12 feature takes a DX11 exception: C14's cloud-march early-out is
  DirectX 12 only for this reason (8.3, F1).
- `xrCore.def` / `xrSound.def` export by ordinal; a new export takes the next free ordinal (1519 is
  free today; `dstorage.md` owns it).
- Never touch `D:\Games\OriginDeadAir\Dead Air Copy`. Never deploy to the live game
  `D:\Games\Dead Air` (the lead does, from the packaging lists this work completes). Kill only your
  own engine processes (`Get-Process xrEngine | ? Path -like '*_qa*'` and your pid file).
- Stand runs are short (60-90 s after load, FPS windows 10-15 s). Do not clear the shader cache
  without a reason (the engine cache is validated by source CRC).

### 0.1b Process: implementation first, all tests at the end (the user's rule, 28.09 ~15:30)

"First everything is written, tests only at the very end when all the work is done - only this way."
From now on the lanes only WRITE the remaining code of their milestones: they build (compile) under
`ninja.lock` to keep the tree compiling, and may compile shaders offline (`Test-Shaders.ps1`, dxc/fxc
on single files), and nothing else - no stand runs, no layer-test runs, no perf batches, no game
launches, no parity. Every milestone gate below is run afterwards, once, in the final test phase
(section 6 plus the lanes' gates), within the budget of 0.1a; decisions that need a measurement
(keep/remove, ladders, calibration) are made there. Code is written so that each feature can be
toggled at runtime for those A/Bs.

### 0.1a Test budget (the user's rule, overrides every gate below)

The user does not accept long test cycles. Every gate is met with the smallest set of runs that
proves it:
- one stand run = at most ~90 s after the level loads (`qa_fps_ms` <= 10000, shots right away);
- parity is judged against the existing Gate-0 references (a, b, c): never re-run references, and a
  control run only once, only to attribute a comparison that failed;
- per milestone at most 4 stand runs (R5 on `qa_pedro` and `foliage`; `trees`/`fogqa`/others only
  when the milestone touches exactly that content; one R4 run only when shared engine code changed);
- several checks go into one run's steps where the scenario allows, instead of one run per check;
- performance only at the lane's perf milestone: one `Invoke-Dx12Perf` batch, one save, `-Pairs 3`;
- no background batches of many runs, no waiting loops longer than 5 minutes.

**Final Perf phase cap (the lead, 28.09 20:15; overrides the Perf prompt's batch list and `-Pairs 3`).**
The work has run for a day; the Perf phase gets about one hour of stand time in total.
- `-Pairs 2` per batch; a third pair only for a batch whose two pairs disagree in sign or whose mean
  difference lies within +-1.5 %.
- Batches, qa_pedro unless noted: async auto vs off; VRS conservative vs off and aggressive vs off (they
  decide `vrs_by_preset`); `r__gpu_driven` 1 vs 0 as shipped (Hi-Z and main view at their defaults, no
  sub-switch batches); `r__grass_mesh` 1 vs 0 on foliage; DXC vs FXC (`-dx12_no_sm6`); RT High vs off
  (+ VRAM peak), the Low and Medium tier costs read from the `rt_*` fields of one single run each, no pair;
  `texture_load` DirectStorage on vs off, 2 pairs, the copy-queue switch decided in the same batch.
- Not measured, decided by the code and the docs as written: infra zero-cost masks, the `-dxc_plain`
  payoff, the M10 grass radius/density columns, C14 march placement.
- After the decisions: one rebuild, and one parity run only if a removal changed frames.

**Applied, then cut shorter by the user.** The async and VRS batches above ran (`perf-async-pedro-*`,
`perf-vrs-pedro-*`, `perf-vrsladder-pedro-*`); then the user asked for one measurement pass instead of
on/off pairs. The remaining batches (GD, mesh grass, DXC vs FXC, RT tiers and VRAM, DirectStorage and
the copy queue) were not run: their costs are read from the per-feature GPU timers of one run with
everything on (`final-onepass-foliage`) and the stand logs already taken. The optional variants (GD phases 2
and 3, the grass Hi-Z and depth-only shadows, the copy queue) were then measured together in one pass against it
(`final-onepass-variants-foliage`). Outcome per lane: 8.4.

### 0.2 Shared tree protocol

- One working tree `D:\Games\Dead Air\DeadAir-x64`, one build dir `build\ninja-x64` (Release
  output `bin\x64\Release`), one stand `D:\Games\Dead Air\_qa\lightdiag`.
- Every file has exactly one owner lane (2.2, 2.3). A lane edits only files it owns, plus:
  - **marker blocks** that the owner pre-created for it in a hotspot file:
    `// [dx12:<lane>] begin <name>` ... `// [dx12:<lane>] end <name>` (C++), `; [dx12:<lane>]`
    (ltx), `<!-- [dx12:<lane>] -->` (xml), `-- [dx12:<lane>]` (Lua), `# [dx12:<lane>]` (CMake,
    Python, PowerShell). Edit strictly between your own markers;
  - **hook lines**: when a needed call site was not pre-placed, a lane may insert **one new line**
    (a call to a function in its own file, tagged `// [dx12:<lane>] hook`) into a hotspot file. It
    never changes or moves another line, and lists the insertion in its status file.
- Read a hotspot file immediately before editing it; if an Edit fails because the file changed, read
  it again and redo the edit. Never write a hotspot file with Write.
- Lane state lives behind one pointer per lane in shared classes (`CRenderTarget::m_da_async`,
  `m_da_vrs`, `m_da_rt`; `xrd3d12::Context::vrs`, `::mesh`; `Device::vrs`, `::mesh`, `::texheap`),
  pre-declared by the owner; the struct is defined in the lane's own file. Shared headers therefore
  change once (at the skeleton milestones) and not with every lane edit, which also keeps rebuilds
  short.
- Lane tags: `infra`, `dxc`, `core`, `async`, `vrs`, `gd`, `mesh`, `ds`, `rt`.

### 0.3 Build

- Only through `locked.ps1` (`Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'`, add `xrCore` when it
  changed), which holds `ninja.lock`. Layer tests build with `tools\qa\d3d12layer\Build-LayerTests.ps1`
  (cl.exe, no ninja lock) and run with `Run-LayerTests.ps1 -HighPerformance` (and `-Gbv`, `-Dxil`).
- The tree must compile after every edit burst of a lane: write a coherent change, build at once. A
  lane that breaks the build in its own files fixes or backs out its change within 10 minutes.
- A build error in another lane's file: retry every 3 minutes for 15 minutes; then write the error
  into that lane's inbox (0.7) and continue with work that needs no build. `core` may fix a trivial
  compile error (missing include, typo) in any file after 30 minutes of breakage and notes it in the
  owner's inbox.
- No new warnings in touched files.

### 0.4 Stand

- Every stand run goes through `tools\qa\dx12\Invoke-Dx12Stand.ps1` (created by `core` at K0): it
  takes `ninja.lock`, copies `bin\x64\Release` (`xrRender_R4/R5.dll+pdb`, `xrCore.dll+pdb`, the four
  new runtime DLLs) into a per-run staging folder and releases `ninja.lock`; then it takes
  `stand.lock`, copies the staged binaries and the overlay (`shaders`, top-level `configs\*.ltx`,
  `configs\ui`, `configs\text\rus|eng`, the option-page scripts) into the rig, records the SHA-256
  of every copied binary in the run folder, runs `tools\qa\Run-WaterQa.ps1` and releases the lock.
  `-Bin <dir> -Overlay <dir>` run a snapshot instead (Gate-0 baseline); `-NinjaHeld` skips the
  ninja step for callers that already hold it (the perf helper). Every run re-copies the runtime
  DLLs, so a fallback test that renamed one in the rig is undone by the next run.
- **Lock order**: `ninja.lock` before `stand.lock`, always. Nobody waits for `ninja.lock` while
  holding `stand.lock` (the stand helper never holds both; the perf helper holds `ninja.lock` and
  then takes `stand.lock` per run), so the two cannot deadlock.
- Runs land in `D:\Games\Dead Air\_work\dx12-features-20260928\qa\<run>`; run names
  `<lane>-<milestone>-<what>[-r4]` (`vrs-v4-sunlight`, `gd-g4-parity-foliage`).
- Default arguments: `-ExtraArgs '-r5 -always_active'` (R5), `'-always_active'` (R4); add
  `-ss_tga` whenever frames are compared.

### 0.5 Performance

- Only `tools\qa\dx12\Invoke-Dx12Perf.ps1` (the in-tree port of the scratchpad `ab_run.ps1` +
  `analyze.ps1`, created at K0). It holds `ninja.lock` for the whole batch (no compile shares the CPU,
  and `bin` cannot change mid-batch), stages `bin` once, runs interleaved variants `a,b,a,b,a,b`
  through `Invoke-Dx12Stand -NinjaHeld` with the first pair as warm-up, samples nvidia-smi
  clocks/power and VRAM, and prints the `analyze` table. Pairs count only when busy GPU clocks agree
  within 3 %.
- Protocol: 2560x1440, `_preset Extreme`, `r__aa st_aa_taa`, `rs_v_sync off`, upscalers and frame
  generation off unless the gate names them, `r__gpu_log 300`, `qa_fps_ms = 12000`. The stand's
  presents are capped (~75 FPS, no vblank) and absolute numbers are not the user's: only relative A/B
  inside one batch is valid.
- A lane's gate is measured **with its own runtime toggle inside one DLL** (`r__async_compute`,
  `r__vrs`, `r__gpu_driven`, `r__grass_mesh`, `r__dstorage`, `r__rt_*`, `-dx12_no_sm6`), so other
  lanes' changes in the shared tree do not confound it. "New vs Gate 0" comparisons happen only at the
  final gate.
- At most one perf batch per lane milestone, at most 8 runs per batch (a batch blocks every build).

### 0.6 Frame parity methodology

- Parity frames are lossless: `-ss_tga` plus one frame of `r__screenshot_every 1` (TGA);
  `r__capture` sequences (JPEG) are for motion review only.
- Determinism: `wind_freeze 1` where the scenario allows, fixed weather (`-Weather`), same save, same
  frame index after load. `render_check` / `upscale_check` with `qa_tga = true` (`Invoke-Dx12Stand -Tga`)
  do all of it: game time stops and the weather clock is pinned on the first frame, the weather is
  forced, the HUD weapon is hidden, `wind_seed` restarts the wind service (every accumulator, 8.1)
  before `wind_freeze`. What still moves (NPCs, particles, water, fire light, volumetric fog, sub-level
  foliage noise) is handled by a **noise floor**:
  every parity comparison has reference runs (two or more runs of the reference config); a pixel counts as
  different only where `|new - ref| > max(2/255, noise(ref_a, ref_b))` after a 2-pixel dilation of the
  moving mask. `tools\qa\Compare-Frames.py` (created at K0; also carries `vrs.md`'s blockiness index
  and heat map) implements it and prints PSNR, max, p99.9, worst 16x16 tile mean, blockiness, and a
  PASS/FAIL against the thresholds passed on the command line.
- Default thresholds: "equal" = 99.9 % of static pixels within 2/255 and no cluster > 16 px beyond
  the floor; "PSNR >= N dB" where a doc names N. With the three Gate-0 references (`a`, `b`, `c`)
  "equal" is calibrated leave-one-out against the references' own spread (8.1).
- **Control run** (added at K2): the references were taken on a quiet machine; with eight lanes
  compiling, loads and frame times stretch and what runs on the device clock (lamp flicker, fire,
  particles, water) lands differently - the Gate-0 snapshot itself then fails "equal" against its own
  references (`core-k2-ctl-pedro-r4`: within 97.7 %, 128 clusters). A parity gate therefore takes one
  **control** of the same save/API from the snapshot at about the same time (`Invoke-Dx12Stand -Bin
  <baseline>\bin -Overlay <baseline>\gamedata -ExtraModules xrEngine`, same globals) and passes it as a
  fourth `--ref`; "equal" stays calibrated, so a real change (a 12x12 px patch, +3 % brightness) still
  fails. `Invoke-Dx12Stand -Tga` also passes each reference save's weather-clock pin (`qa_env_pin`):
  a slow load carried `trees` across 9:10 before frame 1 and pinned a different sky.

### 0.7 Status, inbox, handoff

- `D:\Games\Dead Air\_work\dx12-features-20260928\lanes\<lane>.md`: current milestone, gates
  passed (run names), hook lines inserted into hotspot files, decisions taken, open blockers.
  `<lane>.inbox.md`: messages from other lanes (build breaks, requests). Each lane reads its inbox at
  every milestone.
- A milestone is **reached** when its gate (section 3/4) passed and the status file says so; lanes
  that depend on it (3.4) start the dependent milestone only then.
- Lane **done**: every milestone reached; lane doc status line "implemented", measured numbers in
  it; no TODO/FIXME/debug output left in owned files; review findings (6.3) fixed.

### 0.8 Deciding without the user

Order of precedence: the user's words (RTX separate, not in presets; everything, no leftovers, no
questions) -> project rules 0.1 -> this plan -> the feature doc -> the lane's judgement, written down.
When a measured gate fails, the feature doc's decision rule applies; if it has none, the lane tunes
within its design, and if the gate still fails the feature is removed (0.1) and `core` records it in
section 8 of this file.

## 1. Contradictions, gaps and their resolution

All resolutions below are applied to the feature docs ("Integration amendments" section at the top of
each, plus inline fixes of the statements they replace).

| Id | Docs | Conflict or gap | Resolution |
|---|---|---|---|
| C1 | dxr / infra, dxc, mesh | `dxr.md` vendors DXC 1.8.2502 (~20 MB), the others 1.9.2607 | One copy: DXC **1.9.2607** in `Externals/dxc` (bin + licences + README with hashes; headers from the Windows SDK 10.0.26100, no `inc/`), vendored by `core` at K1 |
| C2 | infra 3.6, dxr 3.2, dxc 3.1, mesh 3.6 | Three compiler paths: infra's `d12_dxc.cpp` + `da_shader_dxc.cpp` + `//grizzly:dxil` opt-in; dxr's own `da_dxc` loader and `r5/rt` cache; dxc.md's single service | `dxc.md` model everywhere: layer service `d12_dxc.cpp` (implemented by `infra` to `dxc.md` 3.1: absolute-path load, self-check PSO, SEH-guarded `CompileShader`, `Reflect`, `Disassemble`, `IsDxil`), engine glue `da_dxc.h/.cpp` (`dxc` lane). `da_shader_dxc.cpp`, `//grizzly:dxil`, `#dxil` are dropped. Native passes, RT and the self-test compile through `da_dxc_compile_native` (HLSL 2021, DXC preprocessor, FS include handler, cache `shaders_cache_grizzly/r5_dxil/<tag>/native/<path>.<entry>.<target>/<defines hash>`) |
| C3 | infra 3.6 / dxc 3.4 | Reflection through an `ID3D11ShaderReflection` adapter vs `ID3D12ShaderReflection` aliases | `dxc.md` wins: `Reflect(bytecode, size, REFIID, void**)` returns `ID3D12ShaderReflection` for DXBC and DXIL; R5 aliases the engine's reflection types (`CommonTypes.h`); no adapter object |
| C4 | gpudriven 6, async 6, vrs 6 | New engine-path shaders declared "FXC SM 5.0 only" / "cs_5_0 like every engine shader", while R5 compiles every engine shader with DXC | New engine-path shaders are written as SM 5.0-compatible HLSL (FXC for R4 and the FXC fallback, DXC on R5) and are created through `CResourceManager`, so their format follows the session's mode; graphics passes are unified by `UnifyShaderFormats`, compute pipelines stand alone. The GD VS variants pair with DXIL pixel shaders on R5 |
| C5 | mesh 3.6-3.7 / dxc 3.6 | Mesh grass names its PS `#dxil` (forced DXIL) while dxc.md makes every R5 engine shader DXIL and unifies mixed passes to DXBC twins (impossible for MS/AS) | `#dxil` dropped. `mesh_ready` additionally requires `dxc=1` (engine shaders are DXIL this session). A pass with a mesh or amplification stage is exempt from `UnifyShaderFormats`; if its PS fell back to FXC the mesh element is unavailable and the session uses the vertex path (one log line, verdict stored). `.ms` and `.as` go through the engine compile path (FXC preprocessing, engine includes) with `-HV 2021` (new-only extensions); the shared `da_detail_common.h` compiles under FXC, HLSL 2018 and 2021 |
| C6 | mesh 3.6 / dxc 3.4 | Constant-buffer slot from the reflection index vs `BindPoint` | `dxc` lane: `R_constant_table::parse` takes the slot from `BindPoint` for DXIL and asserts index == `BindPoint` for DXBC (debug log once when not); covers every DXIL engine shader |
| C7 | dxr 4.1, 7, 8 / infra 3.1 | `rthw` vs `rt_tier`; `-no_rt` vs `-dx12_no_rt`; who sets `rt` | Keys: `rt_tier` (device tier on R5, adapter tier from `render-caps-d3d12.cache` on R4; `rthw` dropped, the menu tests `rt_tier >= 11`), `rt` = RT usable this session (tier >= 11, DXC service up, RT device init ok, no MSAA, no `-dx12_no_rt`), reported by the `rt` lane's query function. One switch `-dx12_no_rt` (masks the cap, so `rt_tier=0`); `-no_rt` dropped |
| C8 | dxr 3.2, 5 / infra 3.4 | RT's own shader-visible heap (32768), own upload ring, own deferred release | Only one CBV/SRV/UAV heap can be bound: RT uses infra's persistent bindless range (`AllocateBindless/WriteBindless/WriteBindlessBuffer`), `AllocateDescriptors` for per-pass tables, `AllocateUpload` for constants/instance descs, `ReleaseNative` for lifetimes, `RootSignature/ComputePipeline` caches. RT's root signature binds its unbounded `Texture2D[]` (space100) and `ByteAddressBuffer[]` (space101) tables at `NativeScope::bindless` |
| C9 | dxr 4 / infra 3.2 | `BufferLocation`, `ViewDescriptor`, `RequireMany`, `TextureWriteGeneration` vs `NativeBufferRange`, `NativeDescriptor` | Infra names win; infra adds `RequireMany(context, resources, count, state)` and `TextureWriteGeneration(device)` (bumped by `UpdateSubresource/Copy*/Map` into non-RT/DS/UAV textures **and** by texture creation/destruction, so RT refreshes material descriptors after a reload) at I2 |
| C10 | gpudriven 4 / mesh 5 / infra 3.4 / vrs 5 | Buffer SRV/UAV views are specified in `gpudriven.md` but needed by mesh (`StructuredBuffer` SRVs through D3D11), RT and VRS | `infra` I4 implements raw, structured and typed buffer SRV/UAV views, committed tracked SRV/UAV buffers, `MISC_DRAWINDIRECT_ARGS`, APPEND/COUNTER refused with a log line; `gpudriven.md`'s five layer scenarios move to infra's `scenarios_views.cpp` |
| C11 | vrs 3.4 / infra 3.5 | Both query OPTIONS6 and QI `ID3D12GraphicsCommandList5`; `vrs.md` assumes `EndNative` invalidates everything, which infra's `EndNative(touched)` no longer does | `infra` queries every cap once (`NativeCaps`; `GetShadingRateCaps` is a view) and caches `list4/5/6` per pooled list at creation (I0). The VRS context state lives in `d12_vrs.cpp` (`vrs` lane) behind hook calls infra pre-places at I0 (`ApplyFixedFunction`, `EnsureDrawStates`, `ReleaseBindings`/`InvalidateApplied`/`ClearState`, `BeginNative`, stats). `BeginNative` resets a non-default list to 1x1/no image and marks VRS dirty; `EndNative(TouchedShadingRate)` re-applies it |
| C12 | async 3.3 / infra 3.3 | Async's layer part (compute recording of D3D11 lists, typed allocators/lists, `transitioned`, read-shared lending, guard refinement) is written as the async lane's, but lives in infra's job code and invariants | Implemented by `infra` in I5 together with jobs; the `async` lane is engine + shaders and consumes `BeginAsyncRecording`/`AsyncJobFromCommandList` |
| C13 | async 3.7, dxr 4, infra 4 | `r__gpu_log`: async prints `ripple` twice and `async=%s`; dxr prints `rt=(...)`; infra adds an `Async` slot that duplicates async's per-job fields; `analyze.ps1` parses only `key=<number>` | One registry (`core`, 2.6): numeric keys only - `ripple`, `join`, `cq_march`, `cq_ripple`, `cq_gtao`, `async` (0 pixel, 1 inline, 2 async), `vrs`, `gd`, `grass`, `rt_scene`, `rt_sun`, `rt_ao`, `rt_refl`. No generic `Async` slot |
| C14 | vrs 3.7 / async 3.1 | The cloud-march early-out needs the current frame's depth; async moves the march to fork A, before the G-buffer exists | The early-out moves to the `async` lane (owner of the march). It runs on the DX12 Pixel path (async off or `r__async_march 1`: march after the G-buffer); DX11 has no depth alias and marches every texel as at Gate 0 (0.1 ranks above this row: 8.3, F1). Decision at A7: async `on` is measured against `off + early-out`; if the march job loses, the march stays a pixel pass after the G-buffer (with early-out) and job A carries the ripple only |
| C15 | dxc 6.4 / async 3.6 | DXC payoff edits target `gtao_render.ps` and `da_clouds_march.ps`, whose bodies async moves into headers shared with compute shaders (quad ops in CS need a 2x2 lane layout) | Payoff edits land after A2, in `gtao.h` / `da_clouds_march.h`. Compute variants either map threads quad-swizzled (lanes 4k..4k+3 = one 2x2 pixel block) or take the non-quad path under `DA_COMPUTE`. Every payoff edit must keep `r__async_verify` stage port at 0 differences |
| C16 | gpudriven 3.2 / dxr 3.3, 3.5 | GD moves eligible tree nodes out of the sector roots into `CSector::gd_nodes`; RT walks the sector roots (misses them). GD draws rigid MU props in the cascades from the GPU, bypassing `insert_static`, so RT shadows + cascades would double their shadows | RT's level walk visits `root()` **and** `gd_nodes` of every sector. GD stores a per-instance `rt_static` bit (from `dxRender_Visual::rt_static`), refreshed by `da_gpu_trees_refresh_flags()` which RT calls after classification; GD's SMAP cull skips those instances when the view's `o.rt_skip_static` is set. Swaying roots are never `rt_static` |
| C17 | gpudriven 3.9 / mesh | GD phase 4 (grass through ExecuteIndirect) duplicates mesh grass | Dropped. Grass on mesh-capable DX12 GPUs is `mesh.md`; elsewhere the vertex path |
| C18 | async 3.4 / dxr 3.6 | RTAO replaces GTAO in the AO slot; both change the GTAO targets' creation | The GTAO job is recorded only when GTAO is the frame's AO technique (not when `da_rt_live(AO)`). GTAO targets are created when `ssao_mode == gtao || da_rt_needs_ao_targets()`, R16F `rt_ssao_temp` with UAV on R5; the block belongs to `async` in `r2_rendertarget.cpp` |
| C19 | dxr 3.7 | Renames `phase_water_velocity_collect` to `phase_forward_water_collect` and moves the call | No rename: the collector becomes idempotent per frame (frame stamp) and RT calls it early from `phase_rt_reflections`; the existing call in `phase_combine` stays |
| C20 | vrs 9.2, mesh 4, gpudriven 4, dxr 4, dstorage 4, async 4 | Layer scenarios spread over `scenarios_render/native/api/engine.cpp` | One scenario file per lane with its own list function, all registered once by `infra` at I0 (2.3) |
| C21 | mesh 3.7 | Mesh verdict written by `r2_test_hw.cpp` next to the caps cache | Own file `render-grass-mesh.cache` (same signature scheme) written by `DetailManager_MS.cpp`; the preset replay asks `da_grass_mesh_expected()` |
| C22 | dstorage 5 | "The installer's licence page names the DirectStorage terms, like the NVIDIA ones" - the installer has no licence page | Obligations recorded in `DEPENDENCIES.md` exactly like the NVIDIA binaries (unmodified signed DLLs, protective terms, no marks, telemetry off) |
| C23 | infra 3.3/12.9 / dstorage | The copy queue for `InitUploader` and DirectStorage address the same uploads | Copy queue (I7) is measured after DirectStorage landed, on top of it; merged default-on only if the load A/B gains, otherwise removed - no `-dx12_copy_queue` flag left. Applied (8.4): default-on, the mask `-dx12_no_copy_queue` turns it off |
| C24 | vrs 5 | Rate histogram in a typed R32_UINT buffer | 8x1 R32_UINT texture UAV + staging copies (the async verify pattern); no dependency on buffer UAVs |
| C25 | vrs 3.7, mesh 4, dxr 6, dxc 4 | Four lanes edit `Test-Shaders.ps1` and `Check-ShaderLinkage.py` | `dxc` lane owns both: `.cs` as `cs_5_0`, DXC mode incl. `.ms/.as`, skip `r3/rt/` and `r3/native/`, MS->PS pairing, `(DA_TREE_GD,)` permutations; lanes add extra pipelines inside their marker blocks |
| C26 | vrs 3.7 + gap | Parity needs lossless shots; `Run-WaterQa` copies only top-level `*.jpg`; nothing deploys new DLLs/runtime files/UI files to the rig | `core` K0: `Run-WaterQa` copies `*.tga` and `capture_*` folders and takes `-Overlay`; `Invoke-Dx12Stand` syncs binaries, runtime DLLs and UI files (0.4); `render_check.lua` gains an optional TGA shot (`qa_tga = true`) |
| C27 | vrs 7, mesh 3.9 | Both edit `xrRender_sync_preset_derived` and the `[preset]` log line | `core` pre-creates marker blocks for `vrs` and `mesh` only (no `rt` block exists there: RT stays out of presets by construction) and formats the `[preset]` line once with `vrs=%d` and the `(mesh)` suffix from lane query functions |
| C28 | dstorage, dxr, vrs, gpudriven, dxc | Each adds its own `r__caps` hook through `da_upscaler_register_caps_hook` | One hook `da_native_caps.cpp` (`core`) fills every new `UpscalerCaps` field from lane query functions (2.4); lanes register no hooks |
| C29 | vrs 3.3 / async | `AoRaw` VRS scope on the GTAO integral, which runs as compute on the async path | `AoRaw` applies on the Pixel path only (compute has no shading rate) |
| C30 | vrs / dxr 3.5 | Coarse sun accumulation reads the RT shadow mask of the block's bottom-right pixel | Luminance edges (hence shadow edges) force 1x1 already; V7 repeats the VRS quality gate with `r__rt_shadows 1` on `qa_pedro` and `trees` |
| C31 | infra 3.7(f) | The self-test's VRS part needs a reset path before the VRS lane lands | Infra's I0 stubs of the VRS hooks already reset a list left non-default by native code to 1x1 |
| C32 | dxr 3.3 | Off-screen RT reflections show crowns in their static pose (reflections do not sway) | Accepted and documented: the sway rule is about shadows; on-screen reflection hits reuse the swaying frame, off-screen hits are rippled/low-frequency |
| C33 | all | "Frames equal the baseline" without a baseline or a noise model | Gate 0 (3.0) snapshots binaries + overlay and records reference pairs; 0.6 defines the comparison |
| C34 | all | No doc tests the features together | Final gate 6.5-6.6: everything-off parity, everything-on matrix incl. `-dxdebug`/`-dx12gbv`, level change, resize, VRAM peak |
| C35 | async, dxr, vrs, mesh, gd | Several lanes insert work at the same frame points | Fixed order in 2.5 (e.g. RT scene build before async fork A, because it may `Flush(wait)` with no job open) |

## 2. Lanes and file ownership

### 2.1 Lanes

| Lane | Scope | Source docs | Starts |
|---|---|---|---|
| `core` | baseline, QA tooling, vendoring (DXC, DirectStorage), build lists, engine skeleton (hooks, stubs, console, caps, timers, backend plumbing), the whole Video-tab UI, integrator during the night, final gate | this plan; UI/console parts of all docs | first |
| `infra` | xrD3D12: caps, DXC service, native sections v2, descriptors/bindless/memory, pipelines, D3D11-path extensions (buffer views, indirect, AS SRV), async queue + jobs + compute recording, self-test, copy queue | infra.md, dxc.md 3.1/3.4 (layer), async.md 3.3 (layer) | first (after K0) |
| `dxc` | DXC engine path, audit tools, shader fixes, reflection aliases, twins, `BindPoint`, QA shader tools, payoff edits | dxc.md | first (after K0) |
| `async` | cloud march / GTAO / ripple compute versions, `da_async`, frame placement, verify, early-out | async.md, vrs.md 3.7 | A2-A3 after K0, A1 and A4+ after K2 |
| `vrs` | `d12_vrs.cpp`, `da_vrs`, rate/luma passes, scopes, calibration | vrs.md | after I0 + K2 |
| `gd` | GPU-driven vegetation phases 1-3 | gpudriven.md | G0 after K0, G1 after K2 (its draw after I4), G2+ after I4 |
| `mesh` | mesh-shader grass | mesh.md | M1-M2 after K0; M3+ after I1, I3, I4, D3 |
| `ds` | DirectStorage textures (+ geometry), placed texture heaps, xrCore prefix decode | dstorage.md | after K1 |
| `rt` | DXR scene, RTAO, RT sun shadows, RT reflections, dynamic + skinned phases | dxr.md | R0 after I2, I3, D3a |

### 2.2 Hotspot files: owner and how other lanes add to them

| File | Owner | Others |
|---|---|---|
| `src/Layers/xrRenderDX12/d12_api.h` | infra | complete API for every lane frozen at I0 (declarations + stubs); later additions only in `[dx12:<lane>]` blocks |
| `d12_layer.h` | infra | per-lane pointers/blocks in `Device`, `Context`, `Applied`, `Resource`, `Shader`, `CommandList`, `RootSignatures`, stats, created at I0 |
| `d12_device.cpp`, `d12_infra.cpp`, `d12_objects.cpp`, `d12_context_ops.cpp`, `d12_context_state.cpp`, `d12_swapchain.cpp` | infra | hook calls into `d12_vrs.cpp` / `d12_mesh.cpp` / `d12_texheap.cpp`, pre-placed at I0 (2.4) |
| `src/Layers/xrRenderDX12/CMakeLists.txt` | infra | all new layer files listed at I0 |
| `tools/qa/d3d12layer/harness.*`, `main.cpp`, `Build-LayerTests.ps1`, `Run-LayerTests.ps1`, `scenarios_api.cpp` (list) | infra | per-lane scenario files, each registered at I0 |
| `xrRender_R2/r2_R_render.cpp`, `render_phase_sun.cpp`, `r2.cpp`, `r2_loader.cpp`, `da_temporal.cpp`, `r2_types.h` | core | hook lines and name blocks pre-placed at K2 |
| `xrRenderPC_R4/r4_rendertarget_phase_combine.cpp` | core | hooks + VRS scopes pre-placed at K2 |
| `xrRenderPC_R4/r4_rendertarget.h` | core | one state pointer per lane (`m_da_async`, `m_da_vrs`, `m_da_rt`) at K2 |
| `xrRender/xrRender_console.cpp/.h` | core | every command of every lane registered at K2; `[dx12:<lane>]` blocks for later changes; preset blocks for `vrs` and `mesh` only |
| `xrRenderPC_R4/da_upscaler.h` | core | all new `UpscalerCaps` fields at K2 |
| `xrRenderDX11/dx11GpuTimers.h/.cpp` | core | all slots + the field registry at K2 |
| `xrRenderDX11/dx11HW.cpp/.h` | core | hook lines at K2 |
| `xrRender/R_Backend.h`, `R_Backend_Runtime.cpp`, `xrRenderDX11/dx11R_Backend_Runtime.h` | core | `set_ShadingRate`, `RenderIndirect`, `set_InstanceStream`, `RenderMesh` implemented at K2 over the layer API |
| `xrRender/FBasicVisual.h`, `r__dsgraph_structure.h` (the `rt_skip_static` bits), `r__dsgraph_build.cpp` (`insert_static` early return) | core at K2, then `gd` owns `r__dsgraph_*` | |
| `xrRender_R2/r2_rendertarget.cpp` | async | `vrs`/`rt` target create/destroy hooks and the `da_rt_needs_ao_targets()` condition pre-placed by core at K2 |
| `xrRenderPC_R4/r4_rendertarget_phase_gtao.cpp` | async | `AoRaw` scope pre-placed by core at K2 |
| `xrRenderPC_R4/sources.cmake`, `xrRenderPC_R5/CMakeLists.txt`, `cmake/XRay.Dependencies.cmake`, `packaging/dead-air-x64/installer/runtime-files.txt`, `docs/dead-air/DEPENDENCIES.md` | core | every new file listed at K2; `[dx12:<lane>]` blocks for late additions |
| `r4_shaders.cpp`, `xrRender/ShaderResourceTraits.h`, `SH_Atomic.h`, `ResourceManager.h`, `xrRenderDX11/dx11ResourceManager_Resources.cpp`, `Blender_Recorder_R3.cpp`, `xrRender/Blender_Recorder.cpp`, `blender_recorder.h`, `CommonTypes.h`, `dx11r_constants.cpp`, `xrRender/r_constants.h`, `dx11ConstantBuffer.cpp` | dxc | `gd` block in `ResourceManager.h` + `dx11ResourceManager_Resources.cpp` (instanced declaration) |
| Video UI: `configs/dead_air_x64_video_options.ltx`, `configs/ui/ui_mm_opt.xml`, `ui_mm_opt_16.xml`, `scripts/ui_mm_opt_video.script`, `configs/text/rus|eng/dead_air_x64_video.xml` | core | nobody else; the one allowed exception: `mesh` edits the sentence of `st_da_x64_video_hint_preset` at M10 if its ladder differs |
| `tools/qa/Test-Shaders.ps1`, `tools/qa/Check-ShaderLinkage.py` | dxc | `[dx12:<lane>]` blocks for extra pipelines |
| `tools/qa/Run-WaterQa.ps1`, `tools/qa/water/render_check.lua`, `tools/qa/Compare-Frames.py`, `tools/qa/dx12/*`, `xrRenderDX11/dx11r_screenshot.cpp` | core | |
| `docs/dead-air/DX12_RENDERER.md`, `TEST_MATRIX.md`, `MODDING.md`, `README.md`, `packaging/dead-air-x64/README_RU.md` | core (final gate) | lanes write their text into their feature doc; core merges |

Exclusively owned existing files (no pre-placement needed): `async` - `r4_rendertarget_phase_clouds.cpp`,
`r4_rendertarget_phase_water_ripple.cpp`, `blenders/blender_gtao.cpp`; `vrs` -
`r4_rendertarget_accum_direct.cpp`, `r3_rendertarget_accum_point.cpp`, `r3_rendertarget_accum_spot.cpp`;
`gd` - `r__dsgraph_render.cpp`, `r__sector.h`, `FLOD.h`, `FTreeVisual.h/.cpp`, `r2_R_calculate.cpp`;
`mesh` - `DetailManager.h/.cpp`, `DetailManager_CACHE.cpp`, `dx11DetailManager_VS.cpp`,
`blenders/Blender_detail_still_deferred.cpp`, `blenders/uber_deffer.cpp/.h`; `ds` -
`dx11Texture.cpp`, `dx11SH_Texture.cpp`, `SH_Texture.h`, `xrRender/ResourceManager.cpp`,
`xrCore/Compression/*`, `xrCore/xrCore.def`; `rt` - `r4_rendertarget_phase_sky_water_velocity.cpp`,
`blenders/blender_light_direct.cpp`, `Blender_Recorder_StandartBinding.cpp`.

Shaders (`packaging/dead-air-x64/compatibility/gamedata/shaders/r3`):

| Owner | Files |
|---|---|
| dxc | `lod.ps`, `water.ps` (duplicate `SV_Position` fix only, then handed to `rt`), `shadow.h`, `ssao_hbao.ps` (overlay copy), `common_defines.h`, `da_taa.ps`; payoff edits in `gtao.h` and `da_clouds_march.h` after A2 (C15) |
| async | `gtao.h`, `gtao_render.ps/.cs`, `gtao_filter.ps/.cs`, `da_clouds_march.h/.ps/.cs`, `da_clouds.ps` (early-out history), `da_water_ripple.h/.ps/.cs`, `da_async_compare.cs` |
| vrs | `da_vrs.h`, `da_vrs_luma.cs`, `da_vrs_rate.cs`, `da_vrs_debug.ps/.s` |
| gd | `da_gd_tree.h`, `da_gd_frustum.h`, `da_gd_tree_cull.cs`, `da_gd_tree_args.cs`, `da_gd_tree_scatter.cs`, `da_gd_tree_wind.cs`, `deffer_tree_flat.vs`, `deffer_tree_bump.vs`, `shadow_direct_tree.vs` |
| mesh | `da_detail_common.h`, `da_detail_mesh.h`, `deffer_detail_mesh.ms`, `deffer_detail_cull.as`, `deffer_detail_w_flat.vs`, `deffer_detail_s_flat.vs`, `deffer_detail_mesh_depth.ps` |
| rt | `gbuffer_pack.h`, `common_functions.h`, `rt/*`, `water_rtreq.ps`, `da_water_rtreq.s`, `da_puddle_refl_req.ps`, `accum_sun_near.ps`, `accum_sun_far.ps`, `combine_1.ps`, `water.ps` (after the dxc fix), `da_puddle_refl.ps/.s` |

### 2.3 New files and who creates them

Stubs (header + empty or "off" bodies, Grizzly/MIT header on engine files) are created by the
creator at the milestone named, listed in the build files at once, and then belong to the owner.
A lane that starts before the stub exists creates the file itself and tells `core`.

| File | Owner | Stub by |
|---|---|---|
| `src/Layers/xrRenderDX12/d12_native.cpp`, `d12_async.cpp`, `d12_dxc.cpp` | infra | infra I0 |
| `d12_vrs.cpp` / `d12_mesh.cpp` / `d12_texheap.cpp` | vrs / mesh / ds | infra I0 |
| `xrRenderPC_R4/da_native_caps.h/.cpp` | core | core K2 |
| `xrRenderPC_R4/da_native_selftest.h/.cpp` | infra | core K2 |
| `xrRenderPC_R4/da_dxc.h/.cpp` | dxc | core K2 |
| `xrRenderPC_R4/da_async.h/.cpp` | async | core K2 |
| `xrRenderPC_R4/da_vrs.h/.cpp`, `r4_rendertarget_phase_vrs.cpp` | vrs | core K2 |
| `xrRenderPC_R4/da_gpu_trees.h/.cpp` | gd | core K2 |
| `xrRenderPC_R4/da_grass_mesh.h`, `xrRender/DetailManager_MS.cpp` | mesh | core K2 |
| `xrRenderPC_R4/da_dstorage.h/.cpp` | ds | core K2 |
| `xrRenderPC_R4/da_rt.h/.cpp`, `da_rt_device.cpp`, `da_rt_scene.cpp`, `da_rt_passes.cpp`, `r4_rendertarget_phase_rt.cpp` | rt | core K2 |
| `tools/qa/d3d12layer/scenarios_native.cpp`, `scenarios_views.cpp`, `scenarios_async.cpp`, `scenarios_dxil.cpp` | infra | infra I0 |
| `tools/qa/d3d12layer/scenarios_vrs.cpp` / `scenarios_mesh.cpp` / `scenarios_texheap.cpp` / `scenarios_rt.cpp` | vrs / mesh / ds / rt | infra I0 |
| `tools/qa/water/native_check.lua` / `async_check.lua` / `vrs_check.lua` / `gd_check.lua` / `grass_ab.lua`, `grass_run.lua` / `texture_load.lua` / `rtx_check.lua` | infra / async / vrs / gd / mesh / ds / rt | owner |
| `tools/qa/dx12/Invoke-Dx12Stand.ps1`, `Invoke-Dx12Perf.ps1`, `Dx12Locks.ps1`; `tools/qa/Compare-Frames.py` | core | core K0 |
| `tools/qa/Check-DxcShaders.py`, `tools/qa/dxc/pso_probe.cpp`, `Run-DxcAudit.ps1` | dxc | dxc D1 |
| `tools/qa/gpudriven/count_tree_draws.py` | gd | gd G0 |
| `tools/qa/dstorage/dsprobe.cpp`, `Run-DsProbe.ps1` | ds | ds S1 |
| `Externals/dxc/*`, `Externals/DirectStorage/*` | core | core K1 |

`Dx12Locks.ps1` uses the same lock directory as the session's `locked.ps1`
(`...\scratchpad\ninja.lock`, `stand.lock`), so older helpers and other agents stay mutually
excluded.

### 2.4 Engine and layer hook functions (stubs pre-placed, bodies owned by the lane)

Engine (all free functions; R4 builds compile them and return "off"):

| Lane | Functions |
|---|---|
| core | `da_native_caps_on_device(ID3D11Device*)` (fills `CHW::native`), the caps hook |
| infra | `da_native_selftest_run()` |
| dxc | `da_dxc_init(ID3D11Device*)`, `da_dxc_active()`, `da_dxc_log_stats()`, `da_dxc_compile_native(...)` |
| async | `da_async_frame_begin()`, `da_async_fork_early(CBackend&) -> bool`, `da_async_fork_gtao(CBackend&)`, `da_async_join(CBackend&)`, `da_async_ao_done() -> bool`, `da_async_path_code() -> int` |
| vrs | `da_vrs_supported()`, `da_vrs_preset_mode()`, `da_vrs_rate_pass(CBackend&)`, `da_vrs_luma_pass(CBackend&)`, `da_vrs_debug_pass(CBackend&)`, `da_vrs_reset(CBackend&)`, `struct da_vrs_scope`, `enum class VrsPass`, `da_vrs_targets_create/destroy(CRenderTarget&)` |
| gd | `da_gpu_trees_live()`, `da_gpu_trees_level_load()`, `da_gpu_trees_level_unload()`, `da_gpu_trees_frame_begin(CBackend&)`, `da_gpu_trees_refresh_flags()`, `da_gpu_trees_for_each_node(CSector*, fn)` |
| mesh | `da_grass_mesh_expected()`, `da_grass_mesh_frame() -> bool`, `da_grass_mesh_frame_prepare(CBackend&)`, `da_grass_ab_gbuffer_begin/end()`, `da_grass_ab_cascade_begin/end(u32 cascade, CBackend&)` |
| ds | `da_dstorage_available()`, `da_dstorage_shutdown()` |
| rt | `da_rt_available()`, `da_rt_device_create/destroy()`, `da_rt_reset_begin/end()`, `da_rt_frame_begin()`, `da_rt_frame_scene(CBackend&)`, `da_rt_sun_pass(CBackend&)`, `da_rt_ao_pass(CBackend&) -> bool`, `da_rt_reflections_pass(CBackend&)`, `da_rt_wants_scene_grab()`, `da_rt_skip_static_casters()`, `da_rt_wants_motion()`, `da_rt_needs_ao_targets()`, `da_rt_targets_create/destroy(CRenderTarget&)`, `da_rt_level_collect()`, `da_rt_level_unload()` |

Layer (members declared in `d12_layer.h` at I0, defined in the lane file):

| Lane | Hooks | Called from |
|---|---|---|
| vrs | `Context::VrsApply(list)`, `VrsRequire()`, `VrsReset(bool keepState)`, `VrsNativeReset()`, `Device::VrsInit()` | `ApplyFixedFunction`, `EnsureDrawStates`, `ReleaseBindings`/`InvalidateApplied`/`ClearState`, `BeginNative`, `Device::Init` |
| mesh | `RootSignatures::InitMesh()`, `PipelineCache::GraphicsMesh(desc)`, `Context::MeshRefuseDraw()` | `RootSignatures::Init`, `PipelineCache::Graphics` (when `desc.meshKind`), `PrepareDraw` |
| ds | `Device::TexturePlace(desc, heapProps, flags, out)` | `InitTexture` (before `CreateCommitted`) |

The I0 stubs keep today's behaviour, except `VrsNativeReset`, which already resets a non-default list
to 1x1 (C31).

### 2.5 Fixed order at shared points (C35)

`CRender::Render` (`r2_R_render.cpp`):
1. after `GpuTimers.FrameBegin()`: `da_gpu_trees_frame_begin` (wind CS);
2. after the `q_sync_point` wait: `da_grass_mesh_frame_prepare` (row copies);
3. after `r_main.sync()`: `da_async_frame_begin`, `da_rt_frame_scene` (first frame of a level: BLAS
   builds, may `Flush(wait)`; must run before any async job is open), then
   `const bool async_early = da_async_fork_early(RCache)` (cloud map on the direct queue, jobs
   `clouds_march`, `water_ripple`);
4. `parallel_details::start`: returns at once when `da_grass_mesh_frame()`;
5. `render_details` of the G-buffer: `da_grass_ab_gbuffer_begin/end` around (`da_grass_hzb` went with the
   grass Hi-Z, 8.4);
6. after `r_rain.sync()`: `da_async_fork_gtao`;
7. `da_water_field_update(); phase_water_ripple();` and `phase_cloud_map(); phase_clouds_march();`
   run only when `!async_early` (visor drops unchanged between them);
8. right before the `DEFER_SUN` block: `da_rt_sun_pass`, then `da_vrs_rate_pass`;
9. `scene_stage_guard` destructor: `da_vrs_reset(RCache)`;
10. `[gpu]` line: printed from the field registry (2.6).

`render_phase_sun.cpp`: `dsgraph.o.rt_skip_static = cascade_ind < 2 && da_rt_skip_static_casters();`
in `process_cascade`; `da_grass_ab_cascade_begin/end` around `Details->Render`.

`phase_combine` (`r4_rendertarget_phase_combine.cpp`): head `da_async_join` (includes the ripple
publish) -> AO slot `if (!da_rt_ao_pass(RCache)) { if (!da_async_ao_done()) { <today's AO block> } }`
-> sky + clouds composite (no VRS scope: `SkyClouds` removed in the final phase, vrs.md 13.7) -> combine_1
(no VRS scope: `Combine1` removed, vrs.md 13.3) -> scene grab condition `|| da_rt_wants_scene_grab()` ->
`da_rt_reflections_pass` -> `phase_da_puddle_refl` in `da_vrs_scope(PuddleRefl)` -> `render_forward`
in `da_vrs_scope(Forward)` -> `phase_combine_volumetric` in `da_vrs_scope(VolumetricCombine)` -> ...
-> `phase_sunshafts` E0-E3 in `da_vrs_scope(SunshaftsBlur)` -> `da_vrs_luma_pass` + `da_vrs_reset`
before `phase_upscale` -> `da_vrs_debug_pass` after `phase_pp`.

`r2_loader.cpp`: `level_Load`: after `LoadOverlayVisuals`: `da_gpu_trees_level_load`; at the end:
`da_rt_level_collect`, `da_dxc_log_stats`. `level_Unload`, first: `da_rt_level_unload`, then
`da_gpu_trees_level_unload`.

`r2.cpp`: `create` -> `da_rt_device_create`, `destroy` -> `da_rt_device_destroy`,
`reset_begin/end` -> `da_rt_reset_begin/end`, `da_rt_frame_begin` right before
`da_temporal_frame_begin` (it reads the latch via `da_rt_wants_motion`); `-sjitter` becomes a
no-op (dxc.md 6.2 finding).
`da_temporal.cpp:120`: `motion |= !menu && da_rt_wants_motion()`.
`dx11HW.cpp` `CreateDevice` (XR_D3D12): `DeviceCreateInfo::disabledFeatures` from
`-dx12_no_rt/-dx12_no_vrs/-dx12_no_ms/-dx12_no_async/-dx12_no_sm6`, after creation
`da_native_caps_on_device`, `da_dxc_init`; `DestroyDevice`: `da_dstorage_shutdown` before
`da_streamline_shutdown`.
`r2_rendertarget.cpp`: after the velocity target `da_vrs_targets_create(*this)`, then
`da_rt_targets_create(*this)`; destroy in reverse order in the destructor.

### 2.6 Contracts

**`r__caps`** (one `GetStatus`, `core`): today's
`api;dx12;dlss;dlssg;dlssg_max;fsr;fsrfg;hags` plus

| Key | Meaning | Filled from |
|---|---|---|
| `rt` | RT usable this session (C7) | `da_rt_available()` |
| `rt_tier` | 0/10/11/12, masked by `-dx12_no_rt`; R4 from `render-caps-d3d12.cache` | `NativeCaps` / cache reader |
| `vrs_tier`, `vrs_tile` | device VRS tier and tile | `NativeCaps` |
| `vrs` | VRS runs (tier 2, shaders, targets, no MSAA) | `da_vrs_supported()` |
| `ms` | mesh tier 1 and DXC SM >= 6.5 | `NativeCaps` |
| `sm` | `min(device, DXC)` as 60..69, 50 without DXC | `NativeCaps` |
| `wave`, `fp16`, `eb`, `acq` | lanes, native 16-bit, enhanced barriers (info), compute-queue jobs | `NativeCaps` |
| `dxc` | engine shaders are DXIL this session | `da_dxc_active()` |
| `gd` | GPU-driven vegetation registry live | `da_gpu_trees_live()` |
| `dstorage` | DirectStorage runtime and queues up | `da_dstorage_available()` |

About 175 of 255 characters; `GetStatus` logs once if it would truncate.

**`r__gpu_log` fields** (registry in `dx11GpuTimers`, numeric only, C13): existing fields unchanged,
plus `ripple` (direct ripple steps), `join` (direct wait at the join), `cq_march`, `cq_ripple`,
`cq_gtao` (compute-queue job times), `async` (0/1/2), `vrs` (rate + luma), `gd` (main-view
cull/args/scatter), `grass`, `rt_scene`, `rt_sun`, `rt_ao`, `rt_refl`. Timer slots: `Ripple`,
`JoinWait`, `VrsRate`, `VrsLuma`, `GdCull`, `Grass`, `RtScene`, `RtSun`, `RtAO`, `RtRefl`; slot
fields print automatically. Values that are not direct-queue slots are registered once by the lane
from its own file: `dx11GpuTimers::RegisterField(const char* key, float (*value)())` (async: `cq_*`
from `xrd3d12::AsyncComputeMs`, `async` from `da_async_path_code()`). A field prints `key=%.3f`
after the existing ones; nothing else writes the `[gpu]` line.

**Console** (registered by `core` at K2 in both modules; values and ranges as in the docs):

| Command | Kind | Lane |
|---|---|---|
| `r__native_selftest` | command | infra |
| `r__dxc` | saved 0/1, default 1, next start | dxc |
| `r__async_compute` | saved token `st_async_off/auto/on`, default auto, applies next frame | async |
| `r__async_path` (runtime 0..3), `r__async_verify` (command) | QA | async |
| `r__vrs` (-1..2), `r__vrs_threshold`, `r__vrs_force`, `r__vrs_debug`, `r__vrs_log` | runtime, not saved | vrs |
| `r__gpu_driven` | runtime 0/1, default 1 | gd |
| `r__grass_mesh` (runtime 0/1, default 1), `r__grass_stats`, `r__dbg_grass_ab` | QA | mesh |
| `r__dstorage` | saved bool, default 1 | ds |
| `r__texload_stats` | command | ds |
| `r__rt_reflections`, `r__rt_shadows`, `r__rtao` | saved 0/1, default 0, `depend="vid"` | rt |
| `r__rt_quality` | saved token `st_rt_low/medium/high`, default medium | rt |
| `r__rt_debug` | runtime 0..4 | rt |

Preset ladders: `vrs_by_preset = {2, 2, 1, 1, 0}` (vrs block), `detail_radius_mesh_by_preset` /
`detail_density_mesh_by_preset` (mesh block, equal to the base columns until M10 measures them).
Nothing `r__rt*` anywhere near a preset.

Command-line switches: `-dx12_no_rt`, `-dx12_no_vrs`, `-dx12_no_ms`, `-dx12_no_async`,
`-dx12_no_sm6`, `-dxc_debug`, `-no_dstorage`, `-dstorage_buffered`, `-dstorage_verify N`.

**Video tab** (`core` at K3):
- `[groups] order = screen, quality, aa, rtx, image, effects, color`; `ui_mm_grp_rtx` = "RTX".
- `[group_screen]` gets `async_compute` (list, `r__async_compute`) right after `renderer`;
  `[group_quality]` gets `dstorage` (check, `r__dstorage`) after `level_texture_unload`;
  `[group_rtx] rows = rt_reflections, rt_shadows, rtao, rt_quality`.
- Nodes `list_async_compute`, `check_dstorage`, `check_rt_reflections`, `check_rt_shadows`,
  `check_rtao`, `list_rt_quality` in `ui_mm_opt.xml` and `ui_mm_opt_16.xml`
  (`depend="runtime"` for async, applied at once for DirectStorage, `depend="vid"` for RT).
- Greying in `InitStates`/`Refresh`, frame-generation pattern: async on `api ~= 12` / `acq ~= 1`;
  DirectStorage on `api ~= 12` / `dstorage ~= 1`; RT rows on `api ~= 12` (`_rt_dx11` when
  `rt_tier >= 11`, else `_rt_dx11_nohw`) / `rt ~= 1` (`_rt_unsupported`); all three greyed while the
  renderer combo is not DirectX 12; `rt_quality` greyed with `_rt_quality_off` while no RT effect is
  ticked.
- Strings rus (cp1251) and eng as written in async.md 7, dstorage.md 7, dxr.md 7; one sentence in
  `st_da_x64_video_hint_preset` about reduced-rate shading on DirectX 12 (vrs.md 7).

## 3. Foundation

### 3.0 Gate 0 (core, before anybody edits built files)

1. Check `ninja.lock`/`stand.lock`: wait for foreign holders (the options agent) to finish.
2. `Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'` on the tree as it is. If it does not build, fix the
   cause (it is somebody's unfinished edit: the smallest fix, noted in PLAN section 8).
3. K0 tooling changes that are scripts only (`Run-WaterQa` copies `*.tga` + `capture_*`, takes
   `-Overlay`; `render_check.lua` optional TGA shot; `tools/qa/dx12/*`; `Compare-Frames.py`).
4. Snapshot `bin\x64\Release` and `packaging\dead-air-x64\compatibility\gamedata` to
   `_work\dx12-features-20260928\baseline\{bin,gamedata}`. From here on lanes may edit.
5. Layer tests: `Build-LayerTests.ps1`, `Run-LayerTests.ps1 -HighPerformance` -> record the count
   (36 today) as the floor.
6. Reference pairs with the snapshot (`-Bin/-Overlay`), two runs each, `-ss_tga`,
   `qa_render_cmds = {"r__aa st_aa_smaa", "wind_freeze 1"}`, `qa_tga = true`:
   `render_check` R5 and R4 on `qa_pedro`, `foliage`, `fogqa`, `trees`; `upscale_check` R5 and R4;
   `motion_check` R5 (TAA + vectors); `options_tab` R5 and R4.
7. Perf reference: `Invoke-Dx12Perf` with the snapshot, R5 and R4, `qa_pedro` and `foliage`, 2 pairs.
8. Status file `lanes/core.md`: "Gate 0 reached" with every run name.

### 3.1 `core` milestones

| M | Work | Gate |
|---|---|---|
| K0 | 3.0 steps 3-4 (tooling), status/inbox directory | scripts run on Gate-0 runs; `Compare-Frames.py` a-vs-b of the reference pair reports the noise floor |
| K1 | Vendor `Externals/dxc` (1.9.2607 from `_work\sdk-downloads\dxc-1.9.2607`, hashes in README) and `Externals/DirectStorage` (NuGet 1.3.0 from `_work\sdk-downloads\dstorage`), `XRAY_DXC_RUNTIME_FILES`, `XRAY_DSTORAGE_RUNTIME_FILES`, `Microsoft::DirectStorageHeaders`, staging by `xray_stage_windows_runtime`, `runtime-files.txt` (+4 DLLs), `DEPENDENCIES.md` rows and obligations (C22) | configure + build stage the four DLLs into `bin\x64\Release`; the runtime-files check of `tools/package/dead_air_x64_compatibility_archive.ps1` (its comparison against the Release output) passes when run against a temp output folder |
| K2 | Engine skeleton: every stub file of 2.3 + `sources.cmake`/`xrRenderPC_R5` CMake; all hooks of 2.5; all commands, blocks, ladders, caps fields, `da_native_caps.cpp`, `[preset]` line; timer slots + registry; backend plumbing (`set_ShadingRate` cached + reset in `Invalidate`, `RenderIndirect`, `set_InstanceStream` slot 1 cached, `RenderMesh`); `rt_static`/`rt_skip_static` + `insert_static` early return; `r4_rendertarget.h` lane pointers; `r2_types.h` name blocks; `-sjitter` no-op | both modules build; layer tests at the floor; R5+R4 `render_check` frames equal Gate 0 (0.6); `upscale_check` R5 all steps; `[preset]` line and `[gpu]` line parse in `analyze`; `r__caps` prints every key (new ones 0 except `rt_tier`/`sm` values from infra when I1 has landed) |
| K3 | Video UI (2.6): rows, nodes, strings rus/eng, script rules | `python tools\qa\Check-OptionHints.py` clean; `options_tab` R5 and R4 pass; RTX header with four rows visible, greyed with the right hint on R4 and on R5 while `rt=0`; async/DirectStorage rows greyed per caps |
| K4 | Integrator during the night: inbox triage, hook-line merges, every ~3 hours a combined regression (R5+R4 `render_check` vs Gate 0, layer tests) with results in `lanes/core.md`; perf-slot fairness | no regression left unowned |
| K5 | Final gate (section 6) | 6 passes |

### 3.2 `infra` milestones

| M | Work | Gate |
|---|---|---|
| I0 | **API freeze**: `d12_api.h` complete for every lane (infra 3.1-3.5, dxc 3.1, async 3.3, vrs 3.4, mesh 3.5 `DispatchMesh`, C9's two functions) with stub implementations; `d12_layer.h` per-lane pointers/blocks; hook calls of 2.4 with stubs in `d12_vrs.cpp`/`d12_mesh.cpp`/`d12_texheap.cpp`; `list4/5/6` cached per pooled list, `ID3D12Device2/5` queried; scenario files + per-file lists; CMake | both modules build; layer tests at the floor; R5 `render_check` equal Gate 0 |
| I1 | Caps (`NativeCaps`, `QueryNativeCaps`, masks, device log lines, `ProbeAdapter` overload, `render-caps-d3d12.cache` write/read in both modules); **`d12_dxc.cpp` complete** (dxc.md 3.1); layer DXIL acceptance: `Shader::Reflect` through `Reflect`, `ProgramType` for DXIL incl. kinds 13/14 (`Shader::kind`), `Shader::dxil`, ISG1, mix guard + `-dxstats` counter; harness `-Dxil` mode; scenarios native 1, `dxil_bindings`, `dxil_dxbc_mix`, native 11 (DXIL compute + VS/PS pair) | layer tests (+ `-Gbv`, `-Dxil`); stand caps runs of infra 9.3 (R5, R4, masked) show the expected keys |
| I2 | Native sections v2: `NativeScope`, `BeginNative/EndNative(touched)` on any context, `RequireView`, `UavBarrier`, `uavWritten` fix, old API as wrappers, `NativeDescriptor`, `NativeBufferRange`, `RequireMany`, `TextureWriteGeneration` | scenarios 2-4; `upscale_check` R5 equal Gate 0 (FSR/DLSS/FG) |
| I3 | Ring base + `BindlessAllocator` (null init, deferred free, view bookkeeping), `AllocateDescriptors`, `AllocateUpload`, `CreateNativeBuffer`, `CreateAccelerationStructureBuffer`, `ReleaseNative`, `QueryVideoMemory`; `RootSignature`, `ComputePipeline`, `StreamPipeline` (library), `RaytracingPipeline`, `CommandSignature` | scenario 8; a stream-PSO and a compute-PSO created in a scenario; R5 `render_check` equal Gate 0 |
| I4 | D3D11-path extensions: buffer SRV/UAV views (C10), indirect draws + count variants, `INDIRECT_ARGUMENT` states, AS SRV kind; scenarios 10, 12 and the five GD scenarios in `scenarios_views.cpp` | layer tests + `-Gbv` |
| I5 | Async: compute queue, `crossFence`/`computeFence`, recorder pool, jobs, Fork/Join markers, Present join, lent guard, fallback, timestamps, `AsyncComputeMs`; compute recording mode (typed allocators/lists, refusals, `NON_PIXEL` inputs, `transitioned`), `AsyncJobFromCommandList`, read-shared lending, `StatAutoJoins` | scenarios 5-7 and async A-E with and without `-Gbv`; `-dxstats` shows 2 submissions/frame unchanged with no job |
| I6 | `r__native_selftest` (`da_native_selftest.cpp`) + `native_check.lua`; the full infra 9.3 matrix; infra 9.4 zero-cost A/B (masks off vs on inside one DLL) | every self-test part `ok` (with `-dxdebug` too); fallback parts `skipped`; FPS +-1 %, passes +-0.05 ms |
| I7 | Copy queue for `InitUploader` (after S8, C23): implement, 5 interleaved `qa_pedro` loads each way with DirectStorage on | merged default-on only if the load wall time drops; else removed |

### 3.3 `dxc` foundation milestones (feature milestones D4-D5 in 4.7)

| M | Work | Gate |
|---|---|---|
| D1 | `Check-DxcShaders.py`, `pso_probe.cpp`, `Run-DxcAudit.ps1` (from the scratchpad `dxc/`), run on today's tree | reports exactly dxc.md 6.2's "before" failures |
| D2 | Shader fixes (`lod.ps`, `water.ps`, `shadow.h` 7x7 fold, `ssao_hbao.ps`), `dx11ConstantBuffer::Similar` type-name fix, `Test-Shaders.ps1` (`.cs`, DXC mode, skips), `Check-ShaderLinkage.py` (DXIL rule, MS->PS, marker blocks) | dxc tests 1-3 ("DXC only 0"); R4 build; R4 `render_check` vs Gate 0 PSNR >= 45 dB (Ultra shadow rounding only) |
| D3a | `da_dxc` init (after I1), SM choice, `r__dxc`, `da_dxc_compile_native` + native cache, `dxc=` query, statistics | an embedded test shader compiles through `da_dxc_compile_native` at start (log line); `[dxc]` setup line on R5 |
| D3b | Engine path: `CommonTypes.h` aliases, `r4_shaders.cpp` DXC branch (FXC preprocessing, arguments, macros, cache `r5_dxil/<tag>`, reflection hand-over, disasm, `#ms/#as` routing, DXIL `create_shader` incl. MS/AS), `dxil/source/RSC_FXC`, `|fxc` keys, `UnifyShaderFormats` (mesh kinds exempt, C5), `BindPoint` (C6) | dxc tests 4-11 (stand runs `dxc-render`, `fxc-render` + capture PSNR >= 40 dB on `qa_pedro` and `foliage`, upscale/motion, fallbacks, cold-compile time, DX11 regression); layer `-Dxil` |

### 3.4 Dependency matrix (feature milestone -> foundation milestones it waits for)

| Waits for | Milestones |
|---|---|
| K0 only | A2-A3, M1-M2, D1-D2, S1 (with K1), G0 |
| K2 only | A1, A4, V2-V5 (engine parts), G0-G1 (except the draw smoke test), M1-M2, M5 (blender code), S2-S3, S5-S8 (engine side), R0 (console/UI/binder parts) |
| I0 | V1, M3 (stubs to fill), S4 |
| I1 + D3a | R0 compile path, M4 (offline DXC compiles work without it) |
| I1 + I3 | M3 |
| I2 + I3 + D3a | R0-R3 native passes, M6 frame prep |
| I4 | G1 smoke draw, G2-G8, M6 (structured SRVs) |
| I5 | A5-A7 |
| D3b | M4-M10 (engine DXIL mode), D4, D5 (with A2) |
| A2 | D5 (payoff edits in the shared bodies) |
| G2 | R1 (walker over `gd_nodes`), R2 (shadow handover with GD) |
| S8 | I7 |

## 4. Feature lanes

Gates reference the test sections of the lane's doc (`<doc> 9.x`). Every milestone also passes: the
build, the layer tests at the current floor, and an R4 `render_check` vs Gate 0 whenever shared engine
code changed.

### 4.1 `async` (async.md)

Owns: `da_async.*`, `r4_rendertarget_phase_clouds.cpp`, `r4_rendertarget_phase_gtao.cpp`,
`r4_rendertarget_phase_water_ripple.cpp`, `blender_gtao.cpp`, `r2_rendertarget.cpp`, its shaders
(2.2), `async_check.lua`.

| M | Work | Gate |
|---|---|---|
| A1 | Read the `Ripple`/`JoinWait` numbers core added on today's code | R5 + R4 `render_check` with `r__gpu_log 300`: ripple cost recorded in async.md |
| A2 | Shared bodies `gtao.h`, `da_clouds_march.h`, `da_water_ripple.h` + pixel wrappers, `da_cloud_target` | `Test-Shaders`, linkage; R4 and R5 `render_check` vs Gate 0 (PSNR >= 45 dB; GTAO filter corner noted) |
| A3 | Compute shaders + blenders, R5 UAV flags, GTAO-mode `rt_ssao_temp` (C18 condition), `phase_*_cs`, `da_ripple_prepare`, publish, `CBackend&` helpers; cloud-march early-out + sentinel history (C14) on the Pixel path | `Test-Shaders`; R4 `render_check` vs Gate 0 (the early-out changes no visible pixel: clouds hidden behind geometry) and the `march` timer lower in a built-up view (measured on R4 then; the final phase made the early-out DX12-only, 8.3 F1) |
| A4 | `da_async` Pixel and ComputeInline paths, frame placement through the core hooks, join point, console behaviour, verify stage port | `async_check` with `r__async_path 2`: port 0 differing texels for `clouds0`, `gtao_0`, `ssao_temp`, `ripple` |
| A5 | ComputeAsync path (after I5), verify stage schedule | async 9.3 runs 1, 2 (`-dxdebug -dx12gbv` at 720p), 6 |
| A6 | Auto rule (`NativeCaps::meshShaderTier`, vendor, UMA) | async 9.3 runs 3, 4, 5, 7 |
| A7 | Perf 9.4 + C14 decision (async on vs off-with-early-out) + join split if `join` > 0.2 ms | qa_pedro frame >= 0.8 ms lower, foliage >= 0.5 ms (or the documented fallback decision); R4 `march`/`ao` +-0.05 ms (the early-out is DX12-only, 8.3 F1) |

Done: every 9.3 run passes; perf gate or decision recorded; DX11 frames equal.

### 4.2 `vrs` (vrs.md)

Owns: `d12_vrs.cpp`, `scenarios_vrs.cpp`, `da_vrs.*`, `r4_rendertarget_phase_vrs.cpp`,
`r4_rendertarget_accum_direct.cpp`, `r3_rendertarget_accum_point.cpp`, `r3_rendertarget_accum_spot.cpp`,
its shaders, `vrs_check.lua`; its `xrRender_console.cpp` / `r2_types.h` blocks and the `m_da_vrs`
state behind `CRenderTarget`.

| M | Work | Gate |
|---|---|---|
| V1 | Layer state in `d12_vrs.cpp` (C11): `SetShadingRate`, apply, `SHADING_RATE_SOURCE` requirement incl. deferred first requirement, safety rules, resets, stats | vrs 9.2: five scenarios + the floor; `-Gbv -Filter vrs` silent |
| V2 | `da_vrs` mode/caps/table/scope bodies, start log | both modules build; `[vrs]` start line on R5 and `off` reason on R4 |
| V3 | Resources, `da_vrs_*` shaders, `phase_vrs_*`, stats texture (C24) | `r__vrs_force 1` + `r__vrs_debug 1` overlay shows the grid, nothing consumes it |
| V4 | Scopes pass by pass (SunLight/SunVolumetric on the cascade contexts, Lights, LightsVolumetric, SunshaftsBlur, SkyClouds, PuddleRefl, Forward, VolumetricCombine, Combine1, AoRaw on the Pixel path, C29) | each with `r__vrs_force 1` shots and its `[gpu]` field falling |
| V5 | Content adaptivity, `vrs_check.lua`, calibration sweep | vrs 9.3 quality table on all seven saves (+ DLSS/FSR, motion) |
| V6 | Robustness | vrs 9.3 robustness runs (`-dxdebug`, 60 s `-dx12gbv`, `upscale_check`, `resize_check`) |
| V7 | Perf 9.4 + ladder decision rule; DX11 parity; the quality gate again with `r__rt_shadows 1` once R2 has landed (C30) | vrs 9.4 table |

Done: ladder final in `xrRender_sync_preset_derived`, preset hint sentence true to the ladder,
vrs.md numbers filled.

### 4.3 `gd` (gpudriven.md)

Owns: `da_gpu_trees.*`, `r__dsgraph_structure.h`, `r__dsgraph_build.cpp`, `r__dsgraph_render.cpp`,
`r__sector.h`, `FLOD.h`, `FTreeVisual.h/.cpp`, `r2_R_calculate.cpp`, its shaders, `gd_check.lua`,
`tools/qa/gpudriven/`; `gd` blocks in `ResourceManager.h`/`dx11ResourceManager_Resources.cpp`.

| M | Work | Gate |
|---|---|---|
| G0 | `count_tree_draws.py` + baseline counts on the Gate-0 snapshot | counts per save recorded (compare with gpudriven 2.6) |
| G1 | `FTreeVisual` read accessors for the registry **and for RT** (xform, window 0 geometry, foliage, `rigid()`, root, height); GD pass variants + instanced declaration; `da_gd_tree.h` + the three VS blocks; smoke draw of every instance from a CPU-built args buffer (after I4) | smoke frame vs GD off within the noise floor with `wind_freeze 1` |
| G2 | Registry, cells, bins, groups, surgery + restore, unload, runtime switch, log; `da_gpu_trees_for_each_node`, per-instance `rt_static` bit + `da_gpu_trees_refresh_flags` (C16) | `[gd] trees:` line; toggle restores; second level rebuilds |
| G3 | Wind CS + `gd_root_state` + `wind_dbg` readback | frozen-wind parity |
| G4 | SMAP views: cells, cull, args, scatter, draw; `o.rt_skip_static` honoured | gpudriven 9.3 parity on four saves + **shadow sway** run |
| G5 | Main view | 9.3 motion vectors, upscalers, toggle stress, second level, `-dxdebug`, DX11 |
| G6 | Stats/timers, perf + profiles | 9.4 |
| G7 | Phase 2 Hi-Z | merged only if `scene` drops by more than the build cost - removed (8.4) |
| G8 | Phase 3 (main view on the GPU, impostors by CS): first a design section in gpudriven.md (3.9 expanded: node test port, Hi-Z replacing HOM, impostor generation, parity rules), then implementation | the phase-1 parity and perf gates again, plus impostor fade parity; merged only if it passes - removed (8.4) |

Done: phases 1-3 decided and implemented or removed with numbers; RT coordination verified with R2.

### 4.4 `mesh` (mesh.md)

Owns: `d12_mesh.cpp`, `scenarios_mesh.cpp`, `da_grass_mesh.h`, `DetailManager*`,
`dx11DetailManager_VS.cpp`, `Blender_detail_still_deferred.cpp`, `uber_deffer.*`, its shaders,
`grass_ab.lua`, `grass_run.lua`; the `mesh` blocks in `xrRender_console.cpp`.

| M | Work | Gate |
|---|---|---|
| M1 | `cache_Task` fix (both APIs) | R4 + R5 `render_check foliage` within the noise floor of Gate 0 |
| M2 | `da_detail_common.h` + VS refactor | `fxc /Fc` identical instruction streams in every permutation (mesh 9.2) |
| M3 | Layer: mesh root signature, stream-PSO branch, `PrepareMesh`, `DispatchMesh`, draw refusal, stat (after I1, I3) | mesh 9.1 four scenarios + `-Gbv` |
| M4 | `da_detail_mesh.h`, `.ms`, `.as` (HLSL 2021, C5) | `Test-Shaders` DXC mode, linkage MS->PS 0 failures |
| M5 | Blender elements 2/3, availability (`dxc=1`, C5), `* [grass-mesh]` log, verdict file (C21) | log line on R5; `-dx12_no_ms` and `r__dxc 0` give the vertex path |
| M6 | Pool/pages/LRU/generations, upload ring, calc-task step, `MeshFramePrepare`, `ms_Render`, hybrid, `r__grass_mesh`, `r__grass_stats`, `Grass` timer | `render_check foliage` mesh 1 vs 0 side by side: no holes |
| M7 | `r__dbg_grass_ab` + `grass_ab.lua` | mesh 9.3 A/B runs (no AA, TAA + vectors, fogqa) |
| M8 | Perf 9.4 + tuning variants 3.10 | FPS(mesh) >= FPS(vertex) - 0.5 % on every save; render-thread grass -70 % |
| M9 | Hi-Z 3.11 | merged only if >= 0.15 ms - removed (8.4) |
| M10 | Ladder sweep (`grass_run.lua`), mesh columns, `(mesh)`, hint sentence if changed | mesh 9.3 sprint + 9.3/9.4 again at the final rungs; `-dx12_no_ms` base column |

Done: mesh columns measured; A/B parity green; DX11 unchanged.

### 4.5 `ds` (dstorage.md)

Owns: `d12_texheap.cpp`, `scenarios_texheap.cpp`, `da_dstorage.*`, `dx11Texture.cpp`,
`dx11SH_Texture.cpp`, `SH_Texture.h`, `xrRender/ResourceManager.cpp`, xrCore compression +
`xrCore.def`, `tools/qa/dstorage/`, `texture_load.lua`.

| M | Work | Gate |
|---|---|---|
| S1 | Land the probe (`tools/qa/dstorage/`, `Run-DsProbe.ps1`) over K1's vendored SDK | probe builds, verify 0 mismatches |
| S2 | `rtc_decompress_prefix` (clipped last literal run) + ordinal 1519 | `Invoke-Ninja 'xrCore'`; a unit check over the 12 headers the safe decoder missed |
| S3 | `[texload]` batch line + in-game sync-load stats on today's path, both modules | baseline numbers recorded on `qa_pedro` and a `k00_marsh` save |
| S4 | `TexturePageAllocator` in `d12_texheap.cpp` + `InitTexture` hook; layer scenario | scenario + floor; `render_check` equal Gate 0; texture creation time lower in `[texload]` |
| S5 | Runtime, configuration, queues, files, status ring, codec pool, eligibility, raw direct + tail, LZO codec, pending state, pool integration, verify mode | dstorage 9 run 1 (`-dstorage_verify 512`: 0 mismatches) |
| S6 | In-game synchronous path | run 3's sync-load p95 row |
| S7 | `r__dstorage` behaviour, `da_dstorage_available`, `r__texload_stats` | runs 2, 4, 5 |
| S8 | `texture_load.lua`, full plan, A/B | runs 3, 6, 7, 8; offline cold probe |
| S9 | `level.geom` through `DESTINATION_BUFFER` (dstorage 3.8, needs I2 `NativeBufferRange`) | merged only if the load A/B improves and the geometry verifies |

Done: every run passes; numbers in dstorage.md and DX12_RENDERER.md.

### 4.6 `rt` (dxr.md)

Owns: `da_rt*.cpp/.h`, `r4_rendertarget_phase_rt.cpp`, `scenarios_rt.cpp`,
`r4_rendertarget_phase_sky_water_velocity.cpp`, `blender_light_direct.cpp`,
`Blender_Recorder_StandartBinding.cpp`, its `r2_types.h` block and the `m_da_rt` state behind
`CRenderTarget`, its shaders, `rtx_check.lua`.

| M | Work | Gate |
|---|---|---|
| R0 | Device part over infra (root signature, PSOs through `RootSignature/ComputePipeline`, bindless tables, upload, C8), `da_rt` binder, `gbuffer_pack.h` split, `rt_tier`/`rt` availability (C7), scenarios "native raytracing" + "view descriptor copy" | layer tests + `-Gbv`; `rtx-dx11` and `rtx-off-parity` (both modules unchanged in frames) |
| R1 | Scene (walk incl. `gd_nodes`, C16), static + MU BLAS/TLAS with compaction and budget, material table, `rt_static` flags + `da_gpu_trees_refresh_flags()`, `r__rt_debug`; RTAO trace, temporal, upsample, guided filter, `combine_1` branch | `rtx-pedro` (rtao steps), `rtx-debug`, `rtx-restart`, `rtx-levelchange`, perf row |
| R2 | Sun shadows: `da_rt_skip_static_casters`, trace, temporal + a-trous, accum shader edits, `s_rt_sun` | **`rtx-foliage` sway** with RT on and off, `rtx-trees` (no doubled shadows, also with GD on and off), perf |
| R3 | Reflections: hit shading, requests, idempotent water collector (C19), trace with reuse, `water.ps`/`da_puddle_refl.ps` branches, grab condition | `rtx-water`, `rtx-puddles`, perf |
| R4 | Dynamic rigid objects in the TLAS (per-frame rebuild) for reflections and AO | `rtx-pedro` with NPC props; TLAS time <= 0.2 ms |
| R5 | Skinned characters: pre-skin CS, refit, nearest 12 within 30 m | `rtx-pedro` reflections of NPCs; no shadow change (cascades keep them) |
| R6 | Tuning per tier to the budget table, final `rtx_check` on `qa_pedro`, `foliage`, `fogqa`, `puddle` | dxr 9 perf: Medium <= +3.0 ms, Low <= +1.5 ms, load <= +1.5 s, VRAM <= +600 MB |

Done: every dxr 9 run passes; `git grep -n "r__rt" -- "*rspec_*.ltx"` empty and no RT name inside
`xrRender_sync_preset_derived`; RTX rows correct on both APIs.

### 4.7 `dxc` feature milestones

| M | Work | Gate |
|---|---|---|
| D4 | Perf baseline DXC vs FXC (dxc 9 test 12, `-dx12_no_sm6` variant) | DXC frame time <= FXC + 1 % at equal clocks; otherwise the regressing passes are found per `[gpu]` field and fixed (flags, `-all-resources-bound`, source), and passes that still lose are pinned to FXC through the twin mechanism by an explicit list in `da_dxc.cpp` |
| D5 | Payoff edits one at a time: `common_defines.h` types, TAA, GTAO (C15), PCSS, clouds; optional `-all-resources-bound` | each kept only with >= 0.05 ms or 3 % on its pass, the capture diff of dxc test 7, and `r__async_verify` port 0 |

## 5. Schedule

```
t0   core Gate 0 + K0 ............................................................
t1        core K1 ---- K2 ---- K3 ---------------- K4 (integration) ------------- K5
t1        infra  I0 -- I1 -- I2 -- I3 -- I4 -- I5 -- I6 ------------------ I7
t1        dxc    D1 -- D2 ------ D3a -- D3b ---- D4 ------- D5
t1        mesh   M1 M2 ............. M3 -- M4 -- M5 -- M6 -- M7 -- M8 -- M9 -- M10
t1        ds        S1 S2 S3 -- S4 -- S5 -- S6 -- S7 -- S8 ------------- S9
after K0  async  A2 A3 (A1, A4 after K2) ....... A5 -- A6 -- A7
after K2  vrs    (V1 after I0) V2 V3 V4 V5 V6 ----------------- V7
after K2  gd     G0 G1 ....... G2 -- G3 -- G4 -- G5 -- G6 -- G7 -- G8
after I3  rt     R0 -- R1 -- R2 -- R3 -- R4 -- R5 -- R6
```

- **Critical path**: I0 -> I1 -> I2 -> I3 -> R0 -> R1 -> R2 -> R3 -> R4 -> R5 -> R6. `infra` therefore
  does I1-I3 before I4-I5, and D3a follows I1 immediately. The second longest chain is
  I4 -> G2 ... G8.
- Up to nine agents run at once; builds are serialised by `ninja.lock`, stand runs by `stand.lock`.
  Lanes batch edits so that each builds at most every ~20 minutes; header changes to shared files
  are forbidden after K2/I0 except inside lane blocks (0.2), which keeps incremental builds small.
- Perf batches (0.5) block builds; each lane runs its batch only at the milestones named and announces
  it in its status file.
- If a lane is blocked on a dependency, it works on the next milestone that does not need it (tables
  in 4.x are ordered so that such work exists), then returns.

## 6. Final gate (`core`, after every lane is done)

### 6.1 Freeze and build
All lanes report done; no lock held. `Invoke-Ninja` with no target (the full Release build: every
module, so nothing else broke), then `Build-LayerTests.ps1`. 0 errors, no new warnings in touched
files.

### 6.2 Static checks
- Layer tests `-HighPerformance`, `-Gbv`, `-Dxil`: all pass, debug layer and GBV silent.
- `Check-DxcShaders.py --sm 6_0/6_5/6_6 [--console-extras]` and `Run-DxcAudit.ps1`: "DXC only 0",
  refused = gated set; `Check-ShaderLinkage.py` (FXC both APIs, DXC): 0 refused;
  `Test-Shaders.ps1` all modes: 0 FAIL; `Check-OptionHints.py` clean.
- Greps: `git grep -n "r__rt\|rtao" -- "*rspec_*.ltx"` empty; no `r__rt*` or `da_rt` inside
  `xrRender_sync_preset_derived`; every new engine file (2.3) has the Grizzly/MIT header and no
  ported/build/.def file got one; no other mod's name in new code or data; `xrCore.def` ordinals
  unique; `dumpbin /imports xrRender_R4.dll` unchanged against Gate 0; no `TODO`/`FIXME` in owned
  files.

### 6.3 Adversarial review
Fresh reviewer agents, one per lane diff plus one cross-cutting review of the frame
(`r2_R_render.cpp`, `phase_combine`, `render_phase_sun.cpp`) and one of the layer invariants
(fences, cross-queue lifetimes, join-before-Present, state hand-off, descriptor lifetimes, VRS
state leaks, PSO format mixing), each at high effort, instructed to find correctness bugs,
rule violations (0.1) and DX11 impact. Every finding is fixed at once by the owning lane (or by
`core` if the lane has finished) and the affected gates are re-run.

### 6.4 DX11 regression (R4)
Against the Gate-0 reference pairs: `render_check` on `qa_pedro`, `foliage`, `fogqa`, `trees`
(equal; known deltas: Ultra shadow rounding and the GTAO filter corner, PSNR >= 45 dB),
`upscale_check`, `motion_check`, `options_tab`, `resize_check`; R4 log without new errors or
warnings; `Invoke-Dx12Perf` Gate-0 R4 vs final R4: FPS +-1 %, every `[gpu]` field +-0.05 ms.
No C14 delta here (8.3, F1): R4 creates no depth alias, so its march skips nothing and writes no
sentinel, and `da_cloud_early.y = 0` makes the march history and the composite take the plain
bilinear read without the sentinel gather - `march` and `clouds` keep +-0.05 ms like every field.

### 6.5 R5 everything-off parity
`-dx12_no_rt -dx12_no_vrs -dx12_no_ms -dx12_no_async -dx12_no_sm6 -no_dstorage`,
`r__gpu_driven 0`, RT rows off: frames equal Gate-0 R5 on the four saves (known deltas as in 6.4).
FPS at most 1 % lower; `[gpu]` fields +-0.05 ms except the two passes of C14, a DX12 change that
stays on here (`-dx12_no_async` runs the pixel march with the early-out; there is no switch for it):
`march` drops where geometry hides the sky (A3, measured on R4 1920x1080: qa_pedro 1.156 -> 0.126 ms,
trees 1.061 -> 0.368, foliage 1.222 -> 0.954) and `clouds` reads the march with one extra
`GatherAlpha` per sky pixel. Both leave the field check, `frame` is compared as
`frame - march - clouds` (+-0.05 ms), and an FPS gain up to the `march` saving is expected.

### 6.6 R5 everything-on matrix
Defaults (async auto, VRS by preset, GD on, mesh on, DirectStorage on, DXC on) plus RT all three on
at Medium:
- `render_check` on `_preset Extreme`, `High`, `Low` with TAA, DLSS Quality, FSR Performance;
  `upscale_check` with both frame generations; `motion_check`; `resize_check`; `rtx_check`;
  `quest_teleport` level change twice (registries rebuilt, VRAM back); `options_tab`;
- the same `render_check` with `-dxdebug`, and at 1280x720 with `-dx12gbv`: no D3D12 error or
  corruption line;
- log: no `FATAL`, `device removed`, `refused`, `mixes DXIL`, `not joined`, `! [rt]`, `! [dxc]`;
- VRAM peak (sampler) at 1440p DLSS Quality + frame generation with everything on: below the
  8 GB card's budget with >= 300 MB headroom, else RT's budget rule drops foliage first (verified).

### 6.7 Performance summary
`Invoke-Dx12Perf` Gate-0 R5 vs final R5 at 2560x1440 Extreme TAA (`qa_pedro`, `foliage`, `fogqa`),
1920x1080 High, 1440p DLSS Quality; per-feature toggle table from the lanes' batches; RT cost per
tier; level-load time (cold DXIL cache vs cold DXBC cache; DirectStorage on/off). Written into
`DX12_RENDERER.md` and section 8 here.

### 6.8 Documentation
- `DX12_RENDERER.md`: new sections - native infrastructure, shader compiler (DXC/SM 6), async
  compute, variable rate shading, GPU-driven vegetation, mesh-shader grass, DirectStorage, ray
  tracing (RTX category); console contract rows (2.6), `r__caps` table, `r__gpu_log` keys,
  command-line switches, packaging (5) with the four DLLs.
- `TEST_MATRIX.md`: a "DirectX 12 features" section with one row per gate run of 6.4-6.6 and the
  lane gates, run names and pass criteria.
- `MODDING.md` "DirectX 12 and mod shaders": DXC compiles mod shaders (HLSL 2018, FXC
  preprocessing, what DXC refuses: duplicate `SV_Position`, non-immediate texel offsets,
  `uniform texture2D`), DXBC twins for a shader DXC cannot build, `r__dxc 0`/`-dx12_no_sm6`; the
  `DA_TREE_GD` block contract of tree vertex shaders; grass vertex shaders and
  `da_detail_common.h` (mesh grass eligibility); RT material classes from shader flags; the new
  Video rows.
- `DEPENDENCIES.md` (done at K1, re-checked), `README.md` and `packaging/dead-air-x64/README_RU.md`:
  the new DirectX 12 features for players and the two new third-party components.
- Each feature doc: status "implemented", measured numbers, removed parts with their numbers.
- This file: section 8 filled.

### 6.9 Packaging (the lead deploys; nothing here touches `D:\Games\Dead Air`)
- `runtime-files.txt` lists `dxcompiler.dll`, `dxil.dll`, `dstorage.dll`, `dstoragecore.dll`; the
  build stages them; the compatibility-archive runtime check passes against a temp output folder.
- The overlay carries every new shader (`.cs`, `.ms`, `.as`, `r3/rt/*.hlsl(i)`, headers) and the
  archive script picks them up (dry run into a temp folder, listing checked).
- No version bump. A handover note for the lead in section 8: files to deploy (runtime files,
  overlay, xdb) and the SHA list.

### 6.10 Cleanup
Scenario leftovers removed from the rig (`qa_water.script`, `qa_autoexec.ltx`), rig `user.ltx`
restored, only our own engine processes stopped, locks released, lane status files kept in `_work`.

## 7. Cross-lane risks and decision rules

### 7.1 Risks

| Risk | Where it bites | Mitigation |
|---|---|---|
| A half-finished edit breaks the shared build | every lane | 0.3 edit bursts, inbox, core's 30-minute rule |
| Hook placed wrong in a hotspot | skeleton | K2/I0 gates compare frames to Gate 0; lanes may add one-line hooks (0.2) |
| Cross-queue lifetime / fence misuse | infra I5, async | join-before-Present, separate timelines, GBV scenarios, 6.3 invariant review |
| VRS state leaking into shadow maps, native SDK sections, UI | vrs, infra, rt | scopes, layer resets, `vrs_clear/native/hazard`, 6.6 `-dx12gbv` |
| PSO format mixing | dxc, mesh, gd | `UnifyShaderFormats`, mix guard log, linkage checker |
| Shadow doubling or missing shadows at the RT handover, esp. with GD | rt, gd | C16, `rtx-trees` with GD on/off, `rtx-foliage` sway |
| Stand contention (9 lanes) | all | short runs, `-KeepShaderCache`, runs named and logged |
| Perf numbers confounded by other lanes | all | own toggles inside one DLL (0.5) |
| 8 GB VRAM with RT + mesh pool + FG | rt, mesh | budget checks, 6.6 VRAM gate |
| Untestable here: AMD/Intel, Windows 10, tile 8/32 VRS | all | caps-driven code, masks, logs naming what the adapter reported |

### 7.2 Decision rules when a gate fails

| Feature | Rule |
|---|---|
| DXC engine path | D4 rule (fix or pin passes to FXC); the path itself stays (mesh grass and compile time depend on it) |
| DXC payoff edits, copy queue, both Hi-Z variants, mesh tuning variants, DirectStorage geometry | kept only if they meet their gain threshold; otherwise removed |
| Async | A7: join split, march placement (C14); auto resolves to off on adapters that do not gain; the row stays (it is a player-facing performance setting) |
| VRS | vrs.md 9.4 ladder rule (High -> off if conservative misses 5 %, etc.) |
| GD phase 1 | must not lose FPS at equal clocks and must cut render-thread time; otherwise removed |
| Mesh grass | FPS(mesh) >= FPS(vertex) - 0.5 % after tuning; otherwise removed |
| DirectStorage | median batch >= 25 % shorter; otherwise removed |
| RT | visual gates are hard (sway, no doubling, no seams); budget misses are tuned per tier until within the table |

### 7.3 Rejected with a measured reason (not deferred)

NVIDIA NRD (licence, NRI integration), opacity micromaps (Agility SDK), the Agility SDK itself (not
needed on Windows 10 2004+), work graphs and enhanced barriers inside the layer (legacy tracking is the
single source of truth), `-enable-16bit-types` (turns 595 `h` literals into real fp16),
previous-frame Hi-Z (TAA flicker), GDeflate for shipped or cached textures (archives are LZO/raw;
our HD bundle shrinks only to 0.833; a 2.7 GB local cache for a CPU-bound share that already overlaps
parsing), GD phase 4 (superseded by mesh grass, C17).

## 8. Results (filled by `core` during and after the run)

- Gate 0: runs, noise floors.
- Per lane: milestone reached, gates, decisions, removed parts with numbers.
- Final gate: 6.1-6.10 outcomes, the performance summary, open items (must be empty).
- Handover for the lead: deploy list and SHA-256.

### 8.1 Gate 0 (reached 28.09.2026 04:55)

- Build of the tree as it was: `ninja: no work to do` - no fix needed (3.0 step 2).
- Baseline snapshot: `_work\dx12-features-20260928aseline\{bin,gamedata}` with `SHA256.txt`. Taken
  02:04, last retaken 04:21 after the determinism fixes below; no lane had edited a built file.
- Layer tests: floor **36** passed, 0 failed (`core-g0-layertests`).
- Determinism (QA paths only; ordinary play never takes them). Two frozen-wind runs of `foliage`
  differed on 34 % of the frame. Causes and fixes: `wind_seed` restarted the wind clock but not its
  accumulators (field scroll, tree phase, cloud run, water runs, smoothing) nor the legacy Perlin gust
  input (per-session random seed, device clock) - `CEnvironment::wind_reseed` resets them all and
  evaluates one zero-length tick; grass clocks kept running under `wind_freeze` and every session drew
  the blades' wave groups from the global `::Random` (`DetailManager.cpp`,
  `DetailManager_Decompress.cpp`: frozen, restarted on reseed, per-slot generator); crown springs kept
  ringing (`FTreeVisual.cpp`: held at the frozen target); the weather clock moved a few tenths of a
  second more in one run than in another (new QA command `env_time_pin h:m[:s]`); the HUD weapon's idle
  motion and eye adaptation after the forced weather. Parity mode (`qa_tga`) of `render_check` and
  `upscale_check`: game time stops and the weather clock is pinned (save's ten minutes) on the first
  frame, the weather is forced on the second, the HUD weapon is hidden (`qa_keep_hud` keeps it),
  adaptation is instant until the shot, `wind_seed 1833.7` + `wind_freeze 1` at the stage, one
  lossless frame. Foliage after: 2.1 % moving pixels over three runs.
- References exist three times (`a`, `b`, `c`): a floor made of two runs did not hold a fourth run of
  the same binaries. `Compare-Frames.py` with three references calibrates "equal" leave-one-out (the
  new run must not look worse than the worst reference against the other two, with small slack);
  a fourth run of the same code passes on foliage R5 and R4, synthetic 30x30 px and +3 % changes fail,
  and so does a 12x12 px +30/255 patch on qa_pedro.
- Noise floors of the three references (moving % / dilated % / PSNR, 1920x1080 Extreme, SMAA):
  qa_pedro R5 0.36 / 5.3 / 56.0 dB, R4 0.73 / 10.3 / 54.8 dB; foliage R5 2.05 / 24.6 / 48.7 dB,
  R4 1.23 / 15.1 / 51.5 dB; trees R5 0.98 / 10.9 / 56.8 dB, R4 1.76 / 19.9 / 45.8 dB; fogqa R5
  13.0 / 48.4 / 39.7 dB, R4 7.8 / 40.2 / 41.7 dB (its campfire light, marsh fog and particles run on
  the device clock - use the other three for tight parity). Runs `core-g0-render-<save>-<a|b|c>[-r4]`;
  `upscale_check` `core-g0-upscale-<a|b|c>[-r4]` (one shot per step, 46-58 dB per step; the R5
  `vectors` step shows the QA overlay in some runs only), `motion_check` `core-g0-motion-<a|b>`,
  `options_tab` `core-g0-options-<a|b>[-r4]` (43 PASS each).
- Perf reference (2560x1440 Extreme TAA, `core-g0-perf-pedro`, `core-g0-perf-foliage`): qa_pedro R5
  57.2/56.6 FPS, R4 55.3/56.1 FPS; foliage R5 57.9/57.4, R4 56.1/56.3. R5 runs 4-5 % lower GPU clocks
  than R4 at the same power, so no R5/R4 pair meets the 3 % clock rule; at 1300 MHz the frame is R5
  16.79 / R4 17.25 ms (qa_pedro) and R5 14.96 / R4 15.82 ms (foliage).
- K1: the runtime-files comparison 3.1 names lives in `tools/package/build_dead_air_x64_installer.ps1`,
  not in the compatibility-archive script; it passes against a temp copy of the Release output.

### 8.2 K2 and K3 (reached 28.09.2026 06:40)

- K2 skeleton: every stub file of 2.3, every hook of 2.5, the console contract of 2.6 in `[dx12:<lane>]` blocks,
  the `r__caps` keys (one hook, `da_native_caps.cpp`), `CHW::native`, the timer slots + `RegisterField` + `LogLine`,
  the backend plumbing, `rt_static`/`rt_skip_static`, the lane pointers, the name blocks; `-sjitter` and `-skinw` are
  no-ops. Gate: both modules build; layer tests 45/0 (floor 40 after I1); engine-isolated `render_check`
  (`core-k2-renderb-<save>[-r4]`, Gate-0 overlay) equal Gate 0 on qa_pedro, foliage and trees for R5 and R4 against the
  references plus a same-period snapshot control (0.6); `upscale_check` R5 all 13 steps; `[preset]`/`[gpu]` parse;
  `r__caps` prints every key.
- K3 Video tab: `async_compute` (screen), `dstorage` (quality), the RTX group (`rt_reflections`, `rt_shadows`, `rtao`,
  `rt_quality`) between anti-aliasing and image, greyed with their reasons by caps and by the renderer combo. Gate:
  `Check-OptionHints.py` clean; `options_tab` 50/50 on R5 (`core-k3-options`) and R4 (`core-k3-options-r4`, `-r4-b`).
- Found on the way: a slow load moved the parity scenarios' weather pin across a ten-minute boundary (now pinned per
  reference save); the snapshot itself fails "equal" under eight lanes' load (control run added to 0.6); the stand
  helper aborted on a previous holder's exiting engine (fixed).

### 8.3 Final gate

- 6.2 `dumpbin /imports xrRender_R4.dll` (finding SG-1, accepted as explained): not the Gate-0 list word for word
  (`final\r4-imports-g0.txt` vs `final\r4-imports-final.txt`, `final\cmp_imports.py`), but the same 30 modules and no
  new DLL. Each difference was re-read in the code; all come from R4 code this plan puts into R4, none is D3D12, DXC
  or DirectStorage:
  - `+` kernel32 `GetFileAttributesExW`, CRT `wcscat_s`: `r2_test_hw.cpp` `GetD3D12RuntimeSignature`, the d3d12.dll
    stamp of the D3D12 caps cache R4 reads for `r__caps` `rt_tier` (2.6, core K2);
  - `+` kernel32 `QueryPerformanceFrequency`, msvcp140 `_Query_perf_counter` / `_Query_perf_frequency`: the `[texload]`
    timing - `da_texload_ticks` (QPC) in `da_dstorage.cpp` and the `std::chrono::steady_clock` wait in
    `ResourceManager.cpp` `TextureUploadPool::Wait`, both modules by design (dstorage.md S3, ds);
  - `-` CRT `strncpy`: `ShaderResourceTraits.h` `CreateShader` copies the shader name with a bounded `CopyMemory`
    plus terminator (dxc);
  - `-` xrEngine `?water_ripple_edge@CEnvironment@@2MB`: `r4_rendertarget_phase_water_ripple.cpp` copies the
    `static constexpr` into `DaRippleStep::edge` instead of binding it to `set_c`'s reference (async).
  DECISION: no code change. 6.2's "unchanged" is read as "no new module and nothing of D3D12/DXC/DirectStorage in
  R4", which this list meets. DX11 intactness is 6.4's verdict (R4 `render_check` equal to Gate 0, FPS +-1 %). If 6.4
  fails on load time or FPS, the per-batch `[texload]` path in R4 is looked at first.
- F1 (async/core review, C14 on DX11) - the cloud-march early-out ran on R4 too: `da_async_targets_create` made the
  `$user$cloud_depth` alias in both modules, so R4 skipped hidden texels (A3: R4 `march` qa_pedro 1.156 -> 0.126 ms)
  and, under camera motion, reweighted its history next to silhouettes where Gate 0 blends. DECISION: DX12 only,
  because 0.1 ("DX11 frames and FPS unchanged") ranks above C14 (0.8); this supersedes the contract lane's docs-only
  reading of the same finding (async-cr-5: `march` out of 6.4's field check). Code: the alias is created under
  `XR_D3D12` only (`da_async.cpp`); `da_cloud_early.y` (alias exists) gates the sentinel-aware read in
  `da_clouds_hist.h`, so without it the march history and the composite do Gate 0's single `SampleLevel`; the march's
  depth gathers sit in a nested branch (`&&` does not short-circuit in HLSL, the four `GatherRed` ran with the
  early-out off). R5 is unchanged: the pixel path keeps the early-out for A7's "off + early-out". 6.4 is back to the
  strict reading; 6.5 names the two C14 passes. Notes: `final\fix-async-core-f1.md`.
- Final stand, functional part (28.09 18:38-20:03, 26 short runs; `final\stand.md`, fixes `final\stand-fixes.md`):
  build + layer tests 81/0; async verify (A5) port + schedule 0 texels, 0 guard joins after S1; look-neutral features
  (async, GD 1, mesh grass 1, DirectStorage, DXC) equal to the same tree's DXC-only control + Gate 0 on qa_pedro and
  foliage (the tree's own margin against Gate 0 is DXC + the GTAO corner, 51.8 / 46.7 dB, >= 45); DirectStorage
  verify 1086 textures + 64 geometry buffers, 0 mismatches; R4 `render_check` equal to the A3 GTAO-corner control
  (54.0 / 46.5 dB against Gate 0), options_tab 50/50 on R4 and R5, R4 imports unchanged; VRS rows met on both saves
  after S3; RT effects reviewed on qa_pedro, fogqa, trees (sway kept); `-dxdebug` + everything on at 1280x720 with
  `vid_restart` and a level change: no D3D12 error line after S4. Fixes:
  - S1 (layer, infra): `ExecuteCommandList` requires an untransitioned read-only requirement with `Satisfies`, not
    exactly - a deferred list sampling a read-shared texture joined the job (`async H` added).
  - S2 (engine backend, R5 only): stale shader stages after `CBackend::Invalidate` made D3D12 refuse pipelines
    (tree shadow VS + aref PS of the previous light batch); the first set of each stage after it now binds.
  - S3 (VRS): `SkyClouds` removed (turn blockiness +24.5 / +32.9 % against 5 / 15 %; without it +0.17 / +0.42 %),
    DECISION by the vrs.md 13.6 rule (weakest pass goes); `SunshaftsBlur` stays; `r__vrs_passes` 0..255.
  - S4 (engine backend, R5 only): GPU markers of pooled backends drawing on the immediate context went into a
    deferred list nobody executes (with `-dxdebug`): now marked on the context the draws go to.
  - S5 (async): a pooled context with stray recorded work is cleaned and used instead of turning every later job
    into the direct-queue fallback.
  - S6 (QA): `rtx_check` sway shots 1.2 s apart.
  Open for the Perf phase: every perf gate (A7, V7, G6-G8, mesh 9.4, S6-S9 timings, I6/I7, D5, RT budget), the
  default flips of `r__gpu_driven` / `r__grass_mesh`. Outside the DX12 work: one crash in the base engine's parallel
  particle `OnFrame` after eight reloads in one session (stand-fixes.md). The static gate's hashed binaries are
  superseded: the lead deploys build 8 (SHA-256 in stand.md).

### 8.4 Perf phase and final outcome per lane (28.09.2026)

The Perf phase ran the async and VRS batches under the cap of 0.1a, then, at the user's request, one measurement
pass instead of the remaining on/off pairs: `final-onepass-foliage` (2560x1440 Extreme TAA, RTX 3070 Laptop,
everything on - async forced on, `r__vrs 1`, GD 1, mesh grass 1, RT shadows + RTAO + reflections at High,
DirectStorage). 49 FPS, GPU frame 20.2 ms; per frame (ms):

| `scene` | `sun` | `lights` | `march` | `combine` (`ao`) | `rt_scene` | `rt_sun` | `rt_ao` | `rt_refl` | `vrs` | `gd` | `grass` | `cq_ripple` | `join` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2.06 | 5.9 | 1.40 | 1.44 | 4.13 (1.82 = RTAO) | 0.38 | 1.75 | 1.83 | 0 (no water; 0.76 on the fogqa ponds) | 0.18 | 0.12 | 1.86 (2.5 ms CPU) | 1.04 (overlapped) | 0.035 |

RT scene 417 BLAS, 1.57 M triangles, 42.7 MB compacted, TLAS 7897 instances, 659 ms at load; 0 shader, DXC or D3D12
error lines. The stand logs of the functional part (same tree, 1920x1080 Extreme SMAA): foliage 68.9 FPS DXC-only
(`final-dxconly-foliage`) against 81.4 with the look-neutral features on (`final-on-foliage`: GD, mesh grass,
DirectStorage, async), render-thread grass 8.6 -> 1.8 ms; qa_pedro 64.5 -> 65.1 (`final-dxconly-pedro` ->
`final-on-nods-pedro`).

| Lane | Outcome |
|---|---|
| `core` | `r__gpu_driven` and `r__grass_mesh` default to 1 (look-neutral PASS of the final stand). RT stays outside every preset, in its own "RTX" Video category; its cost above is the player's price per effect. The particle crash of the reload loop (base engine, `final-robust-level-nort`) is fixed |
| `infra` | I0-I6 in; I7 copy queue on by default (C23 applied): `level.geom` 256 -> 214 ms, `level.geomx` 58 -> 47 ms, texture batch create 611 -> 548 ms, 616 MB of creation-time uploads off the direct queue in the variants pass; `-dx12_no_copy_queue` replaces the opt-in. Zero-cost reading from the masked batch variants: every timed pass within +-0.05 ms, FPS +1.2 / +2.7 % for the masked device from the frame span outside the timed passes (infra.md: audited, what availability cost is now paid on use - compute queue, RT pass pipelines, level walk, `join` stamps; not re-measured) |
| `dxc` | DXC at SM 6.6 is the R5 shader path (mesh grass and RT need it; D4: no pass slower); FXC the fallback (`r__dxc 0`, `-dx12_no_sm6`). Not A/B-measured in the Perf phase; `-dxc_all_bound` stays an unmeasured QA switch |
| `async` | `perf-async-pedro`: off 54.3 FPS, on 50.0, on + pixel march 52.1 (-3.9 % against off over 2 valid pairs), on + split joins 51.6: on NVIDIA Ampere the extra work in flight lowers the clock of the power-limited GPU (1291 -> 1228 MHz) more than the overlap saves. Removed: the compute cloud march (`da_clouds_march.cs`), `r__async_march`, `r__async_join`, the split joins (C14: the pixel march after the G-buffer with its early-out wins). Auto = pixel passes on NVIDIA ("NVIDIA: no measured gain"), compute queue on AMD and Intel discrete; the off/auto/on row stays. Jobs left: GTAO and the water ripple |
| `vrs` | `perf-vrs-pedro`: Extreme conservative +3.9 % (2 valid pairs), aggressive +4.8 %; `perf-vrsladder-pedro`: Low aggressive -1.9 %, Default conservative -1.0 %. `vrs_by_preset = {0, 0, 0, 1, 1}` (`xrRender_console.cpp`): conservative on High and Extreme only. The sky/cloud pass left the VRS pass set (stand fix S3) |
| `gd` | Default 1; `gd` 0.12 ms a frame. Phases 2 and 3 (`r__gpu_driven_hiz`, `r__gpu_driven_main`) removed with their code: in the variants pass `gd` 0.12 -> 0.34 ms, `scene` 2.07 -> 2.26 ms, frame 20.30 -> 20.41 ms, FPS 49.0 -> 47.8 |
| `mesh` | Default 1; `grass` 1.86 ms GPU at 1440p, render-thread grass 8.6 -> 1.8 ms at 1080p; the mesh preset columns equal the base columns. Variants pass: the G-buffer Hi-Z removed (`grass` 1.89 -> 1.94 ms, no saving); the depth-only shadow elements are the mesh shadow path without a switch (look-neutral by construction, `sun` 5.95 -> 5.77 ms) |
| `ds` | On by default for textures and level geometry (S9 kept as built); one-pass early batch 1138 loads (1085 through DirectStorage, 53 CPU, 0 failed), 1417 MB in 678 ms. The load-time A/B was not run |
| `rt` | RTX category unchanged: `rt_sun` 1.75, `rt_ao` 1.83 (replacing GTAO), `rt_refl` 0.76 on water, `rt_scene` 0.38 ms at High. Low/Medium costs and the VRAM peak with frame generation not measured |

Open (none blocks the release; all are data or measurement, no code defect):
- Hint strings that lag the decisions (`text\eng|rus\dead_air_x64_video.xml`): `st_da_x64_video_hint_preset` says VRS
  runs on "every preset but the highest" (now High and Extreme only); `st_da_x64_video_hint_async_compute` names the
  clouds among the async work.
- Not measured: DirectStorage load time, DXC against FXC in the final tree, RT Low/Medium and VRAM peak, C30 (VRS
  quality with RT shadows), other GPU vendors.
- Handover for the lead: `_work\dx12-features-20260928\final\HANDOVER.md`.
