# Human NPC AI

Active specification of the human NPC AI of Dead Air: Refined (engine Grizzly). Mutants are untouched: every
change is gated on the human stalker path, and monster code runs bit for bit as before. Zombified stalkers are
stalkers: they perceive and remember by the new rules, with a "dumb" profile and no speech.

Status: implemented and built; the final test stage has not passed yet. Every fairness, communication, save and
stability check passes. The think-time budget is met on average (0.28-0.39 ms per frame in the faction fights), but
single NPC updates exceed it in most 5 s periods (0.9-1.3 ms at worst), and in a hot fight so do the knowledge and
senses parts. The mercenaries' coordination, the cover share over layouts and the search pair's spacing are open, and
the muzzle flash's last fix (the line to the shooter's head when his muzzle is hidden) is built but not measured (see
"Measured results" and "Known limits").

## What changed in play

- **Nobody knows where you are for free.** A stalker acts on you only through its own eyes and ears, a hit, a bullet
  cracking past, a muzzle flash or a lamp - or because somebody told it. Everything except its own sight is a rough
  estimate that grows staler every second. There is no shared squad memory and no faction-wide alarm.
- **Being noticed takes time.** Awareness builds up with distance, light, your stance, foliage in between, the edge of
  the NPC's field of view and its rank; a calm NPC is 2.5 times slower than an alert one. Stalkers see as far as the
  weather's fog allows, whatever the player's own view distance is.
- **They listen.** Footsteps by gait, ground and carried weight, landings, reloads, torch clicks, eating and medkits,
  gunshots (a suppressor is quieter and much harder to locate), voices. Walls muffle, rain drowns quiet sounds,
  gunfire masks footsteps.
- **Light gives you away.** Muzzle flashes are seen from 80 m by day and 300 m at night (never with a suppressor); a
  lamp pointed at an NPC betrays its carrier from far away at night, and even its lit spot does.
- **They talk, and you can hear it.** Shouts carry as far as the sound does, hushed words only up close, radios only
  between carriers of the same squad (with a squelch you hear within 8 m of a carrier), hand signals only between men
  who see each other, PDA texts slowly. News takes at least 250 ms and usually 1-5 s, and describes a spot, not you:
  "over there, by the trucks". Kill the radio carrier and the squad is off the net. Factions with an HQ (military,
  Duty, Clear Sky, ecologists) relay contacts to their other squads on the level after 30-120 s.
- **No shooting through walls.** An NPC fires only at what it sees. Trained ones may suppress the spot where they last
  saw you for a few seconds; they never aim at where you really are.
- **The weapon decides the fight.** Shotgun and pistol holders do not cross open ground to reach you: they hold cover
  and wait for you to come close, and after they lost you they search by covered bounds or watch from cover. Snipers
  keep their distance and draw a sidearm up close. Everyone closes the distance cover to cover.
- **Squads fight as squads.** Trained leaders order a suppressor (who pins your last seen spot from cover) and a
  flanker, but only when they see a covered route for him; the flanker leaves at once. With no such route they flush
  you with a grenade, advance under the suppression, or hold. They spring ambushes on command, throw grenades to flush
  you from cover (only at a spot they saw or heard you in), fall back when a position is lost. Drilled soldiers you
  catch at close range shoot back for a few seconds before they move, in the open or from the cover they have.
- **They want to live.** Losses, a dead leader, wounds, being clearly outmatched and near misses frighten them. Bandits
  and rookies scatter, rookies and scientists surrender, disciplined factions withdraw in order, the Monolith never
  breaks.
- **They search.** After losing you, a squad searches from where it last knew you were; military, Duty and
  mercenaries sweep in pairs and quietly, and a squad gives up one man at a time.
- **Hostility spreads by knowledge.** Attacking someone makes enemies of those who saw it or were told, not of every
  member of the faction within 100 m.

## Fairness rules

Every part of the AI is bound by these rules; a failed fairness check is fixed at its cause, never loosened.

| Id | Rule |
|---|---|
| F1 | Perception only: an NPC acts on a target only through its own senses (vision, hearing, hit, near miss, light, muzzle flash) or a physical message. |
| F2 | Per-NPC knowledge: every stalker keeps its own beliefs; no shared memory pool is read for a stalker's decision. |
| F3 | Estimates, not truth: every non-visual observation and every message carries a position estimate with an error; the real position is read only while the target is in the NPC's own vision this update. |
| F4 | No aim without sight: aim point and lead come from the belief when the target is not visible. |
| F5 | No fire without line of sight, except honest suppression: a profile with `supp_window_s > 0` may fire bursts at its own last-seen point (never refreshed from truth, never at a reported-only spot) within that window, with an automatic weapon or a rifle, counted as suppression. |
| F6 | Reaction time: identification takes an awareness meter; the first aimed shot waits `react_ms`; aim settles over `settle_s`. |
| F7 | Physical communication: radio only between carriers on their own net; voice and hushed voice through the real sound system; gestures need line of sight and short range; PDA text is slow. Involuntary cries carry no estimate of the attacker. A report's target id only binds it to its entity; the object's position is never read. |
| F8 | Latency: nothing is learned on the frame it happens; at least 250 ms from observation to delivery. |
| F9 | Symmetry: loud callouts are audible to the player, radio squelch near carriers; muzzle flash and lamps work for NPCs and the actor alike. |
| F10 | No omniscient helpers in scripts: no weight or torch detection without line of sight, no corpse -> live actor position, no "one hit and the whole squad knows", no faction-wide first-contact broadcast. |
| F11 | Unknown data never crashes and never cheats: a missing profile falls back to `ai_profile_default`, a switched-off feature to the stock behaviour, never to omniscience. |

## Factions and ranks

Rank band: the NPC's rank (the real rating, mapped through `[game_relations] rating`) gives a band through
`[ai_rank_bands]`: novice and trainee 1, experienced and professional 2, veteran and expert 3, master and legend 4;
`legend` also reacts and settles 10 % faster (`[ai_rank_scale]`). A master of any faction is not dumb: even the
default profile of an unknown community scales with the band.

Reaction is the delay between identification and the first aimed shot (random range, R1 -> R4); radio is the chance
to carry one, rolled once per NPC life (R1/R2/R3/R4, `-` = no NPC of that band spawns). Voice follows DA's voice
tiers: loud where the NPC barks, hushed on DA's silent tiers (`dummy_silent`: mercenaries, military sergeants and up,
Duty, Freedom and Monolith from R2), detected from the NPC's own bark keys, never from a section name.

| Profile | Reaction, ms | Radio | Voice, signals | Fighting | Breaking | Search |
|---|---|---|---|---|---|---|
| Military (`army`) | 800-1100 -> 350-500 | everyone; HQ relay 30-60 s | loud privates, hushed from sergeant; gestures from R1 | cover 0.8-0.95, suppression window 6 s, fire on order, bounding advance, F1 grenades | orderly; no rout below heavy losses, no surrender | pairs, quiet, 120-300 s |
| Duty (`dolg`) | 900-1300 -> 400-600 | .6/.9/1/1, leader +.4; HQ relay from R3 | loud rookies, hushed from R2; gestures from R2; PDA | fire on order from R2, bounding from R3, F1 grenades | orderly, no surrender | pairs, quiet, 90-180 s |
| Mercenaries (`killer`) | 550-800 -> 350-550 | -/.5/.65/.85, leader +.3; no HQ (secret cells) | hushed; gestures from R2 | best cover and flanking of all, fire on order, bounding from R3 | orderly, withdraws when it is not worth it | pairs, quiet, 150-240 s |
| Clear Sky (`csky`) | 1100-1500 -> 450-700 | .5/.8/1/1, leader +.3; HQ relay from R3 | by voice tier; gestures from R3; PDA | rookies fight alone; fire on order from R3 | orderly; rookies may surrender | single, 45-120 s |
| Freedom | 1000-1500 -> 400-650, +300 ms when idle | .15/.4/.8/.9, leader +.3 | loud rookies, hushed from R2; gestures from R3; PDA | aggressive, looser cover | orderly; rookies may surrender | single, 45-120 s |
| Loners (`stalker`) | 1100-1600 -> 400-650 | 0/.1/.3/.6, leader +.2 | loud; gestures from R3; PDA | rookies fight alone and hold back; veterans suppress and flank | rookies scatter; R1-R2 may surrender | rookies wait and call out, R3+ single, 40-120 s |
| Bandits | 1200-1800 -> 500-800 | none | loud gangs (the boss is hushed); gestures only R4 | poor cover, aggression 0.2, 0.6 while winning 1.5:1 | scatter (orderly at R4), surrender up to 30 % | single and noisy, 30-90 s |
| Monolith | 800-1100 -> 350-500 | 0/.7/.9/1; no HQ, no telepathy | loud rookies, hushed from R2; gestures from R2 | aggression 0.75-0.9, trades cover for pressure | fanatics: no fear, never rout or surrender, relocate sideways at most | straight at the last known spot, 180-300 s |
| Ecologists (`ecolog`) | scientists 1300-1800, staff 700-1000 | everyone; HQ relay | loud, no gestures | scientists do not fight; staff suppress and cover | scientists withdraw on contact, often surrender | none (report) |
| Ecologist guards | 700-1000 | everyone; HQ relay | loud; gestures from R2 | disciplined guards | orderly | single |
| Zombified | 1500-2500 | none | none | aggression 1, no cover | never | none |
| Unknown communities | 950-1350 -> 450-650 | .2/.2/.3/.4, leader +.2 | loud; gestures from R3 | scales with the band | orderly | single |

