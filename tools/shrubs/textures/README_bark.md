# Shrub bark textures

`make_bark_textures.py` builds the bark of the procedural shrubs from scratch (numpy + PIL, no photos): 19 types,
each a seamless 512 x 512 tile of **6 x 6 cm** of mature bark (2+ year stems and branches).

## Scale and orientation

- x = u, round the stem; y = v, along it; row 0 is the lower end, growth toward +v (prickles lean toward row 0).
- The engine wraps a whole number of tiles round each stem (`r3/da_shrub_wood_vs.h`), at least one, so features keep
  their real size on stems from about 2 cm up and are squeezed round thinner ones.
- Every field is periodic (FFT noise, Voronoi cells on a torus, stamps wrapped at the edges): the tile repeats in
  both directions without seams.

## Channels

| file | R G B | A |
|---|---|---|
| `build/bark/<type>_d.png` | albedo, sRGB, measured bark levels (most sRGB 55-120, HSV saturation 0.15-0.30) | cavity: 1 open, 0.3-0.5 in deep fissures |
| `build/bark/<type>_n.png` | R, G: tangent normal 0.5 + 0.5 n (x toward +u, y toward +v); B: gloss 0..1 | lichen mask: discrete patches of 1-5 cm (0.7-1, seeking fissure edges and flat old bark) over a third of the type's susceptibility, 0 elsewhere and on fresh surfaces; the shader's own patches pick from them |

The relief is a height field in millimetres; normals, cavity and the lichen mask are derived from it, and the albedo
follows it a little (weathered tops paler, hollows darker).

`bark_manifest.json`: `size`, `metres` (the tile's side), `types` in layer order (`name`, `species`) and `species`
(species id -> layer). `build_arrays.py` packs the layers into the texture arrays; `make_shrubs.py` writes each
species' layer into the shrub data.

## Run

    python make_bark_textures.py                 # all types, the manifest, the review sheets (about a minute)
    python make_bark_textures.py --only elder,hazel --inspect <dir>   # some types, 1:1 close-ups into <dir>

Review: `build/bark_sheet.png` (each type 2x2 flat with v up, a 28 mm stem lit near at 4 px/mm and at play distance
at 1.5 px/mm, shaded the way the engine uses the maps) and `build/bark_crops.png` (full-resolution crops, lit flat,
cavity / gloss / lichen). A partial run writes `*_partial.png` sheets instead.

## Trees

`TREE_TYPES` (after the shrubs' `TYPES`, so their layers keep their indices): 36 types drawn in real millimetres over
a tile of 6 cm x the type's `scale` (the species' `bark_scale`), each type built in its own worker; the manifest's
tree entries carry `scale`, `species` (the base bark) and `upper` (the upper trunk's), and `species_upper` maps the
zone pairs. The old barks are relief systems: `fissured_bark` (V-shaped fissures round plates or ridges on warped
Voronoi borders, strata and splits on the ridges; `TREE_FISSURED`), `flaking_bark` (flakes over one another on the
under-bark, lifted rims with shadow lines; `TREE_FLAKING`), `scaly_bark` (`TREE_SCALY`) and the pine's jigsaw plates,
each with a crust at the texel scale (`micro`); the distance fields come from a periodic exact distance transform
(`edt`, jump flooding). Review: `build/bark_sheet_trees.png`, and against photo packs `review_trees.py` (with the
grazing-light view and the scale measures: docs/dead-air/TREES_TEXTURES.md).
docs/dead-air/TREES_TEXTURES.md
