# GPU-driven vegetation (ExecuteIndirect)

Status: implemented (28.09.2026). G0-G8 in the tree; the functional part of the final test phase
passed (look-neutral with every other feature, sway with RT shadows, level change under `-dxdebug`) and
`r__gpu_driven` defaulted to 1 after the Perf phase. **Since 29.09 the default is -1 (auto, 3.13)**: the
GPU path runs while the CPU is the frame's bottleneck, and local light faces always cull their trees on
the CPU. Phase 2 (`r__gpu_driven_hiz`, same-frame Hi-Z of
the main view's instances) and phase 3 (`r__gpu_driven_main`, the main view's nodes and impostors on the
GPU) were measured in the final one-pass run and **removed** - switches, code paths and `da_gd_hiz.cs`:
they cost more than they save (`gd` 0.12 -> 0.34 ms, `scene` 2.07 -> 2.26 ms; 3.8, 3.9). Numbers:
"Final test phase, performance" at the end.
Module: `xrRender_R5.dll` (DirectX 12) only. `xrRender_R4.dll`
(DirectX 11) compiles the same sources and keeps today's CPU path (dsgraph + `render_tree_batches`)
bit for bit. Builds on `infra.md` (indirect draws 3.5, `NativeCaps`); needs no DXC, no async queue,
no bindless.

## Integration amendments (PLAN.md)

Implemented by the `gd` lane (milestones G0-G8 of `PLAN.md` 4.3). These points supersede the text
below where they differ (ids refer to `PLAN.md` section 1):
- **C10**: buffer SRV/UAV views (raw, structured, typed), committed tracked SRV/UAV buffers,
  `MISC_DRAWINDIRECT_ARGS`, indirect draws and the count variants are built by the `infra` lane (I4);
  the five layer scenarios of 9.1 live in infra's `scenarios_views.cpp`. This lane edits no layer file.
- **C4**: the shaders of section 6 are SM 5.0-compatible HLSL, created through `CResourceManager`;
  their format follows the session's mode (DXIL with DXC on R5, so the GD VS variants pair with the
  DXIL pixel shaders; DXBC with `-dx12_no_sm6`). "FXC SM 5.0 only" no longer holds.
- **C16, ray tracing**: the RT level walk must see the nodes this lane moves into
  `CSector::gd_nodes` - expose them through `da_gpu_trees_for_each_node(sector, fn)`. Keep a
  per-instance `rt_static` bit (from `dxRender_Visual::rt_static`), refreshed by
  `da_gpu_trees_refresh_flags()`, which the RT lane calls after classifying visuals; the SMAP cull
  skips `rt_static` instances when the view's `o.rt_skip_static` is set (near and middle cascades
  with RT shadows), otherwise rigid MU props would cast twice. Swaying roots are never `rt_static`.
  G1's `FTreeVisual` read accessors also serve the RT lane (xform, window-0 geometry, foliage,
  `rigid()`, root, height).
- **C17**: phase 4 (grass) is dropped: grass on mesh-capable DX12 GPUs is `mesh.md`, elsewhere the
  vertex path.
- **Phase 3 is in scope** (G8): its design is written first into 3.9 of this file (node test port,
  Hi-Z replacing HOM, impostor generation, parity rules), then implemented and gated like phase 1;
  merged only if it passes. Implemented, measured and removed (3.9).
- **Ownership**: `core` pre-registers `r__gpu_driven`, the `gd` key of `r__caps` (from
  `da_gpu_trees_live()`), the `GdCull` timer slot and `gd` field, implements
  `CBackend::RenderIndirect` / `set_InstanceStream`, and pre-places `da_gpu_trees_*` calls in
  `r2_R_render.cpp` and `r2_loader.cpp`. The instanced declaration goes into this lane's `gd` blocks
  of `ResourceManager.h` / `dx11ResourceManager_Resources.cpp` (owner `dxc`). Extra pipelines for
  `Check-ShaderLinkage.py` go into its `gd` block.

## 1. Goal

Move per-instance culling and draw submission of the level's instanced vegetation - the
multiple-use models compiled to the tree format (trees, bushes, stumps, logs drawn with
`flora\leaf_wave` / `flora\trunk_wave`) - from the CPU to the GPU on R5:

- persistent GPU copies of every eligible instance, culling node and wind root, uploaded once per level;
- the per-root wind oscillator integrated on the GPU once per frame (a port of
  `FTreeVisual::UpdateWindState`), so nothing per tree is uploaded per frame;
- per view (main camera, each sun cascade, rain map, each shadowed local-light face, z-prefill) a
  compute cull + compaction that writes `DrawIndexedInstanced` argument records and a compacted
  instance-reference stream;
- one `ExecuteIndirect` per (pass, geometry) group instead of today's ~1000 tree draws a frame;
- optional same-frame Hi-Z occlusion for the main view (phase 2: implemented, measured, removed - 3.8).

The output is the image of the CPU path: the same instances per view, LOD windows, shader elements,
passes and wind bend in every pass (G-buffer, motion vectors, sun cascades, lights, rain map) - plant
shadows keep swaying in every shadow technique. Nothing changes the look, so there is no preset and
no menu row, only a QA kill switch.

Out of phase 1: grass (`CDetailManager`, phase 4 in 3.9), the far impostors (`FLOD`, stay on the CPU:
phase 3 was removed), `tree_s` props (`def_shaders\def_objects_lod`, 55 visuals on l01_escape), DirectX 11.

## 2. Current code

### 2.1 Data

- `FTreeVisual` (`src/Layers/xrRender/FTreeVisual.h:33-86`): one visual = one placed instance:
  `xform` (world, from `OGF_TREEDEF2`), `c_scale`/`c_bias` (rgb, hemi, sun), `rm_geom` over the level
  VB/IB (`FTreeVisual::Load`, `FTreeVisual.cpp:124-219`). `FTreeVisual_ST` draws one index range
  (`:548-554`), `FTreeVisual_PM` a sliding window chosen by `SelectLOD` from `calcLOD(ssa)`
  (`:589-607`; `calcLOD`, `r__dsgraph_render.cpp:33-36`).
- `TreeWindShared` (`FTreeVisual.cpp:23-47`): one damped oscillator per root (the visuals of a model
  share the root, keyed by 5 cm cells, `:62-87`), `foliage` gate from the `_aref` pixel shader
  (`:175-181`), integrated on the CPU by the first visual drawn in a frame (`UpdateWindState`,
  `:332-413`) with the gust field of `da_wind_core.h` (the same file the CPU compiles:
  `Environment.cpp:624, 666-674`).
- Instance rows: 11 float4 (`FTreeVisualInstanceData`, `FTreeVisual.h:14-21`; `FillInstanceData`,
  `FTreeVisual.cpp:487-508`): world 3x4, world-view 3x4 (per view), c_scale, c_bias, row 8 (sun
  scale/bias, oscillator state `q`, frequency factor), row 9 (height, root gust, deviation,
  validity + foliage gate), row 10 (previous-frame state for motion vectors).
- Levels: l01_escape 7848 tree-format visuals (93 rigid), k00_marsh 8830 (279 rigid)
  (`[wind-veg]` line, `r2_loader.cpp:179`); `-wvdbg` lists 47 `flora\*` shader kinds and 22
  `def_objects_lod` kinds on l01_escape.

### 2.2 Culling per view

| View | Where | Phase / context |
|---|---|---|
| main | `render_main::calculate`, `r2_R_calculate.cpp:37-64` | NORMAL, immediate, HOM |
| z-prefill | `r2_R_render.cpp:280-310` | SMAP, immediate, HOM; `r2_zfill off` by default |
| sun cascades x3 | `render_sun::calculate` `render_phase_sun.cpp:352-385`, `render` `:397-455` | SMAP, 3 deferred contexts in parallel |
| rain map | `r3_R_rain.cpp:298-311, 317-343` | SMAP, aux context |
| shadowed light faces | `calc_lights` `r2_R_lights.cpp:254-277`, `flush_lights` `:156-215` | SMAP, aux contexts, tasks |

Every view walks the sector/portal graph (`build_subspace_static`, `r__dsgraph_build.cpp:822-899`):
for each visited sector and each portal-clipped frustum (`CSector::r_frustums[context]`,
`r__sector.h:94`; at most 12 planes, `Frustum.h:22`) `add_static(root)` recurses (`:632-773`):
`CFrustum::testSAABB` (`Frustum.h:131-161`), HOM for the main view (`CHOM::visible`,
`HOM.cpp:453-486`, with a random 10-25 frame "stay visible" delay), the value filter
`is_static_geometry_valuable` (`:49-75`, sticky per context through `vis.valuable_frame[ctx]`).
Trees hang under `FLOD` nodes (`FLOD.h:36-38`): `ssa*lod_factor < r_ssaLOD_A` inserts the impostor
into `mapLOD` (or returns below `r_ssaVEG_DISCARD`), `ssa > r_ssaLOD_B` **or PHASE_SMAP** adds every
child through `add_leafs_static(child, true)` (`:734-758`, `:479-507`): no frustum test, HOM per child
for the main view (`:411`). `insert_static` (`:218-311`): SSA = R/d^2 from the **device camera in
every view**, discard below `r_ssaDISCARD`, element by `rimp_select_sh_static` (`r2.cpp:69-79`:
HQ/LQ by `r_dtex_range` in NORMAL, `SE_R2_SHADOW` in SMAP), one item per pass into
`mapNormalPasses[priority][pass][SPass*]`. `smapvis` (`light_smapvis.cpp:32-86`) retires occluded
static casters of a light with occlusion queries over frames.

### 2.3 Draw submission