Anchors kept through tuning: a military private reacts like a loner veteran; a bandit master matches a loner R3-R4
and leads his gang; mercenaries out-class Duty at the same band in cover and flanking; the Monolith trades cover for
aggression and never routs. Special characters get their own sections through `[ai_profile_by_character]`: military
snipers, Monolith snipers and RG-6 gunners, ecologist scientists, field staff and guards, and Clear Sky's
experienced ranks on the best tier (`sim_default_csky_4`).

## Senses

### Vision

- **Awareness meter.** While the vision rays reach an object inside the view distance, its awareness rises per second
  by `intensity / visibility_threshold x rate_<danger|free> x detect_rate x band_rate x peripheral x stance x close x
  concealment`, plus lamp and muzzle-flash cues. `intensity` is the stock visibility value of one time quantum, i.e.
  the Lua hook `visual_memory_manager.get_visible_value` (same arguments) or the stock formula; a hook value at or
  above the threshold still means "seen at once". Unseen, awareness decays, but not below `reacquire` for a target
  identified within 8 s. `identify` (1.0) sets the engine's visible flag; `suspicious` (0.35) gives a danger at a
  rough spot whose error shrinks from 15 m to 2 m as awareness grows. The HUD "being noticed" indicator and the Lua
  `not_yet_visible_objects` values show awareness in the stock units.
- **Calibration targets** (danger state, daylight, target in the open, frontal, <= 80 m): R1 1.0-1.8 s, R2 0.7-1.2 s,
  R3 0.45-0.8 s, R4 0.3-0.6 s; calm x2.5. A crouching target multiplies the rate by 0.6.
- **Sight range.** DA's weathers keep `fog_density = 1` even in clear air, and with `[stalker] fog_density_factor = 12`
  the stock formula cut every NPC's view to 1/13 (stock NPCs saw 20-38 m and hid it behind instant detection). An AI
  stalker's view now ends where the renderer's fog does, `fog_near + fog_sight x (fog_far - fog_near)`
  (`[ai_awareness] fog_sight = 0.7`); the player's view-distance setting no longer changes it. Bodies the player's
  camera never drew have no computed brightness, so `daylight_lum` (0.6 x (1 - darkness)) is their floor, and a
  brightness the renderer has not refreshed since the environment darkened (a body last drawn by day) darkens with the
  environment's light.
- **Line of sight.** Stalker vision rays, the friendly-fire ray and the look-out ray test back faces as bullets do, so
  an NPC sees and aims through exactly what bullets pass. Foliage and fences slow recognition by their transparency
  product; below `opaque_below` (0.05) a ray is blocked. A combat stalker re-traces its selected enemy on the tick
  between vision traces, so a target that ducks out of sight stops being "visible" one update earlier.
- **Identified targets cost no meter maths.** In range an identified target stays identified whatever a glimpse's
  intensity is, so its intensity (the Lua hook, a call per object and update) is not computed; it is again once the
  meter has fallen below `identify`.

### Hearing

Every heard sound, engine sound events and the noise bus alike, goes through one function: the profile's `hear_gain`,
one static occlusion ray per source (the materials' sound occlusion over up to 3 faces, floored at 0.25 because sound
bends around corners, charged to the ray budget; kept for 16 sources per hearer, 2 s, 4 s for a hearer beyond
`lod_near_m`, while the source stays within 2 m + 8 % of its distance: a far source barely turns the line), rain as a
subtracted noise floor (`rain_mask`: quiet
sounds drown, shots do not), then the NPC's own adaptive threshold (a loud sound masks quieter ones for a while).
Above the threshold the NPC gets a position **estimate** (error model below); the sound memory stores the estimate,
never the source's position. A gunshot no longer creates a fake hit carrying the shooter's exact position. A sound
playing more than 6 m from its source object (ricochet, whine) marks only its own spot. A burst is one observation:
the next rounds (and impacts) of the same source from about the same spot within `heard_repeat` (250 ms, 2 m) reuse
the estimate of the first instead of making a new one per round; a calm NPC beyond `lod_near_m` takes a distant
fight's gunfire as one observation per source and `heard_repeat_far` (1 s, 6 m), its first shot at once. One NPC
update takes at most `stimuli_per_update` (8) queued noises, flashes and near misses, oldest first, and casts at most
`pre_rays` (2) new rays for them and its look for lamps (a look that falls due keeps one); a stimulus that needs a
new ray beyond that waits for the next update (K11: a night update with eight stimuli and a look took 1.2 ms); after
its first stimulus an update takes more, and looks for lamps, only within `pre_budget_us` (150 us).

**Noise bus.** What a human's body and hands do reaches the AI as explicit noises: steps (gait: crouch 0.12, walk
0.35, run 0.7, sprint 1.0, in place 0.12; x the ground surface from `[ai_noise_surface]`; x load: +0.015 per kg above
25 kg), the actor's landings (0.9), weapon reloads (0.5), torch clicks (0.2), eating, medkits and bandages
(`[ai_script] item_use_loudness`), and `level.ai_noise`. A noise of loudness L is heard in silence within L x 36 m
(steps, landings) or L x 40 m (everything else), with an inverse-square fall-off inside that radius. Friends' noises
are not queued. While the bus is on, the engine's step, reload and torch sound events of humans are ignored by the AI
(one source per stimulus kind; scripts' `callback.sound` still gets them); gunshots, voices, impacts, explosions and
the grenade pin stay engine sounds.

### Light

`darkness` (0 day .. 1 night) is the larger of the weather's (its brightness between `ambient` 0.6 and 0.15) and the
clock's (`night_hours`: dark from 21:00 to 04:30, fading in from 19:30 and out until 06:00): Dead Air's nights keep a
blue sky colour as bright as an overcast day, so the weather alone left a night half dark. A muzzle flash is seen by
stalkers whose view cone holds it within 80 m by day to 300 m by night with a line of sight to the muzzle or, when that
exact point is hidden, to the shooter's head it lights (none with a suppressor; one event per shooter per 0.5 s): a
`muzzle_flash` estimate at the point seen, an attack danger and faster recognition of the shooter. The muzzle alone
was not enough: the fire point of a third-person weapon was hidden from every observer of a shooter in clear view
(K13), and the engine itself fires an NPC's rounds from his eye.
A lit lamp glows toward an observer by the beam's angle (0.3 side-on to 1.5 straight at him, nothing pointing away)
over 15 m by day to 150 m by night: the glow speeds up recognition of its carrier (crouching does not hide a lamp),
a strong enough glow in view gives a `flashlight` estimate, and at night the patch the beam lights, seen within 60 m,
gives one with the beam length added to the error. One look for lamps (every 0.5 s) takes the geometry of every lamp
first (glow toward the NPC, the lit spot in view); the lamps facing the NPC are judged first, brightest first, then
the lit spots round the level's lamps from where the last look stopped. A look asks who carries a lamp (relation, own
sight) for at most `torch_scan_checks` (8) lamps and casts at most `torch_scan_rays` (4) rays and what the update's
`pre_rays` leave, the shared refresh of a beam's spot included (a night with every NPC's torch lit made one look cost
~1 ms); a look with no ray left waits for the next update; the lamps facing the NPC a capped
look left come first in the next one. This replaces DA's direction-less torch bonus in `visual_memory_manager.script`.

### Hits, near misses, suppression

A hit whose shooter the NPC does not see gives only the reversed bullet direction +-(5 + 20 x (1 - hear_skill))
degrees at the distance of its own heard-shot belief, else 30-60 m, +-40 % (exact within 2.5 m or when the shooter
is in own sight; no estimate for explosions, anomalies and burns). A bullet crossing a stalker's bounding sphere
(queued thread-safely by the bullet manager, drained on the main thread) raises its suppression by 0.25 x closeness,
decaying 0.3/s; friends' bullets do not suppress. The victim also gets a `near_miss` estimate (+-25 degrees, 20-80 m
unless it heard the shot). Tactics (dispersion, fall back), morale (fear) and comms (no PDA typing under fire) read
the suppression.

## Knowledge

