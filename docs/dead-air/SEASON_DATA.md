# Season data (1.6.0)

`gamedata/configs/dead_air_x64_seasons.ltx` holds every number of the continuous seasons: the grass blade
profiles and their season keys, the stock detail classes, the detail content keys, the snow, the precipitation
phase, the water ice, the texture overlay folders, the ground tint and the thresholds of the level snow bake.
The code reads it; nothing in it is code. How the season day itself works (menu, calendar, glide, QA run) is in
[SEASONS.md](SEASONS.md).

The file is generated. Never edit it by hand:

```
python tools/seasons/gen_season_data.py                    # writes the ltx and the generated block of this page
python tools/seasons/gen_season_data.py --check            # exit 1 when the tree differs from a fresh run
python tools/seasons/gen_season_data.py --levels <dir>     # also recompute every KSPR from <dir>/<level>/level.details
                                                           # + build_details.dds (the shipped levels) and compare
```

Inputs, all in `tools/seasons/data/` (copies of the 1.6.0 research, so the run needs nothing outside the repo):

| File | What |
|---|---|
| `keyframes.json` | grass scalars and colours per profile, snow per class, on the 15th of each month (ChEZ, a snowy modern year) |
| `stock_keys.json` | keep / height / burial keys of the stock detail classes |
| `precip_keys.json` | precipitation phase, temperatures, thunder, glaze, frost, water ice |
| `weather_keys.json` | the climate's weather: shares of the hours by sky, precipitation, wind, melt; its scalars (WEATHER.md) |
| `detail_hash_map.json` | the 478 detail content keys (KSPR) and their classes, with the level objects behind each |
| `detail_types_preview.ltx` | the research preview of `[detail_types]` (cross-check) |
| `category_folders.json` | the per-category texture folder table and its painted-snow audit (cross-check) |
| `ground_tint.json` | the canopy colours of the ground tint (cross-check) |
| `grass_profiles.json` | blade render profiles P1..P8 and the seed-head types (from the species and culm/head studies) |

The script re-reads what it wrote the way the engine does (CInifile rules, the float32 `DaSeasonCurve`) and
fails on any check of the "Checks" list below.

## Reading the file

- Open it directly: `CInifile` over `$game_config$\dead_air_x64_seasons.ltx`. It is not included from `system.ltx`.
  ASCII only, CRLF.
- CInifile stores section names in **lower case** (`[grass_profile_p1]`, `[stock_keys_ks_forb]`): `r_section`
  lowers the name it is given, `section_exist` and `line_exist` do not, so pass lower-case section names. Keys keep
  their case (`[detail_types]` keys are lower-case hex, class names are upper case values).
- CInifile removes every blank outside quotes from a value: `14.5:1, 45.5:1` reads `14.5:1,45.5:1`. `;` and `//`
  start a comment. A line holds at most 4096 characters.
- **Day D**: 0 = 1 January 00:00 of a 365-day year (no leap day); a month key is its 15th at noon: 14.5, 45.5,
  73.5, 104.5, 134.5, 165.5, 195.5, 226.5, 257.5, 287.5, 318.5, 348.5. 334 = 1 December 00:00, 58.5 = 28 February
  noon, 304 = 1 November 00:00.
- **Key list**: `field = day:value, day:value, ...`, days ascending and distinct, at most 32 keys. Build one
  `DaSeasonCurve` (`src/xrEngine/da_season.h`): `add(day, value)` for every key, `build()`, then `at(D)`. The curve is
  a periodic monotone cubic: never above or below the two keys around D, and a run of equal keys stays exactly
  that value (zeros are exact zeros). In float32 a run of ones may come out as 0.99999994: test `>= 0.999f`,
  never `== 1`.
- **Colours**: `day:#RRGGBB`, the sRGB encoding of a DRY albedo (wetness is applied by the shaders). Convert each
  channel to linear (`c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055)^2.4`), build one curve per linear channel,
  evaluate, convert back only where an sRGB value is needed.
- **Folder lists** (`[season_folders]`): `day:folder` ascending; the folder on day d is the last entry whose day
  is <= d, before the first entry the last one (the year wraps); `-` = the base set.
- **Tuples**: comma-separated numbers, in the order the field reference gives.

## Decisions in the data

