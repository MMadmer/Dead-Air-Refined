# Seasons (1.6.0)

Dead Air is autumn. 1.6.0 lets the Zone live through a whole year the way the real Chornobyl
Exclusion Zone does: by default it follows the computer's calendar, or it holds one month the player
picks. Two layers make up the look of a day:

- **The season day `D`**, a continuous day of a 365-day year. The procedural grass, the snow, the stock
  details, the ground tint, the precipitation and the ice evaluate their keyframe curves at `D` every
  frame: no rebuild, no reload, no step between months. Their keys sit on the 15th of each month and
  live in `configs/dead_air_x64_seasons.ltx` (field reference: `SEASON_DATA.md`).
- **Texture overlay folders**, discrete sets of textures (the cards of the vegetation that still stands
  as stock models, terrain, the grass atlas, maps, ...) that the renderer prefers over the base textures. They are latched per texture category and change
  only at safe points, because a switch reloads textures.

The legacy DA 1.0 season float (`environment_season`, Lua `level.get_season_k`/`set_season_k`, the
`SEA1` save chunk) is a separate thing kept for mod compatibility; nothing here reads or writes it.

## Choosing

- Main menu or pause menu -> "Опции погоды" -> "Времена года" -> "Месяц": «По календарю ПК» /
  "PC calendar" (top, the default) or one of the twelve months, then "Применить". Cancel leaves the
  choice as it was (`atmosfear_options.script`, page `af_opt_season`; shown only when the engine has
  `r__season`). The same dialog serves both menus.
- Console: `r__season calendar|january|...|december`, kept in user.ltx. `[season] default` of the data
  file names the choice a fresh user.ltx starts with.
- Development builds of 1.6.0 kept a look instead of a month; their tokens migrate once, at the next
  user.ltx save: `autumn` -> `calendar` (it was the untouched default), `winter` -> `january`,
  `spring_early` -> `march`, `spring` -> `april`, `spring_late` -> `may`, `summer` -> `july`.

A month shows its 15th at noon. The calendar shows today: `D = 365 * (yday + seconds / 86400) /
(365 or 366)`, read once per second.

| Month | Jan | Feb | Mar | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `D` of its 15th | 14.5 | 45.5 | 73.5 | 104.5 | 134.5 | 165.5 | 195.5 | 226.5 | 257.5 | 287.5 | 318.5 | 348.5 |

Day 0 is 1 January 00:00. Every keyframe curve is a periodic monotone cubic through its keys
(`DaSeasonCurve` in `src/xrEngine/da_season.h`): no overshoot, and a run of equal keys (the winter
zeros of the grass) stays exactly flat.

## How the day moves

- A new choice glides `D` along the shorter way round the year over `[season] glide` seconds
  (2 by default), counted in wall-clock seconds of a rendered level: picked in the pause menu, the
  grass grows or the snow melts in the first two seconds after the menu closes.
- Startup, a level load and every frame without a level snap `D` to the target at once.
- `da_season_day_prev()` is last frame's `D` (equal to `D` after a snap), so growing blades and settling
  snow carry correct motion vectors through a glide or a QA run.
- The day advances once per frame in `CRenderDevice::FrameMove`, right before `seqFrame.Process()`
  (`da_season_frame()`), so every `OnFrame` of a frame sees one `D`.

Code: `src/xrEngine/da_season.cpp` (the choice, the day, the glide, the latch, the QA commands), its
header `da_season.h` (the API every module reads), Lua `level.season_day()` (`D`) and
`level.season_choice()` (0 = calendar, 1..12 = month) in `src/xrGame/level_script.cpp`.

## Texture folders

Layout:

- `textures/seasons/<folder>/<logical name>.dds` overrides the texture of that name, bump maps
  included.
- `textures/seasons/<folder>/levels/<level folder>/<name>.dds` overrides a texture the level folder
  provides (terrain colour, the `build_details` grass atlas); it is checked first, and only
  for names the level folder itself has.
- `.thm` descriptions are keyed by logical name, so an overlay uses the base texture's bump, detail
  and material.
- Folders: `winter`, `spring_early`, `spring_mid`, `spring_late`, `summer`. Autumn is the base set itself
  (`-`): Dead Air's look and the 1.6.0 base textures. `level_lods` has no look, the engine draws it (below).

Every texture belongs to a category: the longest `[season_categories]` prefix of its logical name
(either slash, any case), `default` when none matches. Each category has its own bands in
`[season_folders]`: `<first day>:<folder>, ...` with `-` for the base set; a band runs to the next
one, the last into the first. The shipped table:

