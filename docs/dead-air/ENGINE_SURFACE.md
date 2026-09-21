# What is XFined-Ray's and what is still OpenXRay's

This file answers two questions with the same measurement.

1. **How much of this engine is actually its own work.** The rename in
   `c0141d334` gave the engine a name; this file says what is behind it. It
   does not replace `UPSTREAM.md`, which owns provenance and attribution -
   read that one first if the question is who wrote what.
2. **Where the engine has not been touched yet.** Code that is still
   byte-for-byte upstream is code nobody here has measured. That is the map
   for performance work: it says where there is room, and - just as usefully -
   where there is none, because the area is dead, unbuilt, or not ours to
   speed up.

Everything below is measured against the fork point recorded in
`UPSTREAM.md`, upstream commit `29030f81b`.

## How to reproduce the numbers

```bash
BASE=29030f81b137f6ea5365b3d71f2b588490832f5b
git ls-tree -r --name-only $BASE -- src/ > upstream.txt
git ls-files -- src/ > ours.txt
git diff --numstat $BASE HEAD -- src/ > numstat.txt
git diff --name-only --diff-filter=A $BASE HEAD -- src/
```

"Untouched" means a file that existed at the fork point, still exists now, and
does not appear in `git diff $BASE HEAD`. Counted extensions are
`.c .cpp .h .hpp .inl`. Line counts are physical lines of the file as it
stands today. Build-system files, project files and resources are excluded.

## The headline

Whole tree since the fork: **1808 files changed, +233141 / -102442**.
`src/` alone: **1038 files, +83682 / -64851** - 112 files added, 130 deleted,
796 modified.

Of the 4557 upstream source files that are still here:

| | files | LOC |
|---|---|---|
| modified by this project | 783 (17%) | 310774 (38%) |
| still byte-for-byte upstream | 3774 (83%) | 499254 (62%) |

Plus 112 files added under `src/` that did not exist upstream at all - 104 of
them code - including four subsystems that are entirely this project's:
`xrContentSync` (the update and content-bundle client, 16 files),
`xrCore/XMS` (the mod-module system), `xrCore/Content` (content pinning) and
the DX11 render phases added under `Layers/xrRenderPC_R4` (water field,
ripples, clouds, cloud map, puddle reflections, HUD shadow, visor drops, TAA,
fire). 95 of the 104 carry this project's own copyright header; the other nine
are ports that keep their source's.

**So: a third of the engine's shipped code has been rewritten or replaced, and
four subsystems are new.** That is a fork with its own engineering in it, not a
patch set. It is also not a rewrite: the other two thirds is upstream, and
`UPSTREAM.md` says so out loud.

## Where the work went

Single-player runtime only - unbuilt trees, multiplayer, Lua glue, debug and
vendored code are excluded here and accounted for in the next section.

| area | stock LOC | changed LOC | still stock | stock files |
|---|---|---|---|---|
| `xrGame` (root) | 106097 | 105088 | 50% | 1081 |
| `xrGame/ai/monsters` | 39609 | 10110 | **80%** | 410 |
| `xrPhysics` | 20005 | 10098 | 66% | 136 |
| `xrGame/ui` | 19287 | 21085 | 48% | 183 |
| `Layers/xrRender` | 18536 | 35433 | 34% | 187 |
| `xrCore` | 15373 | 15928 | 49% | 118 |
| `xrUICore` | 11094 | 5696 | 66% | 99 |
| `xrEngine` | 10503 | 23918 | **31%** | 93 |
| `xrServerEntities` | 8539 | 9115 | 48% | 60 |
| `xrAICore` | 7590 | 2744 | **73%** | 79 |
| `Common` | 4793 | 349 | 93% | 25 |
| `xrGame/ik` | 4792 | 0 | **100%** | 13 |
| `Layers/xrRenderDX11` | 3463 | 10830 | 24% | 21 |
| `Layers/xrRender_R2` | 3297 | 8330 | 28% | 20 |
| `xrSound` | 3086 | 2033 | 60% | 28 |
| `xrCDB` | 2837 | 3674 | 44% | 12 |
| `xrGame/ai/stalker` | 1217 | 3842 | 24% | 7 |
| `xrScriptEngine` | 1156 | 318 | 78% | 12 |
| `xrParticles` | 752 | 3408 | 18% | 9 |
| `xrGame/ai/{trader,phantom,.}` | 1852 | 0 | **100%** | 12 |

