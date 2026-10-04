# Seasons (1.6.0)

Dead Air is autumn. 1.6.0 adds five more looks of the same world: winter, early spring, spring,
late spring and summer. Each look is a set of texture overlays that the renderer prefers over the
base textures while it is active; the base set stays autumn. Autumn has an overlay folder too, for
the one thing the level archives cannot carry: the LOD atlases rendered from the base cards.

## Switching

- Main menu or pause menu -> "Опции погоды" -> "Времена года" -> "Сезон", then "Применить"
  (`atmosfear_options.script`, page `af_opt_season`; shown only when the engine has `r__season`).
- Console: `r__season autumn|winter|spring_early|spring|spring_late|summer` (saved in user.ltx).

The switch applies live. On the frame after the change the renderer reloads only the textures that
have an overlay in the old or the new look, in chunks of 128, keeping the old views alive until the
new ones exist (ray tracing tables and DX12 descriptors stay valid), then resets TAA/upscaler
history. Expect a freeze of a few seconds. Code: `CResourceManager::SyncTextureSeason`
(`src/Layers/xrRender/ResourceManager.cpp`), lookup `texture_season_overlay` (same file), the
resolver hook in `texture_resolve_name` (`src/Layers/xrRenderDX11/dx11Texture.cpp`), the command
in `src/xrEngine/xr_ioc_cmd.cpp` (`da_season_folder()` in `da_season.h`).

## Layout

| Look | Folder |
| --- | --- |
| autumn | `textures/seasons/autumn` (LOD atlases only) |
| winter | `textures/seasons/winter` |
| spring_early | `textures/seasons/spring_early` |
| spring | `textures/seasons/spring_mid` |
| spring_late | `textures/seasons/spring_late` |
| summer | `textures/seasons/summer` |

- `textures/seasons/<folder>/<logical name>.dds` overrides the texture of that name, bump maps
  included.
- `textures/seasons/<folder>/levels/<level folder>/<name>.dds` overrides a texture the level folder
  provides (terrain colour, `build_details` grass atlas, `level_lods`); it is checked first, and
  only for names the level folder itself has.
- `.thm` descriptions are keyed by logical name, so an overlay uses the base texture's bump,
  detail and material.

The bundle layout keys the season folders one level deeper (`textures/seasons/<folder>/trees`,
`.../map`, `.../levels/<level>` and so on; `packaging/dead-air-x64/content/groups.ltx`); the bundle
builder takes the longest assigned prefix of a path.

## What each look contains

- Tree cards (`trees/`): every card family of Dead Air is the project's own (see ASSET_CREDITS.md),
  one grown structure per family so a tree keeps its silhouette through the year. The grown tree is
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
  Reeds, sedge, sprouts, forest walls and the bush follow the same looks from GSC's own pictures.
- Per level:
  - terrain colour recoloured (summer towards the summer pack, winter under snow, spring stages from
    wet bare ground to fresh green);
  - grass atlas (`build_details`) sprite by sprite, each sprite found through `level.details` with
    its height and kind: the shrubs of winter stand whole with snow at their foot and frost above;
    early spring has last year's pale straw with the first green at its foot and no ferns or nettles
    yet; the later stages walk to the summer pack's colours;
  - tree LOD atlas (`level_lods`) rendered from the cards of the look itself, autumn included, so a
    far tree is bare, budding or green exactly like the near one. Each LOD visual lists its tree's
    meshes and eight view quads with their cells in the atlas; colour and alpha follow Dead Air's own
    bake (fitted through a render of the same trees with Dead Air's original textures). Cells of cars,
    ruins and rocks keep Dead Air's pictures in every look.
- PDA maps: Dead Air's maps are already summer renders; winter and spring stages are recoloured.
- Winter only: snowy ground materials (`grnd/`) and terrain detail layers (`detail/detail_grnd_*`),
  with winter bump pairs (the relief and gloss of gravel and leaves do not show through snow); roofs
  under snow with their own bump pairs (90-100 % of every roof texture's area faces up on the levels);
  frost and snow in the cracks of every bark, snow on the cut faces of stumps and trunk caps; the
  fallen-leaf decals gone under the snow; ivy without its leaves.

## Grass by season

The engine takes the grass away by season (`CDetailManager`, `detail_season_grass` in
`src/Layers/xrRender/DetailManager.cpp`): `configs/dead_air_x64_details.ltx` `[season_grass]` gives
per season the share of the grass density the graphics preset sets and of the grass height the player
sets (`r__detail_height`), and the same two for shrubs. The woody details of each level (shrubs and
branchy dry stalks, by object index in its `level.details`) are listed in `[season_shrubs]`
(generated by `pipeline\gen_shrubs.py`). Winter has no grass and full shrubs; early spring keeps a
third of the grass at under half its height; it grows back through spring. A season change rebuilds
the details between frames; the preset and the player's setting stay what they are.

## Making them again

The generator lives outside the repository, in `D:\Games\Dead-Air-AssetCompare\pipeline`
(`trees\seasons.py`, `trees\seasons_gsc.py`, `seasons_levels.py`, `seasons_details.py`,
`seasons_lods.py` with `lod_render.py` and `xr_level.py`, `seasons_ground.py`, `seasons_winter_extra.py`,
`gen_shrubs.py`, `encode_seasons.py`,
`assign_seasons.py`); it writes into `Dead-Air-Refined_Assets\source\gamedata-1.6.0`.

## Checking in the game

`tools/qa/seasons/` has two probes: one walks the six looks on a save and shoots each, the other goes
through the weather dialog's Seasons page the way a player does (README there).

## Known limits

- Snow on walls and props is not part of the overlays (a texture cannot tell which way a surface
  faces); winter covers the ground, the terrain, the vegetation and the roofs.