| Decision | In the data |
|---|---|
| D1 zero-grass window = 1 Dec .. 28 Feb | `cover_open` is exactly 0.9 at its keys 334 and 58.5 and 1 from 15 Dec to 15 Feb, so `grass_visible = saturate((0.9 - cover_open) / 0.6)` is exactly 0 from 1 Dec 00:00 to 28 Feb 12:00 and grows from 0 on either side (0.005 six hours out, 0.917 on 15 Nov, 0.75 on 15 Mar). The other snow classes move by the same share of their Nov->Dec and Feb->Mar steps. `keep` of KS_FORB, KS_HERB, KS_FERN, KS_HORSETAIL and GRASS_LEGACY is exactly 0 over the window (keys 0 at 334, 348.5, 14.5, 45.5, 58.5). Shrubs stand all year; litter and decals hide under 2 cm of snow. |
| D2 no snow in October | every `cover_*`, `depth_*` and the snow on ice have a 0 key at 304 (1 Nov): exactly 0 from 15 May to 1 Nov; the first patchy snow comes in the first half of November (0.35 open cover on 15 Nov). |
| D3 cap 0.536 m, drift = P90 | `cap = 0.536`; every depth key is capped; `depth_drift` is the P90 of the covered drift cells and is the only class that reaches the cap (15 Feb). Open snow 0.10 / 0.17 / 0.22 m on 15 Dec / Jan / Feb. |
| D5 roads | `cover_road` = 1 from 1 Dec to 28 Feb noon (keys 1 at 334 and 58.5), the open cover in Nov and Mar. Road depth = open depth x rho_open / rho_road (4.7 / 8.4 / 12 cm). The road mask and the wide paved yards are bake rules (`[snow_bake] road`, `rail`, `rail_dilation`, `wide_paved`). |
| D6 snow-only precipitation in the window | `[precip_keys]`: `snow` and `snowfall_share` 1, `mixed`, `rain` and `glaze` exactly 0 from 334 to 58.5 (`snow_only = 334, 58.5`); outside it the phase comes from the temperature model (`t_*`, `ts`, `tl`, `w`). |
| D9 Stancia FIX_EMPTY | the 14 keys of the empty Stancia sprites are written as `PG_BUNCH`. |
| D11 no burns | the keyframes' `burnt_patch_share` is not exported. Always the snowy representative year. |
| D12 stock atlas | no atlas weights: `build_details` belongs to the `default` texture category (the latched folder). The stock classes get continuous `keep` / `height` / `hide_rel` curves. `[season_grass]` and `[season_shrubs]` are gone from `dead_air_x64_details.ltx`. |
| D15 texture folders | `[season_categories]` + `[season_folders]` = the brief's per-category table: ground sets never winter; tree cards, maps and decals wear winter only from 15 Dec to 15 Feb (open cover 1); autumn is the base set (`-`), and `level_lods` has no category (the engine composes it, `TREES.md`). |
| flood ice | the floodplain profiles' `flood_ice` equals the still-water ice keys (`[ice_keys] still`): flooded ground freezes with the oxbows (0.85 on 15 Dec). |

## Field reference

### `[season]`

| Field | Meaning |
|---|---|
| `glide` | seconds a menu change of `r__season` takes to slide D to the new day (shorter arc) |
| `default` | the `r__season` token of a fresh profile: `calendar` |

### `[season_categories]` and `[season_folders]`

