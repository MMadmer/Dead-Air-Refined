# Winter: the world side of the snow (1.6.0)

The season day `D` (`SEASONS.md`) decides how much snow lies where, what falls from the sky, whether the
ponds are frozen and how the snow is pressed by everything that walks on it. This page is the engine and
game side of that: the level bake, the snow constants of every frame, the precipitation phase, the snowfall,
the ice and the deformers. The renderer side (the snow grid, the full-screen snow layer, the trail maps) is
`SNOW.md`; the data is `SEASON_DATA.md` (`configs/dead_air_x64_seasons.ltx`).

Code: `src/xrEngine/da_snow_field.{h,cpp}` (storage, frame, precipitation, CPU replica, QA commands),
`src/xrGame/da_snow_bake.cpp` (the level bake), `src/xrGame/da_snow_deformers.cpp` (stamps),
`src/xrEngine/Rain.{h,cpp}` + `src/Layers/xrRender/dxRainRender.cpp` (snowfall), `shaders/r3/water.ps` (ice).

## The level bake

Once per level load, right after the water field (`Level_load.cpp`), `da_snow_bake_level()` rasterises the
collision mesh at 1 m over the level's box and hands the result to `da_snow_field_set()`:

| Field | What it is |
|---|---|
| `ground` | the terrain under everything: the collision surface where it lies inside the `level.details` slot's ground span, the span's middle under roofs and decks, hole-filled (pyramid) where no slot was written |
| `top` | the topmost non-passable surface facing up: snow lies on roofs and decks, nothing under them. Passable materials (crowns, bushes, water, occluders: `[snow_bake] skip`) and tree trunks (`[snow_bake] trunk`: the snow lies round a trunk, not on its branches) are skipped; `solid` = 1 where any collision gave a top, 0 where `top` is the hole-filled ground |
| `weights` | soft class weights (open, under trees, road, drift), RGBA8 summing to 255; zero on water |
| `factors` | RGBA8: A/2 (accumulation), M/2 (melt), noise n, neglect N |
| `lut_S`, `lut_Mc` | 365 days of this level's per-class season scalars |
| `walk` | the highest node of the level's AI in the texel: the surfaces it walks on (a deck's node stands within -0.25..+0.3 m of a top and more than 0.25 m over its own metre's ground, `da_snow_walkable`, `da_snow_deck_node`: within half a metre every low prop by a path was a deck, and on a slope the ground's nodes uphill stood at a stump's edge) |
| `cover` | where `top` is a cover over the ground (more than `da_snow_cover_over` = 0.35 m over it, and no AI node of the 3 x 3 texels round it on it - a deck the AI walks, a bridge, a porch, is ground): 2 an overhang, 1 a solid object |
| `shelter` | 255 deep under an overhang (the overhangs eroded by a texel): no snow from the sky reaches the ground there |

Classes (road over drift over trees over open; ground receivers only, everything else - roofs, steep faces -
is open with its slope taking the snow off):

- **under trees**: passable crowns `crown_h` above the ground, plus a disc round every tree visual, blurred
  over `canopy_blur`; `T >= canopy_class`.
- **road**: road materials at ground level (`[snow_bake] road`: asphalt, gravel, concrete) minus railway
  ballast (gravel within `rail_dilation` of `rail` materials). Paved areas a `wide_paved` disc fits in
  (yards, plazas) stay open class with the road melt (decision D5).
- **drift**: neglect `N >= drift_class` on soil: the lee of walls, fences, buildings and forest edges for the
  winter wind (`wind_from`, Tabler shape: full to `lee_flat` x height, gone at `lee_end` x), the windward toe,
  hollows, yards near buildings and fences.

Accumulation `A = SlopeF * DriftF(lee) * DepressionF * (1 - canopy_shield T)`, melt
`M = 1 + a dot(N, noon sun) + b tree_well - c lee - d road` (clamped to `floor`..2; `tree_well` a smooth bump of T over its band, the lee fading out under crowns from `canopy_lee` to a wood, so neither draws a hard outline), a two-octave value
noise. Depth of a class at a texel, the formula the renderer evaluates as well:

```
A_open = A_trees = A (1 + 0.35 n)   M_open = M_trees = M
A_road = 1 + 0.15 n                 M_road = 1
A_drift = (1 + 0.35 n)(0.5 + N)     M_drift = 1 - 0.3 N
D_c = clamp(S_c A_c - Mc_c M_c, 0, cap)    D = sum_c w_c D_c
```

