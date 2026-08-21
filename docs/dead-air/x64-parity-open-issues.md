# Dead Air x64: open parity issues

Only currently unresolved x86-to-x64 discrepancies belong here. Workflow and
validation rules are defined in [`PROJECT_RULES.md`](../../PROJECT_RULES.md).

## Open issue

- Expanded combo boxes prefer the `ui_inGame2_combobox` frame whose center is
  effectively transparent, allowing labels below the list to overlap its
  option text.

- The split graph build corrupted local shadow-map content. With lamp graphs
  built in phases introduced by `ad14b5d40` (static casters in a task,
  deferred visuals and dynamics collected separately in the flush) plus a
  multi-light queue, a point-light face periodically rendered its smap with a
  wrong caster set: multi-second blinking at rest and shadows dancing across
  interiors while moving (`light_test` save reproductions). Isolated
  empirically via A/B captures: unaffected by occlusion queries, HOM, sun,
  or serializing the split body with a mutex — the split itself was the
  problem. The reference project has no split build at all and runs a stable
  picture, so local lights now collect their whole caster set through the
  single sequential `build_subspace()` pass (`r2_R_lights.cpp`), one light
  at a time; sun cascades and rain keep parallel builds. Two real races were
  fixed along the way (per-context portal traversal marks in `r__sector.h`,
  the reference light_vis hysteresis) plus the empty sun-cascade slice clear
  and the deterministic size-sort tie-break. QA sweep: 100.6 avg vs 101-103
  baseline. Re-parallelize only with a design that keeps one build phase.

- Device removal (`DXGI_ERROR_DRIVER_INTERNAL_ERROR`, 0x887A0020) reported once
  against 1.3.4 itself, on an AMD RX 6600 in `renderer_r3` (feature level 10.1),
  seconds after a quicksave finished loading on Escape, with a completely clean
  log. The same signature on 1.3.3 is a different, closed defect: those sessions
  carry `input layout NOT created ... shadow_direct_base_aref` and were fixed in
  `3c128db98`. The 1.3.4 case has no such marker and does not reproduce offline
  on the reporter's own save across repeated loads on NVIDIA hardware, in both
  `renderer_r4` and `renderer_r3`. Two independent audits of the parallel render
  path found no provable cause and ruled out, with evidence: the reset and
  first-frame path (a level load never calls `Device.Reset`, so nothing dangles),
  the deferred command-list lifecycle (record and `FinishCommandList` are joined
  by the task graph before `flush()` submits), context allocation (all
  `alloc_context` calls happen on the main thread in `Calculate` before any phase
  runs), the occlusion-query set (`light::~light` nulls itself out of
  `Lights_LastFrame`, which `reset_begin` clears), `CKinematics::CalculateBones`
  (recursive lock plus atomic epoch), the multithreaded texture upload (device
  calls only, immutable creation), the detail and constant-buffer paths (both
  per-context), and the completeness of the 1.3.4 particle-race fix. One real
  feature-level defect was found on the way and is fixed: HDAO "ultra" enabled
  its compute path from `ComputeShadersSupported` alone, which a 10.x device
  reports true for CS 4.x, while the UAV bind flag needs feature level 11.0 - the
  dispatch ran against a null UAV. That is a guaranteed device removal for a DX10
  user who selects HDAO Ultra, but the reporter's configuration selects HBAO, so
  it does not explain this report. Narrowing needs data from the affected
  machine: `r2_mt_render 0` plus `r2_mt_calculate 0` forces the sequential
  pipeline the x86 reference used, and `-dxdebug` drains the D3D11 validator into
  the log, naming any invalid bind or draw that precedes the removal.

## Deferred diagnostics candidate

- Early vectored capture of silent fatal failures (heap corruption, stack
  overflow) that bypass `SetUnhandledExceptionFilter` (`xrDebug.cpp:613`).
  Reference concept exists (`DeadAir-x64` engine, commit `abc28441`), but a
  crash handler may ship only after a conclusive safety review on an isolated
  crash harness: no heap allocation, no logger locks, one-shot guard, always
  `EXCEPTION_CONTINUE_SEARCH`. That review has not been performed, so the
  feature is deliberately not integrated yet.