`<texture name prefix> = <category>`: names relative to `textures\`, either slash, case-insensitive; the longest
matching prefix wins, a name without one is `default`. Level textures are matched by their logical name
(`terrain\terrain_escape`, `build_details`).

| Category | Prefixes | Folders |
|---|---|---|
| `default` | `terrain\`, `build_details` (and everything unlisted) | spring_mid from 10 Apr, spring_late from 1 May, summer from 1 Jun, base (autumn) from 20 Sep, spring_early from 1 Nov |
| `trees` | `trees\`, `map\`, `ui\` | as default, but spring_early from 16 Feb and winter from 15 Dec |
| `ground` | `grnd\`, `detail\`, `roof\` | base all year (the snow layer covers them) |
| `decal` | `decal\` | winter from 15 Dec to 15 Feb, else base |
| `vine` | `vine\` | winter (leafless) from 1 Nov to 9 Apr, else base |

Folders are latched at safe points (startup, level load, main menu); the QA year run does not re-latch them.

### `[snow_keys]` (WORLD evaluates it into `SSnowFrame`)

Classes: `open` ground, `trees` (under canopy), `road`, `drift` (drifts and neglected spots).

| Field | Meaning |
|---|---|
| `cap` | depth cap, m (smallest tushkan head top) |
| `cover_open/trees/road/drift` | covered share of the class (a texel counts as covered above `[snow_bake] cover_eps`) |
| `depth_open/trees/road/drift` | m where snow lies: mean of the covered part; drift = P90 of the covered drift cells |
| `state_fresh/settled/old/wet` | weights of the snow state; slush = 1 - their sum |
| `age`, `crust`, `wet`, `drift` | 0..1: grain age, sun/rain crust, liquid water, wind-drift strength |
| `comp_road` | compaction of road snow 0..1 |
| `rho_open`, `rho_road` | snow density kg/m3 (road depth = open depth x rho_open / rho_road) |
| `dirt_road`, `litter_trees` | roadside dirt and litter under trees 0..1 |
| `albedo_open/road/trees` | colour of the snow (sRGB of linear albedo, state-weighted) |

May to October hold the state lists (the snow is 0 there). The per-level solver (WORLD's bake, brief 3.4) turns
the class cover and depth keys into the level's daily `S_c`, `Mc_c`: `D_c = clamp(S_c A_c - Mc_c M_c, 0, cap)`.
Reference result on l01_escape (followup_4, cover and depth reproduced to rounding):

| 15th | open S / Mc | under_trees S / Mc | roads S / Mc | drifts (P90) S / Mc |
|---|---|---|---|---|
| Jan | 0.172 / 0 | 0.489 / 0.190 | 0.084 / 0 | 0.245 / 0 |
| Feb | 0.222 / 0 | 0.636 / 0.249 | 0.120 / 0 | 0.329 / 0 (10 % at the cap) |
| Mar | 0.747 / 0.676 | 0.667 / 0.328 | 0.916 / 0.910 | 0.373 / 0.417 |
| Apr | 0 / 0 | 0.476 / 0.363 | 0 / 0 | 0.670 / 1.545 |
| Nov | 0.177 / 0.159 | 0.056 / 0.029 | 0.378 / 0.374 | 0.105 / 0.169 |
| Dec | 0.101 / 0 | 0.280 / 0.113 | 0.047 / 0 | 0.147 / 0 |

`grass_visible` is not stored: every consumer derives it from `cover_open` (D1 formula).

### `[precip_keys]` (WORLD: `SPrecipState`)

| Field | Meaning |
|---|---|
| `snow`, `mixed`, `rain` | share of precipitation days by phase (sum 1 at the keys; normalise after interpolation) |
| `snowfall_share` | share of falling mass that arrives as snow (snow + 0.5 mixed) |
| `p_precip` | probability of a precipitation day (>= 1 mm) |
| `thunder_weight` | P(thunder given a precipitation day), July = 1: the chance a stormy weather keeps its bolts |
| `glaze` | P(freezing rain given a precipitation day): rain onto frozen ground, crust and icy film |
| `frost_night` | P(daily minimum below 0 C): puddles and wet films freeze that night |
| `t_mean`, `t_min`, `t_max`, `t_range`, `t_sigma` | Chornobyl monthly mean, mean minimum, mean maximum, diurnal range (max - min) and day-to-day sigma of the daily mean, C |
| `t_mean_a`, `t_mean_b`, `t_mean_phase` | annual fit `T_mean(D) = a + b cos(2 pi (D - phase) / 365)` (rms 0.58 C) |
| `ts`, `tl`, `w` | phase model: `P_snow(T) = 1 / (1 + exp((T - ts) / w))`, `P_rain(T) = 1 / (1 + exp(-(T - tl) / w))`, sleet = rest; 50/50 at 1.2 C |
| `t_precip_bias`, `t_precip_sigma` | precipitation days run `t_precip_bias` C colder with `t_precip_sigma` x the day-to-day sigma |
| `snow_only` | the D6 window `from, to` (days): everything falls as snow inside it |

### `[ice_keys]` (WORLD: `SSnowFrame::ice`, `water.ps`)

| Field | Meaning |
|---|---|
| `still` | iced share of still water (ponds, oxbows, marsh pools, floodplain) |
| `flowing` | iced share of channels and streams (1 - flowing in Jan/Feb = polynyas) |
| `snow_cover_on_ice` | snow on the ice: open cover x max(still, flowing) |
| `snow_depth_on_ice` | m: 0.8 x open depth (wind scours open ice) |

### `[weather_keys]` and `[climate]` (WEATHER.md: `da_climate.cpp`)

| Field | Meaning |
|---|---|
| `p_clear`, `p_overcast` | shares of the month's hours under 0-2/10 and 8-10/10 cloud (overcast counts the wet hours) |
| `p_wet`, `precip_mm` | share of hours with precipitation falling; the month's precipitation (mm) |
| `p_fog` | share of hours with fog (calm, dry) |
| `wind_ms` | mean wind at 10 m |
| `p_blizzard`, `p_storm`, `p_thunder` | shares of the snow hours with a blizzard, of the rain hours with a rainstorm, with thunder |
| `cloud_t_corr`, `fog_dawn` | the cloud's correlation with the temperature anomaly; the dawn weight of fog |
| `ddf` | degree-day melt factor on the day's mean (mm w.e. per C per day) |

`[climate]`: the processes' scales (`tau_*`, game days), the wind's spread and the blizzard's least wind, fog's wind
limit, the cloud classes' hysteresis, the afternoon weight of thunder, the rate's spread and the precipitation hour's
phase, the snowpack's settling (`rho_max_*`, `tau_*`), the loose top, blowing snow's wind, the ice (`ice_alpha`,
`ice_snow_ins`, `ice_melt`, the water's cooling, running water's factors, the cover's thicknesses, the rot).

### `[grass_heads]`

`t<N> = length min, length max, width min, width max (m, at r__detail_height 1), branch ribbons, opening angle at
anthesis, opening angle when ripe (deg)`. Types: 1 narrow spike (*Elytrigia*, *Lolium*), 2 cylinder (*Phleum*,
*Alopecurus*, *Corynephorus*), 3 one-sided cluster (*Dactylis*), 4 open pyramid (*Poa*, *Glyceria*), 5 airy cloud
(*Agrostis*, *Deschampsia*), 6 dense lobed panicle (*Calamagrostis*), 7 plume (*Phragmites*), 8 nodding awned
(*Anisantha*, *Bromopsis*), 9 sedge spikes (*Carex*).

### `[grass_profile_p1]` .. `[grass_profile_p8]`

Lengths in m at `r__detail_height` 1, angles in degrees from vertical, pairs = min, max (a blade picks by hash).

| Field | Meaning |
|---|---|
| `name` | profile id (P1_TALL_CALAMAGROSTIS .. P8_SHORT_LAWN_YARD) |
| `floodplain` | 1: `flood` / `flood_ice` keys apply |
| `snow_class` | 0 lodge-mat, 1 standing straw collapsing, 2 rigid standing, 3 wintergreen tuft |
| `leaf_h`, `leaf_w` | summer leaf height, width at the base |
| `leaf_tilt`, `leaf_bend`, `leaf_droop` | tip angle, mid-point offset / height, tip droop 0..1 |
| `leaf_stiff` | wind damping 0..1 (1 rigid) |
| `culm_h`, `culm_w`, `culm_tilt`, `culm_bend`, `culm_stiff` | the same for flowering culms |
| `culm_knee` | geniculate base: the first control point leaves the root by this share of the height |
| `culm_leaves` | cauline leaves per culm (reed); with it `leaf_*` describe those leaves and `density_leaf` is 0 |
| `basal_h`, `basal_w`, `basal_tilt`, `basal_bend`, `basal_droop` | the dense base of short arching leaves, drawn as folded pairs at the leaves' roots near the camera (GRASS.md "The look") |
| `clump_cell`, `clump_pull`, `clump_face_away` | Voronoi clump cell size, pull towards the clump centre, facing away from it 0..1 |
| `clump_lean` | deg: the largest lean a clump shares (its culms fully, its leaves half) |
| `density_leaf`, `density_culm` | blades per m2 at density 1 |
| `density_basal` | leaves of the basal pairs per m2 at density 1 (carried by the leaf blades: 0 without `density_leaf`) |
| `density_cal` | calibration: render density = `density_*` x `density_cal` x `S(r__detail_density)` x key `density`, S = clamp(0.25 / d, 0.25, 1.6) (D4); 0.85, the lawn 0.60 (raised from 0.55 / 0.40 after the first in-game look: the blade meadow read thinner than the stock tufts it replaces) |
| `cover` | share of the ground the profile covers (sand tufts 0.5) |
| `shade_density`, `shade_height` | multipliers under canopy (verge 0.4 / 1.15) |
| `head_share` | share of culms that can carry a head (x the `heads` key) |
| `head_type` | dominant head type of `head_mix` |
| `head_mix` | `type:weight` of the species mix (weights sum 1) |
| `head_nod` | head nod, deg (reed plumes 10..30) |
| `fold_h` | leaves shorter than this (after season and `r__detail_height`, which scales it) fold two per strip |
| `tussock_h`, `tussock_share`, `tussock_r` | raised tussock base (not scaled by `r__detail_height`) on that share of clumps, radius |
| `seed_ramp` | head colour by seed stage `stage:#RRGGBB` (0.25 heading, 0.5 anthesis, 0.75 ripe, 1 weathered) |

