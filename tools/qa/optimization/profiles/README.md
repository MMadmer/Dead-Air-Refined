# Profiles

`before.txt` is the first sampling profile taken of this engine, on l01_escape with 1753 objects
online. `after.txt` is the same run once the two things that profile found had been fixed - the
per-frame .ltx re-parsing and the repeated frustum test per detail slot.

Intermediate runs are not kept. Several of them measured ideas that were then thrown away
(bounds by value in VisiblePart, SSE stores for the constant buffer), and a file full of numbers
for code that does not exist invites someone to quote it. What those runs decided is written down
in `docs/dead-air/OPTIMIZATION_PLAN.md`, which is where it is useful.

`grassbase.txt` and `grass-final.txt` bracket the fourth pass over the detail manager, and are
kept as a pair because that is the first pass measured in frames per second rather than in shares
of the thread. `grass-final.jpg` is the frame that rate was measured on - a profile saying a
renderer got cheaper means nothing if what it stopped drawing was the point.

Take a new one with `tools\qa\optimization\Run-Profile.ps1 -Label <name>`. Each run leaves both
`<name>.txt` and `<name>.jpg` here.