Every online human stalker keeps its own beliefs about creatures (at most `beliefs_max` = 16; when full, the oldest
non-enemy goes first). A belief is an estimate (a position on a valid level vertex at body-centre height, the error
radius the NPC assumes, the observation time, a confidence and a source) plus the last seen velocity, the time of the
NPC's own last sighting and the enemy flag. Items and corpses nobody believes in stay world state. An estimate that
lands off the graph takes the nearest cell a window search finds within `vertex_search_m` (22 m); a spot farther from
every cell takes the first cell on the way back to the observer (windows of the same size, at most
`vertex_march_steps` 16). A window takes the full contour distance only of the cells that can still beat the best one
found (columns outward from the spot, skipped by their flat distance): a failed window of 22 m cost ~0.5 ms before.
Each column's cells are found inside the column (an index of the first cell of every column, built once per level),
not by a binary search over the whole graph: a window over empty ground was up to 65 of those, a report up to five
windows per channel. A
described spot of a report snaps back toward the sender's own estimate, not toward the sender. The stock column
search, milliseconds for a guess far outside the level (the senses' worst updates), is left only for a spot with no
observer.

**Update rule.** Own vision is the only `seen` source: exact while the NPC sees the target. After losing sight the
belief moves on with the last seen velocity for `extrap_s` (0.6 s x 0.8/1.0/1.2/1.4 by band) and then stays. Any
other estimate replaces the belief only if its effective error (`err + 1.5 m/s x age`, capped at 60 m) is smaller; own
sight wins a tie. Unrefreshed beliefs are forgotten after 300 s (enemies) or 60 s (everybody else); the ageing pass
runs every `maintain_ms` (0.25 s, 1 s beyond `lod_near_m`).

**Error model** (`[ai_errors]`; `d` = distance to the source). The realised position is a disk sample of
`err x 0.6` around the truth at observation time, snapped to the level graph; a poorly perceived source multiplies
the error by `1 + 1.5 x (1 - clarity)`.

| Source | Error |
|---|---|
| seen | 0 while seen |
| heard_shot | 3 + 0.08 d; x2 suppressed; x(1 + occlusion); x(1.5 - hear_skill) |
| heard_step | 2 + 0.12 d (same multipliers) |
| heard_reload, heard_item, heard_voice | 2 + 0.10 d |
| hit (shooter unseen) | direction +-(5 + 20 x (1 - hear_skill)) deg, distance +-40 % |
| near_miss | direction +-25 deg, distance prior 20-80 m |
| muzzle_flash | 1 + 0.02 d |
| flashlight | 2 + 0.05 d |
| report_* | sender's error + age growth + `describe_k x d(sender, spot)`, quantised: voice and hushed 0.15 / 5 m, gesture 0.2 / 5 m, radio 0.1 / 10 m, PDA and HQ 0.1 / 25 m |
| witness | the friend's aim or facing +-30 deg, distance prior 30-60 m |
| script (rumour) | 25 m |
| restored (from a save) | saved error + age growth |

**What reads beliefs.** For a stalker, `memory()`, `memory_time()`, `memory_position()`,
`visible_object_time_last_seen()` and so the Lua `memory_time`, `memory_position` and the enemy selection return the
belief; a live creature it neither sees nor believes in is unknown. `see` and `visible_now` are the NPC's own sight:
confirmed at its last visual update and at most `sight_hold_ms` (0.75 s) old, whatever else writes the shared entry.
Enemy candidates come only from beliefs on enemies (out of combat only the ones the NPC has seen itself, whatever
estimate is newest, and told, rumoured and restored ones: a heard-only enemy is a danger to investigate, as the stock
engine kept every seen enemy in its visual memory); a belief's enemy flag is set by each observation and re-read every
`relation_refresh_ms` (1 s). The script's enemy callback (`xr_combat_ignore.is_enemy` and what DA hangs on it, a Lua
call per candidate) is asked once per target and `enemy_callback_ms` (1 s, 3 s beyond `lod_near_m`, +-25 %); a hit
asks it again at once, and a new target is asked at its first candidacy. Dangers come only
from the NPC's own observations, never from pool entries that carry another member's position and time. The agent's
enemy distribution offers each member only its own believed enemies and measures known distances. A reported-only
belief (`report_*`, `witness`, `script`, `restored`) may drive movement, cover, search and look direction, never a shot
or a grenade.

**Aim.** A creature out of own sight is aimed and looked at where the NPC believes it is. A seen actor or stalker is
aimed at a bone rolled per target and held for 3 s: the head with the profile's `aim_head` chance, else the spine. The
velocity lead applies only while the target is seen, off by up to `lead_error`. The finishing shot at a wounded man
through cover needs an own sighting within 3 s.

**"The enemy sees me"** (`EnemySeeMe`, Lua `ai_enemy_sees_me`): only an enemy in own sight shows where it looks. True
when the NPC is inside his facing cone (60 degrees half angle) and a ray from his eyes reaches the NPC's head; an
unseen enemy answers `see_me_unknown` (1: assume he sees me). It drives the sudden attack and the scripts' "is it
safe" checks.

## Communication

A message is `{kind, target id, estimate, sender, origin, channel, extra}`. The estimate is the sender's own belief at
the moment the channel starts, degraded by the channel's describe error. Involuntary cries (pain, death, panic) carry
no estimate of the attacker: hearers learn only that a mate is hurt or dead at the heard spot.

| Channel | Who sends | Who receives | Time |
|---|---|---|---|
| voice | `voice = loud` or `auto` on barking tiers | every non-hostile NPC that hears the line above its threshold (occlusion, rain, gunfire masking) | line length + `understand_ms` (x1.5 when masked) |
| hushed | silent tiers, `voice = hushed` | the same, at `hushed_volume` 0.3 (about 10-15 m) | line length + `understand_ms` x1.5 |
| gesture | band >= `gestures` | non-hostile NPCs within 25 m that see the sender within 120 degrees of their facing and whom it sees | 400-800 ms + `understand_ms` |
| radio | carriers | carriers of the same sim squad, level-wide; squelch and radio voice audible 8 m around sender and receivers, enemies included | `decide_ms` + key 300-600 ms + message 600-2000 ms + `understand_ms` |
| pda | `pda = 1` | own squad members beyond 40 m the radio did not reach; never typed while hit or suppressed | typing 3-6 s + 5-20 s |
| hq | carriers of band >= `hq_relay` | carriers of the community's other squads on the level | `hq_delay_s` |

Hostile hearers of a voice, hushed words or a radio squelch get only a `heard_voice` estimate of the speaker.

**Pipeline and latency.** A sender decides (`decide_ms`, shorter for urgent kinds such as grenades), transmits on the
channels `[ai_comms_kinds]` allows, the receiver hears or sees it and understands (`understand_ms`, +50 % hushed, +50 %
while gunfire raised its hearing threshold); the message is delivered in the receiver's own later update, never in
the frame it was queued and never sooner than 250 ms after the observation, once per origin whatever the number of
channels. Delivery feeds the receiver's beliefs (`report_*`), tactics, morale and search; the receiver still waits its
own `react_ms` before any aimed action. One update takes in at most `deliveries_per_update` (3) due messages and
witness cues, starts at most `sounds_per_update` (2) radio or PDA sounds (a message waits for its own sound) and sends
at most `transmits_per_update` (2) messages (urgent warnings always), in order; the rest wait for the next. After its
first message an update sends (takes in) more only while its sending (taking in) has taken less than
`send_budget_us` (`take_budget_us`, 100 us each). A hand signal is described only when somebody sees it. A pain
or death cry and the report the same hit starts are different messages: hearing the cry does not swallow the report.
Rate limit: one message per kind, target and squad per 4 s.

**Voice is decoded from the real sound.** Stock barks become messages without new sound code through
`[ai_comms_barks]` (alarm = contact, detour = flanking, backup = help, grenade alarms = grenade, death and wounded
cries = man down, ...); a line is decoded once per hearer and line start, never by its speaker. Comms adds explicit
callouts the stock logic never voices: contact (quiet during a sneak attack), "lost him", radio position updates while
the enemy is in sight, "reloading" (loud voices only, exploitable by design), man down, leader down, grenade warnings,
taking fire. Silent tiers (DA's `dummy_silent`) blank every voice key, including death, pain and grenade alarms; the
hushed set loads those lines from the base voice set at hushed volume, so a silent-tier casualty is still audible up
close.

**Radio nets and relays.** The radio trait is rolled once per NPC lifetime (`radio_chance` of its band, plus
`radio_leader_bonus` for the squad's AI leader at that moment) and saved, so neither a load, an offline period, a
promotion nor a leader's death changes it. The squelch and PDA tone sounds are loaded with the comms data when the
first stalker of a level spawns; the squelch is the base game's radio static clicks (`ambient\special\marsh_click_1`
to `_3`, `_3a`, with the ogg comment the engine expects), played at full volume within half of the 8 m squelch
range; a device sound nobody could hear (neither the player's camera nor an NPC that is not the carrier's friend
within 1.25 x that range) is not started. A radio cannot be picked up or handed over. A carrier passes radio and HQ
news on by voice or signal to mates without a radio within 40 m; a mate without a radio tells a carrier, who keys the
radio; one hop per report. Killing a squad's last carrier cuts its net.

**Witness cue.** A friend in own sight who fights or is alarmed shows only where he looks: a `witness` belief along his
facing (+-30 degrees, 30-60 m) and a suspicion, never his enemy's position or his danger object. The agent memory
pooling of combat members is off.

Kinds (the Lua value, 0-based): contact, update, lost, man_down, leader_down, taking_fire, grenade, reloading,
flanking, cover_me, fall_back, attack, help, search_assign, all_clear, surrender. Channels (0-based): voice, hushed,
radio, gesture, pda, hq.

## Tactics

Applies to human stalkers in the engine combat planner against human enemies; fights with mutants and script combat
schemes (zombies, scenes) stay stock.

- **Weapon classes and bands** (`[ai_weapon_classes]`, metres effective / engage_max / min): pistol 25/40/0, smg
  60/100/0, shotgun 20/35/0, rifle_old 200/300/10, rifle 200/300/0, mg 400/500/10, dmr 600/800/30, launcher
  200/300/30. A rifle with a 4x scope reaches `engage_max x 1.33`; a shotgun loaded with slugs 60 m. The class of a
  weapon section comes from ordered data rules (`[ai_weapon_class_rules]`): overrides, not a firearm, pistol slot (DA:
  4), ammo families (launcher, shotgun, pistol calibres in a long gun = smg), automatic with a 45+ magazine = mg, a
  4x+ scope on a magazine of 20 or less = dmr, the old-rifle list, else rifle.
- **Doctrine.** Beyond `engage_max` an NPC closes in only cover to cover: the get-distance sub-planner runs when a
  bound exists that cuts the known distance by `approach_min_gain_m` with acceptable path exposure and its aggression
  allows it. Otherwise it holds an ambush in cover (`DaHoldAmbush`), watching the belief and firing only inside its
  band. Closer than the weapon's `min` it falls back or draws the sidearm. Short-range weapons (`engage_max` up to
  100 m: pistols, SMGs, shotguns) do not cross open ground toward the enemy: a bound, a cover or a search step closer
  to him may cross only `exposure_gap_m` (2 m, the level graph's step between covered cells) of open ground, a trained
  NPC while a mate suppresses (an overheard or own suppression order) `exposure_open_limit_m` (6 m), and never more
  than is left of the fight's `open_fight_budget_m` (12 m toward the enemy in all; a fight ends after
  `alert_memory_s` without a human enemy). Leaving a covered spot away or across they may cross 6 m; one already
  standing in the open may move away or across to any cover.