### `[grass_keys_p1]` .. `[grass_keys_p8]`

Key lists on the 15th of each month (keyframes T1/T2).

| Field | Meaning |
|---|---|
| `new_growth_h` | height of the current cohort / summer height |
| `green` | live share of the visible blade area, both cohorts |
| `cohort_green` | live share of the current cohort = green / (1 - thatch) |
| `thatch` | share of last season's dead blades (cohort rollover about 1 Jan, inside the zero-grass window) |
| `thatch_lodge`, `straw_lodge` | 0 upright .. 1 flat, for the thatch cohort and for the current year's dead blades |
| `heads` | share of the profile's head density with current-year heads |
| `seed_stage` | 0 none, 0.25 heading, 0.5 anthesis, 0.75 ripe, 1 weathered |
| `tip_dry` | dry share of a live blade from its tip |
| `wet` | baseline wetness of blades and litter before live rain |
| `frost` | probability of a frosted-grass morning |
| `density` | blade count relative to July |
| `flood`, `flood_ice` | floodplain profiles: share under water, iced share of it (0 elsewhere) |
| `live_color`, `yellowing_color`, `straw_color`, `thatch_color`, `seed_color` | dry albedo, sRGB |

### `[grass_class_profile]`

Blade class -> profile: `PG_BUNCH`, `PG_FINE` -> P1; `PG_LEAFY`, `PG_PLUME` -> P2; `PG_TUFT` -> P3; `PG_RUSH` -> P4;
`PG_DRY` -> P7. P5 (reed), P6 (sand) and P8 (lawn) have no stock sprite; their placement comes from level masks.

### `[stock_keys_<class>]`

Sections `stock_keys_ks_forb`, `_ks_herb`, `_ks_fern`, `_ks_horsetail`, `_ks_shrub`, `_ks_litter`, `_ks_decal`,
`_grass_legacy` (GRASS_LEGACY serves unknown content keys, KS_UNKNOWN).

| Field | Meaning |
|---|---|
| `keep` | an item is drawn iff `u(root) < keep`, u = the item's stable root hash |
| `height` | multiplies the item's height column |
| `hide_rel`, `hide_abs`, `hide_margin` | hidden where the snow depth at the root >= max(`hide_abs`, `hide_rel` x h_top - `hide_margin`), h_top = model top x item scale x `height` |
| `grass_visible` | 1: `keep` is multiplied by `grass_visible(D)` (GRASS_LEGACY only) |

### `[detail_types]`

