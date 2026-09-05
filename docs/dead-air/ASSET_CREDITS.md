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
| **Stalker Two-K** | **Akinaro**, <https://www.moddb.com/mods/stalker-two-k> | Free, S.T.A.L.K.E.R. games only, author notified, credited, no money including donations; not to be modified without notifying the author and telling players about the edits; the author may withdraw the permission | Environment textures: `textures/briks, crete, detail, door, floor, glas, glass, grnd, mtl, prop, roof, ston, tile, veh, wall, water, wind, wood` (2737 files; 2736 unchanged, one recoloured by the Refined project on 05.09.2026: `grnd/grnd_rocks_02.dds` - the moss patches desaturated to an autumn olive and darkened, the rock untouched - see the texture audit row in TEST_MATRIX.md) |
| **Absolute Nature 4** | **Cromm Cruac** (Marcin Zemczak), <http://absolute.crommcruac.com> | Same four conditions, and modification is allowed | Tree and foliage textures in `textures/trees` (112 files; 55 of them upscaled 1024 -> 2048 by the DeadAir-x64 project; two recoloured by the Refined project on 05.09.2026: `trees_atp_spruce_branch_dry.dds` and `trees_atp_pine_branch_dry.dds` - the dead-branch cards moved from yellow-brown to the dark grey-green of the stock Dead Air textures (Lab mean/spread transfer, HD detail and alpha cutout kept), because Dead Air's spruces mix live and dry cards on one tree and read half yellow - see the texture audit rows in TEST_MATRIX.md) |
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

The two texture authors make notification a condition of use. Write before the release that
ships their work:

| Author | Contact | What to say |
| --- | --- | --- |
| Akinaro (Stalker Two-K) | **akinaro@onet.eu** (or any other channel) | That Dead Air: Refined includes Stalker Two-K free, for S.T.A.L.K.E.R. only, with credit in the readme and the release notes, and - the condition of the set - that one texture (`grnd_rocks_02`, the moss on the boulders) was recoloured for the mod's autumn palette; the release notes tell players the same |
| Cromm Cruac (Absolute Nature 4) | **info@crommcruac.com** | That Dead Air: Refined includes the Absolute Nature 4 tree and foliage textures (55 upscaled by the DeadAir-x64 project), free, no donations, for S.T.A.L.K.E.R. only, with credit; and, as a courtesy (the set allows modification), that two textures (`trees_atp_spruce_branch_dry`, `trees_atp_pine_branch_dry`) were recoloured from yellow-brown to Dead Air's grey-green |

The animation authors did not ask for a notification; the terms are attribution (MIT and
Creative Commons require the notice to travel with the work, which this page and the readme
provide) and, for lizzardman and ZeburG, the permission already given. ModDB profiles, should a
question come up: lizzardman <https://www.moddb.com/members/lizzardman>, ZeburG
<https://www.moddb.com/members/zeburg>.

No e-mail address exists for Feel_Fried, themrdemonized, Mirrowel, lizzardman or ZeburG in any
of the source repositories; the two addresses above are the complete list.

## What is deliberately not included

- `items_anm_dummy` - FDDA's helper inventory item. A section absent from the original game
  can land in a save and break it there; the module runs without it.
- FDDA's cigarette smoke particle (`damage_fx\mod_cig_smoke`) and its texture: the effect is
  not in Dead Air's `particles.xr`, and an unknown particle name is fatal in the engine.
- FDDA's Anomaly version window, MCM pages, `script_light`, `wallmarks_manager` paths.
- Nothing that spawns: no new weapons, items or NPCs. Every added config section is a hud
  section (hands seat, item model, cycle names), and saves stay compatible with the original.
