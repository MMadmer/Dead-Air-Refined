# Third-party content: authors, licences, notifications

The content bundles of Dead Air: Refined carry work by other people. This page is the ledger of
who made what, on what terms it is used, and whom to notify. It is the place to check before a
release that changes the content set, and the list of addresses to write to.

Where a set was taken through the DeadAir-x64 project's redistribution
(`Dead-Air-x64-HD`, `DeadAir-x64-Animation`), the terms below are the ones its README states;
the sets are used unchanged.

## Sets and authors

| Set | Author | Terms | Used for |
| --- | --- | --- | --- |
| Environment textures (replace Stalker Two-K since 1.6.0) | Dead Air: Refined | Part of the mod | `textures/briks, crete, detail, door, floor, glas, glass, grnd, mtl, prop, roof, ston, tile, veh, wall, water, wind, wood`: Dead Air's own textures (or GSC's uncompressed CoP SDK sources of the same picture) upscaled 2-4x with Real-ESRGAN x4plus (BSD-3-Clause, a tool; the output is the project's), colour pinned to Dead Air's file at its own texel scale; bump pairs re-encoded the way the SDK does. Where Two-K was not sharper than Dead Air, or had no Dead Air counterpart, Dead Air's own file is used. Not a single Two-K pixel is used |
| Tree and foliage cards (replace Absolute Nature 4 since 1.6.0) | Dead Air: Refined | Part of the mod | `textures/trees` and the season sets: branches grown procedurally inside the layout of Dead Air's card; leaves from ambientCG LeafSet004/006/007/008/009/012/013/014/016/024 (CC0), needle sprays from Poly Haven `fir_tree_01` and `pine_tree_01` (CC0), bark from ambientCG Bark001/006 (CC0); reeds, sedge, sprouts, forest walls and the bush from GSC's own CoP SDK pictures; colour statistics from Dead Air's cards (and, for summer, from the summer pack shipped with Dead Air - numbers only, no pixels). Absolute Nature is not used, and neither are Dead Air's `trees_atp_*` pictures, which are an older Absolute Nature release |
| Season sets (1.6.0) | Dead Air: Refined | Part of the mod | `textures/seasons/<winter, spring_early, spring_mid, spring_late, summer>`: the tree cards above in each look, Dead Air's own terrain, grass, LOD atlases and PDA maps recoloured (snow for winter), winter versions of the ground materials. Winter's art direction followed screenshots of "Yet Another Winter Mod" (look only; the mod itself was not used) |
| Bark upscale | DeadAir-x64 project (from the game's own textures) | Part of the port | Eight bark textures in `textures/trees` |
| Landing roll sound | Dead Air: Refined (the project's own recording, 05.09.2026) | Part of the mod | `sounds/actor/fall_roll.ogg` - converted to mono 44.1 kHz and given the X-Ray ogg comment by the project |
| **FDDA 0.9b** (food, drink, drugs, harvest) | **Feel_Fried** | Public Domain | `enhanced_animations.script`, `take_item_anim.script`, `ciga_effects.script`, the `item_ea_*` hud sections, `meshes/dynamics/weapons/wpn_eat`, `meshes/anomaly_weapons/hud_hands_animation/zzzz_ea_*`, `textures/usable_items`, `sounds/interface/item_usage`, `anims/itemuse_anm_effects` |
| **Ledge Grabbing** (parkour) | **themrdemonized** | MIT | `demonized_ledge_grabbing.script` and its helpers, `climbing_in.omf`, the baked camera curves |
| **FDDA Enhanced Animations - Food and Drinks** | **Mirrowel** | Creative Commons | `ea_addon_mirrowel.ltx`, `zzzz_ea_a_main.omf`, `zzzz_ea_backpack.omf` (the backpack scene), the matching item models |
| **Headgear Animations** (helmets) | **lizzardman** | Permission given publicly on 27.08.2026 ("everything in open access may be used freely") | `headgear_*` omfs, `meshes/dynamics/weapons/wpn_headgear`, `textures/headgear_hud`, `sounds/actor/headgear_hud` |
| **FDDA Redone / "Anomation"** (outfits, body search) | **lizzardman** | Same permission | `liz_*` omfs, `meshes/anomaly_weapons/liz_outfit_animations`, `textures/liz_outfit_animations`, `sounds/actor/liz_*` |
| **MFS bread and sausage anims** | **ZeburG** / MFS team, <https://www.moddb.com/members/zeburg> | Author's permission (via the DeadAir-x64 project) | `mfs_*` omfs, `meshes/dynamics/mfs`, `textures/usable_items/mfs`, `sounds/interface/item_usage/mfs_*` |
| 3D PDA | Gunslinger team | See `README.md` | `meshes/dynamics/devices/dev_pda`, `pda_hands_animation.omf`, `textures/item/item_kpk*`, `textures/ui/ui_deadpda*`, `ui_pda_loadscreen*` |

## Who has to be notified

Since 1.6.0 no set in the content bundles asks for a notification: Stalker Two-K and Absolute
Nature 4 are gone (see the rows above). Up to 1.5.x the bundles still carried them, and their
authors were written to: Akinaro (Stalker Two-K, akinaro@onet.eu) and Cromm Cruac (Absolute
Nature 4, info@crommcruac.com).

The animation authors did not ask for a notification; the terms are attribution (MIT and
Creative Commons require the notice to travel with the work, which this page and the readme
provide) and, for lizzardman and ZeburG, the permission already given. ModDB profiles, should a
question come up: lizzardman <https://www.moddb.com/members/lizzardman>, ZeburG
<https://www.moddb.com/members/zeburg>.

No e-mail address exists for Feel_Fried, themrdemonized, Mirrowel, lizzardman or ZeburG in any
of the source repositories.

The pipeline that makes the 1.6.0 textures (upscale, palette lock, bump re-encode, tree card
generator, season looks) lives outside the repository, in `D:\Games\Dead-Air-AssetCompare\pipeline`;
the CC0 sources it reads are in `D:\Games\Dead-Air-AssetCompare\sources`.

## What is deliberately not included

- `items_anm_dummy` - FDDA's helper inventory item. A section absent from the original game
  can land in a save and break it there; the module runs without it.
- FDDA's cigarette smoke particle (`damage_fx\mod_cig_smoke`) and its texture: the effect is
  not in Dead Air's `particles.xr`, and an unknown particle name is fatal in the engine.
- FDDA's Anomaly version window, MCM pages, `script_light`, `wallmarks_manager` paths.
- Nothing that spawns: no new weapons, items or NPCs. Every added config section is a hud
  section (hands seat, item model, cycle names), and saves stay compatible with the original.