`render_graph(0)` (`r__dsgraph_render.cpp:336-403`) sorts passes and items, gathers trees whose
root still needs this frame's wind and runs `FTreeVisual::PrepareWind` (`:360-370`, parallel), then
per pass `render_tree_batches` (`:143-275`): collects tree items, sorts by geometry + LOD bucket,
segments of up to 64 equal draws packed into pages of the `tree_instance_data` cbuffer (CB3[704] =
11264 bytes, uploaded whole), `FillInstanceData` per instance per view, one `RenderInstanced` per
segment. Singletons go through `FTreeVisual::Render` (`FTreeVisual.cpp:437-471`: 12 constants into
the VS `$Globals` CB0[65] = 1040 bytes) and `DrawIndexed`. Kill switch `r__tree_batch`
(`r__dsgraph_render.cpp:28`, `xrRender_console.cpp:1763-1765`).

### 2.4 Shaders

`deffer_tree_flat.vs`, `deffer_tree_bump.vs` (G-buffer; the `_d` / `-hq` files of the game archive
wrap them, names from `uber_deffer.cpp`) and `shadow_direct_tree.vs` (`_aref` wraps it) in the overlay
`packaging/dead-air-x64/compatibility/gamedata/shaders/r3`. Instanced mode when
`tree_instance_control.x > 0.5` (`tree_instance.h`): rows from the cbuffer at `SV_InstanceID +
control.y` (`deffer_tree_bump.vs:30-39`, `shadow_direct_tree.vs:31-36`). The bend is `da_tree_bend.h`
in all three (sway, flutter, motors, stiffness); the G-buffer VS bends a second time with the previous
wind for motion vectors (`da_wind_prev_on`). Register map of `deffer_tree_flat.vs`: cb0 `$Globals`,
cb1 `static_globals`, cb2 `da_motion_view`, cb3/cb4 tree instance buffers - all root CBVs on R5.

### 2.5 D3D12 layer

- `DrawIndexedInstancedIndirect` / `DrawInstancedIndirect` / `DispatchIndirect` log "not supported"
  (`d12_context_state.cpp:1276-1289`); `infra.md` 3.5 implements them and the count variants.
- Buffer SRV/UAV views are refused: `ViewResource` "The engine creates no buffer views"
  (`d12_objects.cpp:977-982`). UAV buffers are committed and tracked (`Buffer::Create`,
  `:1337-1397`); tracked vertex buffers and SRV buffers get their transitions at every draw
  (`EnsureDrawStates`, `d12_context_state.cpp:582-632`); per-instance input layouts exist
  (`d12_device.cpp:925-988`); `kMaxSRVs` = 32 (`d12_layer.h:59`); the layer inserts a global UAV
  barrier between consecutive UAV users (`d12_context_state.cpp:1176-1186`).
- Draw cost on the layer 0.30 us against 0.73 us on the D3D11 driver (`DX12_RENDERER.md` section 4):
  what costs CPU is the engine work per tree, not the API call.

### 2.6 Baseline (measured for this design)

Stand `D:\Games\Dead Air\_qa\lightdiag`, R5, 1920x1080 Extreme, TAA, `-always_active`,
RTX 3070 Laptop. Instance counts from hardware execute breakpoints on
`R_dsgraph_structure::render_graph` (caller = view), `FTreeVisual::FillInstanceData` (batched
instance), `FTreeVisual_ST::Render` / `FTreeVisual_PM::Render` (single draw) over 12 frames, then
detach (probe `gd_count.py`, becomes `tools/qa/gpudriven/count_tree_draws.py`, step 0). Per frame:

| Save (level) | View | Tree instances | batched | single draws |
|---|---|---|---|---|
| qa_pedro (l01_escape) | main | 854 | 582 | 272 |
| | 3 cascades | 1455 | 1000 | 455 |
| | 8 light faces | 0 | 0 | 0 |
| trees (l01_escape) | main | 397 | 258 | 139 |
| | 3 cascades | 1446 | 1040 | 406 |
| | 12 light faces | 2 | 0 | 2 |
| foliage (l01_escape) | main | 585 | 372 | 213 |
| | 3 cascades | 1047 | 695 | 352 |
| | 39 light faces | 2 | 0 | 2 |
| fogqa (k00_marsh) | main | 560 | 406 | 154 |
| | 3 cascades | 981 | 727 | 254 |
| | 6 light faces | 4 | 0 | 4 |

(trees/foliage counts are normalised by `render_forward` calls: the HUD self-shadow's own
`render_graph(0)`, `r4_rendertarget_phase_hud_shadow.cpp:118`, doubles the raw main-view count there.)

`-dxstats` draws per frame, `r__tree_batch 1` / `0` (same pose): qa_pedro 4383 / 5630, trees
~2350 / ~3260. Batched draws = batched instances - difference: qa_pedro 1582 - 1247 = **335**,
trees 1298 - 906 = **392**. Tree draws a frame: qa_pedro 335 + 727 = **1062 (24 % of all draws)**,
trees 392 + 547 = **939 (40 %)**.

Game-thread profile (`Run-Profile.ps1 -ExtraArgs "-r5"`, 15 s, 1 kHz; sun cascades record on workers
and are mostly outside it):

| Save | FPS | `render_tree_batches` | `FTreeVisual_ST/PM::Render` | `UpdateWindState` | sum (main view only) |
|---|---|---|---|---|---|
| qa_pedro | 51.5 | 2.86 % | 0.72 + 0.95 % | 0.28 % | 4.8 % = **0.93 ms/frame** |
| qa_pedro (2nd run) | 65.3 | 3.10 % | - | 0.22 % | ~4.7 % = ~0.72 ms |
| trees | 38.6* | 1.47 % | 0.49 + 0.57 % | 0.12 % | 2.7 % = 0.69 ms |

\* another agent was compiling. About **0.9-1.1 us of render-thread time per tree instance**, before
the culling itself (`add_static` 4.1-4.6 %, `add_leafs_static` 2.2-2.6 %, `insert_static`
0.8-0.9 % of the game thread in qa_pedro, trees a large share of them). Dynamic constant uploads for
trees on qa_pedro: 727 x ~1 KB `$Globals` + ~40 pages x 11 KB = **~1.1 MB/frame**.

### 2.7 Gate-0 counts (G0, 28.09.2026)

Tool: `tools/qa/gpudriven/count_tree_draws.py PID` - the probe of 2.6 made robust. Hardware execute
breakpoints on `render_graph`, `FTreeVisual::FillInstanceData`, `FTreeVisual_ST::Render` and
`FTreeVisual_PM::Render` in every thread; the view of a pass comes from its call site through the
PDB **including inline frames**: `CRender::render_hud_shadow` is inlined into `CRender::Render` and
`smapvis::end` into the lights task, which is why the 2.6 probe (caller function names) counted the
HUD self-shadow pass as a second main pass and had to normalise by `render_forward` calls. A frame
ends at the forward pass (`render_graph(1)` in `phase_combine`): the sun cascades record on workers
from `Calculate()` on, concurrently with the main pass, so main-pass boundaries would split them
across frames. One warm-up frame, then `--frames` frames; the breakpoints are cleared with every
thread stopped and pending events are drained before the detach. `--sites <dll>` lists and classifies
every direct `render_graph` call of a build offline (a new call site shows up as `other`) and fails
when one of the four breakpoint targets does not resolve to exactly one symbol of the PDB.
`tools/qa/gpudriven/Run-TreeDrawCount.ps1` is the stand run: `render_check` in the parity recipe of
PLAN 0.6 (the Gate-0 references' arguments: 1920x1080 Extreme, SMAA, `wind_seed 1833.7` +
`wind_freeze 1`, weather `af3_bright_clear_4` with the weather clock pinned to the references' minute,
game time stopped) with a 40 s FPS window; the counter attaches after the lossless shot. The script
holds `stand.lock` until the counter has detached and the engine has exited (`-NoParity`: the
free-running `render_check` of 2.6, TAA, clocks running).

Gate-0 snapshot (`-Bin/-Overlay baseline -ExtraModules xrEngine`), R5, 12 frames per run. Instances
per frame (batched + single draws), passes in brackets; every view had the same count in every frame
of a run:

| Save | main | 3 cascades | light faces | HUD shadow | shadow views total | all | runs |
|---|---|---|---|---|---|---|---|
| qa_pedro | 854 (582 + 272) | 1430 (981 + 449) | 5 (14 passes) or 0 (8 passes) | 0 (0 passes) | 1435 or 1430 | 2289 or 2284 | `gd-g0-count-pedro-c`, `-a`, `-b` |
| trees | 397 (258 + 139) | 1239 (876 + 363) | 2 (12 passes) | 0 (1 pass) | 1241 | 1638 | `gd-g0-count-trees-a`, `-b` |
| foliage | 585 (372 + 213) | 1059 (706 + 353) | 2 (39 passes) | 0 (1 pass) | 1061 | 1646 | `gd-g0-count-foliage-a`, `-b` |
| fogqa | 560 (406 + 154) | 983 (729 + 254) or 981 (727 + 254) | 4 (6 passes) | 0 (0 passes) | 987 or 985 | 1547 or 1545 | `gd-g0-count-fogqa-a`, `-b` |

