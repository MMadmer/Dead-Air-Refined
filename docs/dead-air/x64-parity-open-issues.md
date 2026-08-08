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

## Deferred diagnostics candidate

- Early vectored capture of silent fatal failures (heap corruption, stack
  overflow) that bypass `SetUnhandledExceptionFilter` (`xrDebug.cpp:613`).
  Reference concept exists (`DeadAir-x64` engine, commit `abc28441`), but a
  crash handler may ship only after a conclusive safety review on an isolated
  crash harness: no heap allocation, no logger locks, one-shot guard, always
  `EXCEPTION_CONTINUE_SEARCH`. That review has not been performed, so the
  feature is deliberately not integrated yet.