The shape of it: **the render path and the engine core have been worked over
hard** (renderer 24-34% stock, `xrEngine` 31%, `xrParticles` 18%,
`ai/stalker` 24%), because that is where the visible work of this project has
been - water, clouds, wind, shadows, particles, the HUD, the frame loop.
**Simulation has barely been touched.** Monsters, physics, pathfinding, IK and
the trader are close to untouched, and that is the whole finding of this
document.

## What the 499254 untouched lines actually are

Not all of it is a target. Most of it is not.

| bucket | LOC | files | share | target? |
|---|---|---|---|---|
| single-player runtime | 286148 | 2661 | 57% | **yes** |
| not built | 141868 | 716 | 28% | no |
| multiplayer | 41089 | 145 | 8% | no |
| Lua export glue | 12328 | 193 | 2% | no |
| vendored third-party | 10821 | 21 | 2% | no |
| debug-only | 7000 | 38 | 1% | no |

- **not built** - `src/utils/` (its `CMakeLists.txt` adds exactly two
  subdirectories, `xrMiscMath` and this project's own `DarDelta`; the rest,
  including the 120k-line `mp_gpprof_server`, is never linked), `src/editors/`
  (never referenced by any `add_subdirectory`), and the GL renderer, whose
  sources are deliberately retained but excluded from the build. Deleting any
  of it would change nothing the player runs. Optimising it would change
  nothing at all.
- **multiplayer** - `game_sv_*`, `game_cl_*`, GameSpy, the net server, demo
  record and play, spectator and CTA UI. Dead Air is single-player; none of
  this executes in a normal session.
- **Lua export glue** - `*_script.cpp` luabind registration. It runs once at
  script-engine init. The cost that matters there is LuaJIT's, and that has
  already been addressed (see `DEPENDENCIES.md`).
- **vendored** - tinyxml, NvTriStrip, NVMeshMender, LZO, OPCODE, MxQ. Third
  party by definition; changing it makes it ours to maintain, and the licences
  say what they say.
- **debug-only** - compiled out or gated behind `DEBUG`.

**The honest target surface is 286k lines**, and within it the ranking below.

## The performance map

Ranked by (still stock) x (runs every frame or every NPC tick). Each entry says
what is there and what the shape of a win would be. None of this has been
profiled yet - that is the point of the list, not a claim about it.

### 1. Monster AI - `xrGame/ai/monsters`, 39609 LOC stock, 80% untouched

The largest untouched block that actually runs. 410 files: the control manager
and its twenty-odd `control_*` behaviours, the state machine templates
(`monster_state_*_inline.h`), per-species subclasses, `ai_rat_fsm.cpp` (727),
`controller.cpp` (804). Every online monster steps this each schedule tick, and
Dead Air's A-Life keeps a lot of them online.

Where to look first: `control_manager_custom.cpp` (713), `control_jump.cpp`
(706), `base_monster.h` (620) and the state templates - those are header-inline
templates instantiated per species, so anything quadratic in them is paid many
times over. The upstream design dispatches per tick through `CControl_Manager`
for every active control element.

### 2. Physics - `xrPhysics`, 20005 LOC stock, 66% untouched

`PHShell.cpp` (1531), `PHWorld.cpp` (604), the ODE collider set
(`dCylinder.cpp` 1613, `dTriCylinder.cpp` 865, `dTriBox.cpp` 830,
`__aabb_tri.h` 437) and `MathUtils.cpp` (601). This is stepped at a fixed
frequency regardless of frame rate, so its cost is a floor, not a slope.

The collider files are the 2004-era Knoopc triangle-collider set: scalar maths,
no SIMD, per-triangle allocation patterns. `PHShell`/`PHWorld` own the
island/disable logic that decides how much of the above runs at all - tuning
what goes to sleep is usually worth more than making the maths faster.