- **Fire gate.** Aimed fire needs the target in own sight, the reaction elapsed, and the target inside `engage_max`.
  Reaction = `react_ms` + idle, distance and peripheral add-ons, x fear, x fatigue; a quick re-acquisition takes a
  fraction of it. The gate is checked every frame: an aimed command that waits only for the reaction or for the head
  to turn to the target goes the frame both hold, not at the action's next brain update (which added up to a schedule
  period, longest for NPCs far from the player). A released ambusher who sees his target opens up at the order. An
  unseen target gets only honest suppression (F5). The band's no-fire rule holds for every living
  target (mutants too; the doctrine's movement stays human-only), and in engine combat no round leaves without a
  selected enemy. The gate is decided on the main thread each frame and also asked by the weapon's own fire loop (own
  sight and the band), so neither the next-frame shot nor the single shot a fire state owes leaves without it.
  Suppression needs the same floor: a height difference up to `supp_slope` (0.2) x the distance is terrain, only a
  steeper one than `supp_floor_m` (2 m, centre to centre) close by counts as another floor.
- **Accuracy** multiplies DA's accuracy tier (`dummy_1..7`, kept) by settle (x3 down to x1 over `settle_s` after each
  acquisition), the target's lateral speed, 1 + suppression, wounds, fatigue and the haste of a withdrawal.
- **Cover** adds to the stock cover value: a line of fire from a low wall, the weapon's preferred range, an aggression
  pull (aggressive profiles closer to the enemy, cautious ones farther), travel distance and path exposure toward the
  belief. The cover search reaches to `engage_max` from the enemy while he is inside the band and without a limit once
  he is beyond it, so an outranged NPC backs off into cover instead of holding its ambush in the open (K11: every
  ambush sample of the army, Monolith and loners was uncovered). Take cover runs while exposed.
- **Return fire** (`DaReturnFire`, profile `return_fire_m`): an NPC that sees an enemy it has identified within that
  many metres, who has found it out (a hit, near misses, or it sees him look its way), readies its weapon and shoots
  back from where it stands for `return_fire_ms` (2-4 s) before the planner moves it: in the open before it runs for
  cover, in the cover it already has (crouched behind a low wall) before it looks for a better one. No new exchange
  for `return_fire_cooldown_ms`. Drilled profiles have it (army 30 m, mercs 30, Monolith 30, Duty 25, veteran bands
  15-20), untrained ones (0) move to cover first.
- **Fall back** (`DaShouldFallBack` / `DaFallBack`): the enemy inside the weapon's `min`, a compromised cover (open
  toward the belief, or hit there), exposed under fire with no cover in reach, reloading in the open with the enemy
  near, or worried morale while exposed (a worried man in a cover that covers him stays). The target is a cover
  farther from the belief (a sideways relocation for fanatics); a straight line away over open ground only from an
  enemy inside the weapon's `min`. Otherwise no fall back: take cover picks the best cover. Only a cover point counts
  as cover on arrival. An out-of-band ambush exposed toward the belief first walks into its best cover.
- **Roles.** The NPC that believes itself the squad's leader orders one suppressor (automatic weapon, best `supp`, a man
  he heard fire within `supp_order_window_s` first; the man ordered keeps the job until the order is renewed)
  and, against an enemy holding out of his sight or dug in (his believed spot unchanged for `flush_static_s` and
  covered toward the leader), one flanker (best `flank`, a weapon of `flank_classes`, not tired, not in an exo; a
  man whose band reaches the enemy first, a short gun's bonus only then). Orders are
  `cover_me` / `flanking` messages and apply on delivery; untrained profiles stay opportunists. The suppressor order
  keeps its covering fire going for `supp_order_s` (15 s, the leader renews it every `role_renew_s`); other mates give
  covering fire only after a delivered `flanking` / `cover_me` / `reloading` / `fall_back` message or while they see a
  mate move in the open. Message windows only extend, never cut an order's longer one; mates who overhear the
  suppression order know the squad is covered for `supp_order_s`. The ordered suppressor walks into its best cover
  and fires its bursts from there, at its own last-seen point for up to `supp_order_window_s` (12 s) after its last
  sight. **Flank route first:** before he orders a flank the leader looks, from his own belief, at the ground around
  his man, one step per update: a covered spot `flank_angle_deg` (35-110 degrees) around the enemy within
  `flank_radius_m` (55 m) on the man's side, then on the other side, then two legs through a cover within
  `flank_via_radius_m` (25 m) on either side, with at most `flank_open_limit_m` (20 m; a short gun 2 m, 6 m under the
  suppressor) of open ground in all. A leg's spots are gathered in one update from the circle around its sector only;
  the `flank_agent_checks` (32) best by geometry get the agent's checks (restrictions, mates' covers, known enemies,
  danger) lazily, `flank_agent_per_update` (8) per update, and the `flank_checks` (12) cheapest of those the
  open-ground test, `flank_walks_per_update` (4) walks per update, each walk stopping once its open metres pass the
  limit. With no route for his first man the leader tries the next (`flank_candidates`, 2) before another tactic:
  a short gun may cross 2 m of open ground, a rifleman 20 m, and a squad of SMG holders held every time before. The
  open-ground measure (of the flank route, a bound, a cover's approach) walks the level graph's cells along the
  straight line, each counted with the length of the line inside it.
  The order names the route (the flanker takes it while the order is `flank_route_s` fresh; a short gun refuses it
  past its fight's budget). **No route, no flank:** the leader picks another tactic instead and says which (counted):
  a grenade flush of a dug-in enemy within his throw (his next throw needs no roll), the suppression order renewed so
  his men who bound advance under it, or hold; he looks again after `flank_retry_s` (10 s). **The flanker goes at
  once** (`DaFlank`): the order outranks fighting from where he stands and the search while it holds, a burst under
  way ends first (within `flank_grace_ms`, 1.5 s). He runs the route (via cover, then the end) whatever his band, and
  fires at what he sees on the move only if his profile's `flank_fire` says so (army, mercenaries, Monolith, Duty
  R3+); the role ends at the end cover, where he fights from, or after `flank_max_ms` (30 s). Neither the suppressor
  nor the flanker bounds. A rearguard covers an announced withdrawal.
- **Fire on order** (`fire_on_order = 1`): an undetected squad holds fire until the leader's `attack` (given when the
  enemy he sees is inside his effective range), being fired on, being seen, or `fire_on_order_max_s` (8 s). A leader
  out of reach means a ragged opening.
- **Bounding** (`bounding = 1`, confident): inside the band, with the enemy out of sight and a suppressor covering,
  the NPC bounds cover to cover toward the belief. A heard enemy reload opens a 2 s push for aggressive profiles.
- **Grenades.** Grenades are issued at spawn from the profile's `grenade_issue` (DA's NPCs carry none). A throw needs
  a flush trigger (the belief static behind cover, indoors or unreachable), an own seen or heard belief as the target
  (never a report), the profile's `gren` and a grenade. Warned members and anyone who hears a grenade land near hide
  from it; only members who saw, heard or were warned of it avoid its danger area.
- **Fatigue.** A winded NPC walks; tired NPCs and exo wearers flank at a walk and are not picked as flankers.

## Morale

Every stalker has its own morale; there is no squad-wide state a man could feel without perceiving it. It is
evaluated every 250 ms (every `quiet_update_ms`, 1 s, while calm: no enemy, threat, break or wound; a hit, a message,
a learned death or a selected enemy brings the 250 ms back at once) and is not saved (after a load every NPC starts
calm with its present squad as the baseline).

- **Confidence** = base + ratio term (known friendly strength / known enemy strength) + courage - wounds - low ammo -
  fear - losses + enemy deaths seen + the leader in contact. Friendly strength counts the NPC, the mates it believes
  alive and friendly fighters in own sight; an enemy's armour, weapon and wounds are sampled only while he is in own
  vision, and an enemy known only from a report or a sound counts at `unknown_enemy_strength`. Levels: panicked,
  worried, steady, confident, heroic, with hysteresis.
- **Fear** rises with hits, suppression, learned casualties (more when close, less when only reported), the leader's
  death, a nearby ally's death, broken mates in sight, a mate calling fall back, a close grenade; it is scaled by
  courage and band and decays faster in cover. Fanatics feel none. Tactics turns fear into reaction (+0..50 %), settle
  time (+0..100 %) and lower aggression.