Against 2.6: the **main view is identical on all four saves**, instances and the batched/single split
(854 = 582 + 272, 397 = 258 + 139, 585 = 372 + 213, 560 = 406 + 154), and so are the light faces of
trees (12 / 2), foliage (39 / 2) and fogqa (6 / 4); qa_pedro has one shadowed point light more (six
faces, five tree instances) in some runs only (runs a and c: 14 / 5; run b and 2.6: 8 / 0). The
cascades differ: qa_pedro 1430 (2.6: 1455), trees 1239 (1446), foliage 1059 (1047), fogqa 983 (981).
The parity recipe forces the weather `af3_bright_clear_4`, so the sun stands elsewhere than in the
free-running runs of 2.6 on the save's own cycle: with `-NoParity` (TAA, clocks running, as in 2.6)
the Gate-0 binaries give trees **1446 (1040 + 406), 2.6 exactly**, and qa_pedro 1457 (1003 + 454)
against 1455 (`gd-g0-count-trees-free`, `gd-g0-count-pedro-free`). The minute of the pin does not
matter at this scale: trees pinned at 9:10 (a slow load crossed the ten minutes; runs parked in
`qa\_superseded_gd`) and at 9:00 (the references' pin, `qa_env_pin` of Invoke-Dx12Stand) counted
the same. Run to run the cascades are equal on qa_pedro,
trees and foliage and differ by 2 instances on fogqa (983 / 981, 0.2 %), inside the +-0.5 % shadow
tolerance of 9.3. The HUD self-shadow pass never draws trees (its context only receives the HUD's
casters). `gd-g0-count-pedro-a` ran the counter's first version (log flushing on) and `-b` its second
(frames cut at the main pass, so its per-frame split of the cascades is off); their main and cascade
totals equal `-c`.

These are the references of the 9.3 count check: a later build with `r__gpu_driven 0` must reproduce
them with `Run-TreeDrawCount.ps1` on the same recipe (the CPU path unchanged; qa_pedro's light faces
and fogqa's cascades within the run-to-run spread above), and the `[gd]` line of the GD path must
equal them (main exact, shadow +-0.5 %). `_work\dx12-features-20260928\gd\g0-counts.json` collects
every run; the run folders `_work\dx12-features-20260928\qa\<run>` hold `tree_draws.json` /
`tree_draws.txt` with the per-view and per-frame numbers and the call-site table. First use of that
check on the working tree (14:18, DXIL engine shaders and the G1 code in, `r__gpu_driven 0`):
`gd-g0-count-pedro-tree1418` counts main 854 (582 + 272), cascades 1430 (981 + 449), light faces 0
(8 passes) - run b of the Gate-0 snapshot to the instance (`checks` in `g0-counts.json`).

## 3. Design

### 3.1 Frame flow (R5, `r__gpu_driven 1`)

```
level load   sector graph -> registry (instances, nodes, roots, prototypes, bins, groups, cells)
             -> static GPU buffers; GD nodes leave the sector roots for CSector::gd_nodes
Calculate()  per view, build_subspace_static: portal walk as today, then GdView setup (3.5):
             main view walks gd_nodes (impostors + node list), SMAP views only test cells
Render()     immediate, first thing: da_gd_tree_wind.cs over all roots (3.4)
per view     at the end of the CPU passes of its first render_graph(0), on its own context:
             cull.cs -> args.cs -> scatter.cs (3.6), then per group set_Pass + ExecuteIndirect (3.7)
```

### 3.2 Eligibility and registry

`GpuTrees::Build()` (`da_gpu_trees.cpp`, new) runs in `CRender::level_Load` right after
`LoadOverlayVisuals()` (`r2_loader.cpp:121`) - XMS hides (`HideOverlayGeometry`, `:855-888`) have
already detached their visuals, and the walk starts at every `CSector::root()` (recursing through
`MT_HIERRARHY` and `MT_LOD`), so only reachable visuals are registered.

- **Visual**: `MT_TREE_ST`/`MT_TREE_PM`; elements HQ, LQ and SHADOW all priority 0, none StrictB2F,
  Emissive, Wmark or Distort, no HS/DS; every pass has a GD variant: its VS created again as
  `name + "(DA_TREE_GD,)"` (option-in-name, `ShaderResourceTraits.h:643-646`, cached per name) whose
  input signature contains `GDREF`. A mod shipping its own tree VS without the GD block compiles a VS
  without `GDREF` and stays on the CPU path - a mechanism, no mod names.
- **Root** (`TreeWindShared`): eligible only if all its visuals are, so a crown and its trunk never
  split between CPU and GPU wind.
- **Node**: a `FLOD` whose children are all eligible, or an eligible tree hanging directly under a
  hierarchy. A `FLOD` with a mixed family stays on the CPU with all of it.
- **Prototype**: (vb, ib, declaration, vBase, iBase, SWI or single window). **Bin**: (prototype,
  element pass, LOD window). **Group**: (GD pass, vb, ib, declaration) = one `ExecuteIndirect`;
  its bins are contiguous. **Cell**: 32 m XZ grid over the level, a node belongs to the cell of its
  centre and the cell box is the union of its nodes' boxes (so a visible node always has a visible
  cell); nodes and instances are sorted by (cell, node), each cell knows its node/instance ranges and
  a bitmask of the groups present.
- **GD pass**: per original `SPass`, a copy with `vs = Resources->_CreateVS(name + "(DA_TREE_GD,)")`
  and a constant table merged from that VS and the pass's PS with the same binder mapping
  `CBlender_Compile::r_End` applies (`Blender_Recorder_R2.cpp`: `SetMapping`,
  `_CreateConstantTable`), then `Resources->_CreatePass(copy)` (`ResourceManager.h:198`), cached in
  a map; **instanced declaration**: the tree declaration plus
  `{"GDREF", 0, R32_UINT, slot 1, 0, PER_INSTANCE, 1}` (`SDeclaration::dx11_dcl_code`,
  `SH_Atomic.h:149-168`).
- **Hierarchy surgery**: eligible nodes move from their sector root's `children` into
  `CSector::gd_nodes` (new). A restore list brings them back for `r__gpu_driven 0` and before
  `UnloadOverlayVisuals`/visual teardown. The switch is applied only at the start of
  `CRender::Calculate` (main thread, before any `build_subspace` task).
- Log: `* [gd] trees: N instances, M roots, K nodes, P prototypes, B bins, G groups, C cells; X visuals kept on the CPU (shader Y, mixed root Z)`.

### 3.3 GPU data model

All raw buffers (`ByteAddressBuffer`), DEFAULT, committed and tracked (a structured view of a
page-suballocated buffer would need its page offset divisible by the stride; committed buffers avoid
the question and give UAV-written ones exact state tracking):

| Buffer | Content | Size (l01_escape) | Written |
|---|---|---|---|
| `gd_inst_draw` | world 3x4, c_scale, c_bias (unscaled), sun scale/bias, root, flags (`same_root`) - 96 B | 754 KB | level load |
| `gd_inst_cull` | sphere, box, prototype, node - 48 B | 377 KB | level load |
| `gd_node` | box, sphere, `lod_factor`, value-filter volume, sector, first/count instance, flags - 64 B | ~250 KB | level load |
| `gd_root_static` | root position, omega, height, foliage - 16 B | ~64 KB | level load |
| `gd_root_state` | q, qd, gust, dev, prev q/gust/dev, freq_k, prev freq_k, settled - 48 B | ~190 KB | wind CS, each frame |
| `gd_proto`, `gd_bin` | windows (index count, start index, base vertex), refs region base per (prototype, pass) | < 64 KB | level load |
| `gd_valuable` | value-filter stamps [node][R__NUM_CONTEXTS] | ~140 KB | SMAP cull |
| per context x9 | `vis` (instance, bin, slot) 12 B x N; `bin_count` u32 x B; `args` 20 B x B (`DRAWINDIRECT_ARGS`); `refs` u32 x N x 2 (also bound as vertex buffer) | ~250 KB each | per view |

Per-context buffers follow the renderer rule that every per-frame cache is `[R__NUM_CONTEXTS]`
(cascades record in parallel). Per-view parameters travel in a cbuffer (renamed per view on DISCARD;
an SRV of a dynamic buffer would break on renaming): phase, device camera, `r_ssaDISCARD`,
`r_ssaLOD_A/B`, `r_ssaVEG_DISCARD`, `r_dtex_range`, `r_ssaGLOD_start/end`, value-filter quality and
fov scale, context id, frame, up to 64 frustums x 12 planes, sector -> frustum ranges, visible cells
(up to 1024), the main view's node list (u16 pairs, up to 16k nodes) - at most 64 KB.
Total VRAM about 4 MB.

### 3.4 Wind on the GPU

`da_gd_tree_wind.cs`, one thread per root, dispatched once per frame at the start of
`CRender::Render` on the immediate context (deferred lists of every view execute after it):

- exact port of `UpdateWindState` (`FTreeVisual.cpp:332-413`): gust/deviation from
  `da_wind_core.h` at the root, target `(0.60 + 0.65 g)(1 + 0.6 gust_event)`, settle on the first
  frame, semi-implicit Euler with `steps = dt*omega*4 + 1`, zeta 0.30, clamp 0.35..1.6, previous
  values, continuity broken by `da_history_reset()`, the same `dt` clamp; `freq_k =
  clamp(omega / speed, 0.4, 6)` for the current and the previous `m_fTreeSpeed`
  (`wind_state_row` `:415-425`, `wind_prev_row` `:427-435`).
- Every root integrates every frame (the CPU integrates only roots drawn this frame): a crown
  entering the view no longer resumes from a stale state.
- `wind_dbg 1`: the watched root's state goes to a small readback buffer and is printed two frames
  late in the existing format.
- The VS builds rows 8-10 from `gd_root_state` plus per-instance sun scale/bias x `da_gd_sbc`
  (`ps_r__Tree_SBC * 1.3333`, changeable at run time) and the `same_root` flag
  (`tree_wind_row`, `:296-310`).

### 3.5 View setup (CPU, end of `build_subspace_static`)

`R_dsgraph_structure::gd` (new `GdView`, per context), reset at every build:

1. From `PortalTraverser.r_sectors`: each sector's `r_frustums[context_id]` into the plane table and
   the sector -> range table.
2. Cells: each cell box against the frustums of its sectors (`testSAABB`); visible cells into the
   list, their group masks OR-ed into `gd.groups`. No cell -> no GD work for this view (most light
   faces: 0-4 tree instances measured).
3. **Main view (NORMAL + HOM)**: the CPU walks `sector->gd_nodes` per frustum with today's tests
   (`add_static`'s `MT_LOD` branch: SAABB, HOM, value filter, SSA; impostor into `mapLOD`, node into
   `gd.nodes` when `ssa > r_ssaLOD_B`; standalone trees: SAABB, HOM, value filter), `marker`-deduped.
   It visits nodes (it has to, for the impostors) but never children: no insert, sort, batching,
   wind prep or draw per tree.
4. **SMAP views** skip `gd_nodes` entirely: shadow passes never draw impostors (`render_sun`,
   `render_rain`, `render_lights` do not call `render_lods`), the GPU tests the nodes.
5. `gd.active = psDeviceFlags rsDrawStatic && pmask[0] && (cells or nodes)`.

### 3.6 Cull passes

Recorded on the view's own context at the end of the CPU passes of its first `render_graph(0)`;
`gd.active` is cleared there, so the `smapvis` test draw (`light_smapvis.cpp:78-84`) and the
HUD-shadow graph never repeat it.

1. `da_gd_tree_cull.cs` - main: one group per `gd.nodes` entry; SMAP: one group per visible cell
   looping over its nodes.
   - Node (SMAP only): SAABB against the node's sector frustums (port of `testSAABB`, sphere then box
     per plane); value filter (every node: none sits below another `FLOD`, so none is
     `lod_managed`) with `staticGeometryThresholds[quality]` and stickiness
     `stamp[node][ctx] + 1 >= frame`, stamp written when kept (`r__dsgraph_build.cpp:49-75`);
     `FLOD` gate: skip when `ssa*lod_factor < r_ssaLOD_A && ssa*lod_factor < r_ssaVEG_DISCARD`
     (`:740-746`).
   - Instance (all views): `R/d^2 > r_ssaDISCARD` from the device camera; element NORMAL -> HQ when
     `sqrt(d^2) - R < r_dtex_range` else LQ, SMAP -> shadow; PM window
     `floor((1 - calcLOD(ssa)) * (W - 1) + 0.5)`; per pass of the element: bin,
     `slot = InterlockedAdd(bin_count[bin], 1)`, `vis[InterlockedAdd(vis_count, 1)] = {instance, bin, slot}`.
   - Phase 2: Hi-Z test (3.8; removed).
