# RFC: temporal reconstruction / upscaler renderer migration

Status: **draft, research track — no implementation without explicit user approval.**

Provenance: concept and defect history from `DeadAir-x64/DeadAir-Engine-x64-OpenSource`
(reviewed at `50316f59ad62f595e7d5c6bd2f6ea025027d8285`), which ships DLSS/FSR2-3/XeSS as a
tightly coupled renderer fork. This RFC defines what a Refined-native migration would need.
Nothing from the reference temporal fork may be cherry-picked in isolation: without the full
contract below, copied shaders produce inconsistent screen spaces and resource dimensions.

## Scope and invariants

- A 100% native-resolution parity path remains the default and must stay bit-comparable to
  the current R4 renderer with the feature disabled.
- No save, Lua, gameplay, or default image-quality dependency in any phase.
- Every geometry class (static, skinned, vegetation, particles, water, HUD, sky, details)
  participates in one coherent screen-space contract, or the feature does not ship.
- All upscaler contexts and GPU resources acquire explicit reset/shutdown ownership inside
  the existing device-reset flow (`reset_begin`/`reset_end`, context pool teardown).

## Phase contract

1. **Resolution ownership.** Introduce explicit render-resolution vs display-resolution
   ownership for every RT/DS the frame touches. Today all R4 targets are display-sized;
   every `u_setrt`/viewport site must resolve through one authority. Device reset,
   `vid_mode`, and quality changes re-negotiate both sizes atomically. Reference defect to
   avoid: RT/DS dimension mismatch after render-scale changes, FSR3 render-size mismatch.
2. **Jittered projection.** One jittered projection matrix published per frame
   (Halton sequence), consumed by every world-space path — including particles, details,
   and service geometry. UI, HUD readouts, and history reprojection consume non-jittered
   matrices. Reference defect to avoid: missing geometry/particle jitter (ghosting bands).
3. **Motion vectors.** G-buffer velocity target with defined encoding; previous-frame
   transforms for static (camera-only), skinned (previous bone palettes), vegetation
   (wind-state history — note `FTreeVisual` wind is currently stateless by design; adding
   accumulated wind state requires switching its thread_local snapshot to one-writer
   publication in the same change), and particles. Disocclusion and camera-cut signaling.
4. **Reactive/transparency masks.** Water, rain, glass, particles, and materials without
   depth/velocity coverage get a reactive mask channel. Reference defect to avoid: water
   alpha-clip instability under temporal accumulation.
5. **Postprocess integration.** Reconstruction runs at a fixed point; every later shader
   element (color correction, distortion, bloom chain) consumes the reconstructed image
   through one path. Reference defect to avoid: postprocess element sampling a mismatched
   pre-reconstruction target.
6. **Lifecycle.** Upscaler context creation strictly after target creation, destruction
   strictly before target destruction; covered paths: device reset, resolution/quality
   change, renderer restart, process shutdown. Reference defect to avoid: context
   destruction order crashes.
7. **Shader permutations.** New permutation dimension (jitter on/off, velocity on/off)
   with cache keys that invalidate on contract changes; offline compilation validation.
   Reference defect to avoid: stale shader cache after temporal changes.
8. **Backends and packaging.** SDK-independent internal interface first; backends second.
   Licensing matrix (all subject to re-verification at implementation time):
   - FSR 2/3 — MIT, source integration possible, no runtime redistribution issue;
   - XeSS — Intel SLA, binary SDK redistribution per its license terms;
   - DLSS — NVIDIA SDK license, binary redistribution terms and telemetry clauses must be
     reviewed before any commitment; hardware-gated.
   Unsupported hardware falls back to the parity path silently; backend init failures are
   diagnosed through the existing crash/report facility, never fatal.

## Dependency graph

Phase 1 → 2 → 3 → 5 → 7 are strictly ordered. Phase 4 depends on 3. Phase 6 spans all.
Phase 8 (backend selection) only starts after 1–7 are accepted on the parity path with a
null "identity upscaler" backend used to validate the whole contract without any SDK.

## Milestones

- M1: resolution ownership + identity backend, parity screenshots identical.
- M2: jitter + motion vectors with debug visualization, still identity backend.
- M3: reactive masks + postprocess integration, identity backend.
- M4: first real backend (FSR2 suggested: source-level, MIT), A/B image QA.
- M5: additional backends, packaging, fallback QA.

Each milestone is independently revertible; rollback is deleting the milestone branch.

## Risk register

- Screen-space incoherence (any geometry class missed) — visible ghosting; mitigated by
  per-class debug visualization defined below and the identity backend.
- Resource lifetime regressions on reset — mitigated by phase 6 tests before any backend.
- License/redistribution failure for a binary SDK — mitigated by phase 8 gating and FSR
  first; no SDK binary is committed without verified redistribution rights.
- Maintenance cost: the permutation matrix grows; phase 7 cache keys must carry it.

## QA matrix (defined, not run)

Static and moving camera; vegetation wind; particles and anomalies; water bodies and rain;
scopes/HUD overlays; color correction presets; weather cycle; indoor/outdoor transitions;
resolution and quality changes at runtime; repeated backend switching; device reset loop;
backend init failure and unsupported hardware; long soak with live-object reporting.
Instrumentation: image captures, motion-vector and reactive-mask visualization modes,
D3D debug layer runs, GPU/CPU timings per phase — all through temporary QA builds only.

## Explicitly rejected for this track

Cherry-picking reference temporal shaders; committing SDK binaries without verified
redistribution rights; any default render-scale or image change; treating one backend's
acceptance as validation of the shared contract.