- **Casualties by perception only.** A mate's death is learned by seeing the body, hearing the dying cry, or a
  `man_down` / `leader_down` message. The believed leader stays believed until his death is learned; then the army,
  Duty and the mercenaries hand over to the next in rank after 3-6 s (`[ai_modifiers] leader_handover`), the others
  at the next calm.
- **Breaking** needs a selected enemy and one of: heavy losses (`break_cas`) while not winning; clearly outmatched
  (`break_ratio`) under threat for a while; terrified. Armour, wounds, the leader alive or known dead and a sworn enemy
  shift the thresholds.
- **Retreat styles** (`rout_style`): **orderly** - bound cover to cover away from the believed enemy toward the last
  calm spot, firing back, announcing `fall_back`; **scatter** - each runs at its own angle, no fire, drops into cover
  once out of sight; **relocate** (fanatics) - a cover to the side, never farther from the enemy; **surrender** - the
  profile's chance, rolled when wounded or cornered, never to a mutant, never by a fanatic: the NPC stands, does not
  fire and raises its hands (`ai_morale()` = 5); shot at, it scatters.
- **Wounded** NPCs call for help (hushed and by radio on silent tiers) while they believe a friend is near. **Fatigue**:
  stamina drains with running (armour and wounds make it worse) and recovers at rest; tired NPCs stop sprinting.

## Search and investigation

After losing contact a stalker searches from its own last known position (its belief, reported or not) for the
profile's `search_s` (longer against a sworn enemy) plus a per-NPC stagger, so a squad never gives up at once. Spots:
the last known position first, then covers and level vertices within `spot_radius` hidden from it, ordered by the
estimated escape direction; a spot is cleared only by the searcher's own eyes. The sweeping searcher of the highest
band coordinates and hands spots out by `search_assign` messages; searchers report `all_clear`; a searcher no order
reaches searches its own plan. Styles: `pairs` (one sweeps, the partner covers, roles swap), `single`, `straight` (the
stock walk to the last known spot), `none` (hold until the timer runs out). `quiet_search = 1` stops search barks,
hushes orders and sneaks the last metres to each spot; the others bark on the way. A searcher that sees the target
again is back in combat; a given-up target is searched again only after newer information. The search begins where
the stock ambush chain (look out, hold, detour) ends, and at the latest once the NPC has had no sighting, sound or
report of its enemy for the profile's `search_delay_s` (Monolith 3 s, bandits 5, Freedom 6, default 8, army and
Duty 10, snipers 20): the chain gives way (`DaContactLost`), one step (`DaSearchHandover`) enters the search, and the
weapon band no longer holds the NPC in an ambush on a stale position. A short gun keeps the doctrine's open-ground
rule in the search: to a spot, an overwatch post, the last known position or the ambush cover it walks straight only
when the open ground on the way is within its allowance, else it bounds to the next cover nearer the goal (covered
toward its belief), else it holds, crouched, and watches the goal; after `step_hold_ms` (15 s) of holding the last
known position or the ambush cover counts as watched. A flank order holds the search off. A sneak attack whose contact
is lost stops sneaking, so the handover can run. Spot plans are built at most `plan_builds_per_frame` (1) per frame
over all squads, and in steps: a disk up to `plan_tile_m` (30 m) at once, a wider one in seven circles of half its
radius, one per step, then the spots in order of preference, at most `plan_pops_per_step` (256) candidates per
step; at most `plan_builders` (4) builds run at once and one its owner stopped asking for is dropped after
`plan_build_stale_s` (2 s). The same spots come out as from a build in one piece. A searcher whose plan waits holds
and watches for an update. The per-squad step runs the search part at the next member's update, not in the same one.

A suspicion (a glimpse, a heard shot or step, a hit, a report) becomes a danger at the NPC's own estimate. By the
profile's `investigate`: `pair` - one goes, a mate covers it, a leader sends two men; `single` - the first one done
holding goes and says so; `none` - wait in cover and call out; `advance` - straight at the source (Monolith).

## The Lua side

`dead_air_x64_ai.script` (auto-started) wraps base DA scripts at run time and keeps each original, so
`ai_feature scripts 0` restores base DA behaviour without a reload. It never forks a DAR2 file. Overlay copies:
`xr_combat_ignore.script` and `visual_memory_manager.script` (the function bodies themselves were the leaks) and
`configs\ai_tweaks\xrs_kill_wounded.ltx` (`duty` -> `dolg`). Loose `gamedata`, JSGME and XMS modules still win over
them.

- Hostility towards the actor spreads only by perception and reports: the victim, NPCs of its community that see the
  actor at that moment, and NPCs told by a member of their own community (`[ai_script] hostility_kinds`). DA's 100 m
  first-contact broadcast is gone; the 25 m corpse reaction stays.
- Removed leaks: `xr_danger`'s inventory-weight and torch detection, reactions aimed at a danger source's live
  position, the corpse -> actor position, the heard attack danger at the raw sound spot (now only where the NPC's own
  senses placed the source within `hear_danger_fresh_ms`). Fight-from-cover, friendly-fire and camper holds face
  unseen enemies at their memory position; corpse looters and executioners must see the body or the wounded man;
  "the enemy sees me" checks use `ai_enemy_sees_me`.
