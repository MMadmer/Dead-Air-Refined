# Season probes

In-game probes for the time of year (`r__season`, see `docs/dead-air/SEASONS.md`). Put one at a time
into `<rig>\gamedata\scripts`, run it on a save, remove it afterwards. Each holds the weather clock
(`env_time_pin 12:00`) and the wind (`wind_freeze 1`) where it shoots, so frames differ by the season.

- `qa_season_probe.script` pins the season day to the 15th of each month (`qa_season_pin <month>`,
  which also latches that day's texture folders) and shoots the scene once per month with the engine's
  capture (`appdata\screenshots`, a burst of one or two shots per month). Marker lines:
  `[qa] season <month> day <D>`, `[qa] SHOT season_<month>`, `[qa] qa_season_done`. It releases the pin
  at the end; nothing reaches user.ltx.
- `qa_season_year_probe.script` runs the year once (`qa_season_year 60 from=january`) and asks for a
  window shot at every month's 15th (`[qa] SHOT season_year_<nn>_<month>`, twelve of them, January last).
  The engine itself prints `[qa] season_year <month> day <D> at <s> s` at each 15th and
  `[qa] season_year_done <s> s` at the end; the probe quits after `[qa] qa_season_year_probe_done`. The
  run only counts seconds of a rendered level, so a load or a menu does not eat into it, and the texture
  folders stay latched: only the grass, the snow, the stock details, the ground tint and the
  precipitation move.
- `qa_weather_season_probe.script` opens the weather dialog ("Опции погоды"), shows its Seasons page,
  picks July and cancels (`r__season` must not move), opens it again (the list must be back on the
  choice), picks January and accepts (`r__season january`), then puts the player's choice back. Marker
  lines: `[qa] check <what>: PASS|FAIL`. The engine's capture has no UI: run
  `Watch-QaShots.ps1 -DoneMarker qa_weather_season_done` next to it to grab the window at its
  `[qa] SHOT` lines.

```
pwsh -Command "& tools/qa/Run-ContentProbe.ps1 -Rig '<rig>' -Label seasons -Commands 'rs_always_active on','start server(<save>/single/alife/load)' -RunSeconds 110 -WaitFor 'qa_season_done' -ExtraArgs '-always_active -force_flushlog'"
pwsh -Command "& tools/qa/Run-ContentProbe.ps1 -Rig '<rig>' -Label season-year -Commands 'rs_always_active on','start server(<save>/single/alife/load)' -RunSeconds 150 -WaitFor 'qa_season_year_probe_done' -ExtraArgs '-always_active -force_flushlog'"
```

`rs_always_active on` first: with a player's own user.ltx (`rs_always_active off`) the unfocused engine on
the hidden desktop pauses right after the load, frames keep coming and the actor never updates.

## Console commands (never saved)

- `qa_season_year [seconds=60] [from=<calendar|january..december|0..365>] [loop]` runs the season day
  through the year in that many wall-clock seconds of a rendered level (time_factor, the game time and
  the weather clock stay as they are); `qa_season_year stop` ends it. At the end the day glides back to
  `r__season`.
- `qa_season_pin <0..365|calendar|january..december>` holds the season day and latches its texture
  folders (screenshots, frame parity together with `env_time_pin` and `wind_freeze`);
  `qa_season_pin off` hands both back to `r__season`.
- `qa_season_state` prints the choice, the computer's date, the target and shown day, the glide, the QA
  state and the latched texture folders.
