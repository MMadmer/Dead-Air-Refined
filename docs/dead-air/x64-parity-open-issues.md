# Dead Air x64: open parity issues

Only currently unresolved x86-to-x64 discrepancies belong here. Workflow and
validation rules are defined in [`PROJECT_RULES.md`](../../PROJECT_RULES.md).

## Open issue

- Expanded combo boxes prefer the `ui_inGame2_combobox` frame whose center is
  effectively transparent, allowing labels below the list to overlap its
  option text.

- Parallel graph builds for local shadowed lights corrupt shadow-map content.
  With several lights batched through tasks in `render_lights`
  (`r2_R_lights.cpp`), a point-light face periodically renders its smap with a
  wrong caster set and the face's contribution blinks in multi-second phases
  (Yanov interiors; ceiling lit by the up face was the reproduction). Isolated
  empirically on the `light_test` save via A/B captures: unaffected by
  occlusion queries (`-no_occq`), HOM (`-no_hom`), sun (`r2_sun off`), and by
  serializing `build_subspace_static` bodies with a mutex; only a single-light
  queue is stable. Two shared-state races were fixed on the way (per-context
  portal traversal marks in `r__sector.h`, and reference-ported light_vis
  hysteresis), but the queue-shape dependence remains unexplained. The lamp
  path is therefore serialized (`parallel_static = false`) at no measured
  frame cost (QA sweep: 101.0 avg before, 101.5 after); sun cascades and rain
  keep parallel builds. Re-enable only after the actual shared state is found.

## Deferred diagnostics candidate

- Early vectored capture of silent fatal failures (heap corruption, stack
  overflow) that bypass `SetUnhandledExceptionFilter` (`xrDebug.cpp:613`).
  Reference concept exists (`DeadAir-x64` engine, commit `abc28441`), but a
  crash handler may ship only after a conclusive safety review on an isolated
  crash harness: no heap allocation, no logger locks, one-shot guard, always
  `EXCEPTION_CONTINUE_SEARCH`. That review has not been performed, so the
  feature is deliberately not integrated yet.
