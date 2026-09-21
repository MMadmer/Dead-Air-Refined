# Optimization plan for the untouched engine

`ENGINE_SURFACE.md` says where the stock code is. This file says what to do with
it, in what order, and what each change is allowed to cost in behaviour.

The target is the 286k lines of untouched single-player runtime. Nothing here is
a rewrite for its own sake: every item below is a change that removes work,
never one that merely moves it.

## Rules of engagement

These bind every item. An item that cannot satisfy its tier does not ship.

**Tier A - bit-exact.** Anything whose result is serialized into a save, crosses
the net layer, feeds an RNG draw, or orders a container that is later traversed.
Only transformations that provably produce identical bits: removing redundant
work, changing memory layout, hoisting invariants, SIMD that keeps the same
operation order per lane. No reassociation, no reciprocal-for-divide, no fused
multiply-add where the original had two roundings.

**Tier B - semantically identical.** Pure math on continuous quantities
(distances, collision parameters, cover squares) whose consumers threshold far
from float epsilon. Reassociation permitted only with a stated error bound and
only when bit-exactness costs real performance. Preferred outcome is still
bit-exact; Tier B is an escape hatch, not a licence.

**Tier C - free.** Redundant lookups, dead branches, container misuse, cache
layout, allocation churn. No numerical change is possible at all. Most of this
plan is Tier C, deliberately.

**Compatibility boundaries that override everything:**

- Save format and content (`SAVE_COMPATIBILITY.md`). No struct that is written
  to a save changes layout, order or size.
- The modding contract (`MODDING.md`): exported Lua names are never withdrawn,
  console command names and their value ranges stay, XMS module behaviour stays.
- XFined Editor reads the same file formats through the same code. Anything
  touching `xrCDB` build/serialize, level graph format or `.ogf`/`.omf` parsing
  keeps its on-disk representation byte-identical.
- Original Dead Air data and mods built against it must load unchanged.
- Exported symbols: `xrCore.def` exports by ordinal. Signatures of exported
  functions do not change.

**Every item is verified, not asserted.** The bar is:

1. The engine builds clean and the change is described in terms of operations
   removed, not "should be faster".
2. For anything on a measurable path, a before/after from `dar_bench_*` on the
   QA rig, same rig, same level, same seed.
3. Mods QA 47/47 and a rig boot with a real save, log clean.
4. For Tier A items where the transformation is non-obvious, a self-check that
   the new path and the old one agree on generated input.

## Stage 0 - measurement first

Without numbers this is guesswork, and guesswork is how you spend a week making
something 2% faster. Before a single optimization lands:

- `dar_bench_cdb` - casts a fixed pseudo-random ray set against the loaded
  level's collision model in each of the four query modes, plus box and frustum
  queries, and reports total time, rays/s, and the hit count (the hit count is
  the equivalence check: it must not move).
- `dar_bench_path` - runs a fixed set of level-graph searches between seeded
  vertex pairs and reports time, nodes expanded and path lengths (again, the
  expansion count and path length are the equivalence check).
- Both are deterministic given the same level and seed, print one machine-
  readable line each, and exist only to be run from the QA rig.
- `tools/qa/optimization/Run-OptBench.ps1` boots the rig on a save, runs both,
  and writes the numbers next to the results of the previous run.

Baseline captured **before** any change, from the current binaries.

## Stage 1 - xrCDB, the collision database

2837 stock lines under every ray the game casts: AI vision, bullets, the rain
oracle, fire and wind probes, foot IK, the character controller.

1. **Ordered BVH descent for nearest queries - implemented, measured, and
   reverted.** The theory was sound: `isect_sse` already returns the slab entry
   distance, so visiting the near child first is free and should shrink `rRange`
   sooner. On l01_escape it bought nothing outside the noise band, and it moved
   `mat` and the last digits of `range_sum` on the non-culled nearest query,
   because coincident oppositely-wound triangles land within a ULP of each other
   and the tie then resolves the other way. A Tier B change that buys nothing is
   not worth its risk, so it is gone. Recorded here because the next person to
   read this file will have the same idea.