2. `da_gd_tree_args.cs` - one thread per bin of the groups in `gd.groups`: `args[bin] = {IndexCount,
   bin_count, StartIndex, BaseVertex, StartInstance = region base + counts of the lower windows of
   the same (prototype, pass)}`, then `bin_count = 0` for the next view of this context (no clear
   pass); `vis_count` copied aside and reset.
3. `da_gd_tree_scatter.cs` - one thread per `vis` entry (CPU upper bound = instances of the visible
   cells/nodes, early-out): `refs[args[bin].StartInstance + slot] = instance`.

UAV ordering between the three is the layer's D3D11 rule (global UAV barrier). The `args` and `refs`
transitions (UNORDERED_ACCESS -> INDIRECT_ARGUMENT / VERTEX_AND_CONSTANT_BUFFER and back for the next
view on the same context) come from the layer's tracking, on deferred contexts through the
`ExecuteCommandList` hand-off.

### 3.7 Draw

Same context, right after the cull, pass index 0 first (then 1 for the ATOC elements of MSAA):

```
for each group in gd.groups of this phase (sorted by PSO hash, then textures):
    cmd_list.set_Pass(group.gd_pass); cmd_list.apply_lmaterial();
    FTreeVisual::SetupInstancedGlobals(cmd_list);            // consts, wave, wind, *_prev
    cmd_list.set_c(c_gd_sbc, ps_r__Tree_SBC * 1.3333f);
    cmd_list.SRVSManager.SetVSResource(24, gd_inst_draw); SetVSResource(25, gd_root_state);
    cmd_list.set_Geometry(group.gd_geom);                     // instanced declaration
    cmd_list.set_InstanceStream(ctx.refs, 4);                 // IA slot 1, per instance
    cmd_list.RenderIndirect(ctx.args, group.first_bin * 20, group.bin_count);
cmd_list.set_InstanceStream(nullptr, 0);
```

- `CBackend::RenderIndirect` = the prologue of `RenderInstanced` (`dx11R_Backend_Runtime.h:389-419`)
  + `xrd3d12::DrawIndexedIndirectCount(context, args, offset, maxCount, nullptr, 0)` (`infra.md` 3.5).
  A record with 0 instances costs only the command processor; a group holds at most prototypes x
  windows records (typically 8-40).
- `StartInstanceLocation` offsets the per-instance `GDREF` fetch on both APIs, so the VS needs no
  `SV_InstanceID` base and no root constant (no command-signature root arguments, no change to the
  layer's root signature).
- Trees now draw after every CPU-drawn priority-0 static and dynamic object of the view: the G-buffer
  depth already holds terrain and buildings when the alpha-tested crowns rasterise.
- The instance stream is unbound after the last group, so `refs` is not a tracked vertex buffer of
  unrelated draws.

### 3.8 Occlusion (phase 2): removed

- Phase 1 keeps today's HOM exactly for nodes (CPU walk, 3.5). The per-child HOM test of
  `add_leafs_static` (`:411`) is dropped: all children of a visible node draw (conservative).
- The same-frame Hi-Z of the main view (`r__gpu_driven_hiz`: `da_gd_hiz.cs` built a max-depth chain
  from the depth after the view's CPU passes, the cull tested every instance box against it) was
  implemented and measured together with phase 3 in the final one-pass run (2560x1440 Extreme TAA,
  `foliage`: `final-onepass-foliage` with the defaults against `final-onepass-variants-foliage` with
  `r__gpu_driven_hiz 1`, `r__gpu_driven_main 1`, `r__grass_mesh_hiz 1`, `r__grass_mesh_depth 1` and
  `-dx12_copy_queue`; last `[gpu]` lines: frame 20.30 -> 20.41 ms, `scene` 2.07 -> 2.26, `gd` 0.12 ->
  0.34, `grass` 1.89 -> 1.94, `sun` 5.95 -> 5.77, FPS 49.0 -> 47.8). The Hi-Z build and the
  per-instance tests cost more GPU time than the crowns they hid saved: the switch, the chain, its
  shader and the instance boxes of the cull rows were removed.
- Rejected before: previous-frame reprojected Hi-Z (under TAA a disoccluded crown appears a frame
  late - flicker, the artefact class this project keeps removing); two-phase culling (trees draw last
  and are not occluders of each other here).

### 3.9 Phase 3: the main view on the GPU (removed)

Phase 3 (`r__gpu_driven_main`) moved the main view's node decisions to the GPU: the main view took the
shadow views' cell path, a same-frame Hi-Z replaced HOM (3.8), and the cull wrote the `FLOD` impostor
quads into a GPU vertex buffer that `render_lods` drew before the CPU's. Measured with phase 2 in the
final one-pass run (numbers in 3.8): the GPU work it adds (`gd` 0.12 -> 0.34 ms together with phase 2)
is not won back, the frame got slower, so the main view's cell path, the impostor generation and draw
(`da_gpu_trees_view_lods`, the facets, the quad regions and records) and the switch were removed. The
main view keeps phase 1: the CPU walk decides the nodes (`gd_node_main`, impostors into `mapLOD`), the
GPU culls their instances.

- **Phase 4 - grass**: dropped (PLAN C17) - mesh-capable DX12 GPUs draw grass through `mesh.md`.
- Bindless textures (`infra.md` 3.4) would merge groups that share a PSO into one `ExecuteIndirect`;
  needs SM 5.1/6.x tree shaders - not planned.

### 3.10 Parity rules

1. Same instance set per view: main-view node decisions are the CPU's own; SMAP views port the CPU
   tests exactly (device-camera SSA, value-filter stickiness per context id, the `FLOD` gate).
2. Same LOD window, element, passes and pass order.
3. Same bend: the three VS keep calling `da_tree_bend` / `da_tree_bend_ex` with rows equal to
   `FillInstanceData`'s (world-view = `m_V` x world in the VS, identical up to rounding).
4. Motion vectors: previous rows from `gd_root_state`; `wave_prev`/`wind_prev` unchanged.
5. `r__gpu_driven 0` returns every node to the CPU path the next frame.

### 3.11 Staging: the G1-G2 path (kept as the QA mode `r__gpu_driven 2`, 3.12)

G1 and G2 put the registry and the indirect draw in first and leave every culling decision to the
CPU, so the draw path can be checked on its own against the CPU path before any GPU culling exists:
- **Registry** (`da_gpu_trees.cpp`, 3.2): built at the first `CRender::Calculate` with
  `r__gpu_driven 1` after a level load (`da_gpu_trees_apply_switch`, top of `Calculate`), not at
  the load itself, so a session with the switch off compiles no GD variant and moves no node. Nodes
  are the sector roots' direct children made of eligible trees only (a single tree or an `FLOD` whose
  children are all trees); trees nested deeper stay on the CPU with their roots (`nested` in the log).
  Eligibility is the GD pass variant of every pass of the HQ, LQ and shadow elements (the VS compiled
  again with `(DA_TREE_GD,)` must carry `GDREF` in its input signature and match the PS format,
  DXIL or DXBC); roots are all-or-nothing and a rejected node takes its roots out, iterated to a
  fixed point. Prototypes, bins (pass, prototype, window), groups (pass, vb, ib, declaration) and
  32 m cells with group masks are laid out as in 3.2.
- **Surgery**: while applied, the nodes live in `CSector::gd_nodes`; every view still visits them
  through `add_static_sector` under the sector root's own tests (the root tested once: a second HOM
  test in a frame would pass a hidden root), children first, then `gd_nodes`, so every view selects
  exactly the instances, elements, windows and passes of the CPU path. `r__gpu_driven 0` and
  `level_Unload` put them back (a changed root keeps the change and gets the nodes appended, logged).
  `da_gpu_trees_for_each_node(sector, fn)` walks `gd_nodes` for the RT scene (C16).
