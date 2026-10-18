# Lua runtime: collector, JIT and luabind

The scripts of Dead Air and of every mod run on LuaJIT through luabind. None of the changes below alters what a script
sees: the same functions, the same objects, the same `collectgarbage()`. They change how much of a frame the runtime
itself takes, and above all how evenly.

## Where the time went

On the lamp hall of `light_test` at night the game thread spent 19 % of its Lua time in the collector, about 1.3 ms a
frame, and still stalled: one frame of 37 to 47 ms in three seconds out of four. Two things caused it.

- `CLevel::script_gc` forced a step of 100 KB every frame (`lua_gcstep 100`) against roughly 60 KB the scripts
  allocate. A cycle never ended: every live object was marked over and over.
- Each cycle closes with LuaJIT's atomic phase, which cannot be split. Most of it was `lj_gc_separateudata`, a walk over
  every userdata alive to find the ones to finalize, and every object luabind hands to a script is a userdata with a
  finalizer: each `vector()`, each position a function returns, each handle of a game object. A play session drops
  about 300,000 of them in half a minute, and each was walked twice, marked, finalized by a call through the VM and
  freed a cycle later.

## The collector's pace

`CScriptEngine::pace_garbage_collector` replaces the forced step. The `lua_gcstep` console command is still accepted
with its range (user.ltx carries it, a mod may set it), but it forces nothing: the saved default would bring the step
back on every start. The
collector runs on the scripts' allocations alone, as LuaJIT intends, and a cycle starts once the heap has grown by a
headroom past what the last cycle left: the heap's size, 64 MB at least and 256 MB at most, never more than half the
low arena still free (without `LJ_GC64` every state lives in its 1.5 GB). The heap therefore saws between the live
data and about twice that, 40 and 90 MB on the test save, and comes back to the live size after every cycle. A script
that sets its own pause (`collectgarbage("setpause", n)`) keeps it: the engine notices the value changed under it,
puts it back and stops pacing that state.

`lua_gc_method` still switches to the other modes (`gc_disable`, `gc_timeout`, `gc_full`); `gc_step` is the pacing.

## A frame budget

`luaJIT_gcframe(L, usec)` is called once a frame (1000 us). The collector's implicit steps, those an allocation
triggers, stop for the frame once they have taken that long, and the rest of the cycle goes on in the next frames.
Without it the work landed in whichever frame allocated most: the JIT counts a full step for every allocation of a
trace, and a loop over the NPCs could run a dozen milliseconds of sweep in one frame. Two things are never held back:
`lua_gc()` requests (`collectgarbage("step")`, `"collect"`, the timeout mode), and a heap that has grown past half
the headroom again over the cycle's threshold, so a collector that cannot keep up within the budget catches up at
once instead of letting the heap run away. A budget that is not renewed lapses after a quarter of a second.

## Userdata without finalizers

luabind decides, when it creates an instance, whether destroying it would do anything:

- the holder is a raw pointer the instance does not own (every game object, every `level.object_by_id()`), or a value
  with a trivial destructor (`vector`, `CTime`, flags), and
- it lies inline in the instance (the buffer grew from 32 to 48 bytes, so pointer holders fit and no longer cost a heap
  block of their own), and
- the class is a C++ class, not one a script derived (those may have `__finalize`).

Such an instance leaves LuaJIT's userdata list at birth (`luaJIT_udata_nofinalize`): it is freed by the sweep like a
table, without a finalizer and without the atomic phase ever looking at it. A small trivial value a script constructs
(`vector()`) is built inside the instance instead of on the heap behind an owning pointer: one allocation instead of
three. The objects an instance keeps alive (`dependency`, `return_ref` policies) moved from a registry reference,
which only a finalizer could release, into the instance's own uservalue table, under keys no script can make.

Of the 300,000 finalizers a cycle about 3,000 are left: owned objects such as UI windows, ini files and patrols.

## Marked userdata set aside

Nothing turns a userdata white again within a cycle, so one already marked is certain to survive it. Once propagation
has run dry, the collector moves the marked userdata to a list of their own, 40 at a time, and the atomic phase walks
only the rest. The list goes back at the end of the atomic phase, before a full collection that catches a cycle in the
middle, and at `lua_close`, which finalizes every userdata.

## JIT limits

LuaJIT's defaults (1000 traces, 512 KB of machine code) are sized for small programs. The game keeps about 3,100 traces
alive after a minute; with the defaults the cache filled, was flushed whole and recompiled. The engine starts the JIT
with `maxtrace=8000`, `maxmcode=16384` and `sizemcode=256`; on the test save no flush happens and six traces abort in
ten seconds.

## luabind lookups

Every instance luabind returns looked its class up in the registry by name: a string hashed per call. The class maps
are now also stored under keys of their own addresses. `destroy_instance` finds `__finalize` directly while the
metatable's `__index` is luabind's own, instead of going through the VM.

## LuaJIT update

LuaJIT is upstream v2.1 `dd3b2436` (2026-10-18) with the Dead Air changes merged on top (`DEPENDENCIES.md`). The CMake
wrapper now rebuilds `buildvm` whenever a LuaJIT header changes: the VM's assembly bakes in the offsets of `lj_obj.h`,
and a stale `buildvm` left a VM reading the old layout.

## Measurements

`light_test` at night, 2560x1440, paired runs against the previous build.

| | Before | Pacing only | Now |
| --- | --- | --- | --- |
| Collector, game thread | 1.3 ms a frame | 0.35-0.7 ms | 0.0-0.4 ms |
| Atomic phase | 14 ms every 1.3 s | 16-47 ms every 15 s | 3 ms every 25 s |
| Finalizers a cycle | - | 300,000 | 3,000 |
| R6: frames over 30 ms | in 32 of 44 s | - | in 0 of 44 s |
| R6: worst frame | 45.6 ms | - | 27.5 ms |
| R6: frame rate | 61.4 | - | 65.8 |

DX12 (R5) went from 64.4 to 67.7 and DX11 (R4) from 76.8 to 80.6 with the pacing alone. A save and a load (the state
closed with every finalizer, a new one opened) run without a script error.

## What a mod may notice

- `lua_gcstep` takes and reports its value as before, but no longer forces a step.
- `collectgarbage("count")` reports a heap that grows and falls back in a saw instead of a flat line; the live part
  (the heap right after a cycle) is what it was.
- Userdata die in a slightly different order within a cycle once some were set aside. Finalizers of luabind objects do
  not depend on each other's order.
- A metatable with `__gc` set on a luabind object of a trivial class through `debug.setmetatable` is not honoured.
