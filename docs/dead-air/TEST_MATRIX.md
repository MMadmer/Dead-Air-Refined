# Dead Air x64 validation matrix

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Clean x64 build | `Release`, clean intermediate tree | Pass, zero build errors |
| PE architecture | 43 packaged EXE/DLL files: 40 runtime files and 3 update/maintenance programs | Pass, all AMD64 |
| XDB discovery | Existing `database/*.xdb*`; discovery is conditional for names matching the content bundle shape | Pass, stock and third-party archives mount unchanged, and a bundle-shaped name mounts only when the installed content manifest declares that exact name at that exact size |
| Loose override discovery | Existing `gamedata` | Pass |
| Clean Dead Air new game | Base archives, no DAR2 archives | Pass |
| Clean Dead Air x64 save/reload | `x64_da_newgame` | Pass |
| Existing DAR2 x86 save | Latest `admin - quicksave9` | Pass |
| Original 0.98b pair without `.scov` | `legacy098.scop/.scoc`, exact x86-reference bytes | Pass, 27,634 objects loaded; new Refined save committed and loaded in a separate process; original hashes unchanged and no sidecar added |
| Refined save in original 0.98b | Isolated original x86 runtime and Refined-generated `serializer_ab_01` | Pass for load, 22,958 spawn points / 27,625 objects; the original renderer stalled after load before it could perform the requested resave |
| Lua save API compatibility | Ten legacy `before_save` mutations plus synchronous `capture_encode == false` | Pass, exact `.scoc` results, failed capture preserved the previous trio, zero transaction residue |
| XMS zero-module identity | XMS enabled, `mods/` with no valid manifest, new game | Pass, 22,958 spawn points (baseline) and not a single XMS log line — composition is a true no-op |
| XMS module composition | `xms.sample` enabled, new game | Pass, 22,959 spawn points (+1 from `demo.xspawn`), game graph 5,768 vertices (5,528 base), module level `xms_proba` joined as id 128 |
| XMS visual overlays | `overlay_visuals.ltx` with a static, a hierarchy, a mode-gated and a missing entry on `fake_start` | Pass, 2 attached, mode-gated entry skipped, missing file warned; graceful shutdown detached them without an assert |
| Malformed extension quickload | Valid active `l01_escape` world plus structurally invalid target `.scov` | Pass, load rejected before broadcast; level and actor ID remained unchanged |
| Save transaction fault models | 33 deterministic I/O/fallback cases and 10 durable crash checkpoints | Pass, production ordering verifier matched the implementation |
| NQ quest runtime in game | `tools\qa\nq\Run-NqQa.ps1 -Scenario All`, three reference quests plus two broken assets as a module in an isolated QA root | Pass, 9/9 scenarios, 83 checks, zero failures; the player's `savedgames` manifest is unchanged. `Dialog` drives the real talk window on the hidden desktop through the reply accelerator keys, so the engine walks its own phrase graph (`0>wolf_1>ask_more>wolf_rich>back>wolf_1>accept`) |
| Packed addon | DA Inventory Sort XDB | Pass |
| Loose addon | DAR2 Oxygen HUD scripts/UI | Pass |
| Lua binding parity | Original x86 exports vs x64 exports | Pass, zero missing |
| First-person self-shadow | Maximum preset, `renderer_r4` and `renderer_r3`, an outdoor save with an item in hand | Pass, `$user$smap_hud` created and `hud_shadow.ps` compiled on both renderers; the shadow term follows the model (lit top of the head, shadowed underside), the darkening lands only on HUD pixels and the background is untouched; `_preset Extreme` reports `r__hud_shadow 1` and `_preset High` reports `0` |
| Player world shadow | `r__actor_shadow` on, first person, a save with an item in hand, sun or a lamp overhead | Pass only when the caster is the full third-person model - head, arms and the held item all present in the silhouette - the shadow follows the player's stance with no detached item hanging in the air, the caster is gone from memory while the option is off, and the player's own torch is not shadowed by the body carrying it |
| Stale server-object handle | A save with squads, release one from a script and keep the handle | Pass only when every guarded accessor answers a neutral value - npc_count 0, commander_id 65535, squad_members empty, mutators ignored - logs the refusal with the script stack, and the session keeps running |
| Screen-space sun shafts | R4 renderer | Pass, all shader stages compiled |
| FXAA | R4 renderer | Pass, both shader stages compiled |
| Visor drops and reflections | R4 renderer, `r2_lenswater` and `r2_lensdirt` enabled | Pass only when `combine_2_naa` compiles without fallback shaders and the world remains visible |
| PDA script tabs | Widescreen PDA, relations tab and one native tab | Pass only when text placement remains stable, tabs accept input, and no Lua/UI lifecycle error is logged |
| Debug script tabs | Debug mode, spawner plus every available non-spawner tab | Pass only when each tab is created, drawn, and switched without Lua/UI lifecycle errors |
| Outfit and helmet night vision | Equipped NV-capable outfit and helmet, R4 renderer | Pass only when both activation paths start and stop the configured post-process effector |
| Wind service replay | `wind_seed N` twice on the same save, `wind_dbg 1` | Pass only when both logs print the same `[wind]` lines tick for tick, and `wind_force 1.0` pins the base at 1.00 within one tick |
| Tree crown oscillator | Outdoor save, `wind_dbg 1`, a gust event | Pass only when the watched tree's `[wind-tree]` line shows q following the target with one small overshoot and settling (no ring-down over cycles, never pinned at the clamp), `top` reads as the tree's real height (10-20 m for a tree, 2-4 for a bush), and the shadow of the crown moves with it on every preset but Minimum |
| Cloud deck and its shadow | Midday save, `r__clouds_quality 1` then `3`, `r__clouds_cover 0.25/0.55/0.85` | Pass only when the deck reads as masses with edges (not a haze), the ground shows the shadows of the clouds overhead, the shafts break where the deck is dense, no square edge of the cloud map shows in the sky, and tier 3 costs under 3 ms at 1440p on the rig |
| Lightning in the deck | Midday save, `fov 110`, `r__clouds_quality 3`, `level.set_weather("af3_bright_thunder", true)`, `r__screenshot_every 45`, 20 s; the log's `[lightning] bolt:` lines give azimuth/elevation | Pass only when a flash brightens the clouds and the dome around the bolt (the log's az/el) with the far deck and the far dome staying dark, strikes come from different azimuths across the run, sheet lightning (`hidden=1`) lives 0.6-1.8 s with visible flicker, and no flash lights the whole sky at once |
| Volumetric deck | Midday save, `fov 110`, `r__clouds_quality 3`, `r__clouds_cover 0.25/0.55/0.85` | Pass only when clouds stand overhead as well as at the horizon (no dome cap, no square edge), read as lit volumes with dark bases and bright rims, the ground carries their shadow, and the march stays under 4 ms at 1440p on the rig |
| Trunk and crown as one | Outdoor save, `wind_force 1.0`, a tall pine; then `wind_force 0.0` | Pass only when the trunk leans with the crown (no card sliding off it, no crown swinging at a different rate than its trunk), tips travel more than the trunk, the top of a tall tree reaches a clear storm lean (more than a few degrees), the shadow shows the same lean, a shot through a bush shakes the branches at the trace, and at `wind_force 0.0` the trees are still |
| Console under time_factor | `time_factor 20`, hold a key in the console | Pass only when the repeat rate is the same as at time_factor 1 |
| First-person shadow from a lamp | Indoor save, a lamp beside the player, `r__hud_shadow 1` | Pass only when the hands and the item shade themselves from the lamp's side and the lamp lights the weapon from the side it stands on |
| Rings on all water | Rain weather, `r__puddles_force 1`, `wind_dbg 1`; walk through puddles, drop a grenade in one, shoot a lake, blow a grenade beside it | Pass only when every footstep in a puddle or in a lake logs a `[water] ring` and shows a ring, a dropped body makes one ring sized by its fall, a bullet on a lake rings like on a puddle, a blast beside a lake splashes and rings but leaves the lake (no drain), and a `storm` with nothing happening logs no rings |
| Wind on particles, bodies and water | Smoke source, a dropped can, a lake in a `storm` weather | Pass only when the smoke leans downwind and returns when the wind drops, a resting can stays put in a breeze and rolls in a gale, and the lake shows streaks along the heading that a `clear` weather does not |
| Third-party script compatibility | Binding parity fixture plus a representative packed or loose addon | Pass only when the addon starts without missing export, signature, or Lua ownership errors |
| Level transition | Underground to Agroprom | Pass |
| Transition-save reload | Fresh underground and Agroprom saves | Pass |
| Long-session soak | Fresh Agroprom save, packaged runtime | Pass, 15.29 minutes, responsive, 3.04 GiB private memory |
| Dependency audit | 43-file release package | Pass, no missing local DLLs |
| GUI upgrade installer | Final `Dead Air: Refined 1.1.1` package over the installed 1.1.0 root | Pass, all 46 managed payload hashes matched and 75 saves remained unchanged |
| Main-menu AtmosFear apply | `af_options_dialog:OnBtnAccept()` with no active level | Pass, settings saved without calling the runtime weather manager; process remained responsive without Lua or access-violation errors |
| Installed EXE uninstaller | Upgrade mode | Pass, 13 original runtime files restored across a 43-file scope with zero mismatches |
| Maintenance wizard | Installer-style fixed-size layout and silent action paths | Pass, standard header artwork persisted and the removal path completed without leftovers |
| Patch-only distribution | Installer contents and target validation | Pass, no original Dead Air files included and an empty target is rejected |
| Raw update archive | Manifest schema, exact file set, per-file size and SHA-256 | Pass, 51 ZIP entries and 46 payload files including maintenance and uninstall components |
| Saved-game preservation | Fresh install without `appdata` plus repeat install with an existing save sentinel | Pass, `appdata\savedgames` created and sentinel SHA-256 unchanged |
| External updater | Isolated installed root, backup, apply, maintenance, cleanup | Pass, in-flight snapshot created, obsolete sentinel removed, cache deleted, uninstaller regenerated. The snapshot is a transaction rollback for the update that is running and is deleted once the apply succeeds, so nothing is retained afterwards |
| Empty release list | Isolated update response with no valid stable release | Pass, accepted without an update candidate or blocking the menu |
| Final package smoke | Latest x86 save, 22,958 spawn points / 27,198 objects | Pass |
| Manual diagnostic report | Loaded `async_benchmark_1`, hidden desktop | Pass, valid anonymous ZIP with system, hardware, runtime, content, log, configuration, and minidump data |
| Automatic crash report | Gated release QA access violation | Pass, exception code, module RVA, anonymous stack, and valid `MDMP` attachment captured |
| Crash-report startup prompt | Newest unhandled `dar-report-crash-*.zip` plus a mocked 1.0.5 update | Pass only when the native yes/no confirmation opens first, yes opens the `Отправка crash report` form, and that exact ZIP remains mandatory |
| Crash-report acknowledgement | Handled marker followed by a second menu launch | Pass, the same ZIP did not prompt again and the deferred 1.0.5 update dialog opened |
| Diagnostic privacy scan | All non-save ZIP entries, ASCII and UTF-16 | Pass, no user name, computer name, profile path, game path, e-mail address, or IP address |
| Diagnostic rotation | 13 pre-existing reports plus one new report | Pass, newest report retained and total reduced to 10 |
| Native bug-report layouts | 4:3 and widescreen XML, all child bounds checked against the panel | Pass, zero out-of-panel controls |
| Bug-report upload contract | Live Report Hub, multipart title/description plus valid anonymous diagnostic ZIP | Pass, HTTP 201, attachment accepted, QA reports deleted |