| Category (prefixes) | 1 Nov - 14 Dec | 15 Dec - 15 Feb | 16 Feb - 9 Apr | 10 - 30 Apr | May | 1 Jun - 19 Sep | 20 Sep - 31 Oct |
| --- | --- | --- | --- | --- | --- | --- | --- |
| default (`terrain\`, `build_details`, the rest) | spring_early | spring_early | spring_early | spring_mid | spring_late | summer | base |
| trees (`trees\`, `map\`, `ui\`) | spring_early | winter | spring_early | spring_mid | spring_late | summer | base |
| ground (`grnd\`, `detail\`, `roof\`) | base | base | base | base | base | base | base |
| decal (`decal\`) | base | winter | base | base | base | base | base |
| vine (`vine\`) | winter | winter | winter | base | base | base | base |

The ground never wears the painted winter set: the snow layer whitens open ground continuously, and
painted snow would show under roofs, indoors and in every trail. The tree cards and the maps wear winter
only while the open snow cover is complete; ivy stays leafless from November to early April.
Without the data file or one of its sections the engine uses this same table.

**Latching.** The folders follow the latch day: the choice's day (a month's 15th, or the calendar's
day), or a `qa_season_pin`. A new choice latches at once (the reload happens while the menu is open).
In calendar mode the folders move only at safe points: startup, a level load, and while the main or
the pause menu is up; a band edge crossed at midnight waits for the next of them, so play never
freezes for it (the textures may lag the grass and snow by one band for one session).

**Reload.** Each combination of folders is one immutable latch (`DaSeasonLatch`, alive until exit).
On the frame after a new one is latched the renderer reloads only the loaded textures whose category
changed folder and that have an overlay in the old or the new one, in chunks of 128, keeping the old
views alive until the new ones exist; the ray tracing scene moves its bindless tables to the new views
before the old ones go (`da_rt_textures_replaced`, so no slot ever holds a released view and the DX12
layer logs nothing), then TAA/upscaler history resets. About 0.1-0.2 s, seven times a year at most. Code: `CResourceManager::SyncTextureSeason`
and the lookup `texture_season_overlay` (`src/Layers/xrRender/ResourceManager.cpp`), the resolver hook
in `texture_resolve_name` (`src/Layers/xrRenderDX11/dx11Texture.cpp`). Log lines:
`* [season] textures #<id> for day <D> (<why>): default=... trees=...` and
`* Texture season #<old> -> #<new> (...): <n> textures reloaded in <ms> ms`.

The bundle layout keys the season folders one level deeper (`textures/seasons/<folder>/trees`,
`.../map`, `.../levels/<level>` and so on; `packaging/dead-air-x64/content/groups.ltx`); the bundle
builder takes the longest assigned prefix of a path.

## What each folder contains

- Tree cards (`trees/`): only for what still stands as a stock model. The level trees and bushes are the
  procedural ones and take their year from the species tables, leaf by leaf (`TREES.md`, `SHRUBS.md`), so
  the looks of their old cards are gone (WP7, 08.10.2026). Twenty cards keep them: the reeds and sedge
  (`trees_kamish_big_01`, `_big_02`, `trees_kamish_sux`, `trees_kamysh`, `trees_osoka_sux`; only their far
  billboards wear them - within the blades' radius the reed beds of the blades stand for them, GRASS.md), the duckweed
  (`trees_tina`), the forest walls (`trees_forestwall1`, `_2`), the stock fallen trees and debris
  (`trees_vetkasux2`, `_red`, `trees_new_vetka`, `trees_atp_sosnodub_branch`), a static pine (`trees_sosna_sux`)
  and the cards of props and DAR2 models (`trees_elka`, `trees_fake`, `trees_new_vetka_green1`, `trees_sosna`,
  `trees_vetkabig`, `trees_vetkagreen1`, `trees_vetkagreen5`). Every card family of Dead Air is the project's own
  (see ASSET_CREDITS.md), one grown structure per family so a tree keeps its silhouette through the year. The grown tree is
  fitted into its card (shrunk towards the attachment point, a spray past the border shortened), so no
  foliage runs into the card's edge - a plane cut off by a straight line in the game. Conifers carry
  their needles in tufts at the ends of the boughs. Dead wood (`*_dead`, `*_rotten`, dry conifers
  `*_sux*`) does not follow the year: it keeps its autumn card, winter adds snow.
  - summer: full foliage, coverage and colour statistics of the summer pack that ships with Dead Air;
  - winter: deciduous bare, conifers keep their needles; snow on every upward edge, frost elsewhere;
  - early spring: bare, catkins on alder, birch, poplar and hazel, pussy-willow buds on willows;
  - spring: young small leaves (birch's green haze, alder, poplar, willow), cherry in blossom,
    oak still bare;
  - late spring: nearly full fresh foliage, oak's young bronze leaves, new light shoots on conifers.
  The reeds, the sedge and the forest walls follow the same looks from GSC's own pictures.
- Per level:
  - terrain colour recoloured (summer towards the summer pack, spring stages from wet bare ground to
    fresh green; no winter set: the snow layer whitens the ground);
  - grass atlas (`build_details`) sprite by sprite for the stock details the procedural grass does not
    replace;
  - tree LOD atlas (`level_lods`): no look of it is worn any more, the engine draws it (`TREES.md`, "The
    billboards"). At the level's first frame and whenever the texture latch moves, a runtime copy of the
    level's own atlas gets the cells of every stock-path vegetation FLOD - reeds, sedge, duckweed, the
    stock fallen trees and debris, a mod's vegetation: whatever wears a `trees\` card and neither the
    shrubs nor the trees replace, theirs too while their stock models stand - drawn again from its own parts in the latched cards, eight views along
    the FLOD's own facets, one cell set a model, so a far reed bed is bare, budding or green exactly like
    the near one. Colour and alpha follow Dead Air's own bake: the alpha's quantile map from the coverage
    and the colour's linear gains, measured once a level by drawing the same cells with the base cards
    against Dead Air's pixels (the offline bake fitted them the same way through Dead Air's original
    textures). Cells of cars, ruins and rocks keep Dead Air's pictures; `level_lods_nm` is the base. The
    upgraded trees draw their own far billboards from their own atlas, baked from the trees for the day.
- PDA maps: Dead Air's maps are already summer renders; winter and spring stages are recoloured.
- Winter only: frost and snow in the cracks of the barks stock models still wear (`trees_bark`,
  `_a_01`, `_a_02`, `_mox`, `_sosna_01`, `_sosna_02`, `_sux`, `_sux2`, `_sux3`, `_sux_mox`, `_topol`,
  `trees_dub_bark`), snow on the cut faces (`trees_spil`); the fallen-leaf decals gone under the snow; ivy
  without its leaves. No winter set of the ground materials, roofs or grass atlas: the category table never
  wore them, and they are gone.

## Grass, snow and the rest

They follow `D` and nothing else: the blade grass and its seed heads (`GRASS.md`), the stock details
by class (keep, height and burial curves on the GPU), the snow layer and its depth by class
(`SNOW.md`), precipitation phase, ice and the far ground tint (`WINTER.md`). The keyframes and their
sources are in `SEASON_DATA.md`. With the climate's weather on (`WEATHER.md`, the Seasons page's second row) the
snow's depth and the ice are what the climate's weather left rather than the month's keys, and the length of the day
is the season's. The player's grass density and height settings and the graphics preset
stay what they are under every season.

## QA

Console commands, never saved:

- `qa_season_year [seconds=60] [from=<calendar|january..december|0..365>] [loop]` drives `D` through the
  year in that many wall-clock seconds of a rendered level; `time_factor`, the game time and the weather
  clock are untouched, so the game is not sped up. Log and console lines:
  `[qa] season_year <month> day <D> at <s> s` at each 15th, `[qa] season_year_done <s> s` at the end
  (`loop` prints `[qa] season_year lap <n> at <s> s` instead and runs until `qa_season_year stop`).
  The texture folders stay latched. At the end `D` glides back to `r__season`.
- `qa_season_pin <0..365|calendar|january..december>` holds `D` and latches that day's folders (frame
  parity with `env_time_pin` and `wind_freeze`); `qa_season_pin off` hands both back.
- `qa_season_state` prints the choice, the computer's date, the target and shown `D`, the glide, the QA
  state and the latched folders.

`tools/qa/seasons/` has three probes: the twelve months by pin with a scene shot each, a whole year run
with a window shot at every 15th, and the weather dialog's Seasons page the way a player uses it,
Cancel and Accept included (README there). `tools/qa/Check-OptionHints.py` checks the page's strings in
both languages, that its list carries exactly the engine's `r__season` tokens in order, and that both
layouts open the list to all thirteen entries.

## Making the folders again

The generator lives outside the repository, in `D:\Games\Dead-Air-AssetCompare\pipeline`
(`trees\seasons.py`, `trees\seasons_gsc.py`, `seasons_levels.py`, `seasons_details.py`,
`seasons_lods.py` with `lod_render.py` and `xr_level.py`, `seasons_ground.py`, `seasons_winter_extra.py`,
`encode_seasons.py`, `assign_seasons.py`); it writes into `Dead-Air-Refined_Assets\source\gamedata-1.6.0`.
Since the WP7 cleanup its `install()` refuses every file `pipeline\retired_season_looks.json` lists, and the stages
that only made retired sets end at once (`seasons_lods.py`, `seasons_ground.py`, `gen_shrubs.py`); the files that left
are in the cleanup's journaled backup (`DeadAir-x64\artifacts\_wp7_season_backup_20261008`).
The keyframe data is regenerated by `tools/seasons/gen_season_data.py` (`SEASON_DATA.md`).

## Known limits

- Snow on walls and props is not part of the overlays (a texture cannot tell which way a surface
  faces); the snow layer covers up-facing surfaces with sky access.
- Calendar mode: a folder band edge crossed during play waits for the next menu or level load.