`<16 lowercase hex> = <class>`: the KSPR content key of a detail model (below) and its class. 478 keys: PG_BUNCH,
PG_FINE, PG_LEAFY, PG_PLUME, PG_TUFT, PG_RUSH, PG_DRY become blades; KS_FORB, KS_HERB, KS_FERN, KS_HORSETAIL,
KS_SHRUB, KS_LITTER, KS_DECAL stay stock cards with season keys. A key not listed is KS_UNKNOWN: stock card, its
own texture, GRASS_LEGACY keys, never blades. Any change a mod makes to a model or its sprite therefore falls back
to stock.

### `[ground_tint]`

| Field | Meaning |
|---|---|
| `near`, `far` | tint strength between the blades and where the blades have faded |
| `start`, `end`, `end_margin` | the strength goes from `near` to `far` over [`start` x r__detail_radius, `end` x r__detail_radius + `end_margin` m] |
| `ref_<folder>` | reference day of each latched terrain folder: the brightness ratio is taken against the canopy on that day |
| `canopy_p1` .. `canopy_p8` | canopy-average dry albedo per profile (green x live + thatch x thatch colour + dead x straw; Sep-Oct dead = half yellowing) |
| `canopy_mix` | the same weighted by the stock content the blades replace (reference and QA) |

### `[snow_bake]` (WORLD's 1 m level bake)

Lengths in m, heights above the hole-filled ground, angles in degrees, materials by GMLib name. Axes: +x east,
+z north.

| Field | Meaning |
|---|---|
| `texel` | bake resolution |
| `receiver_ny`, `ground_dy` | a ground receiver faces up (normal y > a) and its top is within `ground_dy` of the ground (no roofs, decks, wall tops) |
| `slope` | `SlopeF = saturate((ny - a) / b)` |
| `crown_h`, `crown` | passable crown materials at least `crown_h` above ground are canopy |
| `tree_disc` | tree visuals add a disc of a x the bbox half-width at weight b |
| `canopy_blur`, `canopy_class`, `canopy_lee`, `canopy_shield` | T = blur(canopy); under_trees where T >= class; lee x (1 - smoothstep(lee, 1, T)) (a lone tree calms the drift a little, a wood all of it); A *= 1 - shield T |
| `tree_well` | a smooth bump over a < T < b, 1 in the middle (`melt` b weighs it) |
| `forest_area` | connected crowns of at least this many m2 act as a drift obstacle |
| `obstacle_h`, `obstacle_cap`, `obstacle_area`, `wall_ny`, `trunk` | obstacles: at least `obstacle_h` high, height capped, blobs under `obstacle_area` m2 and trunks dropped; walls are \|ny\| < `wall_ny` |
| `wind_from`, `wind_weight` | winter wind directions (compass degrees it blows from) and weights |
| `lee_reach`, `lee_flat`, `lee_end` | lee searched to `lee_reach` m; weight 1 to `lee_flat` x obstacle height, 0 at `lee_end` x |
| `toe_reach`, `toe_weight` | windward toe within `toe_reach` x obstacle height, weight |
| `hollow` | ground under its a-metre mean by more than b counts as a hollow, fully at b + c |
| `building`, `yard` | buildings (height, footprint m2); yard weight a x exp(-dist / b) within c m |
| `fence`, `fence_materials` | fences (height, weight, reach m) of those materials |
| `drift_class`, `class_blur` | drifts where N >= a; soft class weights blurred over b m |
| `drift_factor` | `DriftF = clamp(1 + a lee, b, c)` |
| `depression` | `clamp(1 + b (blur_a(H) - H), c, d)` |
| `sun_elevation` | winter noon sun from the south, for the melt term |
| `melt` | `M = 1 + a dot(N, sun) + b tree_well - c lee - d road` |
| `noise`, `noise_amp` | value noise (wavelength, weight pairs, normalised to -1..1); A_open/trees/drift *= 1 + a n, A_road = 1 + b n |
| `drift_shape` | A_drift *= a + N, M_drift = 1 - b N |
| `floor` | minimum A, M |
| `cover_eps` | a texel counts as covered above this depth |
| `lut_bins` | histogram bins of the per-level S/Mc solve |
| `road`, `rail`, `rail_dilation`, `wide_paved` | road mask = road materials on the ground minus gravel within `rail_dilation` of rail materials; paved areas a `wide_paved` m disc fits in get open-class depth with road melt |
| `skip` | passable materials the top raster ignores |
| `soil` | soil materials (drifts form only on soil, never on roads) |

## KSPR: the content key of a detail model

The class of a detail object is bound by the content of its model and sprite, so it survives a mod that reorders
`level.details`, and any change to the model or the sprite falls back to stock. The key is FNV-1a 64
(offset `0xcbf29ce484222325`, prime `0x100000001b3`, per byte `h = (h ^ byte) * prime` mod 2^64) over two parts.
The reference is `kspr()` / `k1()` in `tools/seasons/gen_season_data.py` (identical to the research's
`hash_map.py`); a C++ port must reproduce it byte for byte.

**K1, in `CDetail::Load`, right after the indices are read and BEFORE `Optimize()`** (which permutes vertices and
indices). The stream, all little-endian, in file order:

1. the shader name (`fnS`) lower-cased (ASCII A-Z only, never a locale `tolower`), then one 0 byte;
2. the texture name (`fnT`), the same way, then one 0 byte;
3. `u32` flags (`m_Flags`, as read);
4. `i32 q(min scale, 1000)`, `i32 q(max scale, 1000)`;
5. `u32` vertex count, `u32` index count;
6. per vertex: `i32 q(P.x, 1000)`, `q(P.y, 1000)`, `q(P.z, 1000)`, `q(u, 16384)`, `q(v, 16384)`;
7. the indices as `u16`.