- **Draw** (the G1 smoke draw): `render_graph` hands each pass's items to
  `da_gpu_trees_draw_pass` right after `set_Pass`. The registered trees are laid out by bin in the
  dsgraph's front-to-back order (window by `calcLOD(ssa)` as `FTreeVisual_PM::SelectLOD`), the refs
  and one 20-byte record per bin of every group with instances go into per-context dynamic streams
  (64 KB, `NO_OVERWRITE` within a frame, `DISCARD` per frame or when full), and each group is one
  `set_Pass(GD pass)` + `RenderIndirect` over the instanced declaration (`GDREF`, slot 1,
  `StartInstanceLocation` = the bin's refs offset). Only after both streams were written do the
  registered items leave the pass's list; the rest (unregistered trees, other statics) draw as before
  after the pass is set again. A stream that cannot take a pass leaves that pass to the CPU path (one
  log line per level).
- **Wind until G3**: at the frame's start `da_gpu_trees_frame_begin` integrates the CPU oscillator of
  **every** registered root (`FTreeVisual::PrepareWind` over one sample visual per root: the
  `same_root` flag of every instance is taken against that sample) and uploads the rows
  (`FTreeVisual::wind_state`: state, frequency factor, gust, deviation, the previous frame's four,
  height, foliage gate) into `gd_root_state`; `render_graph` skips registered trees in its wind prep.
  Frozen wind snaps every root to its field, so frozen frames equal the CPU path's.
- **Buffers**: `gd_inst_draw` (96 B/instance: world 3x4, c_scale, c_bias, sun scale/bias, root,
  flags) DEFAULT with a raw SRV at VS `t2`, `gd_root_state` (48 B/root) DEFAULT with a raw SRV at
  `t3` (t0/t1 are the VS's own textures); `ps_r__Tree_SBC * 1.3333` comes per draw in
  `da_gd_params`. The flags carry the root's wind sample bit and the `rt_static` bit
  (`dxRender_Visual::rt_static` of a rigid root, C16); `da_gpu_trees_refresh_flags()` rewrites them
  in the buffer when the RT lane reclassifies.
- **Log**: `* [gd] trees: N instances, M roots, K nodes, P prototypes, B bins, G groups, C cells; X
  visuals kept on the CPU (shader Y, mixed root Z, nested W)`, `* [gd] on|off: N instances in K
  nodes` at every switch, and with `r__gpu_log N` `* [gd] instances main=.. shadow=.. draws=..
  records=..` of the previous frame (GD-drawn instances per phase, ExecuteIndirects, records).
- `r__gpu_driven` defaults to 0 until the G1/G2 gates pass (then 1, 2.6 of PLAN), so no other lane's
  R5 parity run went through an ungated path.

### 3.12 Implementation (G1-G8 code, 28.09.2026; untested until the final test phase)

**Switches** (runtime, QA, `[dx12:gd]` block of `xrRender_console.cpp`): `r__gpu_driven` 0 CPU path,
1 GPU-driven (wind, cull, draw on the GPU), 2 the 3.11 path (CPU cull, GPU draw, CPU wind: attributes
a parity failure to the cull or the draw), -1 auto (3.13). Default 1 after the Perf phase, -1 since 29.09. The phase 2 and 3 switches
(`r__gpu_driven_hiz`, `r__gpu_driven_main`) were removed with their code (3.8, 3.9).

**Registry** (`da_gpu_trees.cpp`, built at the first frame with the switch on, 3.11): as 3.2, plus
- cells per (sector, 32 m square), sorted by sector, so a visited sector's cells are one range and a
  cell's nodes share the sector's frustums;
- kinds = (prototype, the GD passes of HQ/LQ/shadow x pass 0/1): the cull reads the first bin of window 0
  of each (element, pass) from the instance's kind;
- refs regions per (prototype, pass): room for every instance that can be drawn with it once (one
  element per view), the windows laid out after each other by the args pass;
- value-filter slots: the nodes, then every sector root and hierarchy above a node; each node knows its
  chain (root first).

**Buffers** (raw views; static ones IMMUTABLE, the rewritten ones DEFAULT): `gd_inst_draw` 96 B (VS t2),
`gd_root_state` 48 B (VS t3, UAV of the wind pass: row 2 carries the velocity `qd` and the frame of the
last integration, 0xFFFFFFFF = unsettled), `gd_root_static` 32 B (x, z, omega, height, foliage),
`gd_inst_cull` 32 B (sphere, kind, rt_static), `gd_node` 64 B (box, instances, sphere, lod_factor,
chain), `gd_filter` 16 B per slot, `gd_ancestors`, `gd_kind` 32 B, `gd_bin` 32 B (window 0 record, refs
region, window, first bin), `gd_valuable` [slot][9 contexts], statistics (4 frames x 8 counters) and the
wind_dbg row with 3-deep staging rings. Per context x9: counts (bins, then the entry count), entries
(instance, bin, slot), two per instance, records (one per bin, UAV + `DRAWINDIRECT_ARGS`), refs (UAV +
vertex buffer), the view list (>= 256 KB, DEFAULT: the layer gives dynamic buffers no views).

**Frame** (`r__gpu_driven 1`): `da_gpu_trees_frame_begin` (after `GpuTimers.FrameBegin`, immediate):
`da_gd_tree_wind.cs` over every root (3.4; frozen wind snaps to the field, a history reset breaks the
previous rows), the statistics ring (copy, clear this frame's slot). Per view: `build_subspace_static`
calls `da_gpu_trees_view_begin` after the portal traversal - shadow views list the visible cells
(`walk = skip`: `add_static_sector` leaves the nodes out), the main view walks them with `gd_node_main`
(`walk = main`: HOM, value filter, FLOD switch with the impostor into `mapLOD`, the node into the list; a tree listed through another frustum is skipped like
`insert_static`'s marker) - and `da_gpu_trees_view_end` assembles the upload (planes, frustums,
entries). A view whose static lists are empty gets an empty item list of one GD pass, so the callers'
`bNormal` still draws it. `render_graph(0)`, after its CPU passes, calls `da_gpu_trees_view_render`
once per build: upload the list, clear the counts (`ClearUnorderedAccessViewUint`, UAV barrier),
`da_gd_tree_cull.cs` (flags: shadow, rt_skip_static, node list, value filter), `da_gd_tree_args.cs`,
`da_gd_tree_scatter.cs` (the `GdCull` timer brackets them in the main view), then one
`set_Pass(GD pass)` + `RenderIndirect` per group of the view's cells in the view's phase, pass 0
first. The per-view constants are float4
uniforms (the engine's constant tables take float and int types only; counts and offsets are exact as
floats, frames go in 24 bits).

**Statistics**: with `r__gpu_log N`, `* [gd] instances main=.. shadow=.. draws=.. records=.. views=..
nodes=.. entries=../..` (GPU counters of the frame three back; draws and records from the
CPU); `(CPU cull)` lines for mode 2. `wind_dbg 1`: `* [wind-tree] ... (gd root 0)` from the GPU state.
The CPU counters' slot of a frame is zeroed by `da_gpu_trees_apply_switch` at the top of `Calculate`, not in
`frame_begin`: the sun cascades and rain record on workers from `Calculate` on and would otherwise lose the
counts they add before `Render` starts (review finding GD-R1).

**Fallbacks**: a view list that does not fit (sector frustums) -> that view culls on the CPU and draws
through `da_gpu_trees_draw_pass` (one log line); a compute shader or buffer that fails -> mode 1 behaves
like 2 (one log line).

**Final test phase** (PLAN 0.1b, budget 0.1a; R5 unless noted; `gd_check` headers of 9.3):
1. G1/G2: `gd_check -Save qa_pedro -Globals 'qa_gd_mode = 2; qa_gd_toggle = true; qa_gd_next = "fogqa"'`
   (the 3.11 path): on1 vs the off floor equal; `[gd] trees:` line; on2/off4 equal after the toggles;
   the second level rebuilds (`[gd] trees:` twice).
2. G3-G5 (mode 1): `gd_check -Save foliage -Globals 'qa_gd_variants = true; qa_gd_motion = true;
   qa_gd_winddbg = true'` and `-Save qa_pedro -Globals 'qa_gd_variants = true'`: on1 and cpucull vs the
   off floor equal (frozen wind); `[gd] instances main=` equals the `count_tree_draws.py` main count of
   2.7 (the per-child HOM test is dropped, 3.8: a difference is the children HOM hid, reported), shadow +-0.5 %; motion captures: crowns and their shadows sway in both; wind_dbg lines
   present. The rt_skip_static bits: with the rt lane's `rtx-trees` run (C16).
3. G5 extras: `motion_check` and `upscale_check` with `-PreCommands 'r__gpu_driven 1'` against the off
   run; one `-dxdebug` run of `gd_check -Save foliage`; one R4 `render_check` (the DX11 path unchanged,
   `caps ... gd=0`).
4. G6: one `Invoke-Dx12Perf` batch, `qa_pedro`, `-A 'r__gpu_driven 0' -B 'r__gpu_driven 1' -Pairs 3`
   (plus a profile pair): 9.4's pass rule; then the default goes to 1 (7.2 rule: removed if FPS drops at
   equal clocks or the render thread does not gain).
5. G7/G8: phases 2 and 3 were measured in the final one-pass run instead of pairs and removed (3.8,
   3.9); `gd_check`'s variant shots are `cpucull` and `off5` since.

### 3.13 Local light faces and the auto choice (29.09.2026)

Measured on a player's save (a campfire at night on k00_marsh, 2560x1440 Extreme TAA, RTX 3070 Laptop,
i7-11800H; `_work\dx12-features-20260928\qa\qs3-r5-phases`, `[gpu]` lines between the phases): the 28.09
defaults (GD 1, mesh grass 1, VRS 1) 43.9 FPS, `lights` 2.14 ms, `scene` 2.03, `sun` 5.47, `grass` 1.03;
`r__gpu_driven 0` 49.1 FPS, `lights` 0.58, `scene` 1.85, `sun` 5.35; plus `r__grass_mesh 0` 52.0 FPS,
`grass` 0.64, `sun` 5.15-5.33; DirectX 11 on the same save 49.7 FPS, `lights` 0.61. Both offloads cost GPU
time, and that frame is GPU-bound.

**Local light faces.** The campfire's point light has six shadowed faces, and each was a GD shadow view:
the cull over the cells of its sectors, the records of every bin (78994 bins on k00_marsh; `views=19
records=72204` a frame) and the UAV barriers between the passes - 1.56 ms of `lights` for the few trees a
face sees (0-4 instances, 2.6). `R_dsgraph_structure::o.local_light` (set by `calc_lights`, cleared by
`reset()`) now makes `da_gpu_trees_view_begin` answer `cpu`: the face walks the nodes the registry moved
into its sectors' lists through `add_static` after the hierarchy (`add_static_sector`, the mode-2 walk), so
no tree is missing or drawn twice, and `smapvis` retires occluded tree casters again as on DirectX 11;
`render_graph` draws the registered trees through `da_gpu_trees_draw_pass` - the GD passes with the GPU wind
state (`gd_root_state`), so their shadows sway exactly as in the cascades. The main view, the sun cascades,
the rain map and the z-prefill keep the GPU cull. `[gd] instances` adds the CPU-culled instances to the
GPU's. Result (`qs3-offload`, GD on): `lights` 0.58-0.60 ms, `views=4 records=52096`.

**Auto (`r__gpu_driven -1`, `r__grass_mesh -1`, the defaults).** Both offloads trade CPU time for GPU time
(GD: the tree batches of the render thread against `gd` ~0.09 ms and ~0.3 ms in `scene`/`sun`; mesh grass:
`grass_cpu` 8.6 -> 1.8 ms on the stand's foliage save against +0.4-0.6 ms of `grass`), so they pay only
while the CPU limits the frame. `da_offload.cpp` decides for both, once per frame:
- CPU time: the main thread from the engine's `SimulationStart` marker to `OwnFrameLimit` (the parallel
  sequence joined, before the engine's limiter; Reflex sleeps before the marker), minus its waits on the
  GPU: the CPU/GPU sync point (`q_sync_point`) and `Present` (the swap chain and the layer's frame fence).
- GPU time: the GPU timers' `Frame` slot (on wherever timestamp queries exist), capped by that frame's
  main-thread span. The sync point moved ahead of `GpuTimers.FrameBegin()`: on DirectX 12 its event query
  submits what was recorded so far, and the frame's first timestamp riding with that submission counted the
  GPU idling until the frame's own work arrived at Present (a CPU-bound frame looked GPU-bound).
- Both smoothed over `r__offload_smooth` s (1). CPU > GPU x (1 + `r__offload_margin` (0.1)): CPU-bound ->
  the GPU paths; GPU > CPU x (1 + margin): GPU-bound -> the CPU paths; else hold. The verdict must hold
  `r__offload_agree` s (3) and the last switch be `r__offload_dwell` s (10) old (a new level may switch after
  the agreement alone).
- A switch is a trial: `r__offload_dwell` s later the smoothed frame must be shorter than just before it,
  or the path left comes back ("no gain"). The rule's premise does not always hold: mesh grass draws the
  G-buffer grass on the render thread, where the vertex path draws it on a worker (`r2_R_render.cpp`, the
  parallel grass), so it can lengthen the very thread it is meant to relieve.
- A path left less than `r__offload_recall` s (120) ago is taken back by the rule only if its frame was
  shorter by the margin (no flip-flop), and once, `r__offload_recall` s after a kept switch, the path left
  is tried again ("recheck"): the frame the switch was judged against may have been a passing load.
- Nothing is measured for `r__offload_warmup` s (30) after a level load or a return to -1: the post-load
  frames are CPU-bound for a while (below), and a decision there sticks to the wrong path.
- Starts on the CPU paths; without GPU timers the CPU paths. The decision is made at the frame's end and
  read by the next frame through the existing runtime switches (the grass latch in `OnCameraUpdated`, the
  trees at the top of `Calculate`). With auto the registry is built with the level even while the CPU path
  runs, so a later switch has no build hitch. 0/1 (and GD's QA 2) force a path as before; R4: auto = 0.
- Log: `* [offload] auto: start -> trees cpu, grass cpu`; per switch `* [offload] auto: gpu-bound -> trees
  cpu, grass cpu (cpu .. ms, gpu .. ms, frame .. ms)`, `cpu-bound -> ... gpu`, `no gain -> ... (... against
  .. ms before)`, `recheck -> ...`; `resumed -> ...` when a forced switch returns to -1; `no GPU timers ->
  ...` once. With `r__gpu_log N`: `* [offload] path= cpu= gpu= frame= ms warmup= s`.

Stand runs (`async_check`, save `qa_user_qs3`, the pre-commands of `qs3-r5-phases`; phases `auto` (the
defaults), `forced1` (`r__gpu_driven 1`, `r__grass_mesh 1`), `auto2` (both -1); 2 s settle + 6 s):
- `qs3-offload` (the first controller, no warm-up): `start -> trees cpu, grass cpu`, then in the first
  seconds after the load `cpu-bound -> trees gpu, grass gpu (cpu 35.93 ms, gpu 20.70 ms, frame 36.78 ms)`;
  auto 44.8 FPS, forced1 44.9 FPS (`[gpu]` frame 20.3-20.6, `scene` 2.18-2.21, `sun` 5.77-5.83, `lights`
  0.59, `grass` 1.14-1.16, `gd` 0.085), auto2 `resumed -> trees gpu, grass gpu` 41.4 FPS.
- `qs3-offload-b` (10 s warm-up, trial): `start` on the CPU paths (`[gpu]` frame 19.3-19.9, `scene`
  1.89-1.96, `sun` 5.35-5.58, `lights` 0.57-0.58, `grass` 0.67-0.69, `grass_cpu` 8.3-8.7), then ~13 s after
  the load still `cpu-bound -> trees gpu, grass gpu (cpu 27.29 ms, gpu 18.98 ms, frame 29.17 ms)`; auto
  43.2, forced1 43.7, auto2 (`resumed -> ... gpu`) 43.2 FPS. The same save settles at 52 FPS on the CPU
  paths later in a session (`qs3-r5-phases`), and the GPU paths leave the GPU idle ~2 ms a frame (`[gpu]`
  frame 20.3-20.9 ms at 22.3-23.1 ms frames): the post-load CPU load outlasts 13 s, hence the 30 s warm-up,
  and the trial (the GPU paths were kept against the post-load 29 ms) plus the recheck bring such a switch
  back. The final controller (30 s warm-up, trial, recheck) was built but not run on the stand: a run that
  shows its steady state needs more than a minute.

## 4. Integration points

| File | Change |
|---|---|
| `src/Layers/xrRenderPC_R4/da_gpu_trees.h/.cpp` (new) | registry, cells, GD passes/declarations, GPU buffers, `GdView` setup helpers, cull/draw per view, wind dispatch, readback stats; R5 code in `#ifdef XR_D3D12`, inert on R4 |
| `src/Layers/xrRenderPC_R4/sources.cmake` | new files |
| `src/Layers/xrRender/r__dsgraph_structure.h` | `GdView gd` member, reset in `reset()` |
| `src/Layers/xrRender/r__dsgraph_build.cpp` | `build_subspace_static` (`:822-899`): `GdView` setup, main-view walk of `gd_nodes` (reuses the `MT_LOD` branch of `add_static`, `:734-758`), SMAP skip |
| `src/Layers/xrRender/r__dsgraph_render.cpp` | `render_graph(0)`: after the normal and matrix passes, `da_gpu_trees_view_render(*this)` when the build set up a view (`gd_view`); the CPU-cull draw `da_gpu_trees_draw_pass` per pass (mode 2) |
| `src/Layers/xrRender/r__sector.h` | `CSector::gd_nodes` |
| `src/Layers/xrRender/FLOD.h` | `gd_index` |
| `src/Layers/xrRender/FTreeVisual.h/.cpp` | read accessors for the registry (colors, geometry window, `TreeWindShared` root/omega/top/foliage); `SetupInstancedGlobals` reused |
| `src/Layers/xrRender_R2/r2_loader.cpp` | `GpuTrees.Build()` after `LoadOverlayVisuals` (`:121`), `GpuTrees.Destroy()` first in `level_Unload` |
| `src/Layers/xrRender_R2/r2_R_calculate.cpp` | apply a pending `r__gpu_driven` switch at the top of `CRender::Calculate` (`:73`) |
| `src/Layers/xrRender_R2/r2_R_render.cpp` | wind dispatch after `GpuTimers.FrameBegin()` (`:276`); `gd` field in the `r__gpu_log` line (`:730-740`) |
| `src/Layers/xrRenderDX11/dx11R_Backend_Runtime.h`, `src/Layers/xrRender/R_Backend.h` | `RenderIndirect`, `set_InstanceStream` (slot 1, cached, invalidated with the rest) |
| `src/Layers/xrRender/ResourceManager*` | instanced declaration from a base declaration |
| `src/Layers/xrRenderDX11/dx11GpuTimers.h/.cpp` | slot `GdCull` (main view cull + args + scatter) |
| `src/Layers/xrRender/xrRender_console.cpp` | `r__gpu_driven` (runtime 0/1/2, default 1; not saved); `r__caps` key `gd` |
| `src/Layers/xrRenderDX12/d12_objects.cpp` | (built by `infra` I4, PLAN C10) buffer SRV/UAV views: raw (`BUFFEREX_SRV_FLAG_RAW`, `R32_TYPELESS`), structured, typed; `APPEND`/`COUNTER` refused with a log line; buffers with SRV or UAV binding committed and tracked |
| `src/Layers/xrRenderDX12/d12_context_state.cpp` | (built by `infra` I4) indirect draws per `infra.md` 3.5; `MISC_DRAWINDIRECT_ARGS` accepted |
| `tools/qa/d3d12layer/scenarios_api.cpp` | scenarios 9.1 |
| shaders (section 6) | VS variants, new CS and headers |
| `tools/qa/water/gd_check.lua` (new) | stand scenario 9.3 |
| `tools/qa/gpudriven/count_tree_draws.py` (new) | breakpoint counter of the CPU path (2.6), parity reference |
| `docs/dead-air/DX12_RENDERER.md` | console row `r__gpu_driven`, `r__caps gd`, pointer to this file |
| `src/Layers/xrRenderPC_R4/da_offload.h/.cpp` (new, 3.13) | the auto choice of `r__gpu_driven`/`r__grass_mesh`: frame timing from the engine's markers, `OwnFrameLimit` and the sync point's wait (`D3DXRenderBase.cpp`, `r2_R_render.cpp`), the decision, `r__offload_*` |
| `src/Layers/xrRender/r__dsgraph_structure.h`, `src/Layers/xrRender_R2/r2_R_lights.cpp` (3.13) | `o.local_light`, set by `calc_lights`: `da_gpu_trees_view_begin` answers `cpu` |

New engine files start with the XFined-Ray/MIT header.

## 5. Data and resources

- Sizes: section 3.3, about 4 MB VRAM on l01_escape, rebuilt per level (the second-load bug class:
  nothing survives `level_Unload`).
- States: static buffers `NON_PIXEL_SHADER_RESOURCE` after the load upload; `gd_root_state` UAV in
  the wind CS, shader read for the VS; per-context `vis`/`bin_count` UAV only; `args` UAV <->
  `INDIRECT_ARGUMENT`; `refs` UAV <-> `VERTEX_AND_CONSTANT_BUFFER`.
- Lifetime: created by `Build`, released by `Destroy` (layer `DeferredReleaser` covers in-flight
  frames); kept across `vid_restart` (the layer device survives it); per-view cbuffer a dynamic
  constant buffer per context.
- CPU memory: registry arrays kept for the restore list and the readback checks (< 2 MB).

## 6. Shaders

SM 5.0-compatible HLSL created through `CResourceManager`, so every stage follows the session's mode
(PLAN C4): with DXC on R5 the GD VS is DXIL like the pixel shaders it pairs with; a PSO mixing DXBC
and DXIL is refused (`infra.md` 2) and `dxc.md` 3.6 unifies such passes.

| File | Content |
|---|---|
| `da_gd_tree.h` (new) | `ByteAddressBuffer da_gd_inst : register(t24)`, `da_gd_root : register(t25)`; `da_gd_tree_rows(ref)` returning xform, xform_v (`mul((float3x3)m_V, world)`), c_scale, c_bias, c_sun, c_tree, c_tree_prev |
| `deffer_tree_flat.vs`, `deffer_tree_bump.vs`, `shadow_direct_tree.vs` | `#ifdef DA_TREE_GD`: extra input `uint gd_ref : GDREF`, locals from `da_gd_tree_rows`; the instanced/scalar branch unchanged otherwise; the bend code untouched |
| `da_gd_frustum.h` (new) | `testSAABB` port (sphere classify, box p/n vertex per plane) |
| `da_gd_tree_cull.cs` (new) | 3.6 step 1; `(GD_SHADOW,)` variant tests nodes |
| `da_gd_tree_args.cs`, `da_gd_tree_scatter.cs` (new) | 3.6 steps 2-3 |
| `da_gd_tree_wind.cs` (new) | 3.4; includes `da_wind_core.h` |

`tools/qa/Check-ShaderLinkage.py` needs no rule change (VS outputs identical); it gains the
`(DA_TREE_GD,)` permutations.

## 7. UI and presets

None. The feature does not change the look, so it is not a preset rung (`graphics-features-policy`
concerns the look) and not part of the Video tab's RTX category. `r__gpu_driven` (runtime, not saved) is
auto by default (3.13); 0/1/2 force a path for QA. `r__caps` gets `gd=1` on R5 while the registry is live
(information for QA scripts). `r__gpu_log` gains `gd=<ms>` and, on the same cadence, one
`* [gd] views=... instances main=... shadow=... groups=... records=...` line from a readback ring.

## 8. Fallback

- R4 (DirectX 11): CPU path untouched; `r__gpu_driven` inert, `gd=0`.
- R5 with `r__gpu_driven 0`, a failed buffer creation or a failed CS/VS compile: CPU path, one log line.
- R5 with `r__gpu_driven -1` (auto): the CPU path while the GPU is the bottleneck or no GPU timers exist
  (one `[offload]` line), the GPU path while the CPU is (3.13).
- Local light faces: CPU cull, drawn through the GD passes with the GPU wind (3.13).
- Per root: shaders without the GD block (mods), mixed families: that root/node stays on the CPU.
- Views without GD cells: no GD work at all.
- ExecuteIndirect is core D3D12 (tier 1.0), present on every adapter R5 accepts (FL 11_0 + binding
  tier 2).

## 9. Test plan

### 9.1 Layer tests (`tools\qa\d3d12layer\Run-LayerTests.ps1 -HighPerformance`)

Pass: exit code 0, the 36 existing scenarios and `infra.md`'s scenario 10 still pass, debug layer and
GBV silent. New, each against real D3D11:
1. `buffer views`: CS writes `RWByteAddressBuffer` and a structured UAV; VS reads them as
   `ByteAddressBuffer`/`StructuredBuffer` SRVs - pixel-exact.
2. `per-instance stream with StartInstanceLocation`: `DrawIndexedInstanced(..., startInstance=37)`
   with a `R32_UINT` per-instance element - exact.
3. `UAV -> vertex buffer -> UAV`: one buffer written by CS, drawn from as slot 1, written again,
   drawn again, in one list and across a deferred list.
4. `multi-draw`: `DrawIndexedIndirectCount` over 64 records with zero-instance records against the
   same records drawn one by one with `DrawIndexedInstancedIndirect` on D3D11.
5. `deferred cull + draw`: scenario 4 recorded on a deferred context, `ExecuteCommandList`, twice
   in a frame on the same buffers.

### 9.2 Build

`Invoke-Ninja 'xrRenderPC_R5 xrRenderPC_R4'`: no errors, no new warnings in touched files.

### 9.3 Stand (`Invoke-Stand` / `tools\qa\Run-WaterQa.ps1` for `-Weather`, `-ExtraArgs '-r5 -always_active'` unless noted)

`gd_check.lua` (in the tree): after load `wind_seed` + `wind_freeze 1`, the header's commands; lossless
shots `off1`, `off2` (`r__gpu_driven 0`), `on1` (`qa_gd_mode`, default 1), `off3`; the `[gd]` counts every
30 frames (`r__gpu_log 30`). Optional phases: `qa_gd_variants` (shots `cpucull` = mode 2, `off5`),
`qa_gd_toggle` (the switch flips every 30 frames, then `on2`/`off4`), `qa_gd_motion`
(`wind_freeze 0`, `r__capture 3 10` off, then on), `qa_gd_winddbg` (`wind_dbg 1` for 2 s in mode 1),
`qa_gd_next` (the steps again on another level). Ends with `DA_WATER_PROBE_DONE`. The final phase's
selection of runs is 3.12.

| Run | Command | Pass |
|---|---|---|
| Parity x4 | `-Scenario gd_check -Save qa_pedro` / `trees` / `foliage` / `fogqa` | GD-on frames vs GD-off frames: >= 99.5 % of pixels within 2 levels per channel and no cluster larger than 16 px beyond the off-vs-off noise floor (two consecutive GD-off frames); `[gd]` main/shadow instance counts equal the CPU counts of `count_tree_draws.py` on the same save (main exact, shadow +-0.5 %); no `FATAL`, no layer `!` line |
| Shadow sway | `-Scenario gd_check -Save foliage -Weather af3_bright_clear_1 -Globals "qa_gd_motion = true; qa_gd_cmds = { 'r__clouds_cover 0' }"` (commands run after load) | per-frame difference energy inside crown-shadow regions GD on vs off within 5 %; shadows visibly move in the capture (reviewed) |
| Motion vectors | `-Scenario motion_check -Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__motion_vectors 1' }"`, GD on vs off | velocity frames equal within 2 levels over tree pixels |
| Upscalers | `-Scenario upscale_check` | all steps present, frames equal the GD-off run |
| Toggle stress | `-Scenario gd_check -Save qa_pedro -Globals 'qa_gd_toggle = true'` | no crash; counts return to the same values each flip; no leaked/missing tree in the last frames |
| Second level | `gd_check` on `qa_pedro` with `qa_gd_next = "fogqa"` (in-game `load fogqa`, then the parity steps again) | registry rebuilt (`[gd] trees:` line twice), counts right on fogqa, nothing of l01_escape left |
| Validation | `-ExtraArgs '-r5 -always_active -dxdebug'` on `gd_check -Save foliage` | no D3D12 error/corruption lines |
| DX11 | `-Scenario render_check -ExtraArgs '-always_active'` | frames equal the R4 baseline, `caps ... gd=0` |

### 9.4 Performance (ninja.lock held, no compile sharing the CPU)

- `ab_run.ps1`, 2560x1440 Extreme TAA, interleaved GD off/on (`-PreCommands 'r__gpu_driven 0|1'`),
  saves `qa_pedro`, `foliage`, `fogqa`, first run of each series discarded, `analyze.ps1`.
- `Run-Profile.ps1 -Label gd-on|gd-off -Save qa_pedro -ExtraArgs "-r5"` with callers
  `render_tree_batches,FTreeVisual_ST::Render,FTreeVisual_PM::Render,UpdateWindState,insert_static,add_leafs_static,da_gpu_trees_view_render,gd_node_main`.
- Pass: tree path on the game thread (`render_tree_batches` + `FTreeVisual_*::Render` +
  `UpdateWindState`) from 4.7 % to ~0 and the GD issue (`da_gpu_trees_view_render` + `gd_node_main`) below 1.2 %; `-dxstats` draws
  per frame on qa_pedro down by at least 700; FPS not lower at equal clocks on any save;
  `r__gpu_log` `scene + sun` within +0.25 ms, `gd` below 0.1 ms.

## 10. Performance expectation

- **Draws**: qa_pedro 1062 tree draws a frame -> at most 94 (main: 47 kinds x HQ/LQ) + 3 x 47
  (cascades) = **<= 235 ExecuteIndirect** (only groups present in visible cells are issued), so
  `-dxstats` 4383 -> ~3550 (-19 %); trees save 939 -> <= 235 of ~2350 (-30 %).
- **CPU, render thread** (qa_pedro): the measured 0.72-0.93 ms/frame of the tree draw path goes;
  GD costs about 90 groups x ~1.5 us + 3 dispatches = ~0.15 ms: **-0.6..-0.8 ms**, plus the child
  traversal (`add_leafs_static`/`insert_static` for 854 children) that no longer runs.
- **CPU, workers**: cascades draw 1.7-3.6x the main view's instances (1455 on qa_pedro): about
  1.3-1.6 ms of worker time now, ~0.2 ms with GD: **-1.1..-1.4 ms thread time**, ~0.4 ms off the
  cascade critical path (`r_sun.sync`).
- **Uploads**: ~1.1 MB/frame of dynamic constants -> ~0.1 MB.
- **GPU**: wind CS ~0.01 ms; cull + args + scatter ~0.02-0.04 ms per view with trees (4-5 per
  frame) = **+0.10..0.15 ms**; triangles unchanged; trees drawn after the occluders may shave G-buffer
  overdraw. Phase 2 Hi-Z: measured with phase 3 and the other variants at `gd` +0.22 ms, `scene` +0.19 ms - removed (3.8).
- **FPS**: the stand at 1440p is GPU-bound -> within +-1 %; CPU-bound cases (upscaler performance
  modes, frame generation, high refresh, weaker CPUs) on tree-heavy views **+4..8 %**.

## 11. Risks

- **Culling parity drift** (thresholds copied into HLSL): the main view keeps CPU node decisions;
  SMAP tests are small ports; counts are checked against the breakpoint counter per save.
- **Value-filter stickiness** per context id (light contexts are reused within a frame): same
  semantics as the CPU (`valuable_frame[context]`), stamps per context id.
- **Group issue cost** higher than expected (`set_Pass` ~1-2 us): measured in 9.4; mitigation is
  sorting by PSO (only PS SRV tables change) and cell masks; bindless merge is the later lever.
- **Many light faces** (39 a frame on `foliage`): the cell prefilter makes faces without GD nodes
  free.
- **Layer buffer views** are new code: layer scenarios 1-5 against D3D11; dynamic buffers never get
  SRVs (renaming).
- **Wind divergence** GPU vs CPU float math: frozen-wind parity and the motion capture in 9.3.
- **smapvis** no longer retires occluded tree casters of lights: measured 0-4 tree instances a frame
  in light faces on all four saves - negligible.
- **HUD-shadow and smapvis graphs** call `render_graph(0)` again: `gd.active` is consumed once.
- **Mods** replacing tree shaders: per-root eligibility from the compiled signature, no names.
- **XMS hides/overlays**: registry after `LoadOverlayVisuals`, torn down before overlay unload.
- **Shared files** with ray tracing (tree instances/wind for a TLAS), mesh shaders and `infra.md`
  (indirect draws): see the hotspot list in the summary; the registry is written so an RT TLAS
  builder can read `gd_inst_draw` + `gd_root_state` instead of keeping its own copy.
- **DX11**: no behaviour change; `render_check` R4 frames compared.

## 12. Implementation order

0. **Tooling and baseline**: `tools/qa/gpudriven/count_tree_draws.py` (the hardware-breakpoint
   counter used in 2.6) + baseline counts/profiles per save into `_work`.
1. **Layer**: buffer SRV/UAV views, committed SRV/UAV buffers, `MISC_DRAWINDIRECT_ARGS`, indirect
   draws per `infra.md` 3.5 and scenarios 9.1 - all built by the `infra` lane (I4, PLAN C10); this
   lane waits for I4 before its first GPU draw.
2. **Backend plumbing**: `RenderIndirect`, `set_InstanceStream`, instanced declaration, GD pass
   variants (`(DA_TREE_GD,)`), `r__gpu_driven`, `r__caps gd`.
3. **Shaders**: `da_gd_tree.h`, the three VS blocks; smoke test drawing every registered instance
   through one static args buffer (CPU-built) to validate data and bend before any culling.
4. **Registry**: eligibility, nodes, cells, bins, groups, hierarchy surgery + restore, unload,
   runtime switch; log line.
5. **Wind CS** + `gd_root_state`, `wind_dbg` readback; frozen-wind image parity.
6. **SMAP views** (cascades, rain, lights, z-prefill): cells, node/instance cull, args, scatter,
   draw; parity runs; shadow-sway run.
7. **Main view**: `gd_nodes` walk (impostors + node list), NORMAL cull (HQ/LQ), draw; parity,
   motion vectors, upscalers, toggle stress, second level.
8. **Stats and timers** (`gd` timer, `[gd]` readback line), perf A/B + profiles, `DX12_RENDERER.md`.
9. **Phase 2 Hi-Z**: implement, A/B, keep or drop - dropped (3.8).
10. Phase 3: design section in 3.9 first, then implementation and the phase-1 gates (PLAN G8) - removed
    after the measurement (3.9); phase 4 is dropped (PLAN C17).

## Final test phase, functional part (28.09.2026; `_work\dx12-features-20260928\final\stand.md`)

- Look-neutral parity with `r__gpu_driven 1` together with async, mesh grass, DirectStorage and DXC (`final-on-pedro`,
  `final-on-foliage`, `final-on-nods-pedro`): calibrated "equal" against the same tree's DXC-only control + the
  Gate-0 references on qa_pedro; foliage 99.91 % within against the control, 0 clusters > 16 px, 56.4 dB (the known
  DXC/GTAO-corner margin of the tree against Gate 0 aside, dxc.md). `[gd] on: 7848 instances in 2444 nodes`,
  `[gd] instances main=882 shadow=1593` in that render_check view (not gd_check's G2 view; the G2 count comparison
  is gd_check's).
- Sway with RT sun shadows and GD on (`final-rtx-trees`): the crown shadows keep moving (dxr.md 9.2).
- Level change with GD on, `-dxdebug` and every other feature on (`final-robust-level-c`): `[gd] trees: 8830
  instances ...` rebuilt on k00_marsh, no D3D12 error line.
- The default stayed 0 until the Perf phase (flipped to 1 there, below).

## Final test phase, performance (Perf phase, 28.09.2026)

At the user's request the Perf phase ran one measurement pass instead of on/off batches (PLAN 8.4); the G6 batch
was not run, phases 2 and 3 were measured in one pass against the defaults (below).
- `final-onepass-foliage` (2560x1440 Extreme, TAA, every feature on): `gd` (cull + args + scatter of the main view)
  **0.12 ms** a frame, `scene` 2.06 ms; `[gd] trees: 7848 instances, 2444 roots, 2444 nodes, 368 prototypes ...
  built in 162.7 ms`; `[gd] instances main=620 shadow=1122 draws=578 ... views=11`.
- Stand runs of the functional part (1920x1080 Extreme, SMAA): foliage 68.9 FPS with the DXC-only tree
  (`final-dxconly-foliage`: GD 0, mesh grass off, no async, no DirectStorage) against 81.4 FPS with every
  look-neutral feature on (`final-on-foliage`: GD 1, mesh grass 1, DirectStorage, async); qa_pedro 64.5 -> 65.1
  (`final-dxconly-pedro` -> `final-on-nods-pedro`). These are the four features together, not GD alone.
- Decision: **`r__gpu_driven` defaults to 1** (look-neutral PASS of the final stand, no error line with
  `-dxdebug`, level change rebuilt).
- Phases 2 and 3, one pass (2560x1440 Extreme TAA, `foliage`): `final-onepass-foliage` with the defaults
  against `final-onepass-variants-foliage` with `r__gpu_driven_hiz 1`, `r__gpu_driven_main 1` (and the grass
  variants and the copy queue of mesh.md and infra.md); last `[gpu]` lines: frame 20.30 -> 20.41 ms, `scene`
  2.07 -> 2.26, `gd` 0.12 -> 0.34, `grass` 1.89 -> 1.94, `sun` 5.95 -> 5.77, FPS 49.0 -> 47.8: `gd` nearly
  triples and `scene` grows by 0.19 ms, the frame and the FPS are worse, no error line. **`r__gpu_driven_hiz`
  and `r__gpu_driven_main` were removed** with their code (3.8, 3.9); `r__gpu_driven` keeps 0/1 and the QA
  mode 2 that `gd_check` uses.
