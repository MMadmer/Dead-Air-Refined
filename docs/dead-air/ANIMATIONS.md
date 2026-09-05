# First-person animation module

What the module is, how the engine supports it, what a modder can hook, and how it is tested.
Credits and licences are in [`ASSET_CREDITS.md`](ASSET_CREDITS.md); the content bundles that
carry the assets are described in [`CONTENT_BUNDLES.md`](CONTENT_BUNDLES.md).

## What the player gets

- **Item scenes** (FDDA): food, drinks, medical items and cigarettes are used in the hands
  instead of vanishing from the inventory.
- **Skinning** with a knife: a scene on the carcass, then the stock loot window. "Skinning
  animations" in the same menu.
- **Body search**: a scene before the search window opens.
- **Backpack**: opening the inventory plays the bag scene first; the bag model is the one the
  player wears (`da_backpack_<section>_hud`, else the common one).
- **Wear**: putting on an outfit or a helmet from the inventory plays the equip scene with the
  new hands.
- **Parkour**: a jump into an obstacle with the jump key still held climbs onto it when the top
  is 1.4-2.5 m above the feet, flat, with headroom and in view. Release the key and the actor
  only bumps. Stamina and satiety are spent by the load carried, and the climb starts only
  with the stamina it will spend (four thirds of the base drain). "Parkour (hold jump)".

Nothing here spawns: every added config section is a hud section (hands seat, item model,
cycle names). Saves stay loadable in the original game and in every non-graphics mod.

## The engine side

| Piece | Where | What it does |
| --- | --- | --- |
| Two hand halves | `player_hud.{h,cpp}` | The hands are two copies of one model: the right half (`m_model`, left arm hidden) and the left half (`m_model_2`, right arm hidden), each with its own matrix, seat and cycles. A one-hand scene owns one half while the weapon keeps the other, seated where its own section puts it (`attach_pos/attach_rot`, `play_blend`, `calc_transform`). |
| Scene first frame | `player_hud::scene_first_frame` | A scene is started from a script, and the script phase of a frame runs after the hud update of that frame. Without this the frame a scene starts on was drawn with the seat of the weapon just put away, the item matrix of the previous scene's last frame and bones nobody recalculated after the new cycle was laid on - a flash of the previous scene's end pose before the animation began. The seat, both hand halves and the item are brought up to date once, at the start. |
| Scenes | `player_hud::scene_play/scene_stop/scene_active/scene_motion_length` | A hand cycle from any hud section with an optional item model in the hand (`item_visual`, `item_position/orientation/scale`, `item_attached`, `item_root_lock`, `lh_lead_gun`) and no `CHudItem` behind it. The cycle can be stretched to the scene length. A scene also ends by time, so a script that forgets to stop it cannot leave the item in the hands. Sections whose cycle is missing from the hands model are logged and skipped, never asserted. |
| Extra motion sets | `[da_hud_animations]` in `dead_air_x64_animations.ltx`, `SkeletonAnimated.cpp` | `model stem = omf path or dir\*.omf`: appended to the model's own motion sets when the hands load. Every Dead Air and DAR2 hands model is listed (43); the 3D PDA rig is not, its bones differ. |
| Script camera | `da_script_cam.{h,cpp}`, `CActor::script_cam_*` | An effector that owns the camera outright (position and HPB angles, frame-time-aware smoothing, `AbsolutePositioning`). Removed by type; the removal callback clears the actor's handle. |
| Input gate | `Level_input.cpp`, `game.only_allow_movekeys` | While a scene runs, only movement, look, pause, console, screenshot, quit and quick save/load presses pass. Releases always pass. |
| Ladder gate | `xrPhysics/ElevatorState.cpp`, `game.set_actor_allow_ladder` | A climb takes the actor off a ladder and keeps it off. |
| Hooks | `_G.da_before_item_use(npc, item)` (CInventory::Eat), `_G.da_before_inventory()` (UIGameSP kINVENTORY), `_G.da_before_body_search(obj)` (ActorInput), `_G.da_before_wear(obj, slot)` (CUIActorMenu::ToSlot, outfit/helmet/backpack slots) | Each is asked BEFORE the stock action; a false answer cancels it and the script finishes the action itself when the scene is over. |
| Lua exports | `game.play_hud_motion(hand, section, anm, mix, speed[, target_ms])`, `game.get_motion_length`, `game.stop_hud_motion`, `game.world2ui`, `game.only_movekeys_allowed`, `level.set_cam_custom_position_direction(pos, hpb[, smoothing[, hud[, hud_affect]]])`, `level.remove_cam_custom_position_direction`, `obj:iterate_belt`, `obj:get_actor_movement_state`, `obj:move_to_ruck`, `obj:move_to_slot`, `obj:unblock_all_slots`, `obj:change_power`, `obj:change_satiety`, `obj:get_additional_max_weight` / `get_additional_max_walk_weight` (outfits, backpacks and artefacts - Dead Air keeps its kits, class SCRPTART, in the backpack slot), `obj:set_actor_position(pos, skip_collision[, keep_speed])`, `CArtefact:AdditionalInventoryWeight` | The names the community's modded engines use, so Anomaly-born scripts port without renaming. |
| Config overlay | `x_ray.cpp`, `dead_air_x64_animations.ltx` | Merged on top of `system.ltx` like the 3D PDA sections; `#include`s the `items\items\items_anm_*.ltx` hud sections. No DA file is overridden. |

