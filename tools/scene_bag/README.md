# Scene bags

The bag the hands take out and unzip when the inventory opens (`[da_backpack_*_hud]` in
`configs/items/items/items_anm_da_backpack.ltx`). The project's own models replace Mirrowel's
`wpn_backpack_act_hud.ogf`; the motions stay his: `zzzz_ea_backpack.omf` for the hands and
`wpn_backpack_act_hud_anm.omf` for the bag, which moves only the zipper pull.

## What a bag has to keep

- **The skeleton and its bind.** `wpn_body` (root, identity bind) and its child `zipper`. Every
  bag is exported on the original's own armature, so the bind and the motion reference come
  across untouched.
- **The frame.** The engine hangs the bag on the hands' `lead_gun` bone and holds its root at
  identity (`item_root_lock`), so the bag's model space is the hand bone's space. A bag drawn in
  the original's coordinates lands where the original did, with no `item_position`.
- **The outer size.** The hull is a quad cage pulled onto the original body, relaxed by at most
  0.8 mm where the hands touch and 2.5 mm elsewhere.
- **The zipper on the pull's path.** The centreline is fitted through every key of the pull over
  both cycles (`bp_zipcurve.py`, within 0.78 mm). The original's own painted zipper does not
  follow the pull on the left side; ours does.
- **The pull's footprint.** The original pull is an 8 to 12 mm by 22.5 mm by 3 mm plate at the
  bone origin. Ours stays inside that envelope, so the fingers close on it the same way.

## Where the hands are

Measured from the original over both cycles (`bp_contact.py`, `bp_handdump.py`, `bp_clearance.py`):

- The palm sweeps the lid above the zipper, so only flat pieces go there (0.6 mm at most).
- The band just under the zipper (y up to 0.025) clears the hands by 6 to 20 mm. It is the
  part of the front the game shows.
- The left thumb lies 2.2 mm off the lower left of the front (x < -0.05) early in the open
  cycle, so nothing there stands more than 2 mm.

The hands already sink deep into the original bag in its own animation. The check is
therefore how much closer ours comes than theirs (`bp_depth.py`, `bp_depthcmp.py`), not zero
contact.

## Build

Blender with the X-Ray addon, run headless:

```
blender -b --python bp_analyze.py          # original bag: bones, mesh, motions -> bp_orig.json / .blend
python bp_path.py && python bp_zipcurve.py # pull path in the bag frame, zipper centreline
blender -b bp_orig.blend --python bp_hull.py -- 6   # our hull -> hull.blend
./build_all.sh [variant ...]               # bp_build (model, seams, details, Cycles bake, OGF)
                                           # bp_textures (DXT1, X-Ray bump pair, .thm) and bp_preview
python install_bags.py apply               # into the 1.6.0 assets tree; the old bag and its texture out
```

The looks live in `bp_variants.py`: one per backpack section the slot can hold, plus `default`
for no backpack, a slot the suit locks, or a slot holding something that is not a backpack.