`q(x, k)`: `double v = double(x) * k; return v >= 0 ? s32(v + 0.5) : -s32(-v + 0.5);` (double precision, half away
from zero).

**KSPR, in `CDetailManager::Load` after the models**: fold the mip-0 DXT blocks of the level's BASE atlas
(`FS.r_open("$level$", "build_details.dds")`; the season overlays are resolved per texture name, never through FS)
that the model's uv rect touches into K1:

```cpp
// b = the whole .dds file, size = its length; h = K1; fnv1a(data, n, h) continues FNV-1a over n bytes
const auto floordiv = [](s64 a, s64 d) { return a >= 0 ? a / d : -((-a + d - 1) / d); };
const u32 H = *(const u32*)(b + 12), W = *(const u32*)(b + 16);
const bool dx10 = !memcmp(b + 84, "DX10", 4), dxt1 = !memcmp(b + 84, "DXT1", 4);
const s64 off = 128 + (dx10 ? 20 : 0), bs = dxt1 ? 8 : 16, bw = (W + 3) / 4;   // DX10 header: always 16 B blocks
// umin/umax/vmin/vmax over every vertex's u, v as read from the file
const int x0 = std::max(0, int(double(umin) * W)), x1 = std::min(int(W) - 1, int(double(umax) * W - 1e-6));
const int y0 = std::max(0, int(double(vmin) * H)), y1 = std::min(int(H) - 1, int(double(vmax) * H - 1e-6));
// int() truncates toward zero; the divisions by 4 below are FLOOR divisions (x1, y1 can be negative)
for (s64 by = floordiv(y0, 4); by <= floordiv(y1, 4); ++by)
{
    const s64 row = off + (by * bw + floordiv(x0, 4)) * bs;
    const s64 len = (floordiv(x1, 4) - floordiv(x0, 4) + 1) * bs;
    // bytes [row, min(row + len, size)); nothing when len <= 0 or row >= size
    if (len > 0 && row < s64(size))
        h = fnv1a(b + row, size_t(std::min<s64>(len, s64(size) - row)), h);
}
```

The key is printed as `%016llx` (lower case) and looked up in `[detail_types]`. No atlas, an atlas shorter than
its header, or a model without vertices: no key, the object is KS_UNKNOWN. The `--levels` run of the generator
recomputes every key from the shipped levels (`levels.xdb` copies) and compares it with `[detail_types]`, objects
and members included.

<!-- BEGIN GENERATED by tools/seasons/gen_season_data.py -->

### Checks

- 478 detail content keys (KSPR), 14 of them the Stancia FIX_EMPTY sprites written as PG_BUNCH; per class: PG_BUNCH 60, PG_FINE 4, PG_LEAFY 18, PG_PLUME 22, PG_TUFT 12, PG_RUSH 6, PG_DRY 11, KS_FORB 37, KS_HERB 26, KS_FERN 46, KS_HORSETAIL 9, KS_SHRUB 57, KS_LITTER 168, KS_DECAL 2.
- KSPR recomputed from 494 objects of the shipped levels: 478 keys, identical to [detail_types].
- D1 grass_visible over 1 Dec 00:00 .. 28 Feb 12:00: max 0.
- D1 open cover over the window: min 0.9000.
- D1 grass_visible 6 h outside the window (30 Nov 18:00 / 28 Feb 18:00): 0.0051 / 0.0053.
- D1 keep of forb/herb/fern/horsetail/legacy over the window: exactly 0.
- D2 snow cover and depth over 15 May .. 1 Nov 00:00 (every class, on ice too): exactly 0.
- D5 road cover over the window: min 1.0000000.
- D6 precipitation over the window: snow and snowfall_share 1, mixed / rain / glaze exactly 0.
- D3 max depth open / trees / road / drift (m): 0.220 / 0.140 / 0.120 / 0.536.
- folders: trees winter days, min open cover: 63 days, 0.9998.
- folders: decal winter days, min open cover: 63 days, 0.9998.
- curves checked / samples: 1293 / 2858578.
- Overshoot, negative and range errors: none.

### Month summary

**The 15th of each month (the keys)**