Console: `hud_scene_dbg 1` traces every hand cycle and scene; `hud_scene_item_pos/rot/scale`
tune the scene item seat in game and `hud_scene_item_dump` prints the config lines;
`hud_scene_seat_in/out` are the seat slide speeds of a one-hand scene.

## The script side

`packaging/dead-air-x64/compatibility/gamedata/scripts/`:

| Script | Role |
| --- | --- |
| `dead_air_x64_animations` | The glue, loaded from `script.ltx`: the `_G.da_before_item_use` registry, the `actor_effects.use_item` patch (stock effects muted only for what the module covers), the `knife_manager.can_loot` empty-hands guard. |
| `da_mod_compat` | The Anomaly helpers the addons capture at load (`normalize`, `clamp`, `IsMoveState`, `move_state`, `nextTick`, `GetEvent/SetEvent`, `SYS_GetParam`, `ui_options`, ...). Only names that do not exist yet. |
| `enhanced_animations`, `take_item_anim`, `ciga_effects`, `fov_anim_manager`, `ea_callbacks`, `ea_prefetcher` | FDDA. No helper inventory item, no version window, no key toggle. |
| `da_item_anims` | Answers "new" to the addons that ask which mode is active. The module is not optional: no option row, no key in `axr_options.ltx` or `user.ltx`. |
| `da_body_search`, `da_backpack_anim`, `da_wear_anims` | The three hook-driven scenes. A two-hand scene starts only when the weapon is away: the active slot has emptied (the holster cycle has ended) and the holster length has passed, the holster length plus a margin being the cap. Frames are not a clock here: a time event, once due, is called every frame. |
| `demonized_ledge_grabbing` (+ `_animation_data`, `demonized_geometry_ray`, `demonized_randomizing_functions`) | Parkour. Rays only while the jump key is held and at most every 40 ms; a climb needs the key held 100 ms and the actor within 0.75 m of the wall. `probe_now()` runs one pass regardless of the key; `debug_log = true` explains a miss in the log. |

`configs/items/items/`: the FDDA lists (`anims_list.ltx`, `ea_addon_*.ltx`, `anims_skip.ltx`,
`anims_loot_list.ltx`, `anims_cigga_smoke.ltx`, the prefetch lists) and the hud sections
(`items_anm_*.ltx`). `animations_settings.ltx` carries `blood_particles` (the skinning
spray) and `cig_smoke_particle` (empty: the FDDA smoke effect is not in Dead Air's
`particles.xr`, and an unknown particle name is fatal).

## What was left out on purpose

- `items_anm_dummy`: FDDA spawned a helper item into the inventory during a scene. A section
  the original game does not know would land in a save taken mid-scene.
- FDDA's Anomaly version window, the MCM pages, `script_light`, `wallmarks_manager`.
- Upstream's press-to-climb and double-tap input methods, the BHS and helmet checks, the
  debug gizmos.

## Testing

Probes run on the hidden-desktop rig (`tools/qa`, see [`TEST_MATRIX.md`](TEST_MATRIX.md)):

- Sections and cycle lengths: `game.get_motion_length` of every scene section is non-zero
  with the bundles mounted and no loose assets.
- Item scene: spawn a `bread` into the inventory, `db.actor:eat(obj)`; the hook cancels the
  use, the weapon hides, the scene plays 7 s, the item is consumed mid-scene, the weapon
  comes back and the input gate lifts.
- Backpack: `_G.da_before_inventory()` returns false, the bag scene plays and the menu opens
  by itself at its end. Wear: `_G.da_before_wear(outfit, 7)` returns true, the outfit is in
  the slot and the equip scene plays on the new hands.
- Parkour: the probe samples a height field around the rookie village with down rays, puts
  the actor 0.45 m from a 1.8-2.4 m step looking 30 degrees up, and calls `probe_now()` then
  `tryToClimb(true)`. The gaze matters: the detection follows the view ray, so a level or
  downward look only finds steps up to eye height, and a steep look shortens the reach.
- The 3D PDA still raises and lowers on the two-half hands.
