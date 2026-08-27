# Dead Air x64: open parity issues

Only currently unresolved x86-to-x64 discrepancies belong here. Workflow and
validation rules are defined in [`PROJECT_RULES.md`](../../PROJECT_RULES.md).

## Open issue

- Expanded combo boxes prefer the `ui_inGame2_listbox` frame whose center is
  effectively transparent, allowing labels below the list to overlap its
  option text.

- Report 20260826T075952 ("тени, лоды снова"): sun shadows "turn to mush" and
  drop out near the screen edges at particular view angles, and vegetation still
  vanishes at distance, on `renderer_r4` at fov 90 with the Extreme preset and
  `r3_msaa 2x`. The lod half was already audited line-by-line against the x86
  reference in the 1.3.4 tail and the fov-75 pin verifiably ships in the R4
  binary, so what remains is the shadow-edge half. It could not be reproduced on
  the rig: the reporter's own save embeds a third-party addon model
  (`drug_caf.ogf`) and does not load on a clean stand, and the outdoor stand
  save never produced a hard-sun sky through the AtmosFear forcing window. The
  MSAA edge-stencil path of the sun accumulation is the standing suspect since
  every symptom is tied to the one config with MSAA on. Needs the reporter's
  demo or a screenshot pair (msaa 2x vs off, same spot and angle) on 1.3.6,
  where the new `! [hud_shadow] ... MSAA is active` line also confirms the MSAA
  state in the session log.

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

  A timing signature appeared and has since been refuted, which is worth keeping
  because it redirects the search. The first cluster all sat in the first minute:
  48.7 s on the AMD RX 6600, 55.9 s and 44.3 s on an NVIDIA RTX 3070 Laptop, all
  three on Escape and all at a healthy frame rate right up to the fatal. Two later
  removals with the same signature do not fit that window at all: 909.5 s and
  1035.3 s into the session, on an NVIDIA RTX 4060 Ti (feature level 12.1,
  `renderer_r4`), in the underground labs - `l04u_labx18` and `l08u_brainlab` -
  again at a healthy frame rate, 71 and 72 fps. So the wall-clock window is not the
  invariant, and three GPU models across two vendors keep a vendor bug ruled out.

  What survives across every case is the operation, not the clock. The AMD report
  died seconds after a quicksave finished loading; the two lab reports carry
  `last_operation` `load_game` and `save_game` respectively. A save or a load is
  the moment this engine churns the most device-adjacent state in the shortest
  time - resource creation and release, the model pool, the texture upload path -
  and it is the one thing common to the early cluster and the late one. That is the
  next thing to instrument. The 1.3.3 report at 1351 s remains a different, closed
  defect: those sessions carry `input layout NOT created ... shadow_direct_base_aref`
  and were fixed in `3c128db98`.

## Standing constraints

These are closed defects whose fix imposes a rule on future work. They are not
open problems; they are here so the rule is not lost with the bug.

- Local light shadow maps must collect their casters in ONE build phase. The
  split build introduced by `ad14b5d40` (static casters in a task, deferred
  visuals and dynamics collected separately in the flush) plus a multi-light
  queue made a point-light face periodically render its shadow map with a wrong
  caster set: multi-second blinking at rest, shadows dancing across interiors
  while moving. A/B captures cleared occlusion queries, HOM, the sun and even a
  mutex around the split body - the split itself was the cause, and the
  reference project has no split build at all. Local lights therefore build
  through the single sequential `build_subspace()` pass in `r2_R_lights.cpp`,
  one light at a time, while sun cascades and rain keep their parallel builds.
  Re-parallelize the light phase only with a design that keeps one build phase.

## Deferred diagnostics candidate

- Early vectored capture of silent fatal failures (heap corruption, stack
  overflow) that bypass `SetUnhandledExceptionFilter` (`xrDebug.cpp:613`).
  Reference concept exists (`DeadAir-x64` engine, commit `abc28441`), but a
  crash handler may ship only after a conclusive safety review on an isolated
  crash harness: no heap allocation, no logger locks, one-shot guard, always
  `EXCEPTION_CONTINUE_SEARCH`. That review has not been performed, so the
  feature is deliberately not integrated yet.