| day | D | cover open / trees / road / drift | depth cm open / trees / road / drift | grass_visible | snowfall share | ice still / flowing | folders default / trees / vine |
|---|---|---|---|---|---|---|---|
| 15 Jan 12:00 | 14.5 | 1.00 / 0.95 / 1.00 / 1.00 | 17.0 / 11.0 / 8.4 / 40.0 | 0.000 | 1.00 | 1.00 / 0.92 | spring_early / winter / winter |
| 15 Feb 12:00 | 45.5 | 1.00 / 0.95 / 1.00 / 1.00 | 22.0 / 14.0 / 12.0 / 53.6 | 0.000 | 1.00 | 1.00 / 0.92 | spring_early / winter / winter |
| 15 Mar 12:00 | 73.5 | 0.45 / 0.72 / 0.45 / 0.90 | 10.0 / 10.0 / 4.0 / 32.0 | 0.750 | 0.51 | 0.50 / 0.60 | spring_early / spring_early / winter |
| 15 Apr 12:00 | 104.5 | 0.00 / 0.05 / 0.00 / 0.10 | 0.0 / 5.0 / 0.0 / 12.0 | 1.000 | 0.13 | 0.00 / 0.00 | spring_mid / spring_mid / - |
| 15 May 12:00 | 134.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.01 | 0.00 / 0.00 | spring_late / spring_late / - |
| 15 Jun 12:00 | 165.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 15 Jul 12:00 | 195.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 15 Aug 12:00 | 226.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 15 Sep 12:00 | 257.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 15 Oct 12:00 | 287.5 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.17 | 0.00 / 0.00 | - / - / - |
| 15 Nov 12:00 | 318.5 | 0.35 / 0.20 / 0.35 / 0.50 | 3.0 / 1.5 / 2.1 / 6.0 | 0.917 | 0.52 | 0.05 / 0.00 | spring_early / spring_early / winter |
| 15 Dec 12:00 | 348.5 | 1.00 / 0.90 / 1.00 / 1.00 | 10.0 / 6.0 / 4.7 / 24.0 | 0.000 | 1.00 | 0.85 / 0.60 | spring_early / winter / winter |

| day | Calamagrostis epigejos new growth h / green / thatch / heads | stock keep forb / herb / fern / horsetail / shrub / litter / decal | legacy keep x grass_visible |
|---|---|---|---|
| 15 Jan 12:00 | 0.00 / 0.00 / 0.97 / 0.00 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.85 / 1.00 | 0.000 |
| 15 Feb 12:00 | 0.00 / 0.00 / 0.97 / 0.00 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.85 / 1.00 | 0.000 |
| 15 Mar 12:00 | 0.03 / 0.02 / 0.95 / 0.00 | 0.15 / 0.00 / 0.00 / 0.00 / 1.00 / 0.80 / 1.00 | 0.262 |
| 15 Apr 12:00 | 0.15 / 0.25 / 0.72 / 0.00 | 0.45 / 0.30 / 0.08 / 0.20 / 1.00 / 0.70 / 1.00 | 0.650 |
| 15 May 12:00 | 0.50 / 0.78 / 0.18 / 0.06 | 0.80 / 0.80 / 0.60 / 0.70 / 1.00 / 0.55 / 1.00 | 0.900 |
| 15 Jun 12:00 | 0.85 / 0.90 / 0.07 / 0.52 | 1.00 / 1.00 / 1.00 / 1.00 / 1.00 / 0.45 / 1.00 | 1.000 |
| 15 Jul 12:00 | 1.00 / 0.82 / 0.06 / 0.92 | 1.00 / 1.00 / 1.00 / 1.00 / 1.00 / 0.40 / 1.00 | 1.000 |
| 15 Aug 12:00 | 1.00 / 0.65 / 0.03 / 1.00 | 1.00 / 1.00 / 1.00 / 1.00 / 1.00 / 0.45 / 1.00 | 1.000 |
| 15 Sep 12:00 | 1.00 / 0.50 / 0.01 / 0.99 | 1.00 / 0.95 / 1.00 / 0.90 / 1.00 / 0.60 / 1.00 | 1.000 |
| 15 Oct 12:00 | 1.00 / 0.30 / 0.00 / 0.91 | 0.95 / 0.70 / 0.85 / 0.60 / 1.00 / 0.90 / 1.00 | 1.000 |
| 15 Nov 12:00 | 0.97 / 0.12 / 0.00 / 0.80 | 0.65 / 0.20 / 0.30 / 0.10 / 1.00 / 1.00 / 1.00 | 0.917 |
| 15 Dec 12:00 | 0.95 / 0.02 / 0.00 / 0.65 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.90 / 1.00 | 0.000 |

**The 1st of each month, 00:00**

| day | D | cover open / trees / road / drift | depth cm open / trees / road / drift | grass_visible | snowfall share | ice still / flowing | folders default / trees / vine |
|---|---|---|---|---|---|---|---|
| 1 Jan 00:00 | 0 | 1.00 / 0.94 / 1.00 / 1.00 | 13.9 / 8.8 / 6.6 / 33.0 | 0.000 | 1.00 | 0.96 / 0.82 | spring_early / winter / winter |
| 1 Feb 00:00 | 31 | 1.00 / 0.95 / 1.00 / 1.00 | 20.4 / 13.1 / 10.8 / 49.2 | 0.000 | 1.00 | 1.00 / 0.92 | spring_early / winter / winter |
| 1 Mar 00:00 | 59 | 0.89 / 0.91 / 1.00 / 0.98 | 17.6 / 12.6 / 8.9 / 45.8 | 0.011 | 1.00 | 0.82 / 0.82 | spring_early / spring_early / winter |
| 1 Apr 00:00 | 90 | 0.13 / 0.31 / 0.12 / 0.46 | 3.0 / 7.3 / 1.1 / 20.2 | 1.000 | 0.26 | 0.16 / 0.22 | spring_early / spring_early / winter |
| 1 May 00:00 | 120 | 0.00 / 0.01 / 0.00 / 0.03 | 0.0 / 1.8 / 0.0 / 3.9 | 1.000 | 0.05 | 0.00 / 0.00 | spring_late / spring_late / - |
| 1 Jun 00:00 | 151 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 1 Jul 00:00 | 181 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 1 Aug 00:00 | 212 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 1 Sep 00:00 | 243 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.00 | 0.00 / 0.00 | summer / summer / - |
| 1 Oct 00:00 | 273 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.06 | 0.00 / 0.00 | - / - / - |
| 1 Nov 00:00 | 304 | 0.00 / 0.00 / 0.00 / 0.00 | 0.0 / 0.0 / 0.0 / 0.0 | 1.000 | 0.31 | 0.01 / 0.00 | spring_early / spring_early / winter |
| 1 Dec 00:00 | 334 | 0.90 / 0.79 / 1.00 / 0.92 | 6.3 / 3.5 / 3.4 / 14.4 | 0.000 | 1.00 | 0.45 / 0.26 | spring_early / spring_early / winter |

