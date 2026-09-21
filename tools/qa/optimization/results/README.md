# Benchmark results

Two runs of each side, taken the same way on the same rig (l01_escape, a real save, every other
QA scenario parked for the duration). `stock-*` is this tree with the optimization changes
shelved and rebuilt - not a number remembered from an earlier session. Only results taken that
way belong here: an earlier baseline collected while another QA scenario was ticking read about
10% slow, and keeping it around would only invite someone to quote it.

Compare the timings for the effect, and the invariants printed beside them - hit counts, summed
ranges, summed material words, paths found, nodes expanded, total path length - to confirm a
change moved no answers. See `docs/dead-air/OPTIMIZATION_PLAN.md`.
