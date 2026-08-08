# Render QA sweep

Reproducible frame-rate measurement on a loaded level, following the renderer test
procedure in [`PROJECT_RULES.md`](../../../PROJECT_RULES.md) section 8.1.

The harness measures the **installed** engine: it never copies binaries. Only the writable
state is isolated, so the player's `appdata`, `savedgames`, `gamedata`, `MODS` and JSGME
state are left untouched.

## What it builds

Under the game root, for a run labelled `render`:

- `_qa/render/appdata/` — logs, a `user.ltx` derived from the player's own settings with
  the procedure's knobs forced, and a byte-for-byte copy of the save group;
- `_qa/render/gamedata/` — a mirror of the player's loose gamedata plus
  `qa_render_profile.script`, registered in the mirrored `configs/script.ltx`;
- `qa_render.ltx` — an fsltx that keeps every content path pointing at the real game and
  redirects only `$app_data_root$` and `$game_data$`.

## Running

```powershell
tools\qa\render\Run-RenderQa.ps1 -SaveName optimization_test
```

The save name is the base name of the save group in `appdata\savedgames` (no extension,
no path). Useful switches: `-WindowMode fullscreen|borderless|windowed`, `-VSync on|off`,
`-VidMode 2560x1440`, `-FpsLimit 0` (0 = unlimited), `-Label` to run several
configurations side by side.

## Before trusting a number

1. The game must be closed; the script refuses to start otherwise.
2. Verify the physical DWM bounds are exactly `0,0` and `2560x1440` — a `user.ltx` alone is
   not enough, DPI virtualisation at 150% scaling turns the mode into logical `1707x960`.
   The installed executable already carries the `HIGHDPIAWARE` AppCompat layer; a test
   executable placed anywhere else needs the same layer for the duration of the run.
3. Compare like with like: foreground against foreground, background against background.
   `-always_active` does not make a mixed comparison valid.
4. The level must actually be loaded and unpaused — a startup smoke test is not a render
   test. The log line `[RENDER_QA] begin` marks the start of the measured window.

## Reading the log

`[RENDER_QA] waiting_for_first_update` → `stabilizing` → `begin` → one `sample` line per
second → `complete` with the average. The engine quits itself afterwards; check that no
`xrEngine` process is left behind, and scan the log for fatals, assertions, access
violations, reader overflows and Lua or UI lifecycle errors.

## Cleaning up

Remove `_qa/<label>/` and `qa_<label>.ltx` from the game root when the investigation is
over. They are temporary QA state and must not be left in a shipped installation.