- `xr_combat_ignore.is_enemy` range caps (DA's 150/100/75/60 m) measure the NPC's memory position of the enemy, not
  the enemy's true position; an attack by the actor drops combat_ignore only for mates that perceive him or are told.
  The engine asks this enemy callback (`set_enemy_callback`) at most once per NPC and target per `enemy_callback_ms`
  (~1 s near the player, ~3 s beyond `lod_near_m`, spread 75-125 %) and at once again after a hit by that target; the
  engine's own checks run every time.
- Weapon choice (`ai_stalker.update_best_weapon`): the weapon whose band holds the known enemy distance, long guns
  first, the sidearm when the long gun cannot fire that close; hand-to-hand and unknown distances stay with DA.
- Torch discipline: `light_discipline >= 0.5` keeps the torch dark in combat, danger and search; others search with it
  lit at night.
- Surrender: while `ai_morale()` reports 5 the NPC holds DA's hands-up motion.
- A fix outside the AI: DA's stalker binder restored an NPC's remembered level vertex on whatever level it next came
  online; the vertex is now applied only on its own level and within `offline_vertex_max_m` of where ALife brings the
  NPC online. The save format is unchanged.

## Architecture

`CAI_Stalker::shedule_Update`, alive branch, in this order:

1. `agent_manager().update()` - pooling off for stalker groups, distribution by known distances.
2. `Exec_Visibility()` - plus the combat re-trace of the selected enemy.
3. `process_enemies()` - a fighting friend becomes a witness cue.
4. `da_ai_pre_memory` - the NPC's stimulus inbox (noises, flashes, lamps, near misses) becomes estimates.
5. `memory().update(dt)` - the awareness meter runs inside the visual memory, beliefs back the lookups.
6. `da_ai_post_memory` - knowledge (ageing, forgetting) -> comms (sender decisions, due deliveries) -> morale ->
   tactics (roles, fire intent) -> search; then the per-squad step, at most every 250 ms.
7. `Think()` - the planner.

Other hooks: `da_ai_spawn` (end of `net_Spawn`, alive only: state, profile, radio trait, restored beliefs),
`da_ai_destroy`, `da_ai_reload` (profile invalidation, hushed sound set), `da_ai_on_death`, `da_ai_on_hit` (the hit
actually applied, after the Lua before-hit callback), `da_ai_update_cl` (the fire gate, decided on the main thread
before the object handler is queued: with `mt_object_handler` the object handlers of different stalkers run in
parallel, so nothing they reach may run Lua or touch another NPC's state), weapon shot, reload and fire-loop hooks,
steps, torch switches, the bullet near-miss queue, and `da_ai_level_unload` at the start of `~CLevel`.

State: `CAI_Stalker` holds one `DaAi::NpcState*` (profile, sim squad id, band, radio trait and one pointer per part:
knowledge, senses, comms, tactics, morale, search). Squads are keyed by the sim squad id (`m_group_id`; an NPC without
one is a squad of one); the AI leader is the alive member with the highest band (ties: rank, then the engine
commander order), not the engine's lowest-id commander. Offline NPCs have no state and receive nothing. Budget: 48 new
rays per frame round-robin (`[ai_config] rays_per_frame`); rays that already existed and only changed flags are
outside it and never deferred.

Level of detail: at the start of each update an NPC is marked `distant` (farther than `[ai_config] lod_near_m`, 60 m,
from the player's camera, the measure the engine's scheduler already uses for update intervals) and `calm` (no
selected enemy). Only per-target refresh work is coarser for them: the script's enemy callback (3 s instead of 1 s),
the belief ageing pass (1 s instead of 0.25 s), the occlusion ray cache (4 s instead of 2 s) and, for distant and
calm hearers, the repeat window of a distant fight's gunfire (1 s, 6 m). Identification, first reactions, a new
target's first callback and every estimate's error model are the same at any distance.

Code: `src/xrGame/da_ai*.h/.cpp` (core, profiles, script bindings, and one set of files per part), the hooks in the
stalker, memory, sound, weapon, step, torch and bullet files, and `src/xrEngine/Feel_Vision.*` (full-test vision
rays, per-item transparency, single-item re-trace; three exports appended to `xrEngine.def`, two re-signed under
their old ordinals, so `xrEngine.dll` and `xrGame.dll` ship together).

## Planner wiring

IDs live in a range no script uses (`0x00DA0000 + n` properties, `0x00DA0100 + n` operators) and are exported to Lua
as `stalker_ids.da_property_*` / `stalker_ids.da_action_*`.

| Id | Name | Kind | Meaning |
|---|---|---|---|
| `0x00DA0001` | `DaOutOfBand` | property | known distance outside the weapon's band and no covered approach |
| `0x00DA0002` | `DaSuppressRole` | property | suppressor role delivered, own last-seen point fresh, a mate known exposed, weapon allows |
| `0x00DA0003` | `DaShouldFallBack` | property | see Fall back |
| `0x00DA0004` | `DaReturnFire` | property | caught in the open, found out, an identified enemy within `return_fire_m` |
| `0x00DA0005` | `DaContactLost` | property | the selected enemy unseen, unheard and unreported for `search_delay_s` |
| `0x00DA0006` | `DaFlankRole` | property | the ordered flanker with a covered route (not while a burst of the grace ends) |
| `0x00DA0010` | `DaSearchSpot` | property | holds an assigned, uncleared search spot |
| `0x00DA0011` | `DaSearchCover` | property | its search partner moves and it overwatches |
| `0x00DA0101` | `DaSuppressFire` | operator, weight 2 | bursts at the own last-seen point and the cover edges beside it |
| `0x00DA0102` | `DaFallBack` | operator | move to the fall-back cover |
| `0x00DA0103` | `DaHoldAmbush` | operator | hold cover, fire only inside the band |
| `0x00DA0104` | `DaReturnFire` | operator | shoot back from where it stands (one step to `PureEnemy`, so it beats take cover -> kill) |
| `0x00DA0105` | `DaSearchHandover` | operator | lost contact: sets what the search planner needs (out of cover, looked out, held, detoured) |
| `0x00DA0106` | `DaFlank` | operator | run the ordered covered route (one step to `PureEnemy`: kill enemy weighs 4); in cover at its end |
| `0x00DA0110` | `DaSearchSweep` | operator | search sub-planner: sweep a spot |
| `0x00DA0111` | `DaSearchOverwatch` | operator | search sub-planner: cover the partner |

Replaced evaluators keep their ids and hold the stock one, which they call when their feature is off: `Panic`
(morale), `EnemySeeMe` (knowledge), `TooFarToKillEnemy` (tactics; re-enables the stock get-distance sub-planner),
`ShouldThrowGrenade` (tactics). Added conditions: `DaShouldFallBack = false` on take cover, kill enemy, look out, hold
position, detour, the search planner and get distance; `DaOutOfBand = false` on kill enemy, look out, hold position,
detour, the search planner and kill-if-not-visible; `DaContactLost = false` on look out, hold position and detour;
`DaSuppressRole = false` on detour and the search planner (`DaSuppressFire` itself needs no `InCover`);
`DaFlankRole = false` on the search planner, the search handover, kill-if-not-visible, the low-cover planner, return
fire and hold ambush (the other one-step fights and the search wait while a flank order holds).
Only plain evaluators and operators are added (no storage property, no sub-planner), so the planner save stream is
unchanged. With every feature off, the new evaluators return false and the planner is the stock one. A mod that
replaces the Panic or a danger evaluator from Lua still wins (the last replacement wins).

## The data file: `configs\dead_air_x64_ai.ltx`

Every number of the AI lives in this file; every key carries a comment. It ships in the compatibility archive and is
merged into `system.ltx` **before** the XMS config stage, so a module's `configs\xms\*.ltx` sections and `.ltxp`
directives patch any key of it. A loose copy replaces the whole file.

**Profile resolution.** Every key is read from the first section of the NPC's chain that has it:

1. `<[ai_profile_by_character] match>_r<band>`, `<match>` - the NPC's specific character id (else its character
   profile id), longest prefix;
2. `ai_profile_<community>_r<band>`, `ai_profile_<community>` - the community after `[ai_community_alias]`;
3. `ai_profile_default_r<band>`, `ai_profile_default`;
4. the compiled-in defaults (equal to `ai_profile_default`).

Missing sections are skipped. A faction's base section is its R2 row; bands 1, 3 and 4 override. The chain is walked
in code rather than through `[a]:b` parents, because ltx parents copy at parse time and a later `.ltxp` patch of a
parent would not reach its children; with the chain a patch of one key reaches every NPC whose chain does not set that
key closer to its head. One log line per unknown community (`* [ai] profile <community> -> default`); bad values log
`! [ai] profile ...` and keep the next value. Profiles resolve at spawn and on `reload`, and again when a script
changes the NPC's community; the radio trait is never re-rolled.

**Profile keys** (all in `[ai_profile_default]`):

| Group | Keys |
|---|---|
| perception | `react_ms` (range), `react_idle_ms`, `settle_s`, `detect_rate`, `hear_skill`, `hear_gain`, `aim_head`, `lead_error`, `light_discipline` |
| comms | `radio_chance` (R1..R4), `radio_leader_bonus`, `hq_relay` (0 = none, else min band), `hq_delay_s`, `voice` (`auto`/`loud`/`hushed`/`none`; auto = hushed when the NPC's own section leaves its combat bark keys empty), `gestures` (min band, 0 = never), `pda`, `decide_ms`, `understand_ms`, `callouts` |
| tactics | `aggr`, `aggr_winning`, `winning_ratio`, `cover`, `flank`, `supp`, `supp_window_s`, `gren`, `peek`, `bounding`, `fire_on_order`, `return_fire_m` (m, 0 = cover first), `flank_fire` (the ordered flanker fires on the move), `roles` (`opportunist, suppressor, flanker, grenadier, rearguard`), `grenade_issue` (`item, count, chance`) |
| morale | `courage`, `break_cas`, `break_ratio`, `leader_dead`, `rout_style` (`orderly`/`scatter`/`relocate`/`never`), `surrender`, `recover_s`, `fanatic`, `stamina` |
| search | `search_delay_s`, `search_s`, `search_style` (`pairs`/`single`/`straight`/`none`), `investigate` (`pair`/`single`/`none`/`advance`), `quiet_search`, `giveup_stagger_s` |

**Global sections:** `[ai_config]` (ray budget, squad tick, level-of-detail distance), `[ai_rank_bands]`,
`[ai_rank_scale]`, `[ai_community_alias]`, `[ai_profile_by_character]`, `[ai_weapon_classes]`,
`[ai_weapon_class_rules]`, `[ai_weapon_class_overrides]`, `[ai_weapon_class_rifle_old]`, `[ai_slug_ammo]`,
`[ai_armour_classes]`, `[ai_armour_overrides]` (DA NPCs wear no outfit item, so the suit class is read from the visual's
name), `[ai_errors]`, `[ai_awareness]`, `[ai_noise]`, `[ai_noise_surface]`, `[ai_light]`, `[ai_comms]`,
`[ai_comms_barks]`, `[ai_comms_voice]`, `[ai_comms_kinds]`, `[ai_comms_lines]`, `[ai_tactics]`, `[ai_morale]`,
`[ai_morale_weapons]`, `[ai_morale_monsters]`, `[ai_modifiers]`, `[ai_search]`, `[ai_sworn_enemies]`, `[ai_script]`.

A module patches it like any other config (`patch\*.ltxp`):

```ini
[ai_community_alias]
renegade      = bandit                    ; a mod's community fights like bandits

[ai_profile_by_character]
my_mod_sniper = ai_profile_army_sniper    ; a special character gets its own profile

[ai_weapon_class_overrides]
wpn_my_shotgun_rifle = rifle              ; a weapon the generic rules would misclass

[ai_noise_surface]
my_gravel     = 1.3                       ; step loudness of a new ground material

[ai_profile_bandit_r1]
react_ms      = 1400, 2000                ; one key of one band
```

`tools\qa\ai\Check-AiLtx.py` checks the shipped file: sections, duplicate keys, marker blocks, every profile key known
and typed, every character target defined, the class, rule and armour tables well formed.

## Save data

Two versioned `.scov` chunks (`SAVE_COMPATIBILITY.md`); `.scop` and `.scoc` are unchanged, and the memory managers keep
their save layout (the values are estimates for stalkers).

- `AIK1` - beliefs of online stalkers (at most 16 per NPC, younger than their forget time), applied when the NPC
  spawns with the same id and section as source `restored`. A save without it loads with empty beliefs; the stock
  memory pool loaded from `.scop` gives no stalker a belief.
- `AIT1` - the session table of radio traits (rolled or not, carries a radio), for every NPC that has been online, so
  the trait survives loads and offline periods. A save without it rolls every NPC once at its next online spawn.

Awareness, suppression, morale, fatigue, roles, search plans and messages in flight are transient and reset on load.

## Lua API

Game object methods (stalkers; any other object returns the neutral value), one binding per name:

| Method | Returns |
|---|---|
| `npc:ai_awareness(obj)` | 0..1 (awareness / identify) |
| `npc:ai_belief_source(obj)` | source, 0 = none (table below) |
| `npc:ai_belief_error(obj)` | metres at observation, -1 = no belief |
| `npc:ai_belief_age(obj)` | ms, -1 = no belief |
| `npc:ai_belief_position(obj)` | vector, (0,0,0) = no belief |
| `npc:ai_known_distance(obj)` | metres, -1 = unknown |
| `npc:ai_enemy_sees_me(enemy)` | bool estimate |
| `npc:ai_inform(obj, pos, err, source)` | - (an honest scripted report at `pos`; `seen` or `none` becomes `script`) |
| `npc:ai_has_radio()` | bool |
| `npc:ai_is_leader()` | bool (the AI leader: highest band alive) |
| `npc:ai_morale()` | 0 panicked, 1 worried, 2 steady, 3 confident, 4 heroic, 5 surrendered |
| `npc:ai_fear()` | 0..1 |
| `npc:ai_role()` | 0 opportunist, 1 suppressor, 2 flanker, 3 grenadier, 4 rearguard |
| `npc:ai_suppression()` | 0..1 |
| `npc:ai_searching()` | bool (a lost-contact search, or walking to investigate / covering a mate who does) |
| `npc:ai_profile(key)` | the resolved profile value as a string |
| `npc:ai_weapon_class(item)` | `pistol`, `smg`, `shotgun`, `rifle_old`, `rifle`, `mg`, `dmr`, `launcher`, or empty |
| `npc:ai_danger_near(pos, radius, since)` | the type of the nearest danger the NPC holds within `radius` of `pos`, perceived at or after `since` (`time_global`), else -1 |

Sources (0-based): none, seen, heard_shot, heard_step, heard_reload, heard_item, heard_voice, hit, near_miss,
muzzle_flash, flashlight, report_voice, report_hushed, report_radio, report_gesture, report_pda, report_hq, witness,
script, restored.

Globals: `level.ai_noise(pos, loudness, kind, who)` with `kind` one of `step`, `landing`, `reload`, `grenade_draw`,
`item`, `torch_click`, `impact`, `voice`, `shot`, `explosion` (loudness 1.0 = a sprint on neutral ground; `who` may be
nil: an unattributed noise is a possible enemy to everyone who hears it); `level.ai_line_vis(from, to)` (the vision
transparency product of the AI's full-test static line between two points, 0 blocked .. 1 clear; a line counts as clear
above `[ai_awareness] opaque_below`; outside the ray budget); `level.ai_darkness()` (0 day .. 1 night, as the light
sense reads it); `ai_stat(key)` (a counter's current value); `ai_feature_enabled(name)`.

Reports: `dead_air_x64_ai.subscribe_report(fn)` / `unsubscribe_report(fn)`; `fn(receiver, target, kind, channel,
sender)` is called for a delivery, at most once per receiver and target per 2 s, with the kind and channel indices
above, from a once-per-frame dispatch (at most a frame after the delivery, never inside an NPC's update). It is not a
`RegisterScriptCallback` name: DA's callback table is a closed local, an unknown name is dropped silently.

## Console diagnostics

None of these is saved to `user.ltx` or shown in a menu; they are QA parameters of a live feature.

| Command | Effect |
|---|---|
| `ai_feature <name> <0/1>` | switch one part at run time; `all` switches every part |
| `ai_features` | list the parts and their state |
| `ai_stats` | print every non-zero counter (a global line and one line per community), then reset |
| `ai_log_period <ms>` | print `ai_stats` every period; 0 = off (default) |
| `ai_trace <0/1>` | per-NPC `[ai] trace` lines: `drop` (engine combat left while its enemy is alive and believed: the belief's source and age, the own sighting's age, the planner that runs now, combat registration, the agent's and the enemy manager's verdicts, the script's cached answer) and `handover` (the search handover: the enemy's age, the profile's delay, whether a search runs). Costly: a `drop` line asks the enemy manager (the script callback) and writes a log line, 1-11 ms counted in that NPC's search part; a run with the trace on measures no think time |

Feature names: `knowledge`, `awareness`, `concealment`, `full_los`, `hearing`, `noise`, `light`, `near_miss`, `comms`,
`radio`, `doctrine`, `cover_score`, `roles`, `suppression`, `grenades`, `reaction`, `settle`, `morale`, `search`,
`scripts`. A part switched off falls back to the stock path; beliefs keep being recorded, so switching it back on is
seamless.

Output: `[ai] t=<s> dt=<s> frames=<n> key=value ...` and `[ai] c=<community> key=value ...`, a long table on several
lines with the same prefix (under 1000 characters each: the log file keeps 1023 characters of a line and drops its
line break). `p_*_us` are microseconds per second of game time (ms per frame = value x dt / frames / 1000).

| Part | Counters |
|---|---|
| core | `npcs`, `squads`, `profile_default` (NPCs on the default profile), `rays`, `rays_deferred`, `p_total_us`, `p_max_us` (worst single NPC update), `p_max_pre_us`, `p_max_know_us`, `p_max_comms_us`, `p_max_morale_us`, `p_max_tactics_us`, `p_max_search_us`, `p_max_squad_us` (worst of each part of an NPC update: what `p_max_us` is made of), `p_max_comms_tx_us`, `p_max_comms_rx_us` (the comms part's sending and taking in; inside them `p_max_comms_snd_us` the sound starts, `p_max_comms_desc_us` the spot descriptions, `p_max_comms_msg_us` one delivery's handlers), `p_max_flank_us` (a flank route search step), `p_max_sq_comms_us`, `p_max_sq_tactics_us`, `p_max_sq_morale_us`, `p_max_sq_search_us` (each lane's part of the squad step) |
| knowledge | `k_seen`, `k_heard`, `k_hit`, `k_report`, `k_rumour`, `k_restored`, `k_forgot`, `k_aim_fallback`, `k_blind_shot` (a shot at a target not in own sight outside suppression; must stay 0), `k_candidates`, `k_callback` (calls of the script's enemy callback), `p_know_us`, `p_know_cand_us` (its enemy-candidate part) |
| senses | `s_identified`, `s_ident_ms`, `s_suspicious`, `s_noise_step`, `s_noise_reload`, `s_noise_shot`, `s_noise_shot_sil`, `s_flash` (flashes processed), `s_flash_q` (queued for an observer), `s_flash_x_period`, `s_flash_x_range`, `s_flash_x_view`, `s_flash_x_seen`, `s_flash_x_los`, `s_flash_x_wait` (the first rule that rejected a flash: the shooter's flash period per round, beyond the range at this darkness within the night's reach, out of the view cone, the shooter already in sight, no line to the muzzle nor the head, expired waiting for rays), `s_torch`, `s_near_miss`, `s_occluded`, `s_occlusion_rays` (new occlusion rays), `s_heard_err`, `s_retrace`, `p_sense_us` |
| comms | `c_sent_<ch>`, `c_recv_<ch>` (voice, hushed, radio, gesture, pda, hq), `c_lat_voice_ms`, `c_lat_radio_ms`, `c_lat_min_ms`, `c_rep_err`, `c_net_lost`, `c_witness`, `c_relay`, `c_radio_npcs`, `c_radio_violation` (must stay 0), `c_deferred` (work left for the next update by the per-update shares), `p_comms_us` |
| tactics | `t_shots`, `t_shots_supp`, `t_shots_oob`, `t_react_ms` (identification to the first command to fire at him over an unbroken sight; fire-on-order holds and out-of-band targets are left out), `t_react_due_ms` (the reaction the gate waited for, same samples), `t_react_base_ms` (its rolled `react_ms`), `t_cover_ms`, `t_combat_ms`, `t_open_m`, `t_flank`, `t_flank_role` (flank orders taken), `t_flank_noroute` (dropped: no covered route), `t_alt_advance` / `t_alt_grenade` / `t_alt_hold` (the leader found no covered flank route and chose this instead), `t_suppress`, `t_fallback`, `t_ambush`, `t_advance`, `t_grenade`, `t_return_fire` (exchanges started), `p_tactics_us` |
| morale | `m_worried`, `m_panicked`, `m_retreat_orderly`, `m_retreat_scatter`, `m_relocate`, `m_surrender`, `m_leader_known`, `m_casualty_known`, `m_recovered`, `m_help_shout`, `p_morale_us` |
| search | `r_search`, `r_spots`, `r_cleared`, `r_found`, `r_giveup`, `r_giveup_spread_ms`, `r_quiet`, `r_investigate`, `r_torch_on`, `r_handover` (search handovers of a lost contact), `r_drop` (engine combat left while its enemy was alive, believed and not given up), `r_plan_deferred` (spot plans left for a later update), `p_search_us` |

## Performance budget

| Item | Budget |
|---|---|
| all AI code (`p_total_us`) with 16+ online stalkers in combat | average <= 0.5 ms per frame; `p_max_us` <= 0.4 ms for one NPC update |
| each part | <= 0.15 ms per frame (knowledge, senses), <= 0.1 ms (others) |
| new rays | 48 per frame, round-robin, deferred beyond; muzzle flash at most one event per shooter per 500 ms |
| memory | <= 4 KB per NPC, <= 2 KB per squad |
| Lua | wrappers throttled per NPC (>= 250 ms), no loops over all online stalkers |

## Measured results

Final test stage, 29.09.2026 (nine rounds of runs; the four runs of K12: the faction fights `r2m`/`r2n`, the
fairness and night trial `r1k` and the wall and search scenario `r4a9`; last K13's two fairness and night trials
`r1l`/`r1m` for the muzzle flash and the pre step), Cordon (`foliage` save), up to 70 online NPCs
besides the scenario squads (since K12 the R1 and R4a probes keep the level's own creatures around them offline, R2
those near its fights); scenarios and criteria in `TEST_MATRIX.md` ("NPC AI"). Each row gives the latest measurement.

| Check | Result |
|---|---|
| No blind shots (`k_blind_shot`) | 0 in every run after the fire-loop gate (338 rounds in the first faction fight after it; 0 in the four runs of the final round); 4 before it |
| No fire through walls | all five shooters (three bandits, two mercenaries) saw the target before he stepped behind a wall; after it no bandit fired 0.75 s or more after losing sight of him, no mercenary fired unseen, nobody hit him unseen (five times, the last in K12's `r4a9`). The mercenaries' honest suppression at a wall was not exercised (they held fire) |
| Heard-shot estimates | 17.9-21.4 m of error for a burst 100 m away in K13 (earlier 12.6-19.4 m); a suppressed burst 32.0 m (2.06x; earlier 1.8-2.0x) or not heard at all (both K13 runs); no report ever more precise than its sender's belief |
| Steps | running at 15 m behind a calm loner made him suspicious, crouching at 15 m did not (five runs, the last two with the level's own NPCs kept away so the loner stayed calm) |
| Night | the unlit crouched target at 40 m was not identified in 10 s of night; with its torch on and pointed at the observer it was identified 0.62 s after in the last run (0.12-0.47 s in K11-K13). A trial with the lamp side-on and the target's head behind a bush took 6.2 s: the glow follows the beam's angle, and foliage conceals. (A first attempt was void: DA hands its NPCs a torch at night, and the target was found by its own) |
| Muzzle flash | a `muzzle_flash` estimate 0.11 s after the shot on the K6 build. K13 (before the last fix): at 120 m the flash was processed and its cue made the observer identify the shooter 0.10 s after the round; at 150 m, the observer facing the shooter 0.1 degrees off its gaze with a clear line to his weapon, all 8 observers the flash was queued for were rejected on the line to the weapon's exact fire point. Fixed (the shooter's head when the muzzle is hidden), not measured |
| New game | the start level reached from fake_start; 27 online stalkers, every one on its community's profile |
| No hive mind | another squad in the same smart terrain and agent group never learned of the actor; squad mates learned by voice 2.74-3.97 s after the hit; the far mate only by PDA (17.7-26.9 s after it) |
| Radio | the far squad member got the radio report 3.00-4.23 s after the hit (window 1.6-5.5 s; 3.36 s in K12), the mates beside the victim the hushed words or the radio 2.3-3.6 s after it; the second army squad got the HQ report 48-65 s after the first report (51.2 s in K13), nothing before |
| Latency floor | shortest delivery 688 ms in the last faction fight; no report younger than 250 ms |
| Leader death | known only to the 3 who saw it; the next leader took over 0.1 s later |
| Save | 268 beliefs restored; every NPC's radio trait equal before and after the load |
| Radio is physical | `c_radio_violation` 0 in every run; every soldier a carrier, no bandit; 13-15 of 24 carriers in the faction fights |
| Reach | every voice receiver within 60 m of its speaker; 8-31 army radio receivers beyond 60 m; 23-81 Duty radio receivers in the faction fights; no bandit radio |
| Doctrine | no round beyond the band in the last six faction fights (`t_shots_oob` 0; the weapon's own gate refuses a round at any living target beyond it, dogs included). Open ground crossed by short-gun holders toward the enemy: final build 4 m over 2 holders and 20 m over 8, before it 4-9 m over 5-9, against 15 m per holder allowed; 92 m (88 by one army holder) before the per-fight budget and the covered search steps |
| Cover share | the share of the time a member in contact with its paired enemy (seen within 3 s) stands covered from him: last faction fight army 0.04 (its spot on open ground), bandits 1.00, mercenaries 1.00, Duty 0.98. Before the K12 fixes (the probe read a quarter of the directions wrong, an outranged ambush could not back off into cover, the cover score's aggression pull was inverted) the army's share ranged 0.09-1.00 over eight fights and the bandits' 0.02-1.00: the random spots of the layout decide |
| Coordination | the mercenaries now suppress and flank (last fight 1 suppression, 1 flank, 7 exchanges of return fire; their leader tries a second man for a covered route when the first has none). Suppression by order works (the three fights before: army 4, 1 and 2 suppressions, Duty 2, 3 and 1, Monolith 2, 6 and 1); in the last fight the army's leader ordered it, but the bandits stayed in the army's sight and the ordered man fired aimed rounds, so neither a suppression round nor a flank came. Flanks, every one an order taken along a covered route: army 1, 2 and 2, Duty 1, 2, 1 and 2 per fight where the ground offers a route. Return fire from held cover: 1-7 exchanges per faction |
| Morale | bandit novices scattered within 10 s of losing two of four; Monolith never retreated; the army never routed; the losing mercenaries withdrew in order (1-3 retreats, no scatter) |
| Search | wall scenario (K12): after the target stepped behind the wall every shooter searched; the mercenary pair searched together and quietly, 8.1 m apart at the median (15.2 m with the partner watching from 10 m in K10); the bandits called out (12 voice messages); with the target moved out of every search ring the bandits gave up 5.1 s apart (73.5 and 78.6 s), as their stagger says. 24 spots visited |
| Identification | 9 of 12 identifications of the actor inside their band (calm x2.5); three off-axis ones 6-49 s |
| Reaction (`t_react_ms`) | medians in the last faction fight against the reaction the gate required (`t_react_due_ms`): army 0.79/0.78 s, mercenaries 0.81/0.78, Monolith 0.81/0.78, Duty 0.94/0.89, bandits 1.67/1.66, loners 1.15/1.14; every rolled base inside its profile rows (a fight with a mutant is stock combat and not counted) |
| Think time | first faction fight: 1.45 ms per frame, worst NPC update 38 ms. Final round, last faction fight (62 NPCs online): 0.185 ms per frame (knowledge 0.053, senses 0.074, comms 0.011, tactics 0.054, morale 0.019, search 0.002: every part within its budget), worst NPC update 0.44 ms once (a senses pre step; now capped by `pre_budget_us`, not measured), median of the 5 s maxima 0.23 ms, 1 of 17 periods over 0.4 ms. K11's fights had 0.28-0.39 ms per frame, knowledge 0.195 and senses 0.163 in the hotter one, updates up to 1.34 ms in 13-14 of 17 periods: the Lua enemy callback, occlusion rays and the start of sounds, all cut in K12. Night trial: the stimulus step 0.27 ms at worst (1.19 ms before `pre_rays`); with `pre_budget_us` 0.22-0.24 ms at worst in two fairness runs, never over 0.4 (it bounds the step after its first stimulus: 150 us plus the item it lets start); worst NPC update of those runs 0.30 and 0.48 ms (a knowledge part) |

## Known limits

- **Think time: one update over 0.4 ms in the last fight.** Every part now stays within its per-frame share
  (knowledge 0.053, senses 0.074 ms per frame in the last faction fight, 0.185 in all) and the worst updates of K11
  are gone (the Lua enemy callback and the occlusion rays of distant NPCs by level of detail, the start of sounds at
  the root, flank steps and spot plans by time budgets). One update of the last fight reached 0.44 ms: a senses pre
  step of 0.41 ms, now capped by `pre_budget_us` (150 us after its first stimulus), not measured.
- **`see` is bounded.** An NPC's `see` / `visible_now` of a live creature holds at most `sight_hold_ms` (0.75 s, the
  staleness the wall check allows) after its own last sighting; knowledge off keeps the stock rule. Corpses keep the
  stock rule too: the "flicker" once suspected in the wall scenario was the target's corpse.
- **Flanking and suppression need the fight to ask for them.** A flank goes only by a covered route against an
  enemy out of sight or dug in, and suppression fire only at an enemy just lost from sight; an army squad that keeps
  its enemy in sight fires aimed rounds instead (last fight: the order went out, no suppression round, no flank).
  Cordon's open fields often offer no covered route; the leader then tries a second man, flushes, advances under
  suppression or holds.
- **Search pairs** search together and quietly (median 8.1 m apart with the covering partner at `overwatch_dist`
  8 m); searchers give up staggered (5.1 s apart in the last run).
- **Cover share depends on the ground.** One random layout measures the spot more than the choice: in the last
  fight the army's spot lay on open ground (0.04 in contact) while the bandits' was covered (1.00); earlier the army
  ranged 0.09-1.00 over eight fights.
- **Not measured on the final build**: the muzzle flash's last fix (K13 isolated the failure: the line went to the
  weapon's exact fire point, hidden from every observer of a shooter in clear view; the line now falls back to the
  shooter's head) and the mercenaries' honest suppression at a wall.
- `ai_feature all 0` restores the stock decisions at run time, but beliefs keep being recorded: it is a behaviour
  switch, not a zero-cost off switch.
- **Reaction** is measured from identification to the first aimed-fire decision; distance, peripheral, fear and
  fatigue add-ons are part of it, so a frightened squad at 55 m reacts slower than its `react_ms`. A waiting aimed
  command goes the frame the reaction ends and the head faces the target (before, it waited for the next brain update,
  up to a schedule period, longest far from the player: the army came 1.40x its gate, now 1.16x).
- Stock DA NPCs saw 20-38 m in clear weather; stalkers now see to the fog, so open ground is more dangerous than
  before.
