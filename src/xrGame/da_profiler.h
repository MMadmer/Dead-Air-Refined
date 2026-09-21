// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// A sampling profiler for Release builds (docs/dead-air/OPTIMIZATION_PLAN.md).
//
// The optimization work needs to know which of the engine's stock functions actually burn time
// on a real save, and nothing in the engine could answer that outside a DEBUG build: the
// built-in CProfiler is compiled out of Release, and Tracy needs a live client on the other end
// of a socket, which a headless QA rig has no way to provide. This walks the game thread from
// the outside instead - suspend, unwind, resume - and symbolizes once at the end, so a run costs
// a fraction of a percent and needs nothing but the PDBs that are already built.
void da_profiler_register();
