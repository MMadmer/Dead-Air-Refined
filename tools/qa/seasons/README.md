# Season probes

Two in-game probes for `r__season` (see `docs/dead-air/SEASONS.md`). Put one at a time into
`<rig>\gamedata\scripts`, run it on a save, remove it afterwards.

- `qa_season_probe.script` walks the six looks (autumn, winter, early spring, spring, late spring,
  summer) and shoots the scene once per look with the engine's capture (`appdata\screenshots`, a burst
  of one or two shots per look). Marker lines: `[qa] season <look>`, `[qa] SHOT season_<look>`,
  `[qa] qa_season_done`.
- `qa_weather_season_probe.script` opens the weather dialog ("Опции погоды"), shows its Seasons page,
  picks winter, accepts, and checks that `r__season` followed. The engine's capture has no UI: run
  `Watch-QaShots.ps1 -DoneMarker qa_weather_season_done` next to it to grab the window at its `[qa] SHOT` lines.

```
pwsh -Command "& tools/qa/Run-ContentProbe.ps1 -Rig '<rig>' -Label seasons -Commands 'rs_always_active on','start server(<save>/single/alife/load)' -RunSeconds 110 -WaitFor 'qa_season_done' -ExtraArgs '-always_active -force_flushlog'"
```

`rs_always_active on` first: with a player's own user.ltx (`rs_always_active off`) the unfocused engine on
the hidden desktop pauses right after the load, frames keep coming and the actor never updates.