2. **The prefetch hint was the whole story.** `_stab` prefetched one child with
   `_MM_HINT_NTA` - non-temporal, "do not keep this line". The BVH of this level
   is 2.1M triangles, about 80 MB of 40-byte nodes, and the nodes near the root
   are re-read by every single ray the game casts. They are the most reusable
   data in the engine and the collider was explicitly telling the cache to throw
   them away. `_MM_HINT_T0` on both children instead. Tier A, and worth more than
   every arithmetic change in this stage put together.
3. **Cheaper node AABB load.** `_box_sse` builds the box from `mCenter`/
   `mExtents` with six `load_ss` plus four shuffles. The two `Point`s are
   adjacent in `CollisionAABB`, and lane 3 of the result is provably never read
   by `isect_sse`, so two unaligned 4-float loads replace all ten operations.
   Tier A - lanes 0..2 are bit-identical.
4. **Defer the result payload.** `_prim` copies three `Fvector`s and the
   material word into `RESULT` on every improving hit. For a nearest query only
   the last one survives. Track the winning primitive and materialize once.
   Tier A.
5. **Drop the dead FPU collider on x64.** `ray_query` branches on `CPU::HasSSE`
   at every call and instantiates sixteen template variants; eight of them can
   never run on an architecture where SSE2 is part of the ABI. Compile them only
   where they are reachable. Tier A, and it shrinks the instruction cache
   footprint of the hot eight.
6. **Same treatments for `xrCDB_box.cpp` and `xrCDB_frustum.cpp`**, which
   share the shape of the ray collider.
7. **`Intersect.hpp`.** `TestRayTri`, `TestRayTri2`, `TestSphereTri`,
   `TestBBoxTri` are used from `Feel_Vision` (AI sight), `SkeletonX` (hit
   detection), `PHCharacter`/`PHSimpleCharacter` (per-frame character
   collision), `DetailManager`. Remove the repeated recomputation inside them
   and give the `Fvector**` and `Fvector*` overloads one shared body instead of
   two divergent copies. Tier A.
8. **`xrCDB_Collector`.** `RESULT` growth through `emplace_back(RESULT())`
   zero-fills 56 bytes and then copies them, every hit. Reserve and write in
   place. Tier A.

## Stage 2 - xrAICore, pathfinding and the GOAP solver

7590 stock lines, 73% untouched, on the tick of every stalker and monster.

1. **One lookup instead of two.** `CAStar::step` calls
   `data_storage.is_visited(index)` and then `data_storage.get_node(index)` for
   the same index, for every neighbour of every expanded node. Both managers pay
   for it twice - the fixed manager reads the same array twice, the hash manager
   walks the same chain twice. Add `find_vertex(index)` returning a pointer and
   use it. Tier A.
2. **Split the fixed vertex manager into parallel arrays.**
   `CVertexManagerFixed::IndexVertex` is `{u32 path_id; Vertex* vertex;}` packed
   to 12 bytes. `is_visited` is the most frequent operation in the whole search
   and only needs the `u32`. Splitting gives 16 path ids per cache line instead
   of 5.3, and drops the misaligned pointer. Same total memory. Tier A.
3. **`#pragma pack(1)` removal where it buys nothing.** Both vertex managers
   pack structures whose members are already naturally aligned at their packed
   offsets, which only tells the compiler to assume misalignment. Tier A.
4. **Bucket list and binary heap**: remove per-operation recomputation of the
   bucket id and the redundant re-scan in `remove_best_opened`. Tier A.
5. **`level_graph_vertex_inline.h`.** The cover functions
   (`vertex_high_cover`, `compute_square`, `square`) are called per candidate
   vertex by the cover evaluators and recompute `float(cover)/15.f` four times
   per call through a macro. Hoist. Tier A.
6. **`CLevelGraph::distance(position, point0, point1)`** takes a square root and
   then three divisions by the same scalar. Restructure to compare against
   `t*t` and `d*d` so the normalize disappears on the two early-exit paths,
   which are the common ones. Tier A on the early paths.