**The LUT.** For every day and class the bake solves `S_c`, `Mc_c` so that the class hits the `[snow_keys]`
targets on this very level: `Mc` puts the covered share (texels above `cover_eps`) at the cover key in closed
form (a weighted quantile), `S` is bisected until the covered mean - the 90th percentile for drifts - reaches
the depth key. It runs on histograms (`lut_bins` along A by 16 along M) of the hard-class ground receivers,
from the very quantised factors the renderer reads; checked against the full per-texel evaluation of
followup_4's escape bake, every key is hit to rounding. Inside calendar winter (1 Dec - 28 Feb, decision D1)
the targets are pinned: open cover at least 0.9 (not a single grass blade), roads covered (`cover.z = 1`), the
snow under the crowns whole (`cover.y = 1`: thinner than the open snow, never bare - the bare tree wells drew
brown discs round every trunk of a deciduous wood).

The log says what came out:

```
* [snow] <level>: <nx> x <nz> texels of 1.0 m, classes open <share> trees <share> road <share> drift <share>, <ms> ms
* [snow]   15 jan: S <open> <trees> <road> <drift>, Mc <open> <trees> <road> <drift>
```

(one line per month with snow; the class shares are those of the hard-class ground receivers, escape's
reference values are in followup_4: open 0.825, trees 0.045, road 0.017, drift 0.113).

**Ground.** Small things standing on the ground are no ground: the slot's span or the 1.5 m off the filled ground
took a stump's, a block's, a cart's top for the terrain. The ground is opened with a 3 x 3 m square (erode, then
dilate: what is narrower drops out, slopes stay) where it stood more than 0.25 m over that and the AI does not walk
there (`* [snow] ground: <n> texels of small objects off the terrain`). **The covers' top** is the highest solid
surface facing up anywhere in the texel, sampled at nine points of it, against the ground at that very point (the
centre alone missed a stump or a post narrower than the metre).