### 3. `xrGame` root, per-NPC managers - ~16k LOC stock across 40 files

Untouched and on the NPC tick path: `sight_manager.cpp` (798),
`detail_path_manager_smooth.cpp` (882), `agent_enemy_manager.cpp` (705),
`ef_primary.cpp` (618 - the evaluation functions every selector calls),
`stalker_movement_manager_smart_cover*.cpp` (1279 together),
`sound_memory_manager.cpp` (538), `movement_manager.cpp` (451),
`enemy_manager.cpp` (418), `cover_evaluators.cpp` (374).

`ef_primary` and the evaluators are the hottest of these by call count - they
are the leaves of goal selection, called O(NPCs x goals) per tick.
`detail_path_manager_smooth` does the curve fitting for every moving NPC path.

### 4. Pathfinding - `xrAICore`, 7590 LOC stock, 73% untouched

`level_graph_vertex_inline.h` (702) and `level_graph_vertex.cpp` (561) are the
inner loop of every A* expansion; `a_star_inline.h` (200), `dijkstra_inline.h`
(173), `data_storage_bucket_list_inline.h` (254) and
`vertex_manager_hash_fixed_inline.h` (153) are the search itself. The
`problem_solver_inline.h` (442) / `operator_abstract_inline.h` (432) pair is
the GOAP solver behind every stalker decision.

Note what has been done here already: the exact indexed nearest-vertex lookup
and the sustained path-failure backoff (`UPSTREAM.md`). The search core proper
is untouched - bucket-list open set, hash vertex manager, no heuristics beyond
upstream's.

### 5. Collision queries - `xrCDB`, 2837 LOC stock

Small but scalding: `xrCDB_ray.cpp` (589), `xrCDB_box.cpp` (339),
`xrCDB_Collector.cpp` (409), `Intersect.hpp` (917). Every ray the game casts -
AI vision, bullets, the rain-cover oracle, the fire and wind probes this project
added - lands here. The spatial layer above it (`ISpatial_*`, `xr_area.cpp`) has
already been reworked and is SSE where it counts; the CDB triangle test
underneath still is not.

`Intersect.hpp` is the highest value-per-line file in this document: 917 lines
of scalar ray/triangle and AABB tests called from everything.

### 6. Foot IK - `xrGame/ik`, 4792 LOC, **100% untouched**

`IKLimb.cpp` (1262), `math3d.cpp` (1017), `Dof7control.cpp` (725). Runs per
visible NPC per frame. The distance gate added for it (`ph_ik_dist`, parity
default 0, see `UPSTREAM.md`) skips the work at range but does not make the
work cheaper - the solver itself has never been opened.

### 7. Skinning - `Layers/xrRender/FSkinned.cpp` (809), `SkeletonX.cpp` (668)

Untouched in an otherwise heavily reworked renderer: bone transform preparation
and the single/multi-weight paths, per skinned visual per frame.

### 8. UI - `xrUICore` 11094 stock, `xrGame/ui` 19287 stock

`UIListWnd.cpp` (662), `UIMap.cpp` (704), the window and frame primitives. This
only costs anything while a menu, the PDA or the inventory is open - but that is
exactly when this project's own 3D PDA and backpack scenes are also running, so
it is the frame budget of the screens this project cares most about.

## What must not be "optimised"

The rules that already govern this repository apply to all of the above, and
they will kill a naive optimisation faster than a profiler will justify one.

- **Parity is the acceptance test, not frame time.** Simulation code determines
  save content and A-Life outcomes. A faster path that changes a decision, a
  seed order or a serialised field is a regression however fast it is. Anything
  touching the areas above needs a save-compatibility argument
  (`SAVE_COMPATIBILITY.md`) before it needs a benchmark.
- **`pairs()` order, ID order and float determinism are load-bearing.** The
  LuaJIT string-id work (`DEPENDENCIES.md`) exists because of exactly this.
- **Measure on the rig, not on a hunch.** The probes under `tools/qa/` exist; a
  change here without a before/after from one of them is not finished.
- **Third-party stays third-party.** The vendored bucket is off the table by
  policy, not by difficulty.