## Stage 3 - xrPhysics

20005 stock lines, stepped at a fixed frequency, so its cost is a floor.

1. **The Knoopc triangle colliders** (`dTriBox` 830, `dTriCylinder` 865,
   `dTriSphere`, `dTriList`, `__aabb_tri.h` 437). Separating-axis tests that
   recompute `dDOT14(axis, R+i)` for the same axis and rotation repeatedly,
   and `Point` operators that return by value into temporaries. Hoist the
   invariant dot products out of the axis loops. Tier A.
2. **`dxTriList` culling.** Every triangle handed to the collider gets a full
   SAT test; a cheap AABB overlap rejection in front of it removes most of them.
   Tier A - a rejected pair produces no contact either way, and the test is
   conservative.
3. **`PHShell`/`PHWorld` island and disable logic.** What goes to sleep decides
   how much of item 1 runs at all; this is worth more than the maths. Tier A -
   no threshold changes, only removing repeated traversals.
4. **`MathUtils.cpp`** - `dNormalize3`/`dNormalize4` style helpers on paths that
   already know the magnitude. Tier A.

## Stage 4 - the per-tick game managers

~16k stock lines across forty files in `xrGame`, on the NPC tick.

1. **`ef_primary.cpp`** - the evaluation-function leaves, called O(NPCs x goals)
   per tick. Remove repeated `Position()`/`Direction()` round trips and the
   recomputed distances inside a single evaluation. Tier A.
2. **`sight_manager.cpp`, `enemy_manager.cpp`, `agent_enemy_manager.cpp`,
   `cover_evaluators.cpp`, `smart_cover_evaluators.cpp`** - the same pattern:
   per-candidate loops that recompute an invariant per iteration. Tier A.
3. **`detail_path_manager_smooth.cpp`** - curve fitting for every moving NPC.
   Tier A.
4. **`sound_memory_manager.cpp`, `visual_memory_manager`** - the memory
   containers are linearly scanned on every update. Tier A.

## Stage 5 - monsters

39609 stock lines, 80% untouched, the largest block that runs.

1. **`CControl_Manager` dispatch** - per-tick virtual dispatch over every active
   control element, with the element list rebuilt more often than it changes.
2. **`control_manager_custom.cpp`, `control_jump.cpp`** and the
   `monster_state_*_inline.h` templates, instantiated per species so any
   redundancy is multiplied by the bestiary.

## Stage 6 - foot IK

4792 lines, 100% untouched, per visible NPC per frame. `IKLimb.cpp` (1262),
`math3d.cpp` (1017), `Dof7control.cpp` (725). The existing `ph_ik_dist` gate
skips the work at range; this stage makes the work itself cheaper.

## Explicitly not targets

Restating `ENGINE_SURFACE.md` so nobody spends a day here: the 142k lines that
are never built, the 41k of multiplayer, the 12k of luabind registration that
runs once at startup, the 11k of vendored third-party, the 7k behind `DEBUG`.
Also not a target: anything that would change a save, a shipped file format, a
console command's name or range, or an exported Lua name.

## Status

| stage | state |
|---|---|
| 0 - benchmarks and baseline | done - `dar_bench_cdb`, `dar_bench_path`, `dar_bench_frame`, rig runner |
| 1 - xrCDB | done - 16-26% off ray queries, 41% off box queries, bit-identical |
| 2 - xrAICore | done - a few percent off path search, bit-identical |
| 3 - xrPhysics | done - work removed, no bench of its own (see below) |
| 4 - per-tick game managers | done - the duplicated-work sites a scan could find |
| 5 - monsters | partly - the duplicated-work sites; the rest needs a profiler |
| 6 - foot IK | not done - see "What is left" |

Measured results are recorded per stage in the sections above as they land, and
the raw benchmark output under `tools/qa/optimization/results/`.

### Measured