Version rollback has no rows here any more because it no longer exists. The
updater deletes its snapshot as soon as an apply succeeds, and the uninstaller
no longer offers a restore-a-Refined-version action. Restoring the original
32-bit game from `backup-x86` is unaffected and is still covered above.

## Content system

The mount-gate cases run through `tools\qa\Run-ContentProbe.ps1`, which appends
console commands to the QA rig's `user.ltx`, boots the shipped engine and reads
the log back. Nothing is stubbed: filesystem initialisation, the mount gate and
`ContentService::Initialize` all run on a real installation. Every case also
fails on a `stack trace` or `FATAL ERROR` line, and the harness fails if the rig
is left latched. Play refusal is always driven through
`start server(all/single/alife/new)` issued from `user.ltx`, which is the route
the project's own smoke recipe uses and the one a menu-button gate would miss.

The flow cases run the shipped `DeadAirUpdater.exe` in its three content modes
against `tools\qa\Start-ContentAssetMock.ps1` on loopback. The mock is a raw
socket server so it can drop a connection mid-body and ignore `Range` on demand.
`DAR_QA_CONTENT_BASE` redirects the downloader to it and is accepted only for a
`http://` loopback host with no credentials, so a manifest or an environment
cannot point a shipped build at an arbitrary server.

| Gate | Content/runtime | Result |
| --- | --- | --- |
| Mount gate, healthy installation | `Test-ContentGate.ps1`, QA rig, all five declared bundles correct | Pass, `5 bundle(s) present`, nothing skipped, no play refusal |
| Warm state cache | Second launch of the same installation | Pass, `5 bundle(s), 0 hashed` - every verdict came from `content-state.txt` on a matching name, size and mtime, and no bundle was read |
| Missing bundle | One bundle moved aside | Pass, reported missing, the latch is written and the level load is refused with `Cannot start a level` |
| Truncated bundle | 4 KiB removed from the tail, name unchanged | Pass, refused before the archive is opened as `size does not match the manifest` with the declared size named, play refused, no assert |
| Corrupt index at the correct size | Index chunk length overwritten, size unchanged | Pass, refused as `invalid index` before the header is parsed, play refused, and the archive reader never asserts |
| Flipped data byte | One byte inverted at 80 % of the file; size, index and header intact | Pass, mounts and survives the stat pass, and is caught only by the hash pass as `does not match the manifest` - which is why the hash pass exists |
| Stale and unrecognised leftovers | A copy under a wrong hash in a slot the manifest knows, plus a copy in a group it does not have | Pass, `stale bundle` and `unrecognised bundle-shaped archive` respectively; both are refused and reported as notices, and neither blocks play |
| Absent manifest | `content-manifest.txt` moved aside | Pass, `manifest unavailable`, `state=recovery`, play refused - not a silent boot with every bundle unmounted |
| Leftover latch on an intact installation | Hand-written `content-incomplete.txt` on a correct installation | Pass, startup reports the latch and keeps the gate shut, and the background hash pass clears it without a restart |
| Clean fetch | `Test-ContentFlow.ps1`, `--content-fetch` against the asset mock | Pass, every declared bundle lands in `.dead-air-x64\content-cache` under its own SHA-256 |
| Commit | `--content-commit` over that cache | Pass, every bundle is installed into `database\` and the latch is removed |
| Plan against a complete installation | `--content-plan` after the commit | Pass, `missing=0` - an installation that already matches its manifest fetches nothing |
| Resume through repeated drops | Mock closing the socket every quarter of the largest bundle | Pass, the transfer completes, and the mock's log shows `Range` requests from non-zero offsets, so it resumed rather than restarting from zero each time |
| Server that ignores `Range` | `-NoRange` mock answering 200 to a range request over an existing part file | Pass, the part is truncated and refetched from zero instead of having a second copy of the head appended |
| Tampered cache entry | One byte flipped in a finished cache file before the commit | Pass, the commit exits non-zero and nothing enters `database\` |
| Upgrade by delta | `Test-ContentFlow.ps1`, a v2 manifest whose `[deltas]` section carries one real `.darpatch` edge for the largest bundle, mock serving the v2 assets | Pass, the fetch pulls the patch and never the whole v2 bundle, and the locally rebuilt file survives the commit and lands in `database\` under its v2 name. Recorded as a failure when `bin\x64\Release\DarDelta.exe` is not built, because a case that cannot run is not a case that passed |
| Install-time fetch | Silent `Setup.exe` against the asset mock | Pass, five bundles fetched in `PrepareToInstall`, committed at `ssPostInstall`, latch cleared |
| Install with no asset server | The same silent Setup with the mock stopped | Pass, aborted with a message and the target directory left byte-for-byte untouched |
| Uninstall content sweep | Silent uninstall of a content-bearing installation | Pass, `database\` left holding exactly the archives that were there before the install |
| In-game repair | QA rig with a bundle deleted by hand, driven through `dar_content_repair` and through the dialog | Pass, detected, downloaded, installed and the latch cleared, after which the dialog offers a restart; the missing-bundle layout was confirmed on screen with both buttons and the problem list rendered |
| GitHub `Range` probe | A published release asset requested directly from `github.com` | Pass, 206 with a well-formed `Content-Range` on the redirect target, so resume works against the real host and not only the mock |
| Shard layout at full size | `Test-ContentScale.ps1`, 5120 MB of synthetic content spread Zipf-style over 60 directories, through the real builder in `-LayoutOnly` mode | Pass, 18 shards, largest 1094 MB, no group left undivided. It fails when a shard passes the 1536 MB ceiling, when one shard holds more than half of all content, or when a group above 500 MB is left in a single shard. It measures the placement policy only - no archive is packed, transferred or committed |
| Animation module served from bundles | `content-1.5.0` manifest, 18 bundles, no loose assets on the rig; `game.get_motion_length` of `item_ea_bread_hud`, `da_backpack_hud`, `item_anm_ledge_grabbing`, `da_body_search_hud`, `da_wear_outfit_real_hud`, `da_wear_helm_battle_hud` | Pass, 18 of 18 bundles verified in 4.2 s, every cycle length non-zero (5970 / 1768 / 951 / 1367 / 3935 / 2935 ms), `[hud-anim]` reports 43 per-model motion sets, and the 3D PDA raises and lowers on the two-half hands |
| Item scene through the use hook | `bread` spawned into the inventory, `db.actor:eat(obj)`, `hud_scene_dbg 1` | Pass, `_G.da_before_item_use` cancels the stock use, the weapon hides, `item_ea_bread_hud` plays stretched to the 7000 ms scene, the item is consumed mid-scene, the weapon returns and `game.only_movekeys_allowed()` is false again |
| Backpack and wear scenes through their hooks | `_G.da_before_inventory()`; `novice_outfit` spawned, `_G.da_before_wear(obj, 7)` then `move_to_slot` | Pass, the inventory hook answers false and the bag scene plays with the menu opening by itself at its end; the wear hook answers true, the outfit is in the slot and `da_wear_outfit_real_hud` plays on the new hands. With an AK-74 in the hands the bag scene starts 1348 ms after the hook: after the 1267 ms holster cycle and 25 ms after the slot emptied, never over the holster (a time event fires every frame once due, so the old frame-counted wait was a dozen frames). `cfg_save` after `hud_scene_dbg 1` writes no `hud_scene_*` line while the other debug switches are in the file. On the frame the scene starts the bag is already drawn in the hands at the first frame of the cycle (item matrix at the player, blend at 0.0000) instead of at the world origin, and the height rises smoothly over the frames that follow |
| Harvest particles | `particles_object` of every name `animations_settings.ltx` may carry | Pass for `hit_fx\hit_flesh_01`, `hit_fx\hit_flesh_02a`, `hit_fx\hit_knife_flesh_00`, `hit_fx\hit_flesh_headshot`, `damage_fx\smoke`; FDDA's `damage_fx\mod_cig_smoke` is fatal (absent from Dead Air) and is not referenced |
| Parkour (hold jump) | A real ledge 1.4-2.5 m above the feet, the player at the keys: jump into it with the jump key held; release before the contact for the negative case | Manual gate only - the rig cannot pick a ledge or hold a key for the player. Pass only when a held jump into the obstacle climbs it with `item_anm_ledge_grabbing` on the hands, the weapon back in the slot and the input gate lifted afterwards, a released key only bumps, and `demonized_ledge_grabbing.debug_log = true` explains a miss (`no point`, `point off screen`, `path blocked`). Rig-side facts established with `probe_now()`: the ray sweep follows the gaze (a level look finds steps up to eye height, a steep look shortens the reach), `device().cam_dir` is a reference and must be copied before an in-place change, and a bush top is refused only through the `material_name` the ray result now carries |
| Weapon faults from new | Rig, the foliage save's AK-74 (`condition_shot_dec` 0.0004, `condition_coeff` 3, interval 300), 520 rounds at 100 % with `wpn_fault_dbg 1`, ammo topped up by the probe | Pass, no fault before the drawn threshold, the first dirty fault exactly at it (344 rounds at a target of 343.4; 330/329.7 and 326/325.3 on other runs), the second 157 rounds later (128..173 expected), only dirty ids set, wear progress 0 while the condition is above 0.75, the fouling ratio 0.75 at three quarters of the interval, cleaning through `set_weapon_condition_type` resets the fouling to 0 |
| Weapon breakage after a deformation | Same AK at 0.4 with a deformed hammer (id 9) forced through Lua, fired until a break, reloading on every jam | Pass, the break is the broken hammer (id 10) after 758 rounds (stock expectation 605..692 with the jitter, band 478..830), stress advanced one per shot per breakable part, the toolkit path (deformed bit cleared) zeroes stress and wear, the mechanic path (mask 0) zeroes every accumulator |
| Weapon fouling across an offline trip and a save | 40 rounds, the AK dropped, the actor 300 m away and back (ALife switch 250 m), the weapon picked up, `save wfqa_park`, then the save loaded in a fresh process | Pass, `[wfault] park` on the way out and `[wfault] restore foul=40` on the way back, the getter reads 40 after the trip, the `.scov` carries one `WFL1` record (id 29193, section CRC 0xd1c941dd, fouling 40) and the new process restores 40 from it |
| Weapon mask in the original streams | AK mask set to dirty firing pin + chamber-cycle bit (0x02040000), saved and reloaded in a fresh process | Pass, the spawn trace shows the `.scop` mask as 0x00040000 (bits 25/26 absent from both the spawn and the update record), the live mask after `net_Spawn` is 0x02040000 again from `WEX1`, and a weapon with a dirty bit and no fouling record starts at cleaning stage 1 |
| Colour grade at midday | Rig, foliage save, `af3_bright_clear` at 12:30, one frame before and after the grade, same binaries otherwise | Pass, the frame renders with `da_grade_params` bound (no shader error, no stub), green crowns and bushes read olive instead of pure green, the share of green-dominant pixels falls from 1.3 % to 1.0 % while mean luminance stays within 0.01 (0.335 -> 0.324), `r__grade_*`, `r__foliage_vibrance` and `r__lod_sat` are re-applied by the preset sync on start over a `user.ltx` that still carries 1.6 / 2.0 |
| Save loaded without one of its modules | Rig, `modules/test` (XMS dev module that had placed a box and a barrel with its own visuals) removed, the user's `quicksave6` and the QA `foliage` save loaded | Pass, `! XMS: module [test 1.0.0] is gone - released 5 object(s) it had spawned into this save` on both, level reached, no `Can't find model file`; before the release pass the same loads died on `equipments\item_box_01_visual.ogf` and then `test_barrel.ogf` |
| Self-lit detail shader of the animation module | Rig, the animation prefetch of the cigar and cigarette hud items | Pass, `models\selflight_det2` resolves from `shaders/r3/models_selflight_det2.s`; the six `! Shader ... not found in library` lines are gone |
| Sun through clouds | Code path: `cloud_sun_visibility` (cloud map phase) now scales the sun disc and the screen glare in `dxLensFlareRender`, the sun shafts constant and the shafts pass gate | Pass for build and load; the flares already followed it, so the disc, the glare and the god rays fade with the deck they are seen through - visual sign-off at the keys |
| Neutral colour base | Engine defaults `r__color_base_r/g/b` 0.5/0.5/0.5 (were 0.42/0.50/0.65), live and rig `user.ltx` set to the same | Pass, midday frame renders without the cold cast, `Options -> Video -> Colour correction` shows the sliders centred |
| Trees stand on their roots | Rig, the user's `trees` save at the rookie village, six frames around, the three tree vertex shaders with the height clamped at the root | Pass, every trunk base sits on the ground and no bark geometry floats beside a trunk; before the clamp a vertex below the model pivot (roots and butt, spruces above all) got a negative height and the length-keeping drop lifted it by twice its depth, even in still air, which read as a levitating tree with its root flare mirrored upward |
| HD texture colour audit, moss on the boulders | `tools/qa/texture-audit/texture_colour_stats.py` over the 2035 HD textures, the 7811 stock and 2167 DAR2 textures unpacked with converter.exe; per texture: saturation mean/p90, vivid share, green share and green saturation, colourfulness; HD compared with its original by name | Pass: the one scene texture with saturated green the original never had is `grnd/grnd_rocks_02` (Cordon boulders, moss 19 % of the texture at saturation 0.61 against a grey stock rock); recoloured to saturation 0.44 and 18 % darker on the moss only, re-encoded DXT1 with the full 12-level mip chain (texconv, same size), bundle textures_08 rebuilt and deployed to live and rig, rig smoke on the Cordon save clean. The tree foliage (0.54-0.63) keeps the approved look; the HD set reads +0.12..+0.30 saturation over the stock on ~80 surfaces (wood, rust, brick) - plausible materials, left alone |
| Lua GC step on the main thread | Report 1.3.5 "crash while viewing the inventory": access violation in `~CInifile` fired by a LuaJIT finalizer inside `CLevel::script_gc` on a task worker, concurrent with rendering | Pass: `script_gc()` runs on the main thread whatever `mt_script_gc` says (the flag still parses); no finalizer of a Lua-owned object (ini_file, sound, particles, script UI windows) runs off the main thread any more; the frame cost is the GC step itself |
| Merge target under the cursor | Report 1.3.5 "items merge from the fourth or fifth try": drop one stack onto another in the bag, by the pointer | Pass: the target cell is picked under the cursor, the dragged icon's top-left corner only as a fallback over an empty cell; a drop on the item's own cell is still ignored by the scripts (same id) |
| Impostors fade instead of popping | Rig, `trees` save, look across the valley at High and Extreme; then `r__veg_discard 8` for a visible threshold | Pass: a tree impostor thins out through its alpha test over the last 4x span of its size measure before the discard and over the last 12 % before the far plane, never vanishing in one frame; `r__veg_discard 0.5` (default) leaves the near look untouched |
| Detail objects keep a twelve-metre fade band | Rig, walk backwards from a bush at High (80 m radius, `r__grass_fade_start 0.95`) and at Extreme (100 m) | Pass: the bush shrinks over at least twelve metres of travel, not the four or five the 0.95 share left, so the 15-30 frame slot cadence no longer shows it going in three steps; Default (60 m, 0.7) is unchanged |
| Puddles on every preset, reflections by tier | Rain weather, `r__puddles_force 1`, presets Minimum through Extreme | Pass: Minimum and Low draw the puddle mask (15/20 m) with no reflection pass; Default adds the sky reflection (30 m, no depth march, no scene grab); High and Extreme ray-march the world (45/60 m). `r__puddles_refl` reads 0/1/2 by tier and the scene grab runs only at 2 |
| Puddles under a bright sky are water, not snow | Rig, clear and rain weather at noon, Default preset, `r__puddles_force 1`, camera at the feet; variants `r2_sun off`, `r__puddles_refl 0`, `r__puddles_gbuf 0/1`, `r__puddles_gloss 0.2` | Pass: the white sheet followed the flat water normal in the G-buffer (`r__puddles_gbuf 1`) and nothing else - one sun highlight over the whole plane, at any gloss, with the reflection pass off too. Default is the ground normal (0): wet soil darkens (0.65), rim and half gloss stay, the reflection follows Schlick (F0 0.03, exponent 5, power 1.0) and the sun is a tight glint (pow 900) in the reflection pass; the former 0.10 floor at power 1.5 and the flat normal are both gone from the defaults |
| Puddle knobs are session overrides | `r__puddles_dark 1` then `cfg_save`, restart | Pass: user.ltx carries no `r__puddles_*` line and the look comes back at 0.65; an older user.ltx line parses and is overridden by the preset sync |
| Wet ground radius rides the preset | Rain, presets Default through Extreme, stand under a roof edge and look 30-50 m out | Pass: wetness fades at 25/35/50 m and the rain occlusion map covers the same radius at 512/512/1024, so ground under a roof stays dry to the fade edge (the old 20 m cap left the outer ring wet under roofs) |
| Wet foliage darkens, it does not glow | Rig, `af3_dark_rain` at 09:40 (Cordon bush and spruce spot), Extreme; level view at a bush against the sky, pitched down at the grass, the far spruces | Pass: no white outline along bush and crown silhouettes and no salt of glints over grass and leaves; the foliage is darker than dry; a wet road, a concrete block and a trunk keep their sheen and their splash rings (the continuity gate is a depth Laplacian, a plane passes at any grazing angle) |
| Rain splashes by the boot | Rain, camera at the feet, `r__rain_splash 1` | Pass: no light blob appears by the boot; a splash within 0.6 m of the eye is not drawn and one grows to full size only three metres out (the 18 x 22 cm model used to be born at full size under the player) |
| Fall damage is the impact, not the slide | Drop from the same height three times: free, pressed against a wall with forward held, onto a steep slope | Pass: the wall drop costs the same health as the free one or less (the wall brakes the slide), the slope drop costs less than the free one (normal component); the health loss no longer ticks while sliding along a wall |
| Landing roll takes the hit | Drop from a height that costs health, jump held on touchdown; repeat without the key; repeat from a lethal height with the key | Pass: with the key the view turns once forward over 1.2 s, the body drops into a crouch through the usual lerp and moves ahead about two metres at 2.6 to 1.0 m/s whatever the stamina, health or arrival speed, then stands back up, pitch is locked and yaw runs at a quarter sensitivity, the weapon and detector leave the hands at once and return two updates after the roll, the health lost is the stock loss at 0.57 of the landing speed (none below 21 m/s for the stock 12); without the key the stock damage; from a lethal height no roll and the stock death |
| Landing roll blocks the rest | During the roll press movement, weapon, inventory and use keys, scroll the wheel, then Esc | Pass: nothing but yaw moves until the roll ends, Esc opens the menu, console and screenshots work; keys still held when the roll ends take effect at once |
| Climb needs the stamina it will spend | Drain the power bar, hold jump at a ledge; refill, try again | Pass: with less power than the climb drains (four thirds of `getStaminaDrain()`, weight-dependent) the climb does not start and the log says why under `debug_log`; with enough it starts and the bar never runs dry mid-climb |
| Landing roll sound | Any drop that triggers the roll | Pass: `actor\fall_roll` plays once, 2D, as the roll begins; the log shows no `Invalid ogg-comment version` for it and no missing-sound line |
| Landing roll from the console | Rig, `time_factor 0.2`, `r__screenshot_every 1`, `fall_roll_test` | Pass: the frames show the view going down, over and back up in one forward turn with a small overshoot before it settles, the body ends crouched and moved ahead, and the log reads `- fall roll: sound 'actor/fall_roll' handle ok, 179548 bytes, 1.02 s, feedback yes` (a `none` handle or `feedback no` is the failure) |
| Weight bonus of a kit in the backpack slot | Console open, hold jump at a ledge with `kit_hunt` (class SCRPTART) in the backpack slot, with and without enough stamina | Pass: no `cannot access class member GetAdditionalMaxWeight` line - `get_additional_max_weight` returns the artefact's `additional_inventory_weight` - and a stamina refusal is re-tried once a second, not every frame |
| Wind motors do not tear tree crowns | Rookie village, stalkers walking under the poplars, a shot fired through a crown, a grenade beside a tree | Pass: no crown card shoots out into a spike while someone walks past the trunk or a bullet passes through the leaves - on a tree a press reaches only the flexible foliage within 3.2 m of the presser's feet and a wake only the foliage under six metres, both with a bounded travel; the grenade blast still bends the whole tree from its root and it springs back |
| Trunks do not bend against the actor | Cordon, walk into a broadleaf trunk and a spruce trunk and look up; stand pressed against them | Pass: the trunk and its thick branches keep their shape (no lens-like warp at shoulder height); only thin foliage within reach moves, by half a metre at most |
| Bushes answer steps and shots as one plant | Cordon, walk into a 2-3 m bush; fire two shots through it from four metres | Pass: the whole bush leans away from the body and springs back; each shot through the canopy shakes the whole bush for about half a second; no single card tears away from the rest |
| Bullets ignore bushes but the hit plays | Cordon, shoot a stalker through a bush; shoot the bush | Pass: the damage is the same as with no bush in between (`materials\bush` shoot factor 0, density 0); the bush hit still gives its sound and leaf particles |
| Dry conifer branches read as Dead Air conifers | Cordon, a spruce that mixes live and dry branch cards (`trees_atp_spruce_branch` + `_dry`), a pine with dry cards, noon | Pass: the dry cards are dark grey-green like the live ones and like the stock Dead Air textures, not yellow-brown - no half-yellow spruce; the HD needle detail and the alpha cutout are unchanged |
| First-person body hidden during the ledge climb | Cordon, climb a fence or a truck with the jump hold; look down at the start and end of the climb | Pass: no part of the legs mesh appears in the frame at any point of the climb; the body is back the frame the script hands the camera back; the world shadow keeps going throughout |