**Covers.** A ray down from over a cover's top tells its kind: the top entered, the next solid face met either
leaves the solid (faces down) or is a floor (faces up). A slab left well over the ground under it (a roof's or a
deck's underside, a ceiling), or a floor more than 1.2 m under a roof of one sheet with nothing between, is an
**overhang** (2; a slab's underside at least 1.5 m over the floor - a vehicle's or a cart's body is no roof, the
wind fills the snow in under it): it shelters the ground under it, the snow there thins out to nothing a texel inside its edge. A
solid left at the ground (a concrete block's, a car's bottom) or a low object of one surface (a stump) is a
**solid object** (1): the ground's snow runs on at its full depth up to its sides and its own snow lies on its
top. (Sheltering every cover sank a ragged moat a texel or two wide round every block, cart and stump, the 1 m
squares of the field drawn into the snow.) An overhang must be broad enough to keep the snow off: a cover's
connected texels with fewer than five of overhang are a solid object (a tall stump or a post open at its bottom
read as a sheet over a floor more than 1.2 m under it, and sheltered a bare pit round itself). The log says how
much of each:

```
* [snow] covers: <n> texels of overhangs, <n> of solid objects
```

A level without `level.details` (underground, interiors) gets no field: no snow, no stamps.

## The frame

`da_snow_frame_update()` (device.cpp, once per frame before the frame sequence) evaluates `SSnowFrame` at `D`
and `D_prev`: `S`/`Mc` from the level LUT (and their previous-frame values for motion vectors), the class
covers, the state weights (fresh, settled, old, wet), age / crust / wetness / drift strength, roadside dirt,
litter, road compaction, the cap, the snowfall and trail refill rate, the air temperature, the ice keys, the
snow colours (linear) and `grass_visible = saturate((0.9 - cover_open) / 0.6)`. `mean_depth` and `active`
let the renderer skip every snow pass in the months without snow.

Between the keys the snow surface lives: every 2 cm of fresh snowfall makes it fresh again (the state shifts
toward fresh, the age resets), without new snow it ages on a 7-game-day scale up to the key's age; freezing
rain leaves a crust that fades over 3 game days; rain on snow soaks it. None of this is saved: a load starts
from the keys.

## What falls: the precipitation phase

The weather says how much falls (`rain_density`, every weather mod unchanged); the season and the air say
what (`da_precip()`, ticked in `CEnvironment::OnFrame` after the weather lerp):

- **Air temperature**: the monthly means (`[precip_keys] t_mean`), a synoptic anomaly (smooth noise over game
  time, ~2.5 game days, `t_sigma` spread) and the diurnal swing (`t_range`, flattened by cloud and rain).
- **Phase**, drawn once per six game hours from the logistic model (`ts`, `tl`, `w`; precipitation days
  `t_precip_bias` colder and `t_precip_sigma` wider): snow, sleet or rain. Inside `snow_only` (1 Dec - 28 Feb,
  decision D6) it is always snow while the climate is off; the climate's winters (WEATHER.md) thaw and rain now and
  then, as Chornobyl's do, and draw the phase on the hour's own fit (`[climate] phase_bias`, `phase_sigma`). A pure function of game time: a reload replays it. While it falls the air is
  kept consistent with it (no snow at +8 C, no rain at -6 C); freezing rain (`glaze`) holds the air below 0.
- **liquid** (rain 1, sleet 0.5, snow 0) eases over seconds; `snow_rate = density^1.5 (1 - liquid)`.
- **Thunder**: a block keeps its lightning with the probability `thunder_weight` (July 1, January 0.02), and
  only while it rains.
- **Frost and dew** for the grass: hoarfrost below +0.5 C on dry nights, dew on dry nights above freezing.

Everything the rain drives follows the liquid share:

| System | Rule |
|---|---|
| rain rate, fog | `rain_rate_mmh = 25 d^1.5 liquid`; snow thickens the haze: `ext += 2.5 S^0.7` (S mm/h w.e.) |
| published wetness | `GetCurrentWetness()` = authored wetness x liquid (the wet look, the rain map); `GetCurrentSnowFactor()` = max(authored, snow rate) |
| rain sheet | each item is born a drop or a flake by the liquid share |
| rain bed sound | x liquid (snowfall is silent) |
| visor | (liquid + 0.3 snow) x sky cover |
| crowns | none from flakes, none on snow, a crown on ice instead of a ring |
| lightning | `thunder_ok` |
| campfires | doused by density x (liquid + 0.3 snow) |
| AI hearing | the rain noise floor x (liquid + 0.15 snow) |
| `level.get_rain_volume()` | unchanged (all precipitation), so shipped scripts keep their meaning |

## Snowfall

The snow mode of the rain sheet (`CEffect_Rain`, `dxRainRender`): a flake sinks at 0.6-1.5 m/s and moves with
the whole wind (slant `atan(wind / fall)`, capped at 75 degrees), swings 5-20 cm round its line at 1-3 rad/s,
is born in a 14 m column through the same shelter ray as the drops (no snow indoors or under roofs, crowns let
it through), and lands without a crown or a sound. Flakes land in the inner 60 % of the sheet's disc (past some 8 m one is under a pixel) and are camera-facing
quads of 2-4.5 cm, wound as the streaks are (the pass culls back faces), drawn by
`da_rain_flake.ps` (`effects_rain_flake.s`): a soft disc with a denser core, lit like the streaks with ice's
forward scatter, fogged and soft against the scene - no texture asset. The count follows the heavier of the
rain and snow rates on the same `r__rain_quality` ladder.

## Ice

With the climate on (WEATHER.md) the ice keys below are the climate's - the cover closes over with the thickness
the frosts grew - and the ice carries a man where it is thick enough, cracks and gives way where it is not, and holes
(WEATHER.md, ice); with it off, the season keys and the ice is visual only (decision D7). `CEnvironment::ice_at(p)`,
`water.ps` and the snow passes share one rule
(`shaders/r3/da_snow_ice.h`): the season's ice keys (`[ice_keys]`: still and running water), a front that grows
from the banks (`key x 38 m` on still water, `key x 40 m` on running water: both close over at key 1, and the
running key lags, so a channel keeps an open lane while its ice is young), ragged by a 20 m noise. Running water is told from standing water at load
(`CEnvironment::water_class`: connected sheets of the water field narrower than 60 m and at least six widths
long). Water the field does not carry (the marsh's radiation water) takes the still key, patchy while partial.
Water is frozen from half its ice cover on, as gameplay takes it; the outer half of the cover is the front
where the ice thins into open water.

What the ice looks like (`da_snow_ice.h`, the same in both renderers):

- **Bare ice**: black ice, dark grey-green over the water column (linear albedo 0.03-0.08) with a 0.3 m mottle,
  greyer blotches of 5-10 m and 1-2 m where air and snow froze into it, pale crack lines of a coarse (3 m) net
  along a few long runs and a fine (0.9 m) one in rare patches that fade out below the pixel footprint (most
  borders stay sound, so the ice never reads as paving), and streaks of wind-blown snow dust along the winter
  wind over a part of it (more of it in the snowy months). Gloss at most 0.06 (a pale sky mirror washed the
  March ice to mid grey); `water.ps` keeps a quarter to 0.6 of the water's mirror on it (ice is rough), so a
  frozen pond never reads as open water.
- **Snow on the ice**: the `snow_depth_on_ice` key where the `snow_cover_on_ice` key reaches, over a 3 m and a
  1 m noise, so a partial cover is ragged patches whose depth tapers to nothing at their margin; none on young
  ice. January and February cover the ice whole (14-18 cm, a few wind-swept spots), December mostly, March in
  scattered patches over bare ice.

Near the camera (the ground window, 64-128 m by preset) the snow grid draws the frozen water as a sheet: on
the water surface, raised by the snow on the ice or a few centimetres over bare ice, shaded by the snow layer
like the ground (SNOW.md) - the bank's snow runs on to it without a step, the ice ends where the ground rises
out of the pond's level along a smooth, slightly meandering line (per pixel from continuous quantities, never
the water texels' squares), ground below the pond's surface that the collision's water faces miss is part of
the ice, and the flat shore just above the waterline never lies under the top of the ice (the water mesh's bob
would draw its ice over it). Beyond it `water.ps` draws the ice:
it drops waves, ripples, rings, rain, caustics, foam, film and leaves and the refraction offset, and draws the
bare ice (the column showing through a third while young) with the surface's own mirror, dimmer on dusty ice,
under the same snow in the open snow's colour. The ray traced reflection request skips frozen water.
Gameplay: `da_water_surface` - the one choke point of step, bullet and body rings and splashes - says "no
water" on ice, wading wakes stop, a blast throws no fountain off ice.

## Deformers

Stamps (`SSnowStamp`: xz, foot_y, radius, sharp, push, kind, sink, xz0, foot_y0, pack, fall) are added from any
thread during the frame and published at the next frame start (`da_snow_stamps()`). A stamp is the capsule its
deformer swept since the last frame (from `xz0` / `foot_y0` to `xz` / `foot_y`; a disc when they are equal), so a
fast leg or body leaves a continuous furrow. How the snow under it gives way (SNOW.md, trails):

- `pack` > 0 - a planted foot or a body lying on the snow packs the loose snow under it to this density (kg/m3)
  and sinks until the packed column carries it: the floor stands at `D * rho / pack` over the ground (`rho` the
  open snow's density of the day: fresh powder about 80, January's settled snow about 220, old snow 350);
- `fall` > 0 - a swinging leg or a low body ploughs through the snow: what lies above it is pushed aside and this
  share falls back into the furrow behind;
- both 0 - carved down to `foot_y` (a crater);

and never deeper than `sink` under the snow surface (the creature's reach; 0 = no limit; a negative sink leaves the
snow alone). The grass push map uses `radius` and `push` of every stamp.

| kind | Who | Rule |
|---|---|---|
| 0 foot | every creature within 96 m of the camera, every frame | the foot bones the step manager reads (`[foot_bones]` of the visual, else the section's `foot_bones`), the bone's world position (`get_foot_position`'s transform), the sole = bone y - its bind-pose height; stamped while the sole is on or under the snow surface (the contact gate, so a tushkan's jump or a chimera's leap stamps too) and not under a roof; a planted foot (its horizontal speed under 0.35 of the body's plus 0.25 m/s) packs the snow by its creature's `pack`, a swinging one ploughs a furrow at its own height (`fall` 0.3): where the snow is deeper than the legs lift, the prints join into a trail; push = the foot's smoothed velocity |
| 1 body | the same creatures | a capsule under the body at its belly height (a wounded stalker lies, a crouching actor kneels): the low bodies plough deep snow (`fall` 0.35, 0.6 for a body under 0.15 m: a tushkan in the deepest drift swims in a shallow trough over its back, not an open trench), every body pushes the grass |
| 1 moving | physics in motion within 40 m of the camera (an enabled shell, no parent, no living creature; up to 8 objects of up to 16 elements, rescanned every 0.15 s; an element faster than 0.25 m/s - a prop settling after a load creeps by millimetres and ploughed a pit round every crate) | each element's box bottom swept since the last frame ploughs a furrow at its own height, the narrower of its flattest axes wide (0.06-0.4 m), `fall` 0.35: a barrel rolling, a crate pushed, a body dragged or kicked; at rest again the settle callback lays its print |
| 2 resting | corpses, loot, artefacts (a prop - a crate, a plank, a barrel, a cart - stands in the snow up to its sides: it was there before the snow; its hollow read as a pit round it, under a cart's body the bare ground) | each physics element's box laid on the ground as discs. Its physics lies on the ground under the snow, so a loose object lies on the bare, flat floor of its own hollow (discs of at least 0.3 m radius, laid dense enough that their flat floors - `dist^8` walls - join into one over the box, no compacted residual - packed by its weight, a rifle went under a drift whole, and a hollow narrower than the snow grid's 12.5 cm cells closed over it again); a corpse packs it by its creature's `pack`, at most 0.4 of each element's height under the snow (`sink`), each disc a little off its place and size, with ten shallow ploughed scuffs round the body (the snow it fell into churned); when it comes to rest (`on_physics_disable`) and whenever it comes into the window (scans every 0.5 s; again on crossing 44 m and 22 m) |
| 3 crater | explosions near the ground (not an air burst more than 0.4 of the hole's radius over the snow) | an irregular hole, 0.3 x the blast radius (0.6-3 m): a core and seven lobes of random reach and size, each a flat bare floor in steep walls (`r^4`; a bowl from the centre read as a soft dimple, not a blast) |
| 3 bullet | every bullet striking the static world under the snow within 60 m (`Level_bullet_manager_firetrace`; under the snow by the surface bilinear at the point - the lowest of the texels round it lay under a slope's ground, and shallow snow on a slope took no marks; or on a cover's top under its open snow - a roof, a trailer, a wall top: into the roof map, a third wider) | the channel it bored from where it went into the snow (back along its flight, a metre at most) to where it struck, 7-12 cm wide by the wallmark, with a dent where it went in; a load of shot lands as a cluster of them |
| 3 campfire | every campfire within 60 m, every frame | the pit never holds more than a 3 cm dusting (the fire can be lit, its logs and stones show); a lit fire thaws the snow round it over 90 s (`CZoneCampfire::snow_melt`, back over 30 min once out): bare in the pit, thinner and thinner out to 2.5 m - `sink` 1, a soft bowl with no edge. What melted comes back only with fresh snow |

A frame keeps at most 4096 stamps (`da_snow_stamp_add` returns false past it, `da_snow_stamps_room` says how many
more fit). A resting object's print (up to 48 stamps) is laid whole or not at all and recorded only when laid:
the snow arriving, or a level loading with snow, finds every corpse, loot item and prop in the window at once,
so a scan lays them nearest first within 1024 stamps, and neither the scan nor the settle callback ever takes
the last 512 places of the list, which the creatures updating after the actor need for their feet and bodies.
What did not fit is laid by the next scan, half a second later.

Psy-dog phantoms and poltergeists press nothing (explicit casts). Radii, belly heights, reach and pack per
creature class are built in from the deformer audit and can be set per class id in `[snow_deformers]` (reach
and pack are optional; a line without them keeps the class's built-in ones):

```ini
[snow_deformers]          ; <class id> = foot radius, body radius, belly height [, reach [, pack]] (m, kg/m3) | none
SM_DOG_S = 0.07, 0.20, 0.42, 0.35, 400
SM_POLTR = none
```

Pack grows with the foot's pressure: in January's settled snow (about 220 kg/m3) a man sinks to a little under
half its depth (6-7 cm prints in 15 cm, 15 cm in a 0.3 m drift), a dog to about two fifths, a tushkan or a rat
hardly at all - it runs over settled snow and swims in a drift; in fresh powder (about 80 kg/m3) everyone sinks
most of the way.

| Class | Feet | Body | Belly | Reach | Pack |
|---|---|---|---|---|---|
| actor, stalkers | 0.12 | 0.30 | 0.90 (grass only) | 0.50 | 520 |
| tushkano / rats | 0.06 / - | 0.12 / 0.08 | 0.10 / 0.03 | 0.20 / 0.08 | 260 / 220 |
| blind dogs, pseudodogs, psy-dogs | 0.07, 0.08 | 0.20, 0.22 | 0.42, 0.44 | 0.35, 0.38 | 400, 440 |
| boars, flesh, cats | 0.10, 0.10, 0.06 | 0.35, 0.40, 0.15 | 0.35, 0.30, 0.30 | 0.40, 0.40, 0.25 | 580, 600, 300 |
| snorks (crawl) | 0.10 | 0.30 | 0.20 | 0.45 | 480 |
| bloodsuckers, controllers, izlom, zombies | 0.12 | 0.30 | 0.90 | 0.50 | 560, 520, 520, 520 |
| burers, chimeras, pseudogiants | 0.14, 0.12, 0.25 | 0.35, 0.40, 0.80 | 0.45, 0.40, 0.45 | 0.45, 0.50, 0.60 | 540, 600, 700 |
| any other class | 0.10 | 0.30 | 0.60 | 0.45 | 480 |

Craters have no pack: their floor is bare ground.

**On covers.** The contact gate (`in_snow`) takes a deformer's bottom on the ground's snow or on a cover's top
(a roof, a trailer, a block: within 0.8 m under the texel's top - a slope's run and the beams a roof carries -
up to the snow lying there, `da_snow_open_depth_at`), so the actor, the NPCs and the monsters leave their prints
on the snow the shells lay on the roofs as well (SNOW.md, the roof map).

## Other code

- `da_snow_depth_at(x, z)`, `da_snow_surface_y(x, z)`: the season snow (no trails) bilinear at a point, x (1 -
  shelter); under a cover the surface is the ground's.
- `da_snow_open_depth_at(x, z)`: the same without the shelter: the snow on a cover's top.
- `da_snow_walkable(top, walk)`: whether the AI walks a top (the cover rule).
- `da_snow_texel_depth(field, i, S, Mc)`: the depth formula of one texel.
- `da_snow_targets()`: the class targets with the winter pins (what the bake calibrates against).
- `CEnvironment::ice_at(p)`, `CEnvironment::water_class`.

## QA

Console (never saved):

| Command | Does |
|---|---|
| `qa_snow_state` | prints the frame (S, Mc, covers, state, surface, fall, ice, mean depth), the precipitation (phase, liquid, snow rate, air, frost, dew, thunder) and the field (size, class shares, stamps, depth and surface under the camera) |
| `qa_precip auto\|snow\|sleet\|rain` | forces what falls (the weather still says how much) |
| `qa_air_temp <C>\|off` | pins the air temperature |
| `qa_snow_shot <n> [spread]` | n strikes along the view (spread in degrees round it) on the static world through the bullets' snow path, and how many marked the snow: a burst or a load of shot without a weapon; a negative spread presses a boot where the ray strikes instead (a roof's print without a walk) and probes there |
| `qa_snow_fire_melt [0..1]` | sets every campfire's thaw at once (a lit fire takes 90 s to get there) and lists their places, states and thaw |
| `qa_snow_probe` | the contact gate at the actor's feet: top, ground, cover, depth, open depth, in snow or not |
| `qa_snow_objects [radius]` | the small raised things of the bake round the actor (connected texels standing over the ground, under 40 of them), nearest first: centre, top, height, cover kinds |

With `qa_season_year` / `qa_season_pin` (SEASONS.md) the phase is redrawn every half second from the
climatological shares of the day, the synoptic noise and the snow surface dynamics stand still and the hour is
frozen, so a year sweep shows the season itself.

To look at: a winter month with a rainy weather (flakes, no rain bed, no lightning, no puddle rings), the same
weather in July (rain and thunder), a frozen pond in January (snowed over, the bank running on to it, no rings
from steps or bullets) and in March (dark ice with cracks and dust under snow patches), an open lane on a wide
river in December, `qa_snow_state` next to a road (cover road 1, depth under the camera), a dog running
through a drift (a trail of joined prints and furrows), a stalker walking (prints with drag marks in the open,
a trench in a drift), a corpse falling into snow (half sunk in a packed hollow, the snow round it churned) and
dragged (a broad furrow with ridges), an explosive barrel blown in a yard (a crater of steep walls), single shots
and a load of shot (`qa_snow_shot 4 1.5`, `qa_snow_shot 12 3`), a campfire out and at full thaw
(`qa_snow_fire_melt 1`), the actor walking a roof (prints in the shell) and a stump, a cart, concrete blocks
(full snow up to their sides, their own snow on top, no ragged holes round them).