Both sides measured the same way on the same rig: l01_escape, 2166276 triangles, 200000 rays
per mode, 1000 path searches, two runs each, with every other QA scenario parked so nothing
else ticks during the run. "Stock" is this tree with the optimization commits shelved and
rebuilt, not a number remembered from earlier - an earlier baseline taken with another QA
scenario running was inflated by about 10%, and comparing against it would have claimed credit
the code did not earn.

| query | stock (ms) | optimized (ms) | |
|---|---|---|---|
| ray, nearest + cull | 140.7 / 148.8 | 114.3 / 114.7 | -19% |
| ray, nearest | 141.1 / 146.1 | 114.9 / 114.1 | -19% |
| ray, first + cull | 110.4 / 115.5 | 87.1 / 86.7 | -21% |
| ray, all hits | 184.2 / 177.6 | 137.4 / 134.5 | -26% |
| ray, nearest, 10 m | 56.3 / 53.2 | 44.7 / 44.7 | -16% |
| box | 5.86 / 5.59 | 3.35 / 3.46 | -41% |
| level path search | 160.6 / 168.5 | 159.5 / 157.7 | -3% |

Every workload invariant is unchanged to the last digit on both sides - hit counts, summed
ranges, summed material words, paths found, nodes expanded, total path length. That is what
Tier A is for: the queries got a fifth to two fifths cheaper and not one answer moved.

The path search result is the honest one and it is small. `find_vertex` and the parallel-array
split remove real work and the optimized runs are markedly more consistent (157.7-159.5 against
160.6-168.5), but the gain is a few percent, not the 8% an earlier contaminated comparison
suggested. It is kept because it is strictly less work, not because it is a headline.

### Stage 1 detail

Of everything in this stage, the prefetch hint is worth more than all the
arithmetic put together. The lesson generalises: before rewriting maths in this
engine, look at what the cache is being told about the data it walks.

## Tried, measured, rejected

Kept here so the next person does not spend the same day on them.

- **Ordered BVH descent for nearest ray queries.** Visit the near child first, keyed on the
  slab entry distance `isect_sse` already returns. Textbook-correct and free to compute. On real
  level data it landed inside the noise band, and it moved `mat` and the last digits of
  `range_sum` on the non-culled nearest query: coincident oppositely-wound triangles compute `t`
  within a ULP of each other and the tie then falls the other way. A Tier B change that buys
  nothing is not worth its risk.
- **Prefetching a node's neighbours in `CAStar::step`.** A second pass over the neighbours
  issuing `_mm_prefetch` on both index arrays, by analogy with the BVH win. Measured **+13%** on
  the path bench: a search's working set is already resident, so the extra `get_value` pass cost
  more than the misses it hid. Reverted along with the `prefetch_vertex` hook it needed.

## What is left

Honestly stated, because the alternative is pretending 286k lines were all reviewed.

- **Stage 5, monsters.** The duplicated-work sites a mechanical scan can find are fixed. The
  remaining 39k stock lines are 410 files of per-species state machines, and optimizing them
  without a profiler that can attribute time inside them would be exactly the guesswork this
  plan opens by warning against. `CControl_Manager`'s dispatch was read and is not the problem:
  `m_active_elems` holds at most a couple of dozen pointers and the cost is inside the virtual
  `update_frame`/`update_schedule` bodies.
- **Stage 6, foot IK.** Read, not changed. `IKLimb`/`math3d`/`Dof7control` are already tight
  about their square roots - each one is used once - so there is no free win of the kind the
  other stages had, and anything beyond that is a solver rewrite.
- **The GOAP solver.** Read. It enumerates every operator for every expanded vertex by design,
  and the world state is a sorted vector walked by merge. No redundancy to remove without
  changing the algorithm.
- **The bucket list's division.** `compute_bucket_id` divides per insert. Caching the reciprocal
  would be a reassociation that changes bucket assignment at boundaries, which changes A*
  expansion order, which changes chosen paths. Tier A forbids it and it stays.
- **A frame-time gate.** `dar_bench_frame` exists and works, but the QA probe runs the engine on
  a hidden desktop without `-always_active` and an unfocused engine throttles its frame loop, so
  it is opt-in (`-Frames N`) and has to be run from a session that can keep the window active.