## Content coverage limits

Everything above ran against the QA fixture: five bundles totalling about 19 MB,
served from loopback. Nothing has been exercised at the size this system exists
for. `Test-ContentScale.ps1` rehearses the shard layout policy at that size and
nothing else, so the cold hash duration over several gigabytes, the growth of
the filesystem index and of `_initialize` with tens of thousands of content
files, commit duration and peak disk during a full fetch are all projections.
The 250 MB shard target and the 1536 MB ceiling in `groups.ltx` have never been
produced by a real build.

No content release exists yet. The only traffic to the real host is the `Range`
probe above; no bundle has been fetched from a published release asset, and the
pinned assets repository has never served a fetch. Update-time content is
equally unproven: the only content step on the update path is the
`--content-commit` appended to the updater's command line, which commits a cache
something else filled, and no run has taken an installation across two content
sets.

Binary deltas have shipped. `src/xrContentSync/ContentDelta.{h,cpp}` applies
them, `src/utils/DarDelta` builds them, `dead_air_x64_content_bundles.ps1` emits
the `[deltas]` section whenever it is given `-PreviousManifest`, and the manifest
parser validates the cumulative edges. One end-to-end case exercises the healthy
path: `Test-ContentFlow.ps1` overwrites 4 KiB in the middle of the largest
bundle, builds a real `.darpatch` with `DarDelta.exe`, writes a v2 manifest
carrying that single edge with a recomputed `content-id`, installs v1 for real,
then repoints the mock at the v2 assets and fetches. It asserts that the fetch
succeeds, that the mock's log shows the `.darpatch` requested, that it never
shows the whole v2 bundle requested, and that the locally rebuilt file passes the
commit and lands in `database\`. That is one hop on a healthy disk and nothing
else. When `bin\x64\Release\DarDelta.exe` is not built the case is recorded
as a failure rather than skipped, so a run that dropped the only delta
coverage cannot end by reporting that every case passed.

The failure modes are all still uncovered, and they are a pre-release blocker,
because the apply path is the only place in the system where the client
manufactures bytes it never downloaded. A corrupt patch has to be shown falling
back to the whole bundle for every malformed shape `ContentDelta::Apply`
distinguishes - bad magic or version, a body that ends early, a COPY reaching
outside the base, an INSERT larger than the target can hold, an unknown opcode,
and ops that all complete but produce the wrong length - every one of which is an
`Integrity` verdict. A patch applied against the wrong base is deliberately not
one of them: the base is re-hashed immediately before the apply rather than
trusted from the plan, and a mismatch is `Transient`, so it has to fall back
silently and must not be remembered. A cancel mid-apply is untested in both
halves: there is no cancel check inside the COPY and INSERT byte loops, only
between ops, so a single op over a large base sets the worst-case latency, and
the partial output has to be gone afterwards. The blacklist round trip has never
been walked: `rejected-deltas.txt` takes an entry only for an `Integrity`
verdict, while a disk that filled mid-apply, a cancel, a wrong base, and an
output that matches the `.darpatch` header's target hash but not the manifest's
are all retried instead, because a permanent blacklist turns a 20 MB delta into a
400 MB download for good - and neither the resolver dropping a blacklisted edge
nor `ContentCommit` clearing the file when the content-id changes has been
exercised. And no multi-hop chain has ever been built: each intermediate is freed
only once the next hop's apply returns, so peak cache use is two bundles plus the
hop's delta asset rather than the whole path, and a chain that does not end at
the bundle the manifest wants must be refused rather than committed.

Also untested: a disk that fills during a fetch or a commit; the installer's
free-space refusal on the directory page; a bundle that cannot be deleted during
uninstall, which should be named in a message rather than silently skipped; and
repair on an installation where a mod disabled automatic updates, which nothing
in `ContentService` consults but which has never been run. The diagnostic report
gained `content_id`, `content_manifest`, `content_incomplete` and
`skipped_bundles`; no report has been captured since, so the privacy scan row
above does not cover them.

## Runtime acceptance rules

A runtime test passes only when the intended game state is reached, the process
remains responsive after loading, and the log contains no new fatal error,
assertion, reader overflow, or Lua compatibility failure. A content bug already
present in the original x86 runtime is recorded separately and is not treated
as an x64 regression.

Every runtime gate in this document now presupposes a complete content set.
`CLevel::net_start1` refuses the load through `ContentService::PlayBlocked()`,
which switches on the service state and never reads the latch file: only
`Complete` and `Verifying` are let through, and `Verifying` deliberately so,
because the cheap pass has already confirmed every bundle is present and the
right size and a hash pass in flight is no reason to hold the player at the
menu. Everything else refuses - `Incomplete` and `Recovery` obviously, `Unknown`
because the service has not looked yet, `Repairing` because it is still
downloading, and `Repaired` too: the bundles are back on disk but the filesystem
was indexed without them, so a level loaded before the restart would still be
missing every asset they carry. A run that never reaches a level because content
is missing is therefore a content failure, not a runtime regression, and is
recorded as one.
