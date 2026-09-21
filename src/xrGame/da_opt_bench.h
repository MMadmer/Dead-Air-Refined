// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// Benchmarks for the engine's stock hot paths (docs/dead-air/OPTIMIZATION_PLAN.md).
// They exist to give an optimization a before/after on real level data instead of a
// guess, and they double as equivalence checks: every one of them prints a workload
// invariant (hit counts, summed ranges, expanded nodes) that an optimization is not
// allowed to move.
void da_opt_bench_register();
