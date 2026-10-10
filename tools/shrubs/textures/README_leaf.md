# Shrub leaf textures

`make_leaf_textures.py` draws every species' leaves from its botany (outline, margin, venation, surface) in
millimetres at 1536 px and box-filters them to 512 px RGBA8 layers. The sizes come from `tools/shrubs/make_shrubs.py`
(leaf length and width, petiole, spray, rachis, leaflets); nothing is taken from photographs.

    python tools/shrubs/textures/make_leaf_textures.py              all layers, manifest, review sheets (~2.5 min)
    python tools/shrubs/textures/make_leaf_textures.py --only cor_ave,fra_aln --ss 2    quick look at a few species

`build_arrays.py` turns the layers into the engine's texture arrays.

## Coordinates

u = texture x (0..1) across the object, the midrib (shoot, rachis) at u 0.5; v = image row, row 0 (v 0) where the
petiole (shoot) attaches, the last row the tip. The object fills the square: its widest part spans u 0.03..0.97, the
base sits at v 0.016 (blades) or 0.006 (the others), the tip at v 0.984 / 0.99. Everything is drawn at its physical
proportions, so the square is meant to be stretched to the layer's `box_mm` (length along v, width across u).

## Channels

`<layer>_d.png` (linear data, not colour)

| | |
|---|---|
| R | upper-side luminance factor x 0.5 (128 = x1): the lamina's mean 1.0, its p5..p95 about 0.77..1.26 (areole and intercostal cells, the domes' crests lighter and folds darker, mottling, the margin). Impressed veins (hazel, willows, alder buckthorn, guelder rose, raspberry, dewberry, currants) a little darker on their line and 12-20% darker along their grooves (alder buckthorn's line itself lighter, a glossy species); raised veins (elders, spindles, lilac, roses, bird cherry, cherry) 13% lighter. On the b layers only: a few small spots, pale patches, sometimes a browned margin. No holes, bites or tears anywhere (the shader adds bites) |
| G | vein mask: 1 midrib / petiole / rachis / twig, 0.75 a lobe's primary, ~0.4 secondaries, 0.15-0.2 tertiaries, ~0.05 areoles, 0 lamina |
| B | light transmission: 1 thin lamina, 0.3-0.5 veins, 0.15 petiole and rachis, 0 wood |
| A | coverage, antialiased (teeth, lobes, bites and tears included) |

`<layer>_n.png`

| | |
|---|---|
| R, G | upper-side tangent normal, 0.5 + 0.5 n (x toward +u, y toward +v, z positive): impressed (or raised) veins, the fields between the major veins domed (hazel pleated), fine cushions, the midrib's V, waves, rolled margins. Each leaf's relief is scaled to its species' slope p95 (hazel 0.5, rugose leaves 0.33-0.37, smooth ones 0.25-0.3); the height field runs on past the outline, so the margin's normal has no cliff |
| B | gloss 0..1 (lamina 0.85-0.95 glossy, 0.3-0.5 matte or hairy; veins lower) |
| A | underside luminance factor x 0.5 (128 = x1): raised paler veins, tomentum, glands, scales |

Outside the coverage the colour channels are pushed outward 6 texels (so the mips do not darken the edges), then
R 128, G 0, B 0 and a flat normal with B and A at 128.

## Layers (leaf_manifest.json)

- `a`, `b`: a leaf of the species, the second one its own outline, veins and blemishes. Kind `simple` (one blade,
  with `hull`: the three pairs (v, full width) of the base-pairs-tip polygon holding every covered texel), `lobed`,
  `compound` (the whole leaf: petiole, rachis, leaflets, stipules) or `needles` (a juniper sprig).
- `leaflet`: a compound (and a trifoliate broom's) single leaflet, kind `simple` with a hull.
- `card`: a spray species' stretch of shoot seen from the side, its leaves round it by the phyllotaxis, foreshortened.
- `jun_com` `a` / `b` (kind `needles`): a juniper sprig from a 3D model - whorls of three needles every 3-3.6 mm, each
  whorl turned about 40 degrees from the last, the needles 10-15 mm at 45-75 degrees toward the tip - projected as seen
  40 degrees off the shoot's side (square from the side it is a herringbone, a fern frond in a row of cards) and
  drawn far to near: farther needles darker, the white stomatal band where a needle's upper face turns to the viewer.
  The young shoot is green needle bases, no twig flag (G 0).
- `species`: each species' layer indices `a`, `b` (= a when there is one), `card`, `leaflet` (-1 none).
- `box_mm`: the physical length and width the square stands for; a species' `a` and `b` share one box (the engine
  draws b in a's square), each leaf at its own size within it. A layer's box never shrinks below the manifest's
  (the engine sizes its elements by it): a new outline only grows it. `form: trifoliate` marks the brooms' leaf
  layers.

The run prints, per layer, R's p5 / p95 over the lamina and the normal map's slope p95 inside the blade and at its
outline.

## Review

`build/leaf_sheet.png` (every layer lit as the engine lights it: summer green x R, the normal map under a sun from
the upper left and a little sky, tip up; and its coverage), `build/leaf_review_NN.png` (lit, at the real proportions,
underside, coverage), `build/leaf_crop_*.png` (full resolution, lit and underside).

## Trees

The trees' layers follow the shrubs' in the same array (the shrubs' indices never move): `tree_leaf_botany.py` holds
their sizes and sprays, the generator their morphology (`M_TREES`, `LOBES_TREES`, `COMPOUND_TREES`, `PALMATE`,
`CARD_TREES`) and two needle tufts drawn as curved twisted strokes (`scene_pine`, `scene_spruce`, their branch-tip
cards). A compound species gets a card of whole compound leaves too; the sour cherry grown as a tree shares the
shrub's layers. `--fresh-trees` lets the trees' boxes shrink while they are tuned. docs/dead-air/TREES_TEXTURES.md
