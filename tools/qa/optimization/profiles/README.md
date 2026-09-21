# Profiles

`before.txt` is the first sampling profile taken of this engine, on l01_escape with 1753 objects
online. `after.txt` is the same run once the two things that profile found had been fixed - the
per-frame .ltx re-parsing and the repeated frustum test per detail slot.

Intermediate runs are not kept. Several of them measured ideas that were then thrown away
(bounds by value in VisiblePart, SSE stores for the constant buffer), and a file full of numbers
for code that does not exist invites someone to quote it. What those runs decided is written down
in `docs/dead-air/OPTIMIZATION_PLAN.md`, which is where it is useful.

Take a new one with `tools\qa\optimization\Run-Profile.ps1 -Label <name>`.
