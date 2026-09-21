# Profiles

`before.txt` is the first sampling profile taken of this engine, on l01_escape with 1753 objects
online. `after.txt` is the same run once the two things that profile found had been fixed - the
per-frame .ltx re-parsing and the repeated frustum test per detail slot.

Intermediate runs are not kept. Several of them measured ideas that were then thrown away
(bounds by value in VisiblePart, SSE stores for the constant buffer), and a file full of numbers
for code that does not exist invites someone to quote it. What those runs decided is written down
in `docs/dead-air/OPTIMIZATION_PLAN.md`, which is where it is useful.

`round2-before.txt` and `round2-after.txt` bracket the fourth to eighth passes - the detail
upload, the constant-table switch, and smart_cast - and are kept as a pair because that is where
the measurement moved from shares of the thread to frames per second: **66.8 -> 72.1 FPS** on the
same save, same 30 seconds, uncapped. `round2-after.jpg` is a frame from that run, because a
profile saying a renderer got cheaper means nothing if what it stopped drawing was the point.

Take a new one with `tools\qa\optimization\Run-Profile.ps1 -Label <name>`. Each run leaves both
`<name>.txt` and `<name>.jpg` here.