| day | Calamagrostis epigejos new growth h / green / thatch / heads | stock keep forb / herb / fern / horsetail / shrub / litter / decal | legacy keep x grass_visible |
|---|---|---|---|
| 1 Jan 00:00 | 0.42 / 0.01 / 0.53 / 0.26 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.86 / 1.00 | 0.000 |
| 1 Feb 00:00 | 0.00 / 0.00 / 0.97 / 0.00 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.85 / 1.00 | 0.000 |
| 1 Mar 00:00 | 0.01 / 0.01 / 0.96 / 0.00 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.83 / 1.00 | 0.000 |
| 1 Apr 00:00 | 0.08 / 0.11 / 0.86 / 0.00 | 0.31 / 0.11 / 0.03 / 0.07 / 1.00 / 0.75 / 1.00 | 0.530 |
| 1 May 00:00 | 0.31 / 0.54 / 0.42 / 0.02 | 0.64 / 0.57 / 0.31 / 0.45 / 1.00 / 0.62 / 1.00 | 0.796 |
| 1 Jun 00:00 | 0.70 / 0.87 / 0.10 / 0.27 | 0.94 / 0.94 / 0.87 / 0.91 / 1.00 / 0.49 / 1.00 | 0.972 |
| 1 Jul 00:00 | 0.95 / 0.87 / 0.06 / 0.77 | 1.00 / 1.00 / 1.00 / 1.00 / 1.00 / 0.42 / 1.00 | 1.000 |
| 1 Aug 00:00 | 1.00 / 0.73 / 0.04 / 0.98 | 1.00 / 1.00 / 1.00 / 1.00 / 1.00 / 0.42 / 1.00 | 1.000 |
| 1 Sep 00:00 | 1.00 / 0.57 / 0.02 / 1.00 | 1.00 / 0.98 / 1.00 / 0.97 / 1.00 / 0.51 / 1.00 | 1.000 |
| 1 Oct 00:00 | 1.00 / 0.40 / 0.00 / 0.96 | 0.98 / 0.85 / 0.95 / 0.77 / 1.00 / 0.76 / 1.00 | 1.000 |
| 1 Nov 00:00 | 0.99 / 0.20 / 0.00 / 0.86 | 0.84 / 0.44 / 0.60 / 0.32 / 1.00 / 0.97 / 1.00 | 1.000 |
| 1 Dec 00:00 | 0.96 / 0.06 / 0.00 / 0.74 | 0.00 / 0.00 / 0.00 / 0.00 / 1.00 / 0.96 / 1.00 | 0.000 |

**Derived dates** (0.25-day samples, float32 DaSeasonCurve)

- zero-grass window (grass_visible = 0): 1 Dec 00:00 .. 28 Feb 12:00
- any open snow (cover_open > 0): 1 Nov 06:00 .. 15 Apr 06:00
- open cover >= 0.5: 19 Nov 18:00 .. 13 Mar 06:00
- open cover >= 0.9: 1 Dec 00:00 .. 28 Feb 12:00
- roads fully covered (cover_road >= 0.99): 29 Nov 18:00 .. 1 Mar 18:00
- snow under trees (cover_trees > 0): 1 Nov 06:00 .. 15 May 06:00
- drifts (cover_drift > 0): 1 Nov 06:00 .. 15 May 06:00
- snow-only precipitation (snowfall_share >= 0.999): 30 Nov 18:00 .. 28 Feb 18:00
- snowfall share >= 0.5: 14 Nov 12:00 .. 16 Mar 00:00
- still water ice >= 0.5: 2 Dec 12:00 .. 15 Mar 12:00
- flowing water ice >= 0.5: 10 Dec 06:00 .. 20 Mar 18:00
- KS_FORB keep exactly 0: 1 Dec 00:00 .. 28 Feb 12:00
- KS_HERB keep exactly 0: 1 Dec 00:00 .. 15 Mar 12:00
- KS_FERN keep exactly 0: 1 Dec 00:00 .. 15 Mar 12:00
- KS_HORSETAIL keep exactly 0: 1 Dec 00:00 .. 15 Mar 12:00
- GRASS_LEGACY keep exactly 0: 1 Dec 00:00 .. 28 Feb 12:00
- deepest drift: 0.536 m on 15 Feb 12:00
- deepest open snow: 0.220 m on 15 Feb 12:00

<!-- END GENERATED -->
