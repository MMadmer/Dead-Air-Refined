# NQ runtime acceptance test

Runs the node-quest runtime (`xms_nq*.script` plus the C++ additions in `src\`) inside the real
game and reports one PASS/FAIL line per check. This is the game-side acceptance test of NQ; the
headless harness in `tools\nq` covers the same logic without an engine and must stay green
alongside it.

## Running it

```powershell
pwsh -NoProfile -File tools\qa\nq\Run-NqQa.ps1 -Scenario All -ResultLabel run1
```

The engine is started detached on a **hidden desktop** (PROJECT_RULES 8/8.1: only render
measurements run on the main desktop), so no game window ever appears on the user's screen. The
command line of every launch is

```
xrEngine.exe -i -fsltx fsgame.ltx -always_active -silent_error_mode -force_flushlog -r4 -start "server(<save>/single/alife/load)"
```

with the working directory set to the QA root. The `fsltx` is the bare file name on purpose: the
QA root path contains a space, and pasting it into a command line is what produced
`Cannot open file "D:\Games\Dead"` in earlier attempts.

`-i` is what makes the talk window drivable at all. Without it the engine constructs `CInput` in
exclusive mode (`x_ray.cpp`: `captureInput = !strstr(Core.Params, "-i")`), SDL runs the mouse in
relative mode and takes motion from raw input only. Raw input — like every other synthetic hardware
event — is delivered to the window station's *input* desktop, which a hidden desktop is not, so
the cursor never moved and every click was lost. With `-i` the cursor is bound to the system cursor
and posted `WM_MOUSEMOVE`/`WM_LBUTTONDOWN` messages reach the UI.

Useful parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `-SaveName` | `admin - autosave` | save group copied out of the player's `savedgames`; every derived save is written inside the QA root |
| `-ResultLabel` | timestamp | subdirectory of `tools\qa\nq\results` |
| `-Scenario` | `All` | `Scout`, `Main`, `Reload`, `Dialog`, `LevelChange`, `ModuleRemoved`, `ChangedGraph`, `EmptyModule`, `Control` |
| `-TimeoutSeconds` | `900` | per launch |
| `-RebuildRoot` | off | rebuild `_qa\nq` from scratch instead of refreshing it |
| `-AnyMode` | off | write `mode = *` into the module manifest copied into the QA root, so a save made inside a campaign mode can drive the run. The tracked manifest is never touched |
| `-KeepRoot` | off | cosmetic; the root is always kept for the next run |
| `-CaptureEverySeconds` | `0` | periodic hidden-desktop screenshots for diagnostics |
| `-ReplyX`, `-FirstReplyY`, `-ReplyRowHeight`, `-TopicRow` | `300`, `599`, `20`, `3` | talk-window reply geometry, in screenshot pixels at `-VidMode 1024x768` |

Scenarios chain through saves written by earlier ones (`Main` → `nq_qa_mid` → `Reload` →
`nq_qa_scov` → `Dialog` → `nq_qa_dlg` → `LevelChange`), so a single scenario refuses to start when
its input save is missing and names the scenario that produces it. The scenarios that start from
the base save re-copy it from the player's directory first: a level change makes the game write its
own autosave into the QA root, and if that is the save the run starts from, the next run would
begin mid-quest.

## What the QA root looks like

`D:\Games\Dead Air\_qa\nq` is built next to the installed game and never touches it:

* every file of `packaging\dead-air-x64\installer\runtime-files.txt` from `bin\x64\Release` (the NQ
  engine changes are not deployed to the player's installation, so the acceptance test runs the
  freshly built binaries), plus the extra runtime DLLs that only exist in the install root;
* `database` as a directory junction to the real content;
* `gamedata\` = a copy of the player's loose layer overlaid with
  `packaging\dead-air-x64\compatibility\gamedata` (which carries `xms_nq*.script` and
  `configs\nq\catalog.ltx`), plus `nq_qa_probe.script` registered by appending `, nq_qa_probe` to
  the first `script =` line of the mirrored `configs\script.ltx`. The player's layer is copied
  whole, empty directories included — those are real. The compatibility layer is copied with
  `robocopy /S`, which leaves its empty directories behind, and `Assert-QaGameDataShape` then fails
  the run if the mirror holds an empty directory the player's `gamedata` does not. This is not
  housekeeping: git cannot track an empty directory and the deploy carries files, so one that
  exists only in a working tree reaches nobody's install — yet the engine answers a missing
  directory and an empty one differently. An empty `configs\nq\kinds` left in the packaging tree
  is what once let a crash that killed **every** new game pass this suite, because
  `file_list_open` answers a null vector for a directory that is not there and the old
  `FS_file_list::Size()` dereferenced it;
* `appdata\` with a `user.ltx` derived from the player's (`g_pause_in_background 0`, windowed
  1024x768, sound off), the seeded shader/collision caches and a copy of one save group;
* `fsgame.ltx` copied from the install root — it resolves everything relative to `$fs_root$`;
* `modules\nq_qa\` — the test module (`mod.ltx` + `quests\*.nqasset`). XMS reads modules from
  `$fs_root$`, so the module lives here and never in the installed game.

The module manifest carries no `mode =` key, which means it applies only when no campaign mode is
active. A save made inside a campaign (Revolution II and the like) makes the module apply to
nothing, every check that needs a quest fails at once, and each one blames something else - so the
runner recognises that shape and names the real reason instead. Pass **`-AnyMode`** for such a
save: it writes `mode = *` (the engine's own opt-out, `XMS::ModuleApplies`) into the manifest
**copied into the QA root**, leaving `tools\qa\nq\module\mod.ltx` alone. It is off by default
because the gating is worth testing and the default run is the one that tests it.

Before and after every run the script hashes the player's `appdata\savedgames` and fails if the
manifest changed. It also removes the `HIGHDPIAWARE` AppCompat value it adds for the copied
executable, and stops only the processes it started.

## The module

| Directory | Used by | Contents |
|---|---|---|
| `module\` | most scenarios | the three reference quests from `docs\nq\examples` (editor repo) plus two deliberately broken assets: `broken_code.nqasset` (code outside strings → E003) and `broken_pin.nqasset` (undeclared pin and a dangling edge → E007) |
| `module-changed\` | `ChangedGraph` | `linear_fetch` with the node that holds the live token deleted |
| `module-empty\` | `EmptyModule` | a manifest with no quests at all |

`dialog_branching` and `parallel_triggers` reference `esc_2_12_stalker_wolf` and
`esc_smart_terrain_3_16`, which exist on `l01_escape`; the save used by default is on that level.
For a save on another level the module's copies have to be re-pointed at ids from that level
(`configs\creatures\spawn_sections_<level>.ltx`, `configs\misc\simulation.ltx`) — the originals in
`docs\nq\examples` are never edited.

## The probe

`nq_qa_probe.script` is QA-only tooling: the runner copies it into the mirrored gamedata and it
never ships. It registers on `actor_on_first_update` (there is no `db.actor` before that), then
runs a step machine from a level call — one attempt per frame, never blocking — driving NQ through
its public API (`xms_nq.*`, `xms_nq_console.exec`) and ordinary game calls. It never moves the
actor: items are spawned straight into the inventory with `alife():create`, and the dialog partner
is brought to the actor rather than the other way round.

One launch runs one phase, taken from `configs\nq_qa.ltx`, which the runner rewrites between
launches:

```ini
[qa]
phase = p1
save  = nq_qa_mid
quit  = true
```

Output contract, one line per check:

```
NQQA: <step> PASS <detail>
NQQA: <step> FAIL <detail>
NQQA-info: <text>
NQQA-AWAIT: <tag> <hint>          the runner must click a reply
NQQA: DONE <passed>/<total> (failed=<n> skipped=<n> phase=<p>)
```

The runner prints every one of those lines, copies the engine log next to the results, and fails a
scenario on any `FAIL`, a missing `DONE`, a non-zero exit code, a crash dump, or any of
`FATAL ERROR`, `stack trace:`, `Assertion`, `Expression`, `access violation`, `reader overflow`,
`Lua error`, `No available phrase to say` in the log.

### Driving the talk window

Scenario `Dialog` is the one that needs the real UI. The probe prints `NQQA-AWAIT: <tag>` and waits
for the state the click is supposed to produce; the runner captures the hidden-desktop window as
evidence and clicks the reply row.

The capture helper fills a *client-sized* bitmap starting at the *window* origin, so a point read
off a screenshot is neither screen nor client space. The runner reads the window rectangle
(`Get-HiddenDesktopWindowInfo.ps1`), derives the border and caption heights from the known client
size, and passes the physical cursor position (screen) and the posted-message position (client)
separately — `Send-HiddenDesktopMouseClick.ps1` takes `-ClientX`/`-ClientY` for exactly that.

Motion and the button press are also sent as two calls a few frames apart (`-MoveOnly` first).
`CInput::OnFrame` dispatches a button event the moment it peels it off the SDL queue but only hands
the accumulated motion to the UI at the end of the batch, so a move and a click that arrive
together are resolved against the *previous* cursor position.

A click that produced nothing is repeated (`-ClickAttempts`, `-ClickRetrySeconds`), and the probe
reopens the talk window and asks again if a stray click closed it. Screenshots of every attempt end
up in `results\<label>\Dialog\screens`.

**As of this writing the click does not land.** With `-i`, correct screen/client coordinates, a
posted activation (`Focus-HiddenDesktopWindow.ps1`) and motion separated from the press, the game's
own cursor still does not move, so nothing in the talk window reacts. Everything the harness can do
from outside the process is in place; what is left is a question about how this engine consumes
mouse input on a desktop that is not the window station's input desktop.

If the clicks still do not take within 90 s, the probe prints `d.topic_open FAIL` and switches to
the documented last resort: it walks the phrase graph by calling `xms_nq_dialog.act`/`.pre`, the
same callbacks `CPhraseScript` invokes for a real click. Every following check still runs, and
`NQQA: d.path` states which of the two paths the run used — read that line before believing the
scenario-d result.

## Reading the results

```
tools\qa\nq\results\<label>\
  summary.json                     one row per scenario
  failures.txt                     empty on a clean run
  player-savedgames-before.json    hash manifest of the player's saves
  player-savedgames-after.json     must be identical
  <Scenario>\engine.log            the engine log of that launch
  <Scenario>\engine.pid, launcher.pid, exit-code.txt
  Dialog\screens\*.png             talk-window evidence
```

Scenarios `ModuleRemoved` and `Control` run with no module at all, which also switches XMS off, so
the runtime cannot read its blob and reports `is_ready=false` and no records — that is the expected
picture, and the record's survival is proved by the `ModuleBack` phase that follows. Those two
scenarios are also the ones that must produce no `! [nq]` / `~ [nq]` line whatsoever; the single
`[nq] runtime is not initialised` line they do contain is the console answering the probe's own
`nq list`.

## Note on Lua string escapes

This engine's script loader does not turn a decimal escape into a byte: `"\208"` reaches the Lua
state as the three characters `2`, `0`, `8` (symbolic escapes such as `\n`, `\t` and `\\` are
fine). Anything in the runtime or the probe that needs a specific byte therefore builds it with
`string.char`. The probe prints what the engine does with escapes in its `*.encoding` step, so a
future change of that behaviour shows up in the log instead of silently corrupting text.
