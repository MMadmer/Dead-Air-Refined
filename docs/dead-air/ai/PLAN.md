# Human NPC AI: implementation plan

Status: binding plan for the parallel implementation of the human NPC AI rework (28.09.2026).
Inputs (read all of them before starting a lane): `D:\Games\Dead Air\_work\ai-npc\research\games.md`,
`lore.md`, `engine.md`, `cheats.md`, `scripts.md`. Where a research file and this plan disagree,
**this plan wins** (resolutions in 1.4). Research file references below are written `cheats.md W1`,
`engine.md 12.2`, `lore.md 8.2` and so on.

The task, in the user's words (translated): human NPCs are dumb and slow, mutants are fine. Make
enemy AI noticeably smarter, learning from the games famous for their AI (F.E.A.R., Halo, ...).
Behaviour follows each faction's lore; ranks, equipment and weapons matter; NPCs want to survive
and win and flee when clearly outmatched or after heavy losses. No cheating: no hive mind, no
aimbot through walls, but they must challenge the player. **Addendum (binding):** communication is
physical, never telepathic - radio where a faction's lore and equipment give one (a per-faction
and per-rank trait, decided per NPC at spawn and stable for its life), voice shouts with a short
loudness-dependent range, gestures with line of sight; a report carries the sender's position
estimate at observation time with an error, never the target object or live tracking; nobody
learns anything on the same frame; the player's noises are heard through the sound system with
distance and occlusion; and all such small details. Faction lore drives everything.

## 0. Rules for every lane

### 0.1 Project rules (repeated so a lane agent needs nothing else)

- Read `PROJECT_RULES.md` once. Nobody asks the user anything or waits for a permission: decide
  within these rules and write the decision and its reason into the lane status file (0.4).
- No commits, no git worktrees, no `git checkout/stash/reset/clean`, no reformatting of whole files.
  The tree carries other uncommitted work (mod catalog, options pages, animation scripts, shader
  work): never revert or touch a file outside your lane's ownership (3.2-3.4).
- Every **new** engine source file starts with `// Copyright (c) 2026 XFined-Ray` and
  `// Released under the MIT licence, see License.txt` (after a BOM if the file has one). Not in
  modified upstream files, build files or `.def` files. Lua and ltx files follow the overlay's
  convention (no licence header; an ltx starts with a `;` comment saying what it is).
- Code comments in English, brief, only where the reason is not obvious. Language sugar where it
  costs nothing. Raw pointers are tested by implicit bool conversion, never against `nullptr`. Style
  of the file; 4 spaces, UTF-8, final newline, <= 120 columns. New lookup maps default to
  `xr_flat_hash_map`.
- **Never hardcode another mod's names in code.** Faction, rank, weapon, material, sound and
  character knowledge lives in `dead_air_x64_ai.ltx` (2.3). Base Dead Air script modules
  (`xr_danger`, `xr_npc_extra`, `xr_combat_ignore`, `sr_light`, ...) may be named in our Lua, always
  behind an existence check; DAR2 names never appear in code (data may name them).
- **Mutants are untouched.** Every engine change is gated on the human stalker path
  (`m_stalker` / `smart_cast<CAI_Stalker*>`); monster code paths stay bit-identical. Zombified
  stalkers are stalkers: they get the perception/knowledge rules and a "dumb" profile.
- **Etalon order.** The research already ran IDA x86 -> CoC vs DA 1.0 diff -> x64 for every path in
  `cheats.md 2` and `engine.md 3.1/10.1`. A lane that changes a path not covered there does that
  check first and records it. Every behaviour change in this plan is a deliberate change the task
  asks for (PROJECT_RULES 3, "the task explicitly requires changing that behaviour"); the status
  file names the reference behaviour it replaces.
- Saves: `.scop`/`.scoc` formats do not change (6.1). New persistent state only in the `.scov`
  chunks `AIK1` (2.10, beliefs) and `AIT1` (2.11, radio traits). Lua: public names and call forms
  stay; semantics change only where 6.2 says so; new API is additive, one binding per name (luabind
  picks nothing on an ambiguous overload set).
- No Lua concurrency; no write to a game object from another thread (the bullet manager may run
  off the main thread: near misses go through a main-thread queue, 4.3 S6).
- Never touch `D:\Games\OriginDeadAir\Dead Air Copy`. Never deploy into `D:\Games\Dead Air` (the lead
  does, from the handover in 7.7). Never launch the game during implementation (0.1b).

### 0.1b Process: implementation first, all tests at the end (the user's rule)

"First everything is written, tests only at the very end when all the work is done." Lanes only
WRITE code and data. They compile (0.3) to keep the tree building, and nothing else: no stand runs,
no game launches, no perf batches. The one test stage (section 7) runs after every lane reports
done, within the budget of 0.1a; decisions that need a measurement (tuning, keep/remove) are made
there. Every feature is written so that it can be switched at runtime (`ai_feature`, 2.8) and its
cost is visible in its own timer, so the final stage can attribute effects without extra builds.

### 0.1a Test budget (the user's rule, overrides everything below)

- One stand run = at most ~90 s after the level loads; the scenario prints its verdict and a done
  marker, and quits.
- At most **8 stand runs** for the whole final stage (7.3 uses 7), plus at most **2 tuning reruns**
  (7.6). Several checks share one run; no background batches; no waits longer than 5 minutes.
- Performance is read from the per-lane timers of the runs that happen anyway (one pass), not from
  on/off pairs.

### 0.2 Shared tree protocol

- One working tree `D:\Games\Dead Air\DeadAir-x64`, build dir `build\ninja-x64`, output
  `bin\x64\Release`. Stand (final stage only): `D:\Games\Dead Air\_qa\lightdiag`.
- Every file has exactly one owner lane (3.2-3.4). A lane edits only files it owns, plus:
  - **marker blocks** the owner pre-created for it in a hotspot file:
    `// [ai:<lane>] begin <name>` ... `// [ai:<lane>] end <name>` (C++), `; [ai:<lane>]` (ltx),
    `-- [ai:<lane>]` (Lua), `# [ai:<lane>]` (CMake, PowerShell, Python). Edit strictly between
    your own markers;
  - **hook lines**: when a needed call site was not pre-placed, a lane may insert **one new line**
    (a call into its own file, tagged `// [ai:<lane>] hook`) into a hotspot file. It never changes or
    moves another line and lists the insertion in its status file.
- Read a hotspot file right before editing it; if an Edit fails because the file changed, read it
  again and redo the edit. Never write a hotspot file with Write.
- Lane state lives behind one pointer per lane in the shared per-NPC and per-squad structs (2.2),
  declared by `core` at K1 and defined in the lane's own header. Shared headers change once (K1),
  never with every lane edit.
- Lane tags: `core`, `know`, `sense`, `comms`, `tactics`, `morale`, `search`, `script`.

### 0.3 Build

- Own translation units: `tools\build\Check-Compile.ps1 -File <file>` (private output, no lock;
  `-Template` for a file CMake does not list yet). Use it after every edit burst.
- Linking the modules: `. tools\qa\dx12\Dx12Locks.ps1; Invoke-Ninja 'xrGame xrEngine'` (takes
  `ninja.lock`; add `xrCore` only if a lane touched it - none should). At most one link per lane
  every ~20 minutes; `core` links after every merge wave (4.1 K3).
- The tree must compile after every edit burst of a lane. A lane that breaks the build in its own
  files fixes or backs out within 10 minutes. A break in another lane's file: note it in that lane's
  inbox and continue with work that needs no link; `core` may fix a trivial compile error (include,
  typo) in any file after 30 minutes and notes it in the owner's inbox.
- Release warnings-as-errors applies; no new warnings in touched files.

### 0.4 Status, inbox, done

- `D:\Games\Dead Air\_work\ai-npc\lanes\<lane>.md`: current milestone, hook lines inserted,
  decisions taken (with the reference behaviour replaced), open blockers. `<lane>.inbox.md`:
  messages from other lanes. Read the inbox at every milestone.
- A milestone is **reached** when its work is written, its files compile (`Check-Compile`), the
  modules link, and the status file says so. Nothing is "tested" before section 7.
- Lane **done**: every milestone reached; no `TODO`/`FIXME`/debug output in owned files; every
  counter of 2.8 that the lane owns is emitted; the lane's text for the spec (7.7) is in its status
  file.

### 0.5 Deciding without the user

Order of precedence: the user's words (task + addendum, the test rules) -> project rules (0.1) ->
this plan -> the research files -> the lane's judgement, written down. A numeric value this plan
does not give goes into `dead_air_x64_ai.ltx`, never into code.

## 1. Goals, fairness, contradictions

### 1.1 Goals

1. Human NPCs perceive, remember and communicate like people: per-NPC knowledge, graded awareness,
   hearing with distance/occlusion/surface, light and muzzle flash, reaction time, estimates with
   error; reports travel by voice, radio, gesture or PDA with latency.
2. Tactics follow weapon, rank and faction: engagement bands (no shotgun duels at 100 m, no stroll
   across open ground), weapon-aware cover scoring with path exposure, peeking discipline,
   suppression + flanking roles for trained squads, grenades to flush, fall back to better cover.
3. Morale: NPCs want to live - fear, losses, leader death, being outmatched and wounds lead to
   falling back, orderly withdrawal, scatter or surrender by faction; Monolith fanatics push and
   never rout.
4. Search after lost contact from the last known or reported position: spread search, pairs for
   trained factions, quiet searching, staggered give-up.
5. Every cheat of `cheats.md` and `scripts.md 4` removed at its root (1.5).
6. Everything data-driven by faction x rank band x weapon class, patchable by XMS modules through
   `.ltxp`; unknown or modded communities get sane defaults.

### 1.2 Non-goals

- Mutant AI (untouched), ALife offline simulation, squad travel and smart-terrain jobs.
- The accuracy ladder `dummy_1..dummy_7` (DA decision) stays the accuracy floor; profiles act on top
  of it (reaction, settle, suppression, fatigue). The GSC dispersion state inversion
  (`cheats.md W10`, stock and identical in DA 1.0) is not changed.
- No hit-probability tokens, no rubber-banding, no difficulty-dependent perception: challenge comes
  from behaviour.
- No new audio or animation assets in this batch (content goes through bundles, PROJECT_RULES 4).
  Callouts reuse the shipped voice sets; the bark and radio sound mapping is data, so a later
  content release can add lines without code.
- Reputation effects without witnesses (`cheats.md H11`, goodwill on kill) are a design topic, not
  perception; unchanged. Companion vision assists (`scripts.md C7`) stay (they help the player).
- No new player-facing option: all AI switches are console-only QA parameters (1.4 C2).

### 1.3 Fairness rules (every lane is bound by them)

| Id | Rule |
|---|---|
| F1 | **Perception only.** An NPC acts on a target only through its own senses (vision, hearing, hit, near miss, light, muzzle flash) or a physical message (2.6). |
| F2 | **Per-NPC knowledge.** Every stalker keeps its own beliefs (2.5). No shared memory pool is read for a stalker's decision; the agent manager's pooling is gone. |
| F3 | **Estimates, not truth.** Every non-visual observation and every message carries a position estimate with error (distance, time, visibility). The target's real position is read only while the target is in the NPC's own vision this update. |
| F4 | **No aim without sight.** Aim point and lead come from the belief when the target is not visible; no head lock or perfect lead through walls. |
| F5 | **No fire without line of sight**, except honest suppression: a profile with `supp_window_s > 0` may fire bursts at its **own** last-seen position (never refreshed from truth, never at a reported-only position) within that window, with an automatic weapon or rifle; counted as suppression. |
| F6 | **Reaction time.** Identification takes an awareness meter (sense); the first aimed shot waits `react_ms`; aim settles over `settle_s`. |
| F7 | **Physical communication** by the addendum: radio only between NPCs who carry one, on their own net (a non-carrier learns net traffic only from a carrier's relay by voice or gesture); voice and hushed voice through the real sound system (audible to the player and to enemies); gestures need line of sight and short range; PDA text is slow. Involuntary cries (pain, death, wounded moans) carry no target estimate. The target id binds a report to its entity; its position is never read from the object. |
| F8 | **Latency.** Nothing is learned on the same frame: notice + decide + speak/key + hear + understand, with a hard floor of 250 ms end to end (2.6). |
| F9 | **Symmetry.** Loud callouts are audible to the player; radio squelch is audible near carriers; muzzle flash and torch rules apply to NPCs and the actor alike. |
| F10 | **No omniscient helpers in scripts** (weight/torch detection without LOS, corpse -> live actor position, hit -> whole squad knows, first contact -> faction within 100 m knows). |
| F11 | Unknown data never crashes and never cheats: a missing profile falls back to `ai_profile_default`; a missing feature path falls back to stock behaviour, not to omniscience. |

### 1.4 Contradictions and gaps and their resolution

| Id | Sources | Conflict or gap | Resolution |
|---|---|---|---|
| C1 | PROJECT_RULES 6 / the user's test request | "No diagnostic code or console commands in the final batch" vs "each lane emits log counters" | Permanent QA diagnostics of a live feature, like `r__gpu_log`/`r__texload_stats` of the DirectX 12 work: `ai_stats`, `ai_log_period`, `ai_feature` (2.8). Counters are plain increments, printing is off by default, nothing leaves the log |
| C2 | implement-then-test ("every feature switchable") / PROJECT_RULES 1 ("no per-feature switches") | Runtime A/B vs no player options | `ai_feature <name> 0/1`: console only, not saved, not in any menu, default all on |
| C3 | `cheats.md W7` / `engine.md 3.1` | DA x86 and DA 1.0 **assign** the visibility value (no meter) - a DA decision | Replaced by a dt-normalised awareness meter (sense S1) because the task asks for fair detection and reaction; the Lua hook `visual_memory_manager.get_visible_value` is still called and its value is read as intensity; mutants keep the stock path |
| C4 | `scripts.md 7` ("never fork base scripts") / `cheats.md 6` ("patch base DA in the overlay") | How to change base DA scripts | Wrap from our new `dead_air_x64_ai.script` by default. Overlay copies only for `scripts/visual_memory_manager.script` (the function body itself is the bug, W8), `scripts/xr_combat_ignore.script` (the body of `is_enemy` reads true positions, C25) and `configs/ai_tweaks/xrs_kill_wounded.ltx` (data read through its own ini). Never a DAR2 file. Loose `gamedata` and XMS modules still override both copies |
| C5 | `scripts.md 1` / `src/xrEngine/x_ray.cpp:211-230` | `.ltxp` patches exist only for XMS modules, and our overlay ltx files are merged **after** `XMS::ApplyConfigStage`, so a module patch would be overwritten | `dead_air_x64_ai.ltx` is merged into `pSettings` **before** `XMS::ApplyConfigStage` (core K1); module `configs/xms/*.ltx` sections and `.ltxp` directives then land on top. The two existing overlays keep their place |
| C6 | `games.md M20` (Monolith "psi link") / the user | Telepathic channel for Monolith | Rejected: Monolith has voice and radio only (by band) |
| C7 | `games.md 0` / `engine.md 15` | "Shotgun cover must be within 5 m" | `engine.md` is right (the band only forbids moving the wrong way); doctrine as in 2.7/4.5 |
| C8 | `scripts.md C10` / `cheats.md 8` | `kill_if_not_visible` as a wall path | Not a wall path; no change |
| C9 | `games.md 4`, `engine.md 11` (new planners) / `engine.md 12.2` (save stream) | New sub-planners and storage-backed properties shift the planner save stream | Only plain operators and plain evaluators are added; new world properties are evaluator-backed, never stored in a planner's `m_storage`; no new `CActionPlannerAction` |
| C10 | `games.md M2` (new awareness levels via danger types) / `danger_object.h`, `xr_danger.script:24-39` | Danger type enum is exported to Lua and filtered by `xr_danger` | No new danger type. Suspicion uses `eDangerTypeEnemySound` / `eDangerTypeAttackSound` with the right perceive type (`visual`/`sound`/`hit`) and the **estimated** position |
| C11 | `lore.md 0.7` | No NPC carries grenades (DAR2 comments every `extra` out), so grenade tactics are dead | Grenade issue at spawn from profile data (`grenade_issue`, script L4), in its own ltx keys so balance can empty it without touching code |
| C12 | `engine.md 3.2` / `step_manager.cpp:429-456`, `material_manager.cpp:81-127` | Whether footstep `.ogg` files carry an AI game type is unknown | Footsteps become explicit noise events (sense S4) with gait, surface and load; one source per stimulus kind: engine step, reload (`WEAPON_RECHARGING`) and item-use sound events from humans are ignored by the new hearing because the bus carries them, so nothing is counted twice |
| C13 | addendum (radio) / content | No radio squelch asset is verified in the shipped archives | `[ai_comms] radio_sound` names a shipped sound (comms picks one by listing the archives read-only); empty = no squelch, the report is still delivered and the voice line plays at radio volume |
| C14 | `cheats.md H14` | On load every faction member gets every saved memory | Beliefs persist per NPC in `.scov` chunk `AIK1`; a save without it starts with no beliefs (never seeded from the shared pool) |
| C15 | `cheats.md 6` / Lua API | `make_object_visible_somewhen` gave the exact live position | For stalkers it becomes a **rumour**: a belief at a snapshot with `[ai_errors] script` error, never a sighting. Documented in `MODDING.md` |
| C16 | `engine.md 3.4` / addendum | Agent group = one community per smart terrain, not a squad | The comms "squad" is the sim squad (`CSE_ALifeMonsterAbstract::m_group_id`); its AI leader is the highest band alive (C22), the engine's `CSE_ALifeOnlineOfflineGroup::commander_id()` (lowest id) only breaks ties; an NPC without a sim squad is a squad of one. Voice, hushed and gestures still cross squads (2.6) |
| C17 | `scripts.md 3.3` / our knowledge rule | `xr_npc_extra` spreads hostility (goodwill -1000) to a whole faction within 100 m of the actor | Hostility spreads only with knowledge: our script unsubscribes the broadcast and sets the same goodwill in the report handler for NPCs that were actually informed or perceived the attack (script L1) |
| C18 | `engine.md 1` / F8 | Vision rays run every second schedule tick, so "visible" can stay true up to ~1.25 s after LOS is lost | A combat NPC re-traces its selected enemy on the off tick (one ray, sense S2) |
| C19 | `scripts.md 2` | `npc_on_update` fires twice per throttled tick (base DA bug) | Our subscribers are idempotent and throttled per NPC |
| C20 | `lore.md 3.1` | `csky_4` has experienced ranks on the best tier | Band comes from rank; exceptions through `[ai_profile_by_character]` data (`sim_default_csky_4` -> `ai_profile_csky_r4`) |
| C21 | `xr_combat_zombied` | Zombies fight with a script combat scheme, not the planner | Tactics and morale leave zombie combat alone; perception, knowledge and "no comms" apply |
| C22 | 2.3.4 (old) / `alife_online_offline_group.cpp:356-361` | The engine's squad commander is `m_members.begin()` of a sorted `AssociativeVector`, i.e. the **lowest object id** alive, not the highest rank; a radio bonus "for the commander at spawn" re-rolls at every online switch and load, so a successor gains the dead leader's radio | Leader for AI purposes = the alive member with the highest band (ties: higher rank value, then the engine commander order), `SquadState::leader`. The radio trait is rolled **once per NPC lifetime** and persisted in the `.scov` chunk `AIT1` (2.11), so it survives offline periods and loads unchanged |
| C23 | `dummy_silent` (DAR2 `m_stalker.ltx:1897-1925`) | Silent tiers blank **every** voice key, including `sound_death`, `sound_hit`, `sound_wounded` and the grenade alarms, so the plan's death cries, help shouts and grenade warnings do not exist for mercs, sergeants+, Duty/Freedom R2+, Monolith R2+ | The hushed set (comms C3) loads these lines too: a silent-tier casualty is audible within the hushed range (involuntary, no estimate), a wounded one calls for help hushed and by radio; `dummy_silent` stays as DA decided for everything else |
| C24 | 2.9 (old) / `axr_main.script:24` (`local intercepts`), `callback_set` | `SendScriptCallback("npc_on_ai_report")` and `RegisterScriptCallback` silently do nothing for a name missing from the closed local table (`da_mod_compat.script:75-81` documents the same limit) | `dead_air_x64_ai.script` keeps its own subscriber list: `dead_air_x64_ai.subscribe_report(fn)` / `unsubscribe_report(fn)`; documented in `MODDING.md` |
| C25 | `xr_combat_ignore.script:195-250` | `is_enemy` range caps (150/100/75/60 m) measure `enemy:position()`: an NPC keeps or drops a believed enemy by the target's true distance | Overlay copy of the base DA `scripts/xr_combat_ignore.script` (DAR2 does not ship one) whose caps measure `npc:memory_position(enemy)` (the belief); the caps themselves are DA's and stay; the H9 squad-wide drop is limited in the same copy. Loose mods still replace it whole |
| C26 | `configs/alife.ltx:8-9` (`switch_distance 250`, factor 0.1) | Test layouts that put NPCs 250-400 m from the actor test offline NPCs, which have no AI state at all | Every stand-run NPC stays within 200 m of the actor (7.3) |
| C27 | 2.3.2 (old) / DAR2 `w_pm.ltx:95`, `w_kiparis.ltx`, `w_bizon.ltx` | DA puts pistols in `slot = 4` and long guns in `slot = 2`; `ammo_9x18` feeds both the PM and the Kiparis/Bizon, so "ammo prefix first, slot 1 = pistol" makes SMGs pistols | Ordered rules in 2.3.2: pistol by slot (data), then ammo families, with pistol-calibre ammo in a long-gun slot = `smg` |
| C28 | `memory_manager.cpp:154-184`, `ai_stalker_misc.cpp:196-203` | Dangers are built from shared pool entries whose position and time belong to the **last writer** of the group, and `process_enemies` copies a seen friend's danger object | For stalkers, dangers come only from the NPC's own estimates (`Know::observe/suspect`, know N2); a friend's danger becomes a witness cue (comms C4) |

### 1.5 Cheat map (every item of `cheats.md` and `scripts.md 4`)

| Item | Lane / milestone | Fix |
|---|---|---|
| W1 real-bone aim when not visible | know N3 (+ tactics T2 fire gate) | `sight_action.cpp` `predict_object_position`/`execute_object`/state 0 and `sight_manager.cpp:301-320` aim at the belief when the target is not in own vision; `stalker_kill_wounded_actions.cpp:180,275,312,318-338` and `smart_cover_loophole_planner_actions.cpp:96-104,158,171-172,186` gated the same way |
| W2 `visible_now` kept alive by mates | know N1 | Per-member own-sight time; `visual_memory_manager.cpp:193-217,724-732` read it for stalkers |
| W3 ground-truth reads in decisions | know N3 (own files), tactics T2 (combat files) | Every read replaced by `Know::known_position/distance/direction` (list in 4.2 N3 and 4.5 T2) |
| W4 oracle of the player's vision | know N4 | New `EnemySeeMe` evaluator and `enemy_sees_me()` estimate from the NPC's own perception; used by `stalker_combat_planner.cpp:143-149` (core hook) and `stalker_combat_actions.cpp:1168-1173` (tactics) |
| W5 back-face culled LOS | sense S2 | `Feel::Vision` full test for stalkers; `can_kill_entity` (`ai_stalker_fire.cpp:723`) and look-out ray (`stalker_combat_actions.cpp:622`) through `Sense::los()` |
| W6 binary foliage | sense S2 | Transparency product (`Feel::Vision` `Cache_vis`) scales the stimulus; threshold only as "fully blocked" |
| W7 instant detection | sense S1, tactics T2 | Awareness meter + reaction time |
| W8 torch math in Lua | script L4, sense S5 | Overlay `visual_memory_manager.script` without the global torch bonus and with a positive ratio floor; the engine's beam sense replaces it |
| W9 `xr_danger` weight/torch/live positions | script L2, sense S4/S5 | Unsubscribe the leak, weight -> footstep loudness, torch -> beam sense, danger reactions use the danger's own position |
| W10 head lock + perfect lead | know N3 | Aim bone and lead error from the profile (`aim_head`, `lead_error`) |
| H1 lookups ignore the squad mask | know N1 | Lookups read beliefs |
| H2 combat pooling | comms C5 | `agent_memory_manager.cpp:20-35` pooling removed for stalker groups |
| H3 exact positions materialised | know N2 | `agent_enemy_manager.cpp:394-406` no longer calls `make_object_visible_somewhen`; distribution by known distances among members who know |
| H4 seeing a busy friend | comms C4 | Witness cue (direction + prior distance, large error) instead of the friend's enemy |
| H5 callouts transfer truth, bypass hearing | sense S3 (order), comms C3 (content) | Visitor after the hearing threshold; message = the speaker's estimate, delivered after understanding |
| H6/H7 hits and gunshots exact | sense S3 | Hit -> direction cone + distance guess (exact only if the shooter is visible); gunshot -> sound estimate, no fake hit for stalkers; sound memory stores the estimate; CDB occlusion |
| H8 DA/DAR2 broadcasts | script L1/L2 | Unsubscribe `xr_npc_extra` first-contact and hit broadcasts, `xr_danger` corpse -> actor; hostility via reports (C17) |
| H9 hit drops `combat_ignore` for the squad | script L3 | Only members that perceived the attack or were told |
| H10 smart alarm | - | Unused (`gulag_general` stubs), left |
| H11 goodwill on kill | - | Non-goal (1.2) |
| H12 shared danger covers | tactics T3 | Danger locations shared only with members that heard/received it |
| H13 pool capacity evictions | know N2 | Enemy candidates come from beliefs |
| H14 load gives everyone everything | know N5 | `AIK1` |
| scripts C4-C6 | = W9/W8 | |
| scripts C8 | script L3 | Corpse detection needs sight; `be:see(npc)` "is it safe" checks use `ai_enemy_sees_me` |
| scripts C9 (1 km sight) | sense S1 | Ranges kept; the meter makes far detection slow (distance term) |
| Shared-pool dangers (C28) | know N2 | Danger objects of a stalker only from its own estimates; the pool entry's last-writer position and time are never read |
| Friend's danger copy (C28) | comms C4 | Witness cue (the friend's facing), never the friend's danger object |
| `is_enemy` truth range caps (C25) | script L3 | Overlay `xr_combat_ignore.script`: caps on the belief distance |
| Stock cover fire reads the agent's detour list (`stalker_combat_actions.cpp:765-780`) | tactics T4 | A holder covers a flanker only after a delivered `flanking`/`cover_me` message or while it sees him move |
| Morale reads unseen enemies' armour, weapon, wounds | morale M1 | Attributes sampled only while the enemy is in own vision, cached in the belief |
| Grenade at a reported-only position | tactics T5 | Grenade target = an own `seen`/`heard_*` belief |

## 2. Architecture and contracts

### 2.1 Data flow and the order inside one stalker update

```
senses (vision rays, sound events, noise bus, hits, near misses, light, muzzle flash)
   -> sense: awareness per target, estimates                       [sense]
   -> know: beliefs {estimate, source, confidence, own-sight time} [know]
   -> decisions: planner evaluators/actions read beliefs           [tactics, morale, search]
   -> comms: requests -> channel, latency -> receivers' beliefs    [comms]
```

`CAI_Stalker::shedule_Update` (`ai_stalker.cpp:935-963`, alive branch), fixed order (core places the
hooks at K1):

1. `agent_manager().update()` (existing; pooling removed by comms, distribution by know)
2. `Exec_Visibility()` (existing; sense adds the combat re-trace)
3. `process_enemies()` (existing; the friend transfer becomes a witness cue, comms)
4. **`da_ai_pre_memory(*this, dt)`**: sense drains the NPC's stimulus inbox (noises, flashes,
   torches, near misses) into estimates -> `Know::observe` / `Know::suspect`
5. `memory().update(dt)` (existing; sense's meter runs inside `visual().update`, know's lookups and
   candidate filter inside `update()`)
6. **`da_ai_post_memory(*this, dt)`**: `Know::update` (ageing, forgetting) -> `Comms::update`
   (sender decisions, due deliveries) -> `Morale::update` -> `Tactics::update` (roles, fire intent)
   -> `Search::update`; then `DaAi::squad_tick(*this)` runs the per-squad step at most every 250 ms
7. `Think()` (planner, existing)

Deliveries are processed only in step 6 of the receiver's own later update, after `t_available`;
a message sent in this frame can never be read in this frame (F8).

Other fixed points (core pre-places the hook lines at K1):

| Where | Hook | Consumers |
|---|---|---|
| `CAI_Stalker::reload` (`ai_stalker.cpp:293`) end | `da_ai_reload(*this, section)` | core (profile invalidation), comms (hushed sound set) |
| `CAI_Stalker::net_Spawn` (`:518`) end, alive only | `da_ai_spawn(*this)` | core (state, profile, radio roll), know (apply staged `AIK1`) |
| `CAI_Stalker::net_Destroy` (`:627`) start | `da_ai_destroy(*this)` | all lanes free their state; comms removes the carrier from its net |
| `CAI_Stalker::Die` (`:454`) start | `da_ai_on_death(*this, who)` | comms (net), tactics (roles), morale (squad registry) |
| `CAI_Stalker::UpdateCL` alive branch, before the object handler is queued (added 29.09, R1) | `da_ai_update_cl(*this)` | tactics (the fire gate the worker-thread object handler reads) |
| `CAI_Stalker::Hit` (`ai_stalker_fire.cpp:229`) after `memory().hit().add` | `da_ai_on_hit(*this, *pHDS)` | morale (fear), comms (`taking_fire`), tactics (cover compromised) |
| `stalker_combat_planner.cpp:143-149` (`UseSuddenness`) | `Know::enemy_sees_me` instead of the enemy's vision | know |
| `stalker_combat_planner.cpp:159-166` (`alarm` bark) | `Comms::request(Kind::contact)` when `comms` is on, stock bark otherwise | comms |
| `CWeaponMagazined::OnShot` (`WeaponMagazined.cpp:896-918`) | `da_ai_sense_on_shot(*this)` | sense |
| reload start in `CWeaponMagazined` (where `sndReload` plays) | `da_ai_sense_on_reload(*this)` | sense |
| `CStepManager::material_sound::play_next` (`step_manager.cpp:429`) | `da_ai_sense_on_step(*object, volume, mtl_pair)` | sense |
| `CMaterialManager::update` step play (`material_manager.cpp:109-113`) | same | sense |
| `CTorch::Switch(bool)` (`Torch.cpp:233`) | `da_ai_sense_on_torch(*this, light_on)` | sense |
| bullet vs actor/stalker sphere (`Level_bullet_manager_firetrace.cpp:65-152`) | `da_ai_sense_queue_near_miss(...)` (thread-safe queue) | sense |
| level destruction (`CLevel` destructor, before objects go) | `da_ai_level_unload()` | core (registries), all lanes |
| `src/xrEngine/x_ray.cpp` before `XMS::ApplyConfigStage` | merge `dead_air_x64_ai.ltx` into `pSettings` (C5) | core |

### 2.2 Per-NPC and per-squad state

`core` defines in `src/xrGame/da_ai_npc.h`:

```cpp
namespace DaAi {
struct Profile;                 // resolved profile (da_ai_profile.h, core)
struct KnowState;  struct SenseState;  struct CommsState;
struct TacticsState; struct MoraleState; struct SearchState;   // defined in the lane headers

struct NpcState {
    const Profile* profile{};   // cached, re-resolved when the community index changes
    u16 squad_id{u16(-1)};      // sim squad (m_group_id) or own id
    u8  band{};                 // rank band 1..4
    bool has_radio{};           // rolled once (2.3.4)
    KnowState* know{}; SenseState* sense{}; CommsState* comms{};
    TacticsState* tactics{}; MoraleState* morale{}; SearchState* search{};
};
struct SquadState {             // keyed by sim squad id, level lifetime
    u16 id{}; u16 commander{u16(-1)}; u8 community{};   // commander = engine order (lowest id alive)
    u16 leader{u16(-1)};        // highest band alive (C22); orders, radio bonus, leader death
    xr_vector<u16> members;     // online stalkers of the squad
    struct CommsSquad* comms{}; struct TacticsSquad* tactics{};
    struct MoraleSquad* morale{}; struct SearchSquad* search{};
};
NpcState* npc(const CAI_Stalker&);          // null for monsters and when the NPC has no state
SquadState* squad(u16 id);                  // registry in da_ai_npc.cpp
}
```

`CAI_Stalker` gets one member `DaAi::NpcState* m_da_ai{}` with an accessor (core, `ai_stalker.h`).
Each lane provides `create/destroy` for its state (and its squad part) in its own file; core calls
them from `da_ai_spawn/destroy` and the squad registry. Everything here is runtime-only except the
beliefs written to `AIK1` and the traits written to `AIT1`. The trait table (2.11) is a session
registry, not level state: `da_ai_level_unload` does not clear it. Offline NPCs have no `NpcState`
and receive nothing; a message addressed to one is dropped.

### 2.3 Profile system (`configs/dead_air_x64_ai.ltx`, core)

#### 2.3.1 Resolution

1. `[ai_profile_by_character]`: the NPC's specific-character id is matched by longest prefix
   (`sim_default_stalker_ecolog_guard = ai_profile_ecolog_guard`, `sim_default_csky_4 =
   ai_profile_csky_r4`). Mods add their special NPCs here.
2. Otherwise the community (after `[ai_community_alias]`, e.g. a mod's `renegade = bandit`) and the
   band: `ai_profile_<community>_r<band>`, else `ai_profile_<community>`, else `ai_profile_default`.
3. Sections inherit with ltx parents (`[ai_profile_army_r2]:ai_profile_army`,
   `[ai_profile_army]:ai_profile_default`), so a module's `.ltxp` can patch one key anywhere.
4. Every key is read with the default of `ai_profile_default`; if even that section is missing, the
   compiled-in defaults (equal to the default row of 2.4) apply. One log line per missing community
   (`* [ai] profile <community> -> default`), never a crash (F11).
5. Band: the rank value (`CInventoryOwner::Rank()`, the real rating, **not** the 0..100 clamp of
   `ai_stalker.cpp:592-593`) is mapped to a rank name through `[game_relations] rating`, then through
   `[ai_rank_bands]` (`novice = 1, trainee = 1, experienced = 2, professional = 2, veteran = 3,
   expert = 3, master = 4, legend = 4`; unknown names -> 2). `[ai_rank_scale]` multiplies
   `react_ms` and `settle_s` per rank name (`legend = 0.9`, lore.md 8.2 note; absent = 1).
6. Resolved once at spawn and at `reload`; re-resolved when the community index differs from the
   cached one (script `set_character_community`). The radio trait is **not** re-rolled then (2.3.4).

#### 2.3.2 Weapon classes

`[ai_weapon_classes]` class -> `effective, engage_max, min` metres (lore.md 7):
`pistol = 25, 40, 0`, `smg = 60, 100, 0`, `shotgun = 20, 35, 0`, `rifle_old = 200, 300, 10`,
`rifle = 200, 300, 0`, `mg = 400, 500, 10`, `dmr = 600, 800, 30`, `launcher = 200, 300, 30`.
`[ai_weapon_class_rules]` (ordered, generic, first match wins; C27): (1)
`[ai_weapon_class_overrides]` weapon section -> class (mods add theirs by `.ltxp`); (2) the weapon's
`slot` is in `pistol_slots` (DA data: `4`) -> pistol; (3) first `ammo_class` entry by prefix family:
launcher ammo (`ammo_vog-25`, `ammo_og-7`) -> launcher, `ammo_12x70`/`ammo_12x76` -> shotgun; (4)
automatic with a 45+ magazine -> mg (before the calibre rules: the PKM shares 7.62x54 with the
SVD); (5) a built-in or attached scope with zoom >= 4 -> dmr; (6) pistol-calibre families
(`ammo_9x18`, `ammo_9x19`, `ammo_11.43x23`, `ammo_5.7x28`) in a long-gun slot -> smg (Kiparis and
Bizon use the PM's `ammo_9x18`); (7) sections listed in `[ai_weapon_class_rifle_old]` -> rifle_old;
fallback `rifle`. Never `ef_weapon_type` (lore.md 7). Result cached per weapon section. Modifiers: a
scoped `rifle` gets `engage_max` x `scoped_range_mult` (1.33); slugs (ammo in `[ai_slug_ammo]`)
give a shotgun `engage_max = 60`.

Armour class (for the lore.md 8.3 modifiers, morale and tactics): the equipped outfit's
`GetDefHitTypeProtection(eHitTypeFireWound)`, else the NPC's own fire-wound immunity, against
`[ai_armour_classes]` thresholds (`none < 0.15 <= light < 0.35 <= heavy < 0.55 <= exo`);
`[ai_armour_overrides]` maps visual or outfit section prefixes to a class (data only, never names
in code). Sampled at spawn and on outfit change.

#### 2.3.3 Profile keys (all in `ai_profile_default`; lanes read only through `DaAi::Profile`)

| Group | Keys (default row value) | Reader |
|---|---|---|
| perception | `react_ms = 700, 1000`; `react_idle_ms = 0` (added when not in danger/combat); `settle_s = 1.0`; `detect_rate = 1.0`; `hear_skill = 0.5` (localisation quality); `hear_gain = 1.0` (received power multiplier); `aim_head = 0.15` (chance of head as aim point, else spine); `lead_error = 0.4` (fraction of velocity lead error); `light_discipline = 0.5` | sense, tactics, know |
| comms | `radio_chance = 0.2, 0.2, 0.3, 0.4` (R1..R4); `radio_leader_bonus = 0.2`; `hq_relay = 0` (0 = none, else min band); `hq_delay_s = 60, 120`; `voice = auto` (`auto`/`loud`/`hushed`/`none`; auto = hushed when the NPC's own section leaves its combat bark keys empty, as DA's `dummy_silent` tiers do - detected from the keys, never by section name); `gestures = 3` (min band, 0 = never); `pda = 0`; `decide_ms = 400, 900`; `understand_ms = 300, 700`; `callouts = 1` | comms |
| tactics | `aggr = 0.5`; `cover = 0.7`; `flank = 0.3`; `supp = 0.3`; `supp_window_s = 3`; `gren = 0.2`; `peek` (default = the row's `cover`); `bounding = 0`; `fire_on_order = 0` (1: while the squad is undetected, hold aimed fire until the leader's `attack` message, being fired on or detected, or `[ai_tactics] fire_on_order_max_s`); `roles = opportunist, suppressor, flanker, grenadier, rearguard`; `grenade_issue =` (empty; `item, count, chance`) | tactics, script |
| morale | `courage = 0.5`; `break_cas = 0.45`; `break_ratio = 0.5`; `leader_dead = -0.10`; `rout_style = orderly` (`orderly`/`scatter`/`relocate`/`never`); `surrender = 0`; `recover_s = 30`; `fanatic = 0`; `stamina = 0.6` | morale, tactics |
| search | `search_delay_s = 8` (after K6: no own sighting, sound or report of the selected enemy this long hands the ambush chain over to the search; < 0 = never); `search_s = 90`; `search_style = single` (`pairs`/`single`/`straight`/`none`); `investigate = single` (`pair`/`single`/`none`/`advance`); `quiet_search = 0`; `giveup_stagger_s = 4, 12` | search |

Global sections (lanes own their keys through marker blocks in the ltx, core creates the blocks):
`[ai_awareness]` (sense), `[ai_errors]` (know), `[ai_noise]` + `[ai_noise_surface]` +
`[ai_light]` (sense), `[ai_comms]` + `[ai_comms_barks]` (comms), `[ai_tactics]` (tactics),
`[ai_morale]` + `[ai_modifiers]` (morale; lore.md 8.3), `[ai_search]` (search), `[ai_slug_ammo]`
(core).

#### 2.3.4 Radio trait

`has_radio` is rolled **once per NPC lifetime**, at the NPC's first online spawn that finds no trait
record for it, and is then stable for its life: `u = splitmix64(object_id ^ section_crc ^ 0xDA) /
2^64`, `has_radio = u < min(1, radio_chance[band] + (npc is SquadState::leader at that moment ?
radio_leader_bonus : 0))`. The result goes into the session trait table (keyed by object id +
section CRC) and is saved in `AIT1` (2.11), so neither an offline/online switch, a load, a rank
promotion nor a leader's death re-rolls it (C22: the engine commander changes when the lowest-id
member dies, a successor must not inherit the radio). The same two keys express every lore case:
"everybody" (`1/1/1/1`), "per NPC, equipment varies" (fractions, mercs), "only the leader"
(`0/0/0/0` + bonus `1`), "only from a rank up" (zeros below that band). A radio cannot be picked up
or handed over; when the last carrier of a squad dies the squad has no net. The roll and the trait
table are in core (`da_ai_profile.cpp`); comms only reads `NpcState::has_radio`.

### 2.4 Initial profile values

Tactics, morale and search columns: **`lore.md 8.2` verbatim** (React -> `react_ms`, Settle ->
`settle_s`, Aggr, Cover, Flank, Supp, Gren -> `gren` + `grenade_issue` count, Break -> `break_cas`,
`break_ratio`, `rout_style`, `surrender`, Search -> `search_s`; lore's style words map to
`search_style` and `surrender` values as fixed in 2.4.1), modifiers of
`lore.md 8.3` -> `[ai_modifiers]`. Bandits' "Aggr .60 if ratio >= 1.5, else .20" -> `aggr = 0.2` +
`aggr_winning = 0.6`, `winning_ratio = 1.5` (morale supplies the ratio). Military sniper, ecologist
scientist, ecologist guard, Monolith sniper/RG-6 -> their own sections through
`[ai_profile_by_character]`. Comms (the addendum's per-faction trait) and the remaining keys:

| Profile | radio_chance R1/R2/R3/R4 | leader bonus | hq_relay (band, delay s) | voice | gestures | pda | supp_window_s | quiet_search | investigate | detect_rate R1..R4 |
|---|---|---|---|---|---|---|---|---|---|---|
| army | 1/1/1/1 (standard issue, unit net) | - | R1, 30-60 | auto (loud private, hushed NCOs+) | R1 | 0 | 6 | 1 | pair | .9/1.0/1.1/1.2 |
| dolg (Duty, "police of the Zone") | .6/.9/1/1 | .4 | R3, 60-120 | auto | R2 | 1 | 4 | 1 | pair (leader sends two) | .8/.95/1.05/1.15 |
| killer (mercs, equipment varies) | -/.5/.65/.85 | .3 | none (secret cells) | hushed | R2 | 0 | 5 | 1 | pair | -/1.0/1.1/1.2 |
| csky | .5/.8/1/1 | .3 | R3, 60-120 | auto | R3 | 1 | 3 | 0 | single | .75/.9/1.0/1.1 |
| freedom | .15/.4/.8/.9 | .3 | none | auto (loud, banter) | R3 | 1 | 3 | 0 | single | .7/.85/1.0/1.1 (+300 ms react when idle, lore.md 6.4) |
| stalker (loners) | 0/.1/.3/.6 | .2 | none | auto | R3 | 1 | 0/0/3/3 | 0 | none R1-R2 (wait, call out), single R3+ | .7/.85/1.0/1.1 |
| bandit | 0/0/0/0 | - | none | auto (loud gangs; the boss is hushed by his silent tier) | R4 | 1 | 0/0/0/2 | 0 | single (often alone, noisy) | .6/.7/.85/1.0 |
| monolith | 0/.7/.9/1 | - | none (no telepathy, C6) | auto (loud R1) | R2 | 0 | 4 | 1 | advance (straight at the source) | .9/1.0/1.1/1.2 |
| ecolog (scientists, staff) | -/1/1/- | - | R2, 30-60 (base) | loud | 0 | 1 | 0 / 3 (staff) | 0 | none (report) | .6/.7/.9/- |
| ecolog_guard (character map) | 1/1/1/1 | - | R1, 30-60 | auto | R2 | 1 | 3 | 0 | single | .8/.9/1.0/1.1 |
| zombied | 0 | - | none | none | 0 | 0 | 0 | 1 | none | .4 |
| default (unknown communities) | .2/.2/.3/.4 | .2 | none | auto | R3 | 0 | 0/2/3/3 | 0 | single | .75/.9/1.0/1.1 |

Anchors that must survive tuning (lore.md 8.2): a military private reacts like a loner veteran; a
bandit master matches a loner R3-R4 and leads his gang; mercs out-class Duty at the same band in
cover and flanking; Monolith trades cover for aggression and never routs.

#### 2.4.1 Remaining per-faction keys (lore.md 6, 8.3, 9)

Without this table every faction would get the default for these keys, and hearing, light
discipline, aim, fatigue, fanaticism and fire discipline would not differ by faction or rank.
R1/R2/R3/R4 where a key scales by band; `-` = no NPC of that band spawns. Military sniper, Monolith
snipers/RG-6 and ecologist guards keep their `[ai_profile_by_character]` sections (snipers:
`aim_head = 0.5`, `lead_error = 0.1`).

| Profile | hear_skill | aim_head | lead_error | light_discipline | stamina | courage | fanatic | surrender | bounding | fire_on_order | search_style | other |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| army | .6/.7/.8/.85 | .15/.2/.25/.3 | .4/.3/.2/.15 | .7/.8/.9/.9 | .8 | .6/.65/.7/.75 | 0 | 0 | 1 | 1 | pairs | |
| dolg | .5/.6/.7/.8 | .1/.15/.2/.25 | .5/.35/.25/.2 | .4/.6/.7/.8 | .75 | .6/.65/.7/.75 | 0 | 0 | 0/0/1/1 | 0/1/1/1 | pairs | |
| killer | -/.7/.8/.85 | -/.25/.3/.35 | -/.3/.2/.15 | -/.8/.9/.9 | .8 | -/.6/.65/.7 | 0 | 0 | 0/0/1/1 | 1 | pairs | |
| csky | .5/.6/.7/.8 | .1/.1/.15/.2 | .5/.4/.3/.25 | .2/.3/.6/.7 | .6 | .45/.5/.55/.6 | 0 | .1/0/0/0 | 0 | 0/0/1/1 | single | |
| freedom | .4/.5/.6/.7 | .1/.15/.2/.3 | .5/.4/.3/.2 | .3/.4/.6/.7 | .5 | .5/.55/.6/.65 | 0 | .1/0/0/0 | 0 | 0 | single | `react_idle_ms = 300` |
| stalker | .3/.5/.7/.85 | .05/.1/.2/.25 | .6/.45/.3/.2 | .2/.3/.6/.8 | .6 | .35/.45/.55/.6 | 0 | .2/.1/0/0 | 0 | 0 | none/none/single/single | |
| bandit | .2/.3/.45/.6 | .1/.1/.15/.2 | .7/.6/.4/.25 | .1/.2/.4/.6 | .4 | .3/.35/.45/.6 | 0 | .3/.3/.2/0 | 0 | 0 | single | |
| monolith | .5/.6/.7/.8 | .2/.25/.3/.35 | .4/.3/.2/.15 | .3/.7/.8/.9 | .85 | 1 | 1 | 0 | 0 | 0 | straight | |
| ecolog | -/.3/.4/- | -/.05/.1/- | -/.7/.5/- | -/.2/.3/- | -/.3/.5/- | -/.2/.4/- | 0 | -/.5/.1/- | 0 | 0 | none | |
| ecolog_guard | .6 | .15 | .35 | .6 | .65 | .6 | 0 | 0 | 0 | 0 | single | |
| zombied | .1 | 0 | 1.0 | 0 | .3 | 1 | 1 | 0 | 0 | 0 | none | `hear_gain = 0.5` |
| default | .5 | .15 | .4 | .5 | .6 | .5 | 0 | 0 | 0 | 0 | single | |

Armour modifiers of lore.md 8.3 (`[ai_modifiers]`) apply on top by the armour class of 2.3.2; exo
also clears sprinting flanks (tactics T6).

### 2.5 Estimates, beliefs and the error model (know)

```cpp
namespace DaAi {
enum class Source : u8 { none, seen, heard_shot, heard_step, heard_reload, heard_item, heard_voice,
    hit, near_miss, muzzle_flash, flashlight, report_voice, report_hushed, report_radio,
    report_gesture, report_pda, report_hq, witness, script, restored };
struct Estimate { Fvector pos; float err; u32 t_observed; u16 target; u16 reporter; Source source; float confidence; };
struct Belief   { Estimate e; Fvector vel; u32 t_received; u32 t_seen_own; u32 t_seen_frame; bool enemy; };
}
```

- One small vector of beliefs per NPC (cap 16, evict the oldest non-enemy, then the oldest).
- Every estimate carries a **valid level vertex**: the vertex of the realised position when it lies
  on the level graph, else the nearest valid vertex, with `pos.y` taken from it. Movement, cover and
  search consume `m_level_vertex_id` of memory lookups, and a disk sample in a wall or in the air
  would otherwise hand them an invalid id.
- The realised position is wrong: `pos = truth + disk_sample(err * 0.6)` at observation time;
  `err` is the radius the NPC itself assumes. While the target is in the NPC's own vision the
  belief is exact (`seen`, err 0) and tracks it; after LOS loss the position extrapolates with the
  last seen velocity for at most `[ai_errors] extrap_s` (0.6 s, rank-scaled), then stays.
- Error model `[ai_errors]` (defaults; `d` = distance, `age` = seconds since observation):

| Source | err = e0 + k*d | Notes |
|---|---|---|
| seen | 0 | exact only while seen |
| heard_shot | 3 + 0.08 d | x2 suppressed, x(1 + occlusion), x(1.5 - hear_skill) |
| heard_step | 2 + 0.12 d | |
| heard_reload, heard_item, heard_voice | 2 + 0.10 d | |
| hit (shooter unseen) | direction +-(5 + 20*(1-hear_skill)) deg, distance +-40 % | distance prior from the heard shot, else 30-60 m |
| near_miss | direction +-25 deg, distance prior 20-80 m | |
| muzzle_flash | 1 + 0.02 d | |
| flashlight | 2 + 0.05 d | |
| report_* | sender's err + age growth + `describe_k[channel] * d(sender, estimate)`, quantised | describing a spot by sector or landmark: `describe_k` voice/hushed 0.15, gesture 0.2, radio 0.1, pda/hq 0.1; grid voice 5 m, hushed 5 m, radio 10 m, gesture 5 m, pda 25 m, hq 25 m |
| witness | friend's aim direction +-30 deg, distance prior 30-60 m | |
| script (rumour) | 25 m | |
| restored | saved err + age growth | |

  Age growth: `err += 1.5 m/s * age` (walking-speed prior), capped at 60 m; poor visibility:
  `err *= 1 + 1.5 * (1 - clarity)` where clarity is the awareness intensity at identification.
- Lookups for stalkers (`memory()`, `memory_time()`, `memory_position()`, `visible_now()`,
  `visible_right_now()`, `visible_object_time_last_seen()`) read beliefs: position = belief
  position, time = `t_observed`, visible = own sight this update. The shared pool is still filled
  (monsters, save format) but never read for a stalker decision.
- Enemy candidates **and danger objects** of a stalker come from its beliefs and own estimates
  (resolved by id), not from pool entries (C28); the enemy callback (`xr_combat_ignore.is_enemy`)
  still filters them.
- A reported-only belief (`report_*`, `witness`, `script`, `restored`) may drive movement, cover,
  search and aim direction, never a shot or a grenade (F5, T5).

### 2.6 Communication model (comms)

Channels:

| Channel | Who | Reach (physical) | Transmit time | Receiver condition | Player |
|---|---|---|---|---|---|
| voice | `voice = loud` or `auto` on a barking tier | the bark's real sound: `max_ai_distance x volume`, occlusion, hearing threshold | bark length (sound length; fallback 0.8-1.5 s) | heard >= threshold | hears it |
| hushed | silent tiers, `voice = hushed` | the same line from the base voice set at `[ai_comms] hushed_volume` (0.3), ~10-15 m | bark length | heard >= threshold | hears it up close |
| radio | `has_radio` | every alive carrier of the same sim squad, level-wide | `radio_key_ms` 300-600 + message 600-2000 ms | alive, carries a radio | squelch + voice at radio volume within `radio_squelch_range` (8 m) of sender and receivers |
| gesture | band >= `gestures` | <= `gesture_range` (25 m) | 400-800 ms | receiver sees the sender (own vision list), within `gesture_fov` 120 deg of its facing | sees it only as a head turn |
| pda | `pda = 1` | own sim squad only | typing 3-6 s + 5-20 s, never while under fire | squad member alive | - |
| hq | carriers of band >= `hq_relay` | other squads of the same community on this level, via radio | `hq_delay_s` | a carrier in the receiving squad | - |

Who decodes: voice, hushed and gesture reach **every non-hostile hearer or viewer**, of any squad
(a garrison of several small squads talks by voice, games.md 6.4); radio and PDA only the own sim
squad's net; hq only the own community. Hostile hearers get only a `heard_voice` estimate of the
speaker. The radio squelch and the radio voice are real sounds near sender and receivers: enemies
within their range get `heard_voice` estimates of the carrier.

Relay: a carrier that receives a radio or hq report passes it on by voice/hushed/gesture to squad
mates without a radio (normal channel latency and error, payload = its own new belief), so a
leader-only-radio squad still shares the net's news; a non-carrier that has news tells a carrier
the same way, who then keys the radio. One relay hop per report (a relayed report is never relayed
again), counted as `c_relay`.

Content: `{kind, target id, Estimate (quantised), sender, channel, extra}`; kinds `contact, update,
lost, man_down, leader_down, taking_fire, grenade, reloading, flanking(side), cover_me, fall_back,
attack, help, search_assign(spot), all_clear, surrender`. The payload is `Know::report_of(sender,
target)` at the moment the message starts: the sender's own belief degraded by its age. The target
id only binds the report to its entity (C16/F7); a receiver never reads the object's position.
**Involuntary cries** (pain `injuring`, `dying`, wounded moans) carry no target estimate: hearers
learn only "a mate is hurt or dead at the heard spot" (danger + morale); the speaker's estimate of
the attacker travels only in a deliberate callout it decides to make after `decide_ms`.

Latency (all random ranges from the profiles, 2.3.3):

`notice` (sense meter) -> `decide_ms` (sender) -> transmit (table) -> hear/receive -> `understand_ms`
(receiver; +50 % hushed, +50 % while the receiver's hearing threshold is raised by gunfire) ->
delivery (`Know::observe` with `report_*`) -> the receiver's own `react_ms` before any aimed action.
Hard floor 250 ms between `t_observed` and delivery (`[ai_comms] latency_floor_ms`).

Voice is decoded from the real sound: every bark already carries `CStalkerSoundData`; the hearer's
visitor (after the threshold, sense S3) calls `Comms::on_heard_voice(hearer, speaker, sound_type,
power)`, which maps the bark type to a kind (`[ai_comms_barks]`) and queues the delivery. The engine
re-emits the AI event of a playing sound every 0.5 s (`s_f_def_event_pulse`,
`IGame_Level.cpp:275-327`), so one bark is decoded once per hearer (dedupe by speaker + bark start
time) and a speaker never decodes its own voice. Barks the
engine already plays (alarm, attack, backup, cover fire, detour, search, grenade, enemy down, injuring,
dying, help) therefore become messages without new sound code; comms adds explicit sends for kinds the
stock logic never voices (contact without alarm, lost, man down, radio traffic), rate-limited per kind
per squad (`min_interval_ms`, 4000) and arbitrated like today (one talker at a time in the group,
`agent_member_manager.cpp:203-215`). Enemies hear loud and hushed callouts too: they get
`heard_voice` estimates of the speaker (sense).

Details that must exist (addendum e): a wounded NPC shouts for help (`help` kind, the existing
`help/wounded` lines, hushed plus radio on silent tiers, C23; friends in earshot come, enemies hear
where he lies); a reload shout only from
loud-voice profiles (exploitable); grenade warnings by anyone who sees or hears a grenade land;
death cries (`SOUND_TYPE_DYING`) and a seen corpse tell morale about casualties, nothing else does;
killing the only radio carrier cuts the squad net (`c_net_lost`); radio squelch reveals carriers to
a close player; searching squads with `quiet_search` stop non-essential barks (search Q3); a
suppressed weapon is quieter, localises worse and shows no flash (sense); a `fire_on_order` squad
(army, Duty R2+, mercs) holds an undetected ambush until the leader's `attack` order arrives by its
channel, so a squad whose leader is out of reach opens fire raggedly (tactics T4); a man down is
known to mates only through a seen death, a heard (possibly hushed) cry or a `man_down` message.

### 2.7 Planner wiring (core writes it at K1; lanes implement the classes)

IDs in `src/xrGame/da_ai_ids.h`, a range no script uses (scripts offset from `property_script` =
74 and `action_script` = 92 up to ~188k; mods hard-code values like 18803 and 188113):
properties `0x00DA0000 + n`, operators `0x00DA0100 + n`. Exported to Lua as `stalker_ids.da_*`
(appended, core). Every new operator is added to the `NODEFAULT` switch of
`CAI_Stalker::can_cry_enemy_is_wounded` (`ai_stalker_fire.cpp:1218-1243`, returns false) and to the
debug name tables (`ai_stalker_debug.cpp`). No new storage property, no new sub-planner (C9).

New properties (plain evaluators):

| Id | Name | Owner | True when |
|---|---|---|---|
| `0x00DA0001` | `DaOutOfBand` | tactics | known distance is outside the active weapon's `[min, engage_max]` **and** no covered approach exists (a covered approach makes `TooFarToKillEnemy` true instead) |
| `0x00DA0002` | `DaSuppressRole` | tactics | squad role = suppressor (delivered order), own last-seen point younger than `supp_window_s`, a mate is flanking/advancing/reloading exposed **as known from a delivered `flanking`/`cover_me`/`reloading` message or seen in own vision** (never read from the mate's planner or the agent's detour list), weapon class allows (smg, rifle, mg) |
| `0x00DA0003` | `DaShouldFallBack` | tactics (reads morale) | morale worried, cover compromised (belief outside the cover arc, or hit in cover), exposed with no acceptable cover in reach (T3), enemy closer than the weapon's `min`, or reloading with the enemy near; cooldown `[ai_tactics] fallback_cooldown_s`. For `fanatic = 1` the fall back is a relocation: the new cover is not farther from the belief than the current one |
| `0x00DA0004` | `DaReturnFire` | tactics (after K4) | profile `return_fire_m` > 0, the identified enemy in own sight within it, the NPC found out (own hit, near misses, `enemy_sees_me`), in the open or in the cover it has (closing round); lasts `return_fire_ms`, then `return_fire_cooldown_ms` |
| `0x00DA0006` | `DaFlankRole` | tactics (closing round) | the delivered flanker role with a covered route (the leader's, named by the order), not while a burst under way when the order came ends (`flank_grace_ms`) |
| `0x00DA0010` | `DaSearchSpot` | search | the NPC holds an assigned, uncleared search spot (search sub-planner) |
| `0x00DA0011` | `DaSearchCover` | search | its search partner is moving and it overwatches |
| `0x00DA0005` | `DaContactLost` | search (after K6) | the selected enemy is out of own sight and the NPC has held no newer own, heard or told estimate of it for the profile's `search_delay_s` |

Replaced evaluators (same id; the new class holds the stock one and calls it when its feature is off):
`eWorldPropertyPanic` -> morale `CDaAiEvaluatorPanic`; `eWorldPropertyEnemySeeMe` -> know
`CDaAiEvaluatorEnemySeeMe`; `eWorldPropertyTooFarToKillEnemy` -> tactics `CDaAiEvaluatorTooFar`
(re-enables the dead get-distance sub-planner, `engine.md 2.2`); `eWorldPropertyShouldThrowGrenade`
-> tactics `CDaAiEvaluatorShouldThrowGrenade`. `ReadyToDetour` stays stock (it only means "magazine
more than half full"; replacing it would make the planner loop on `get_ready_to_detour`): the flanker
role is applied where the stock quota lives, `CAgentMemberManager::can_detour()` and its callers
(`hold_position`, `stalker_combat_actions.cpp:765`; the smart-cover action, `:1434`), so non-flankers
keep holding and give the stock cover fire while the flanker moves.

New operators (plain `CStalkerActionBase` subclasses; all conditions include
`CriticallyWounded = false`, `DangerGrenade = false`, `InSmartCover = false`):

| Id | Name | Owner | Extra conditions | Effects | Weight |
|---|---|---|---|---|---|
| `0x00DA0101` | `DaSuppressFire` | tactics | `UseSuddenness f`, `ReadyToKill t`, `SeeEnemy f`, `Panic f`, `EnemyWounded f`, `PlayerOnThePath f`, `ShouldThrowGrenade f`, `DaSuppressRole t`, `DaShouldFallBack f` (final tactics round: no `InCover t`; the operator walks into its best cover itself and fires only there) | `PureEnemy f` | 2 |
| `0x00DA0102` | `DaFallBack` | tactics | `ItemToKill t`, `ItemCanKill t`, `DaShouldFallBack t` | `DaShouldFallBack f`, `InCover t`, `LookedOut f`, `PositionHolded f` | 1 |
| `0x00DA0103` | `DaHoldAmbush` | tactics | `UseSuddenness f`, `ReadyToKill t`, `InCover t`, `Panic f`, `EnemyWounded f`, `PlayerOnThePath f`, `ShouldThrowGrenade f`, `DaOutOfBand t`, `DaShouldFallBack f` | `PureEnemy f` | 1 |

`PlayerOnThePath f` and `ShouldThrowGrenade f` mirror `look_out`/`hold_position`: without them the
weight-2 and weight-1 shortcuts to `PureEnemy f` would always beat the stock
`throw_grenade` -> `look_out` chain and the grenade flush of T5 would never run.
| `0x00DA0104` | `DaReturnFire` | tactics (after K4) | `UseSuddenness f`, `ReadyToKill t`, `SeeEnemy t`, `Panic f`, `EnemyWounded f`, `PlayerOnThePath f`, `TooFarToKillEnemy f`, `DaReturnFire t`, `DaOutOfBand f`, `DaShouldFallBack f` | `PureEnemy f` | 1 |
| `0x00DA0105` | `DaSearchHandover` | search (after K6) | `UseSuddenness f`, `SeeEnemy f`, `Panic f`, `DaContactLost t`, `DaSuppressRole f`, `DaFlankRole f`, `DaShouldFallBack f` | `InCover f`, `LookedOut t`, `PositionHolded t`, `EnemyDetoured t` (the search planner's own conditions) | 1 |
| `0x00DA0106` | `DaFlank` | tactics (closing round) | `UseSuddenness f`, `ReadyToKill t`, `Panic f`, `EnemyWounded f`, `EnemyCriticallyWounded f`, `PlayerOnThePath f`, `ShouldThrowGrenade f`, `DaFlankRole t`, `DaShouldFallBack f` | `PureEnemy f`; at the route's end `InCover t` (the role ends) | 1 (kill_enemy weighs 4) |
| `0x00DA0110` | `DaSearchSweep` | search | inside the search sub-planner, lane design | lane design | |
| `0x00DA0111` | `DaSearchOverwatch` | search | inside the search sub-planner, lane design | lane design | |

Added conditions on existing operators (core, `stalker_combat_planner.cpp`): `DaShouldFallBack =
false` on `take_cover`, `kill_enemy`, `look_out`, `hold_position`, `detour_enemy`, the search
planner and `get distance`; `DaOutOfBand = false` on `kill_enemy`, `look_out`, `hold_position`,
`detour_enemy`, the search planner and `kill_if_not_visible`; in the final tactics round `DaSuppressRole = false` on
`detour_enemy` and the search planner (an ordered suppressor's window is not outbid by the stock chain); after K6
`DaContactLost = false` on `look_out`,
`hold_position` and `detour_enemy` (a lost contact ends the ambush chain whatever step it reached; the doctrine
drops `DaOutOfBand`/`TooFarToKillEnemy` for a lost contact, so the search planner is reachable); in the closing round
`DaFlankRole = false` on the search planner, `DaSearchHandover`, `kill_if_not_visible`, the low-cover planner,
`DaReturnFire` and `DaHoldAmbush` (the other one-step fights and the search wait while a flank order holds). With every
`ai_feature` off, the new evaluators return false and the planner is the stock one.

Morale does not add an operator: `Panic` (its evaluator) selects the existing
`retreat_from_enemy`, whose body runs the profile's style (`[ai:morale]` block, 4.6 M3).

### 2.8 Console, counters, feature flags (core)

Console (registered at K1 in `console_commands.cpp`, none saved to `user.ltx`):

| Command | Kind |
|---|---|
| `ai_feature <name> <0/1>` / `ai_features` | runtime switch / list; names: `knowledge`, `awareness`, `concealment`, `full_los`, `hearing`, `noise`, `light`, `near_miss`, `comms`, `radio`, `doctrine`, `cover_score`, `roles`, `suppression`, `grenades`, `reaction`, `settle`, `morale`, `search`, `scripts`, `all` |
| `ai_stats` | prints every non-zero counter (global line + one line per community), then resets |
| `ai_log_period <ms>` | 0 = off (default); otherwise prints `ai_stats` every period |

`DaAi::stat_add(Stat, value, community)` and `DaAi::ScopedTimer(Stat)`. The `Stat` enum holds every
key below from K1 (lanes never extend it outside their `[ai:<lane>]` block). Output lines:
`[ai] t=<s> <key>=<number> ...` and `[ai] c=<community> <key>=<number> ...`; values are numbers
only (`%u`, averages `%.1f`). `ai_stat("<key>")` returns a counter to Lua (probes).

| Lane | Counters |
|---|---|
| core | `npcs`, `squads`, `profile_default` (NPCs on the default profile), `rays` (per second), `rays_deferred`, `p_total_us` (all AI hooks, per second), `p_max_us` (worst single NPC update) |
| know | `k_seen`, `k_heard`, `k_hit`, `k_report`, `k_rumour`, `k_restored`, `k_forgot`, `k_aim_fallback`, `k_blind_shot` (a shot while the target is not in own vision and not in suppression mode; must stay 0), `k_candidates`, `p_know_us` |
| sense | `s_identified`, `s_ident_ms` (avg first stimulus -> identified), `s_suspicious`, `s_noise_step`, `s_noise_reload`, `s_noise_shot`, `s_noise_shot_sil`, `s_flash`, `s_torch`, `s_near_miss`, `s_occluded`, `s_heard_err` (avg m), `s_retrace`, `p_sense_us` |
| comms | `c_sent_<ch>`, `c_recv_<ch>` for `voice`, `hushed`, `radio`, `gesture`, `pda`, `hq`; `c_lat_voice_ms`, `c_lat_radio_ms` (avg), `c_lat_min_ms`, `c_rep_err` (avg m), `c_net_lost`, `c_witness`, `c_relay`, `c_radio_npcs` (carriers rolled, per community), `c_radio_violation` (a radio/hq message sent by or delivered to an NPC without `has_radio`, or across nets; must stay 0), `p_comms_us` |
| tactics | `t_shots`, `t_shots_supp`, `t_shots_oob` (beyond `engage_max`), `t_react_ms` (avg identified -> first shot), `t_cover_ms` / `t_combat_ms` (share), `t_open_m` (metres moved without cover by pistol/shotgun/smg holders toward an enemy), `t_flank`, `t_suppress`, `t_fallback`, `t_ambush`, `t_advance`, `t_grenade`, `p_tactics_us` |
| morale | `m_worried`, `m_panicked`, `m_retreat_orderly`, `m_retreat_scatter`, `m_relocate`, `m_surrender`, `m_leader_known`, `m_casualty_known`, `m_recovered`, `m_help_shout`, `p_morale_us` |
| search | `r_search`, `r_spots`, `r_cleared`, `r_found`, `r_giveup`, `r_giveup_spread_ms`, `r_quiet`, `r_investigate`, `r_torch_on`, `p_search_us` |

### 2.9 Lua API (core writes the bindings at K1 as forwards; lanes implement the bodies)

Game object methods (stalkers; other objects return the neutral value), one binding per name:

| Method | Returns | Body |
|---|---|---|
| `ai_awareness(obj)` | 0..1 | sense |
| `ai_belief_source(obj)` / `ai_belief_error(obj)` / `ai_belief_age(obj)` / `ai_belief_position(obj)` | int (2.5 `Source`, 0 none) / m / ms / vector | know |
| `ai_known_distance(obj)` | m (-1 unknown) | know |
| `ai_enemy_sees_me(obj)` | bool estimate | know |
| `ai_inform(obj, pos, err, source)` | - (honest scripted report; `source` 2.5 int) | know |
| `ai_has_radio()` | bool | comms |
| `ai_is_leader()` | bool (`SquadState::leader`, C22) | core |
| `ai_morale()` / `ai_fear()` | level 0..4 (panicked..heroic) / 0..1 | morale |
| `ai_role()` / `ai_suppression()` | int / 0..1 | tactics / sense |
| `ai_searching()` | bool | search |
| `ai_profile(key)` | string value of the resolved profile | core |
| `ai_weapon_class(item)` | class name | core |

Globals: `level.ai_noise(pos, loudness, kind, who)` (sense; `kind` = `"step"`, `"reload"`,
`"item"`, `"impact"`, `"voice"`), `ai_stat(key)`, `ai_feature_enabled(name)` (core). Constants:
`stalker_ids.da_*`. Engine -> Lua: when `dead_air_x64_ai.on_ai_report` exists, comms calls it as
`(receiver, target, kind, channel)` on a delivery, at most once per receiver and target per
`[ai_comms] report_callback_ms` (2000); our script forwards it to the functions registered with
`dead_air_x64_ai.subscribe_report(fn)` / `unsubscribe_report(fn)`. Not `SendScriptCallback`: DA's
callback table is a closed local, an unknown name is dropped silently (C24).

### 2.10 Save chunk `AIK1` (know)

`SaveExtensionChunkIds::AiKnowledge = 0x314B4941` ('AIK1'), registered by core at K1. Version 1,
record stream `u32 count`, then per record: `u16 npcId`, `u16 targetId`, `u32 npcSectionCRC`,
`f32 x, y, z`, `f32 err`, `u32 ageMs`, `u8 source`, `u8 confidence (0..255)`, `u16 flags` (bit 0 =
enemy). Only online stalkers, only beliefs younger than their forget time, at most 16 per NPC. Load:
records are staged and applied in `da_ai_spawn` when id and section CRC match; a target resolves by
id at first use. Invalid, non-finite or duplicate records are ignored (SAVE_COMPATIBILITY rules).
Absent chunk -> empty beliefs. Capture: a phase of `continue_capture` with the Begin/Continue pair
pattern of the weapon capture, 16 NPCs per step within the remaining share of the 3 ms budget
(PROJECT_RULES 5); staging and `forget_object` follow the existing `Stage...SaveState` pattern.

### 2.11 Save chunk `AIT1` (core)

`SaveExtensionChunkIds::AiTraits = 0x31544941` ('AIT1'), registered with `AIK1` at K1. Version 1,
record stream `u32 count`, then per record: `u16 npcId`, `u32 npcSectionCRC`, `u8 flags` (bit 0 =
rolled, bit 1 = `has_radio`). Holds the session trait table of 2.3.4: every NPC rolled this session
or restored from the chunk, online or offline. Capture drops records whose ALife object no longer
exists or whose section CRC differs (`ai().alife().objects().object(id, true)`), so the table stays
bounded by the living stalkers that have ever been online; entries also go on `forget_object`. The
table is cleared with `SaveExtensionGameplay::clear_state` (new game, load) and restored by
`stage_chunks`, which runs before the ALife load spawns anything (`alife_storage_manager.cpp:2431`);
an NPC without a record rolls once at its next online spawn.
Absent chunk (older saves) -> every NPC rolls once and keeps it.

## 3. Lanes and file ownership

### 3.1 Lanes

| Lane | Scope | Research | Starts |
|---|---|---|---|
| `core` | skeleton (shared headers, IDs, per-NPC/per-squad state, hooks in every hotspot, planner wiring, console/counters, Lua bindings, CMake, config merge order, chunk id), profile system and `dead_air_x64_ai.ltx`, integration, the final test stage, docs | this plan, lore.md | first |
| `know` | beliefs, lookups, honest injectors, enemy selection and distribution, aim fallback, ground-truth reads in its files, `EnemySeeMe`, `AIK1`, danger objects | cheats.md W1-W4, W10, H1, H3, H13, H14 | K1 (may start its new files at once) |
| `sense` | awareness meter, concealment, full-test LOS, combat re-trace, hearing estimates and occlusion, noise bus (steps, reloads, items, weight), muzzle flash, torch beams, near misses and suppression, suspicion | cheats.md W5-W7, H5-H7; games.md M2-M4 | K1 |
| `comms` | message queue and latency, channels (voice, hushed, radio, gesture, pda, hq), radio nets, voice decoding, witness cue, pooling removal, callouts, report callback | cheats.md H2, H4, H5; lore.md 9; games.md M19-M20 | K1 |
| `tactics` | weapon doctrine and bands, fire gate (reaction, settle, out-of-band hold, suppression), accuracy multipliers, cover scoring and path exposure, peeking, fall back, roles, suppress + flank, grenades, danger covers | engine.md 7-8, 11; games.md M6-M15; lore.md 7 | K1 |
| `morale` | confidence/fear model, casualties and leader death by perception, retreat styles, surrender, fanatics, wounded, fatigue | engine.md 9, lore.md 8-9, games.md M17-M18, M22 | K1 |
| `search` | lost-contact search, spot pool, pairs and overwatch, quiet search, give-up stagger, investigation of suspicion | engine.md 9, games.md M16 | K1 |
| `script` | `dead_air_x64_ai.script` (unsubscriptions, wrappers, report handler, doctrine weapon choice, torch discipline, grenade issue, surrender glue), overlay copies (C4), QA scenarios and parser | scripts.md, cheats.md 4.7-4.9 | K1 (Lua against the stub API) |

### 3.2 Hotspot files (shared; owner and how others add to them)

| File | Owner | Others |
|---|---|---|
| `src/xrGame/CMakeLists.txt` | core | every new file of 3.4 listed at K1 |
| `src/xrGame/ai/stalker/ai_stalker.h` | core | `m_da_ai` + accessor at K1; nothing else |
| `src/xrGame/ai/stalker/ai_stalker.cpp` | core | hooks of 2.1 at K1; `[ai:comms]` block in `LoadSounds` (hushed set) |
| `src/xrGame/ai/stalker/ai_stalker_fire.cpp` | core | blocks: `[ai:tactics]` in `GetWeaponAccuracy` (`:70-106`), `fire_make_sense` (`:860-902`), `too_far_to_kill_enemy` (`:1287-1311`), `Hit` danger-cover marking (`:314-320`, H12); `[ai:sense]` in `Hit` (hit estimate), `can_kill_entity` (`:676-799`, full test); `[ai:morale]` in `Hit` (fear); `[ai:core]` switch additions |
| `src/xrGame/stalker_combat_planner.cpp` | core | all wiring of 2.7 at K1; hooks at `:143-149` (know) and `:159-166` (comms) |
| `src/xrGame/stalker_combat_actions.cpp` | tactics | `[ai:morale]` block around `CStalkerActionRetreatFromEnemy` (`:188-274`) |
| `src/xrGame/visual_memory_manager.cpp/.h` | sense | core pre-places know hook lines at K1 in `visible_now`, `visible_right_now`, `visible_object_time_last_seen`, the still-visible clear (`:724-732`), `add_visible_object` (`:504-525`) |
| `src/xrGame/sound_memory_manager.cpp` | sense | `[ai:comms]` block at the visitor call (after the threshold) |
| `src/xrGame/agent_member_manager.cpp/.h` | tactics | `[ai:comms]` block in the voice arbitration (`:203-215`) |
| `src/xrGame/save_extension_gameplay.cpp`, `save_extension_chunk_ids.h` | core | `[ai:know]` blocks for `AIK1` capture/stage/apply at K1; `AIT1` is core's own |
| `src/xrGame/script_game_object*.cpp`, `ai_stalker_script.cpp`, `level_script.cpp` | core | bindings of 2.9 at K1, forwarding to lane functions; `[ai:know]` blocks in the list getters `memory_visible_objects`/`memory_sound_objects`/`memory_hit_objects` (`script_game_object3.cpp:120-155`) and `not_yet_visible_objects` (`script_game_object2.cpp:262`) - they return a **reference** bound with `return_stl_iterator`, so the filtered list lives in a per-NPC scratch vector in `KnowState`, rebuilt on each call |
| `src/xrGame/console_commands.cpp` | core | 2.8 commands at K1 |
| `src/xrGame/WeaponMagazined.cpp`, `step_manager.cpp`, `material_manager.cpp`, `Torch.cpp`, `Level_bullet_manager_firetrace.cpp` | core (hook lines only, K1) | sense owns the called functions |
| `src/xrEngine/x_ray.cpp` | core | the pre-stage merge (C5) |
| `src/xrEngine/Feel_Vision.h/.cpp` | sense | - |
| `packaging/.../gamedata/configs/dead_air_x64_ai.ltx` | core | `; [ai:<lane>]` blocks for every global section of 2.3.3 |

### 3.3 Exclusively owned existing files

- `know`: `memory_manager.cpp/.h`, `enemy_manager.cpp`, `agent_enemy_manager.cpp`,
  `danger_manager.cpp`, `sight_action.cpp`, `sight_manager.cpp`, `sight_manager_target.cpp`,
  `stalker_kill_wounded_actions.cpp`, `smart_cover_loophole_planner_actions.cpp`,
  `stalker_movement_manager_smart_cover_loopholes.cpp`, `hit_memory_manager.cpp` (knowledge side only:
  sense edits it inside a `[ai:sense]` block that know creates at N1 - the estimate at `add`).
- `sense`: `visual_memory_manager.*`, `sound_memory_manager.cpp`, `CustomMonster.cpp` (the combat
  re-trace in `Exec_Visibility`, `:697-714`, stalker-gated), `xrEngine/Feel_Vision.*`.
- `comms`: `agent_memory_manager.cpp`, `stalker_sound_data_visitor.cpp/.h`, `stalker_sound_data.h`,
  `ai/stalker/ai_stalker_misc.cpp` (`process_enemies`), `ai/stalker/ai_stalker_space.h` (bark enum -
  append only).
- `tactics`: `stalker_combat_actions.cpp` (except the morale block), `stalker_combat_action_base.cpp`,
  `object_actions.cpp` (fire continuation gate), `ai/stalker/ai_stalker_cover.cpp`,
  `cover_evaluators.cpp/.h`, `stalker_get_distance_actions.cpp`, `stalker_get_distance_planner.cpp`,
  `agent_location_manager.cpp`, `agent_explosive_manager.cpp`, `agent_member_manager.*`.
- `morale`: `agent_corpse_manager.cpp`.
- `search`: `stalker_search_actions.cpp/.h`, `stalker_search_planner.cpp/.h`,
  `stalker_danger_in_direction_actions.cpp`, `stalker_danger_unknown_actions.cpp` (investigation).
- `script`: `tools/qa/water/ai_*.lua`, `tools/qa/ai/Parse-AiLog.py` (`tools/qa/ai/Check-AiLtx.py` is
  core's, 3.4).

### 3.4 New files

| File | Owner | Stub by |
|---|---|---|
| `src/xrGame/da_ai.h/.cpp` (flags, counters, timers, ray budget, hooks of 2.1, squad registry tick) | core | K1 |
| `src/xrGame/da_ai_ids.h`, `da_ai_npc.h/.cpp`, `da_ai_profile.h/.cpp`, `da_ai_script.cpp` | core | K1 |
| `src/xrGame/da_ai_knowledge.h/.cpp`, `da_ai_know_evaluators.cpp`, `da_ai_know_save.cpp` | know | K1 |
| `src/xrGame/da_ai_senses.h/.cpp`, `da_ai_hearing.cpp`, `da_ai_light.cpp` | sense | K1 |
| `src/xrGame/da_ai_comms.h/.cpp`, `da_ai_comms_voice.cpp` | comms | K1 |
| `src/xrGame/da_ai_tactics.h/.cpp`, `da_ai_tactics_planner.cpp` (evaluators + operators), `da_ai_cover.cpp` | tactics | K1 |
| `src/xrGame/da_ai_morale.h/.cpp`, `da_ai_morale_planner.cpp` | morale | K1 |
| `src/xrGame/da_ai_search.h/.cpp`, `da_ai_search_planner.cpp` | search | K1 |
| `packaging/.../gamedata/configs/dead_air_x64_ai.ltx` | core | K1 |
| `packaging/.../gamedata/scripts/dead_air_x64_ai.script` | script | script L1 |
| `packaging/.../gamedata/scripts/visual_memory_manager.script` (overlay copy, C4) | script | L4 |
| `packaging/.../gamedata/configs/ai_tweaks/xrs_kill_wounded.ltx` (overlay copy, C4) | script | L4 |
| `packaging/.../gamedata/scripts/xr_combat_ignore.script` (overlay copy of base DA, C25) | script | L3 |
| `tools/qa/water/ai_fair.lua`, `ai_factions.lua`, `ai_actor.lua`, `ai_save.lua` | script | L5 |
| `tools/qa/ai/Parse-AiLog.py` | script | L5 |
| `tools/qa/ai/Check-AiLtx.py` | core | K2 |
| `docs/dead-air/NPC_AI.md` | core | K5 |

A lane that starts before a stub exists creates the file itself (with the header) and tells `core`.

### 3.5 Cross-lane functions (declared by core at K1 with stock-behaviour stubs; the owner fills them)

Callers never wait for an owner: a stub returns today's behaviour.

| Owner | Functions (namespace `DaAi`) |
|---|---|
| core | `feature(Feature)`, `npc()`, `squad()`, `leader(squad)`, `profile()`, `weapon_class()`, `armour_class()`, `stat_add()`, `ScopedTimer`, `RayBudget::take(n)`, `da_ai_spawn/destroy/reload/pre_memory/post_memory/on_hit/on_death/level_unload` |
| know | `Know::on_own_shot(npc)`, `Know::observe(npc, target, Estimate)`, `Know::suspect(npc, Estimate, danger_type, perceive_type)`, `Know::belief(npc, id)`, `Know::sees_now(npc, obj)`, `Know::known_position/known_distance/known_direction(npc, obj)`, `Know::last_seen_time(npc, obj)`, `Know::report_of(sender, id)`, `Know::rumour(npc, obj, err, Source)`, `Know::enemy_sees_me(npc, enemy)`, `Know::forget(npc, id)` |
| sense | `Sense::los(from, to, ignore)` (full-test ray under the ray budget), `Sense::awareness(npc, obj)`, `Sense::suppression(npc)`, `Sense::hear(hearer, pos, power, kind, who)` (the one hearing function of S3, used by engine sounds and the noise bus), `da_ai_sense_on_shot/on_reload/on_step/on_torch`, `da_ai_sense_queue_near_miss`, `Sense::emit_noise(pos, loudness, kind, who)` |
| comms | `Comms::request(npc, Kind, target, extra)`, `Comms::on_heard_voice(hearer, speaker, sound_type, power)`, `Comms::witness(viewer, friend)`, `Comms::pooling_allowed(agent_manager)`, `Comms::has_net(npc)`, `Comms::last_heard(npc, Kind, from)` (age of the last delivered message of a kind, for tactics' role and cover-fire tests) |
| tactics | `Tactics::on_own_shot(npc)` (`t_shots*`, called from `da_ai_sense_on_shot`), `Tactics::fire_mode(npc, enemy)` -> `none/aimed/suppress`, `Tactics::accuracy_mult(npc)`, `Tactics::role(npc)`, `Tactics::band(npc)` |
| morale | `Morale::level(npc)`, `Morale::fear(npc)`, `Morale::on_casualty(npc, victim, was_leader, Source)`, `Morale::retreat_execute(action)`, `Morale::accuracy_mult(npc)`, `Morale::stamina(npc)` |
| search | `Search::active(npc)`, `Search::on_contact_lost(npc, id)`, `Search::on_message(npc, msg)` |

## 4. Lane milestones

"Done when" = written, compiled, linked, status file updated (0.4). The behavioural checks of each
milestone are listed in section 7 and run only there.

### 4.1 `core`

| M | Work | Done when |
|---|---|---|
| K0 | Create `_work\ai-npc\lanes\` (status + inbox per lane) and `_work\ai-npc\qa\`; read the research and this plan | directories exist |
| K1 | **Skeleton**: every file of 3.4 with header and stub bodies; `CMakeLists.txt`; `da_ai_ids.h`; `NpcState`/`SquadState` + registry keyed by `m_group_id` (engine commander via `CSE_ALifeOnlineOfflineGroup::commander_id()`, AI leader by band, C22; NPC without squad = squad of one); `m_da_ai` in `CAI_Stalker`; every hook of 2.1 and every marker block of 3.2; planner wiring of 2.7 (new classes are stubs that return the stock value; ids in the `NODEFAULT` switch and debug names; `stalker_ids.da_*`); console, counters, timers, `ai_stat`/`ai_feature_enabled`; Lua bindings of 2.9 (forwards); `AIK1` and `AIT1` ids + `registered` array; `x_ray.cpp` pre-stage merge of `dead_air_x64_ai.ltx` (verify in `Xr_ini.cpp` that `xms_load_overlay` keeps `[a]:b` parents - if not, load the file with `CInifile` semantics that do); `RayBudget` (per-frame cap from `[ai_config] rays_per_frame`, default 48, round-robin) | `Invoke-Ninja 'xrGame xrEngine'` links; with every lane stub the game logic is the stock one by construction (every stub returns the stock value) |
| K2 | **Profiles**: resolution 2.3.1, bands, weapon classes and armour classes 2.3.2, `SquadState::leader`, radio roll + session trait table + `AIT1` (2.3.4, 2.11), `ai_profile(key)`, `ai_weapon_class`, `ai_is_leader`; the full `dead_air_x64_ai.ltx` with 2.4 and 2.4.1 (lore.md 8.2 numbers verbatim, the comms table, the remaining-keys table, modifiers, weapon classes and rules with base-game slot and ammo names; every lane's global section as a block with the defaults of this plan) | links; `Check-Compile` clean; `tools\qa\ai\Check-AiLtx.py` (core writes it: every section of 2.4 present, parents exist, no duplicate keys, every key of 2.3.3 in `ai_profile_default`) is clean. The engine's own parse is confirmed later by `profile_default` in run R6 |
| K3 | **Integration** while lanes work: inbox triage, hook-line merges, a full link after every merge wave, keep a list of every `->Position()` of an enemy object still read in stalker combat/danger/search code, and of every `:position()` of an enemy or the actor in decision code of our overlay scripts and the base DA scripts we wrap (grep), and push each to its owner | no link break older than 30 minutes; the grep list is empty at the end (or each remaining read is justified in its owner's status file as "target visible here") |
| K4 | **Final test stage** (section 7) | 7 passes |
| K5 | **Docs and handover** (7.7) | done |

### 4.2 `know`

| M | Work |
|---|---|
| N1 | `KnowState`, belief store, `observe`, `belief`, `sees_now` (own sight: set in the member's own `CVisualMemoryManager::visible` result, cleared by its own next vision update - W2), lookups of 2.5 in `memory_manager.cpp:187-312` and the visual-memory hook lines; `forget` (profile/`xr_combat_ignore` timers stay in Lua); per-member own seen time replaces the shared `m_level_time` test for stalkers; `[ai:sense]` block in `hit_memory_manager.cpp::add` for sense |
| N2 | Honest injectors: `make_object_visible_somewhen` (`memory_manager.cpp:338-362`) -> `Know::rumour` for stalkers (C15); `agent_enemy_manager.cpp:376-414` no mask spreading and no materialisation for stalker groups, distribution (`:289-315`, `:350-373`, `:416-612`) only among members with a belief, by known distances (distribution only orders a member's own candidates, never adds one); `CMemoryManager::update(objects, add_enemies)` (`:154-184`) for stalkers: candidates from beliefs (H13) and **no `danger().add` from pool entries** - the stock danger types (fresh corpse, ricochet, attack sound, entity attacked, entity death, enemy sound, attacked) are built from the NPC's own observations (its own visible corpse, its own heard-sound estimate, its own hit estimate) through `Know::observe/suspect` (C28), items still from the pool; `enemy_manager.cpp:131` score by known distance; every estimate gets a valid level vertex (2.5); Lua list getters (`memory_visible_objects`, `memory_sound_objects`, `memory_hit_objects`, `not_yet_visible_objects`) filtered to the NPC's beliefs for stalkers, returned from a per-NPC scratch vector (3.2 blocks) |
| N3 | Aim and ground truth in know files: `sight_action.cpp:162-196,291-331,339-362,372-376,386,401,414` (belief aim point when not seen; lead only while seen, lead error and aim bone from the profile), `sight_manager.cpp:301-320`, `sight_manager_target.cpp:55-66` (aim bone: head with `aim_head` chance, else spine), `danger_manager.cpp:281-298` (hit danger at the estimate, not the live position), `stalker_kill_wounded_actions.cpp:180,275,312,318-338`, smart cover files (W1/W3 list), `k_aim_fallback`, `k_blind_shot` (counted in `Know::on_own_shot(npc)`, which `da_ai_sense_on_shot` calls for stalker shooters: a shot while `!sees_now(selected enemy)` and `fire_mode != suppress`) |
| N4 | `CDaAiEvaluatorEnemySeeMe` and `enemy_sees_me(npc, enemy)`: true only if the enemy is visible to me and I am inside the enemy's facing cone (its view direction is observable when I see it) and an LOS ray passes; unknown (enemy not visible) -> `[ai_errors] see_me_unknown` (default true, cautious); `Suspect()` danger objects (C10) |
| N5 | `AIK1` (2.10): encode/decode, capture, staging, apply in `da_ai_spawn`, forget on permanent unregistration; `k_restored` |
| N6 | Lua bodies (`ai_belief_*`, `ai_known_distance`, `ai_enemy_sees_me`, `ai_inform`), counters, spec text |

### 4.3 `sense`

| M | Work |
|---|---|
| S1 | Awareness meter in `CVisualMemoryManager::visible` (`:370-426`), stalkers only: intensity `I` = the Lua hook value (or the stock formula) divided by `time_delta/time_quant` (dt-independent); `awareness += I / threshold * detect_rate * rate_state * peripheral * stance * dt` (`stance`: the target crouching or lying still, `[ai_awareness] crouch_mult` 0.6), decay when unseen; `>= identify` (1.0) -> visible (the engine's own visible flag), `>= suspicious` (0.35) -> `Know::suspect` with `err = lerp(15, 2, awareness)`; HUD "being noticed" value (`:777-792`) = awareness; target calibration table 7.5 B1 in `[ai_awareness]` |
| S2 | `Feel::Vision` full test (flag set by stalkers, W5); `can_kill_entity` (`ai_stalker_fire.cpp:723`) and the look-out ray switch to `OPT_FULL_TEST` in place - a flag change on rays that already run, **not** charged to the ray budget and never deferred (a deferred friendly-fire check is a stale one); `Sense::los()` (full test, budgeted) only for the new rays; transparency product `Cache_vis` scales `I` (W6; threshold stays "fully blocked"); combat re-trace of the selected enemy on the off tick (`CustomMonster.cpp:697-714`, C18) |
| S3 | Hearing: in `sound_memory_manager.cpp` for stalkers - no fake hit on gunshots (`:172-179`), store the **estimate** (`:277-295`), visitor after the threshold and the alive check (`:128-129` -> after `:170`, H5), `Know::observe` with `heard_*`; one function `Sense::hear` for every heard stimulus: one CDB occlusion ray (budgeted, cached 0.5 s per source-hearer pair) multiplying power by `fSndOcclusionFactor` of the hit material, rain masking (`[ai_noise] rain_mask`), `hear_gain`, the NPC's own adaptive threshold (masking by recent loud sounds, `:81-95,196-198`), localisation error by `hear_skill`; `CHitMemoryManager::add` block: direction cone + distance guess unless the shooter is visible; enemy voices -> `heard_voice` |
| S4 | Noise bus: `emit_noise` pushes to the inboxes of stalkers within `range = loudness x [ai_noise] step_range` (spatial query), processed in `da_ai_pre_memory` **through `Sense::hear`** (occlusion, masking by gunfire, rain, gain - a footstep is not heard through a firefight or a wall any more than a real sound). Steps: gait from `volume` + movement state (crouch 0.12 / walk 0.35 / run 0.7 / sprint 1.0), surface factor from `[ai_noise_surface]` by material name, load `+weight_step_mult per kg over 25` (replaces `xr_danger` weight detection), landing; reload, grenade draw, item use, torch click; `level.ai_noise`. One source per stimulus kind (C12 generalised): steps, landings, reloads, grenade draw, item use and torch clicks come only from the bus, so the engine sound events of those types from humans (step types, `SOUND_TYPE_WEAPON_RECHARGING`, `SOUND_TYPE_ITEM_USING`) are ignored by the new hearing (script `callback.sound` still gets them); gunshots, voices, impacts and explosions come only from engine sound events |
| S5 | Light: muzzle flash on `OnShot` (NPCs whose frustum holds the muzzle, that do not already see the shooter, and whose LOS passes; range by ambient light `[ai_light] muzzle_range_day/night`, x `muzzle_silenced`; at most one flash event per shooter per `[ai_light] flash_period_ms` (500) so an automatic burst costs one set of rays, not one per round), torch beams (actor and NPC torches: glow visible when the carrier is in FOV with LOS, stronger when the beam faces the viewer; lit-spot cue at night) as visual stimuli with estimates |
| S6 | Near misses: bullet-sphere test queues `(shooter, victim, point, dir)` into a mutex-protected per-level queue drained on the main thread (the bullet manager may run on a worker thread); victim gets `near_miss` direction estimate and `suppression += k` (decays), which tactics (accuracy, peeking) and morale (fear) read |
| S7 | Counters, Lua `ai_awareness`/`ai_suppression`/`level.ai_noise`, spec text |

### 4.4 `comms`

| M | Work |
|---|---|
| C1 | `CommsState`/`CommsSquad`, message queue, latency model 2.6, delivery to `Know::observe` + `Morale`/`Search`/`Tactics` `on_message`, floor 250 ms, per-kind per-squad rate limits, reach rules of 2.6 (non-hostile hearers for voice/hushed/gesture, own net for radio/PDA) |
| C2 | Radio nets per squad (carriers from `has_radio`), carrier death -> `c_net_lost`, relay to and from non-carriers (one hop, `c_relay`), HQ relay by community on the level after `hq_delay_s`, PDA channel; radio squelch sound from `[ai_comms] radio_sound` (C13) played at sender and receivers with `radio_squelch_range`, heard by enemies too; `c_radio_violation` counted at send and at delivery |
| C3 | Voice: `stalker_sound_data_visitor.cpp` body = `Comms::on_heard_voice` (no `make_object_visible_somewhen`, no danger copy at `:33`); dedupe per speaker and bark start (the 0.5 s AI event pulse), no self-decoding; bark -> kind map `[ai_comms_barks]`, involuntary barks carry no estimate (2.6); hushed sets loaded in `LoadSounds` block from the base `[stalker]` bark keys with the NPC's voice prefix, **including `sound_hit`, `sound_death`, `sound_wounded` and the grenade alarms** that silent tiers blank (C23), played at `hushed_volume`; explicit sends for kinds the stock logic never voices; `voice = none` for zombies |
| C4 | Gestures (sender and receiver see each other in their own vision lists + range + facing, 2.6) and the witness cue replacing `process_enemies` (`ai_stalker_misc.cpp:173-214`): seeing a fighting or alarmed friend gives a `witness` estimate (direction of his aim or facing, prior distance) and a suspicion, never his enemy's position and never a copy of his danger object (`:196-203`, C28) |
| C5 | `agent_memory_manager.cpp` pooling (`reset_memory_masks`, `:20-35`) off for stalker groups when `comms` is on (masks updated only on member removal, `update_memory_masks`, unchanged); `stalker_combat_planner.cpp:159-166` contact message; wounded help shout, grenade warnings, man down / leader down messages; voice arbitration block in `agent_member_manager.cpp` |
| C6 | `on_ai_report` Lua call (throttled, 2.9), counters, Lua `ai_has_radio`, spec text |

### 4.5 `tactics`

| M | Work |
|---|---|
| T1 | Doctrine: `Tactics::band(npc)` from `weapon_class` of the active weapon; `CDaAiEvaluatorTooFar` (out of band **and** a covered approach exists **and** `aggr` permits - re-enables the get-distance sub-planner). The evaluator caches the cover point it found in `TacticsState`; `CStalkerActionRunToCover` (`stalker_get_distance_actions.cpp:36-75`) runs to that point and never falls back to running at the enemy's vertex (`:64-75`) - no point, no move. Progress rule: the cached cover must cut the known distance by at least `[ai_tactics] approach_min_gain_m` (5) and have acceptable path exposure (T3), else the evaluator returns false and `DaOutOfBand` takes over; otherwise `WaitInCover` -> re-evaluate -> the same cover would loop the get-distance sub-planner forever. `DaOutOfBand`, `DaHoldAmbush` (stay in cover, watch the belief's approach, fire only inside the band; a sniper too close falls back or switches to the sidearm via the script weapon choice) |
| T2 | Fire gate `Tactics::fire_mode`: `aimed` only if `Know::sees_now` and `now - t_identified >= react_ms` (distance/light/peripheral add-ons, x `Morale` fear, fatigue) and the target inside `engage_max`; `suppress` only by F5; `none` otherwise. Applied in `stalker_combat_action_base.cpp:53-86` (angle test against the belief direction, queue sizes by known distance), `object_actions.cpp:229-317` (a queue continues only while `fire_mode != none`), `fire_make_sense` block (`ai_stalker_fire.cpp:860-902`: belief-based, profile `supp`, weapon class), and every W3 read in its files: `stalker_combat_actions.cpp:144,180,494,622,1168-1173,1261,1369`, `stalker_get_distance_actions.cpp:87`. `GetWeaponAccuracy` block: `x settle(t since (re)acquisition)` from 3.0 to 1.0 over `settle_s`, `x (1 + suppression)`, `x wounds`, `x fatigue`, `x target lateral speed` - on top of the tier value; `t_react_ms`, `t_shots*` |
| T3 | Cover: `cover_evaluators.cpp:162-230` + `ai_stalker_cover.cpp:72-143`: score = cover value toward the **belief** + weapon band preference (soft, `effective`) + line of fire from the cover (the existing neighbour test) - path exposure (level-graph cover-in-direction along the path toward the belief; short-range weapons refuse covers whose path has more than `exposure_open_limit_m` open metres) - distance to travel - ally spacing (existing 5 m) - danger locations; no acceptable cover in reach -> `DaShouldFallBack` (move away from the belief, never toward it); take-cover runs when exposed (`:478`); `DaShouldFallBack` + `DaFallBack` (cover compromised, too close, reloading exposed, morale worried, cooldown); look-out discipline (`peek`: vary side and stance, do not reuse the last peek point, shorter exposure for trained); danger locations (`agent_location_manager.cpp`, `ai_stalker_fire.cpp:314-320`) marked for the hit member and shared only via comms `taking_fire` receivers (H12) |
| T4 | Roles per sim squad (`TacticsSquad`): opportunist (default), suppressor (auto weapon, `supp`), flanker (best `flank`, not the leader, covered route, combat-vector cost around the belief as in TLOU), grenadier, rearguard (withdrawal). A role is assigned by the leader (`SquadState::leader`) through a comms message (`cover_me`/`flanking`) and applies only on delivery; untrained profiles stay opportunists. The flanker role replaces the half-squad quota inside `CAgentMemberManager::can_detour()` (`agent_member_manager.cpp:187-191`) and its callers (`stalker_combat_actions.cpp:765,1434`); holders give the stock cover fire (`:765-780`) only while a `flanking`/`cover_me` message younger than `[ai_tactics] cover_fire_msg_s` has been delivered to them or they see the flanker move - not because the agent's detour list says so. `fire_on_order`: while no member's `enemy_sees_me` estimate is true and nobody has been fired on, members hold aimed fire; the leader sends `attack` when the believed enemy enters his band or the squad is detected; members open fire on its delivery, on being fired on or detected, or after `fire_on_order_max_s` (8). `DaSuppressRole` + `DaSuppressFire` extend it to the other moments a mate is exposed (advance, reload, withdrawal): bursts `[ai_tactics] suppress_burst` at the own last-seen point and the cover edges near it, pauses, stop after `supp_window_s`; bounding advance for `bounding = 1` profiles when morale is confident: advance through the get-distance planner while a suppressor fires; a heard reload of a believed enemy (`heard_reload`) opens a short push/peek window (`[ai_tactics] reload_window_s`, 2 s) for profiles with `aggr >= 0.5` |
| T5 | Grenades: `CDaAiEvaluatorShouldThrowGrenade` (stock conditions + profile `gren` + flush triggers: belief static in cover for `[ai_tactics] flush_static_s`, enemy indoors/unreachable; carried grenade required; target = an own `seen`/`heard_*` belief younger than `flush_static_s + 5 s`, never a reported-only one), warning via comms `grenade`; the explosive manager's reactions stay gated on the member's own `visible_now` of the grenade (`agent_explosive_manager.cpp:74`), others react only to a delivered `grenade` warning; `t_grenade` |
| T6 | Fatigue movement choice (walk when `Morale::stamina` low), exhausted NPCs and exo wearers do not sprint-flank; `react_idle_ms`; counters, Lua `ai_role`, spec text |

### 4.6 `morale`

| M | Work |
|---|---|
| M1 | Confidence per NPC (Days Gone form): `known friendly strength / known enemy strength` from **beliefs** (armour class, weapon class, wounded, suppressed - enemy attributes sampled only while the enemy is in own vision and cached in its belief; a report-only enemy counts at `[ai_morale] unknown_enemy_strength`; friends = squad members the NPC believes alive, i.e. not seen dying and no `man_down` received), own health, ammo, suppression, witnessed losses, leader known alive, faction `courage`, band, armour modifiers (lore.md 8.3); levels panicked < worried < steady < confident < heroic (`[ai_morale] levels`), hysteresis and decay (`recover_s`); `CDaAiEvaluatorPanic` = panicked and `rout_style != never`; worried feeds `DaShouldFallBack`; `aggr_winning` for bandits |
| M2 | Casualties by perception only: a death seen (own vision), a dying cry heard, or a `man_down`/`leader_down` message; `agent_corpse_manager.cpp` reactions scoped to those who know; leader death (leader = `SquadState::leader`, the highest band, not the engine's lowest-id commander, C22): army/Duty (after K6 also mercs: ex-military teams, lore.md 6.2) the next highest band takes over 3-6 s after a member learns of the death (perception or `leader_down`), bandits break check at -25 pp, mercs -5 pp, Monolith/zombied none (`[ai_modifiers]`); `break_cas` and `break_ratio` |
| M3 | Retreat styles in the `[ai:morale]` block of `CStalkerActionRetreatFromEnemy`: `orderly` (withdraw cover to cover away from the belief toward the squad's smart or last safe point, a mate suppresses, fire when visible), `scatter` (run in divergent directions, no fire, drop into cover when out of sight), `relocate` (Monolith: change cover laterally, never away from the fight), `surrender` (profile `surrender` and wounded/cornered: stop firing; the script scheme plays `hands_up`, L4); recover after `recover_s` without new threat |
| M4 | Fanatics (`fanatic = 1`: fear immune, suppression affects aim only, push when heroic), wounded (help shout request, crawl to cover through existing `xr_wounded`), fatigue/stamina (sprint drain, recovery; read by tactics); `Morale::accuracy_mult` (fear adds reaction and settle time, lore.md 8.3 "Fear state") |
| M5 | Counters, Lua `ai_morale`/`ai_fear`, spec text |

### 4.7 `search`

| M | Work |
|---|---|
| Q1 | `SearchState`: LKP from the belief (own or reported), estimated velocity, per-profile duration `search_s` (sworn enemies +30 %), give-up per NPC with `giveup_stagger_s` so a squad never stops at once; forget via `Know::forget` |
| Q2 | `SearchSquad` spot pool per target: covers and level vertices within `[ai_search] spot_radius` of the LKP that are hidden from it, ordered by the estimated escape direction; spots cleared only by the member's own vision; assignment and clear-state travel as comms `search_assign`/`all_clear` messages (members out of reach keep their old plan); `DaSearchSpot`/`DaSearchSweep`, `DaSearchCover`/`DaSearchOverwatch` in `stalker_search_planner.cpp` (pairs for `search_style = pairs`, one searcher for `single`, straight walk for `straight`) |
| Q3 | Quiet search (`quiet_search = 1` suppresses non-essential barks; others bark `searching_enemy`), torch discipline flag exposed to the script (`ai_searching()` + profile `light_discipline`) |
| Q4 | Investigation of suspicion in the danger sub-planners (`investigate`: `pair` - one moves, one covers; `single` - one goes, often noisy; `none` - wait in cover and call out; `advance` - Monolith straight at the source); counters, Lua `ai_searching`, spec text |

### 4.8 `script`

All in `dead_air_x64_ai.script` (auto-started by `axr_main`), every wrapper guarded by existence
checks and `ai_feature_enabled("scripts")`, globals written through `_G.`, per-NPC throttles.

| M | Work |
|---|---|
| L1 | Skeleton; `on_ai_report` -> own subscriber list (`subscribe_report`/`unsubscribe_report`, C24); unsubscribe `xr_npc_extra.npc_on_fighting_actor` and replace the hit broadcast (`npc_on_hit_callback`: only the victim and those who perceived or were told) - hostility (goodwill -1000 as DA did) is applied in the report handler to informed NPCs of the victim's community only (C17); keep the 25 m corpse reaction (plausible, `cheats.md 4.7`) |
| L2 | `xr_danger`: remove the `npc_on_update` weight/torch leak (the engine senses replace it), danger actions look at `danger:position()` / `memory_position` instead of the live object (`:436,463,473,482,485,727-729`), corpse -> `killer_last_known_position` only, no `make_object_visible_somewhen(actor)` (`:615-620`); `axr_fight_from_cover.script:217-223` uses `memory_position`; `rx_ff.script:121-131,187,191` hold look at the memory position after `see()` fails |
| L3 | Overlay `xr_combat_ignore.script` (C25, a copy of base DA with only these changes): the `is_enemy` range caps (`:195-250`) measure `npc:memory_position(enemy)` instead of `enemy:position()`; squad-wide ignore drop only for members with a belief on the attacker (H9); `ai_stalker.update_best_weapon` doctrine wrapper (weapon whose class band holds the known enemy distance; primary first; sidearm when too close; honours DA's other checks); `sr_light.script` torch rule (night search with torch for `light_discipline < 0.5`, dark for the rest); `xr_corpse_detection` needs sight; `xr_help_wounded`/`xr_eat_medkit` `be:see(npc)` -> `npc:ai_enemy_sees_me(be)` where the check is in a replaceable function |
| L4 | Overlay `visual_memory_manager.script` (C4, W8): DA's function minus the direction-less torch/lighter luminosity and always-visible bonuses (the engine beam sense replaces them), plus a positive denominator floor; companions and the underground bonus unchanged. Overlay `ai_tweaks/xrs_kill_wounded.ltx` with `duty` -> `dolg` (`lore.md 0.10`). Grenade issue in the `ai_stalker.CSE_ALifeObject_spawn_supplies` path from the profile's `grenade_issue`. Surrender glue: `hands_up` state while `ai_morale()` reports surrender |
| L5 | QA: `tools/qa/water/ai_fair.lua`, `ai_factions.lua`, `ai_actor.lua`, `ai_save.lua` (section 7.3 timelines; run through `tools\qa\Run-WaterQa.ps1 -Scenario ai_<x>`), `tools/qa/ai/Parse-AiLog.py` (summarises `[ai]` and `[qa-ai]` lines into the 7.5 verdict table) |

## 5. Schedule and dependencies

```
t0   core K0 -- K1 (skeleton, all stubs) -- K2 (profiles) -- K3 (integration) ......... K4 -- K5
t0+  every lane writes its new files against the declared API while K1 lands
K1   know   N1 -- N2 -- N3 -- N4 -- N5 -- N6
K1   sense  S1 -- S2 -- S3 -- S4 -- S5 -- S6 -- S7
K1   comms  C1 -- C2 -- C3 -- C4 -- C5 -- C6
K1   tactics T1 -- T2 -- T3 -- T4 -- T5 -- T6
K1   morale M1 -- M2 -- M3 -- M4 -- M5
K1   search Q1 -- Q2 -- Q3 -- Q4
K1   script L1 -- L2 -- L3 -- L4 -- L5
all done -> K4 (final test stage) -> K5
```

- Only compile dependencies exist: every cross-lane call has a stub from K1 (3.5), so no lane waits
  for another lane's milestone. Semantic dependencies are resolved in K4.
- Up to eight agents at once; links serialised by `ninja.lock`; each lane links at most every ~20
  minutes; shared headers do not change after K1 except inside lane blocks.
- If a lane is blocked (a hook missing, a signature wrong), it writes the request into `core`'s inbox
  and continues with the next milestone.

## 6. Compatibility, performance, risks

### 6.1 Save compatibility

- `.scop`/`.scoc`: unchanged. Memory managers keep their save/load layout (`visual_memory_manager.cpp:810-947`,
  `sound_memory_manager.cpp:382-502`, `hit_memory_manager.cpp:263-362`); values are estimates for
  stalkers, same fields. Planners: only plain evaluators/operators (save nothing), no new storage
  property, no new sub-planner (C9). Re-enabling the get-distance sub-planner writes pairs of its
  existing properties into its existing variable-length storage list (same format).
- `.scov`: `AIK1` v1 (2.10) and `AIT1` v1 (2.11), registered centrally, preserved byte-for-byte by
  older Refined builds; the original 0.98b ignores the `.scov`. Old saves load with empty beliefs
  and roll every radio trait once.
- The profile is derived (no save data); the radio trait is saved in `AIT1` because a derived roll
  cannot stay stable through leader deaths and offline periods (C22); awareness, suppression,
  morale, fatigue, roles, search plans, messages in flight: transient, reset on load.
- `SAVE_COMPATIBILITY.md` gets the `AIK1` and `AIT1` rows at K5.

### 6.2 Script and mod compatibility

- Unchanged names and call forms: `see`, `memory_position`, `memory_time`, `best_enemy`,
  `make_object_visible_somewhen`, `set_enemy_callback`, `register_in_combat`,
  `visual_memory_manager.get_visible_value` (same arguments), `ai_stalker.update_best_weapon`,
  `CAI_Stalker__BeforeHitCallback`, `callback.sound`, `stalker_ids` values, danger types.
- Changed semantics (documented in `MODDING.md`): memory lookups are per NPC; `see` is the NPC's own
  sight; `make_object_visible_somewhen` on a stalker is a rumour (C15); the Lua visibility value is an
  intensity integrated over time (C3); the memory list getters return what the NPC knows.
- DAR2 is never forked; base DA scripts are wrapped (C4); loose `gamedata`, JSGME and XMS modules
  still win over our overlay copies (a loose `xr_combat_ignore.script` brings DA's true-distance
  caps back for that install - acceptable, it is the mod's choice).
- The report callback is `dead_air_x64_ai.subscribe_report(fn)`, not a `RegisterScriptCallback`
  name (C24); `MODDING.md` says so. `dead_air_x64_ai.ltx` is patchable by XMS `.ltxp` and
  `configs/xms/*.ltx` (C5); a loose copy replaces it whole.
- Mods that call the removed leak paths keep working (they get honest results). Mods that replace
  the Panic or danger evaluators from Lua still win (last replacement wins).

### 6.3 Performance budget (AI think time)

| Item | Budget |
|---|---|
| all new AI code (`p_total_us`) with >= 16 online stalkers in combat | average <= 0.5 ms per frame; `p_max_us` <= 0.4 ms for a single NPC update |
| each lane timer (`p_<lane>_us`) | <= 0.15 ms per frame average (know, sense), <= 0.1 ms (others) |
| new rays (hearing occlusion, muzzle flash, torch, gesture LOS, combat re-trace, exposure) | `RayBudget` <= 48 per frame, round-robin, deferred beyond (`rays_deferred`); rays that already exist and only change flags (`Feel::Vision`, `can_kill_entity`, the look-out ray) are outside the budget and never deferred; muzzle flash at most one event per shooter per 500 ms (S5) |
| memory | <= 4 KB per NPC, <= 2 KB per squad |
| Lua | our wrappers throttled per NPC (>= 250 ms), no O(N^2) loops over `db.OnlineStalkers` |

Rules: no per-frame work for an NPC outside its schedule update except the queued stimuli; spatial
queries for noise and light, never loops over all objects; the full-test LOS cost is read from
`p_sense_us` in 7.3. Decision on a miss: 6.5.

### 6.4 Risks per lane

| Lane | Risk | Mitigation |
|---|---|---|
| core | hook placed wrong or planner wiring breaks the stock chain | every stub returns the stock value; with `ai_feature all 0` the wiring is inert by construction (new evaluators false); review 7.2 checks the planner table line by line |
| know | a missed ground-truth read (W3) keeps a leak; beliefs make NPCs forget enemies they should track | K3 grep list; `k_blind_shot`; `sees_now` keeps exact tracking while visible; extrapolation window |
| know | `AIK1` capture exceeds the save budget | a `continue_capture` phase in portions of 16 NPCs within the remaining budget; chunk rules of SAVE_COMPATIBILITY |
| sense | meter calibration makes NPCs blind or psychic; full-test rays cost | `[ai_awareness]` data + 7.5 B1 table; ray budget; `p_sense_us` |
| sense | near-miss queue touched from the bullet thread | mutex queue, drained on the main thread only |
| comms | message spam, bark overlap, voice too loud/quiet | rate limits, arbitration, data ranges; the player hears exactly what NPCs hear |
| comms | hushed sets double sound memory | loaded only for silent-tier NPCs, shared sound handles |
| comms | relays bounce reports around a squad | one hop per report, a receiver ignores a report older than its own belief |
| core | trait table grows over a long playthrough | capture drops dead/released ids and CRC mismatches; `forget_object` |
| tactics | out-of-band hold makes shotgunners passive forever | covered approach -> get-distance; enemy closing -> band; aggression pushes when winning |
| tactics | suppression looks like wall hacks | own LKP only, window, burst + pause, counted separately |
| morale | squads flee too easily (boring) or never | `break_cas`/`break_ratio` data, hysteresis, recover; Monolith `never` |
| search | squads search forever / stop at once | per-profile duration, stagger |
| script | wrapper breaks a base scheme or a mod's replacement | existence checks, call the original, `ai_feature scripts 0`, `npc_on_update` idempotent |

### 6.5 Decision rules when a final check fails

| Area | Rule |
|---|---|
| fairness (F1-F10, 7.5 P) | hard: fix the cause and rerun; never loosen a fairness criterion |
| behaviour (7.5 B) | tune the data (`dead_air_x64_ai.ltx`) within the anchors of 2.4; one tuning rerun; if still off, fix the logic if it is a bug, else record the measured value and the reason in section 8 |
| performance (6.3) | the lane with the largest timer lowers its slice rates (vision re-trace every 2nd tick, occlusion cache longer, ray budget 32) without dropping a fairness rule; a feature whose cost cannot meet the budget is simplified, not switched off |
| save/load | hard: any load failure or foreign state -> fix |

### 6.6 Rejected, with the reason

Global hit tokens (a simulation cheat, `games.md 4`), TLOU's "5 m and you are gone" pacing hack
(cheat), per-player adaptation across saves (`games.md M23`: a persistent learning system the user
did not ask for; would need its own `.scov` design), a Monolith psi link (C6), a player-facing AI
option (C2), new voice assets (1.2), a radio as a lootable inventory item (the addendum allows a
per-NPC trait, and an item needs a new model and icon, 1.2).

## 7. Final test stage (the only test stage; `core` K4)

### 7.1 Freeze, build, static checks

1. Every lane reports done; no lock held. `Invoke-Ninja` with no target (full Release, warnings as
   errors), then an incremental build that finds no work.
2. Static checks: `git diff --check`; every new engine file has the XFined-Ray/MIT header and no
   modified upstream/build file got one; `git grep` finds no DAR2-specific name in new code (data
   files only); no `TODO`/`FIXME`/temporary logs in owned files; K3's enemy-`Position()` list is
   empty or justified; the planner wiring equals 2.7; `Check-AiLtx.py` clean; the compatibility archive script
   (`tools/package/dead_air_x64_compatibility_archive.ps1`, dry run into a temp folder) lists the new
   overlay files; the new
   Lua files load in a plain LuaJIT syntax check (`luajit -bl` of the game's LuaJIT or the engine's
   own loader in run R1).

### 7.2 Adversarial review

Fresh reviewer agents at high effort: one per lane diff, one cross-cutting (the update order of
2.1, the planner wiring, save/Lua compatibility), one fairness auditor (walks every path that can
set an aim point, fire, throw, create a memory entry or danger object, pick a suppression target or
deliver a message - engine and our Lua - and checks F1-F10 and C22-C28). Every
finding is fixed at once by the owning lane (or by `core` if the lane has finished) before 7.3.

### 7.3 Stand runs (`_qa\lightdiag`, at most 7 runs + 2 tuning reruns, each <= ~90 s after load)

All runs: `tools\qa\Run-WaterQa.ps1 -Name <run> -Scenario ai_<x> -Save <save> -Seconds 150
-ExtraArgs "-force_flushlog -always_active"`, fresh rig `user.ltx`, the rules of
`qa-launch-requirements` (unpause every frame after `actor_on_first_update`), `g_god 1`,
`ai_log_period 5000`, the probe prints `[qa-ai]` lines and the harness's done marker
(`DA_WATER_PROBE_DONE`, the `-WaitFor` of `Run-WaterQa.ps1`), then quits. Scenario
geometry is found at run time (open level vertices at the wanted distances via
`level.vertex_position`, walls via `level.ray_pick`, cover via `level.high_cover_in_direction`), so
any outdoor save works; default `fogqa` (day 9:50; never skip time without `g_god`). Squads are spawned
with `alife():create("<faction>_sim_squad_<level>", ...)` + `create_npc` + smart assignment
(`scripts.md 8.2`) so generic schemes are set up. **Every NPC stays within 200 m of the actor**: the
ALife switch distance is 250 m with a 10 % band (`configs/alife.ltx:8-9`) and an offline NPC has no
AI state, so a layout that puts a squad farther tests nothing (C26); layouts are rings around the
actor. Squads are assigned to the smart terrain nearest the actor and held at their spots; the probe
logs every required separation once per second, and a run in which one broke (a squad walked to
its smart) is reported as **void**, not as passed. Runs land in `_work\ai-npc\qa\<run>`.

| Run | Scenario (one timeline, several checks) |
|---|---|
| R1 `ai-fair` | The actor stands still (god mode) and never shoots; every NPC 60-200 m from it (ring layout). **Hive mind:** bandit squads A and B (two sim squads) assigned to the **same smart terrain**, so they share one agent manager and one memory pool and the stock paths H1-H3 would leak between them; A1-A3 around P1 120 m from the actor, A4 >= 150 m from P1; B >= 150 m from P1 and from A4, kept in combat from t=0 by two weak hostile mutants spawned beside it (the stock pooling only reaches combat members, H2); no LOS from A, B to the actor; the probe hits A1 once at t=5 s (`hit()`, `draftsman = db.actor`, `direction` = unit vector actor -> A1, power 0.05, no gunshot sound); it polls every 100 ms and logs per NPC each change of `ai_belief_source(actor)` with its age and error (changes only, plus `see(actor)` flips). **Voice vs radio vs HQ:** army squad C (one sim squad) >= 150 m from A and B, no LOS to anyone: C1-C3 together, C4 >= 150 m from them; a second army squad C' (another sim squad) >= 150 m from every member of C; the probe hits C1 the same way at t=20 s. **Hearing:** a calm loner E >= 60 m from every other NPC; synthetic `level.ai_noise` steps (crouch, walk, run) at 8/15/30 m and a reload at 12 m behind E from t=35 s (awareness and suspicion times logged); an NPC shooter 100 m from E fires one unsuppressed and one suppressed burst (heard error logged). **Night:** at t=65 s time jumps to 01:00 (`level.change_game_time` + `level.set_weather(<current>, true)`; `g_god 1` keeps the radiated `fogqa` actor alive); the observer is a loner R3 spawned at t=62 s >= 100 m from every earlier event (a loner has no HQ relay, so squad C's HQ report cannot put it into danger vision), 40 m from a crouched motionless invulnerable hostile target: unlit 10 s, then the target's torch on; a muzzle flash 150 m from the observer at t=80 s; detection logged until t=90 s |
| R2 `ai-factions` | `ai_ignore_actor 1`. Three simultaneous fights around the actor, each within 150 m of it and >= 150 m from the others: `army_sim_squad_advanced` vs `bandit_sim_squad_novice` (two bandits forced onto BM-16/TOZ-34 with ammo) at 70 m open ground; `monolith_sim_squad_advanced` vs `stalker_sim_squad_advanced` at 60 m; `merc_sim_squad_advanced` vs `duty_sim_squad_advanced` at 80 m; the probe tops every squad up to 4 members with `sim_squad_scripted:add_squad_member` (the squad's own `npc` sections), so >= 24 stalkers fight. 85 s, per-community `[ai]` lines every 5 s; the probe logs per side alive count, planner action histogram (`current_action_id` of the combat sub-planner, decoded like `xrs_debug_tools`), cover share (`level.high_cover_in_direction`), retreats, first-shot times, and once per NPC its `ai_has_radio()`. This run is also the performance pass (>= 16 stalkers in combat): `p_*`, `rays`, `r__fps_log 5` |
| R3 `ai-actor` | The actor is the target (god mode, standing still). `army_sim_squad_advanced` at 80 m: identification and reaction (`t_react_ms`), settle (hits on the actor per second after identification), flank attempts, radio latency; at t=30 s the probe kills the army leader (the member with `ai_is_leader()`; lethal scripted hit, `draftsman = db.actor`): `m_leader_known` only for those who perceived or were told, leader handover. `bandit_sim_squad_novice` (4) at 60 m on another side: the probe kills two at t=40 s: expect scatter. `monolith_sim_squad_novice` (4) at 60 m on a third side: kill two at t=50 s: expect no retreat. Log to t=90 s |
| R4a `ai-save-a` | **Wall, suppression, search.** An invulnerable hostile loner target T (`npc:invulnerable(true)`) 25 m in front of bandit squad D (3) and a merc pair D2 (`merc_sim_squad_advanced` cut to two), all in LOS. Mercs, not army: army is hostile to bandits and D, D2 would fight each other, while mercs are neutral to bandits and hostile to loners (`game_relations.ltx:37-49`); mercs are also the case of a faction whose radios vary. At t=8 s the probe moves T (`set_npc_position`) 3 m sideways behind the wall its ray found; for 10 s it counts per shooter the shots (ammo delta), `t_shots_supp`, and hits on T while `see(T)` is false. At t=20 s T is moved 60 m away out of every LOS: search 35 s (spots, pairs, stagger, quiet mercs vs loud bandits, `r_*`). At t=60 s the probe prints each NPC's beliefs (source, error, age) and `ai_has_radio()`, runs `save qa_ai_mid`, quits |
| R4b `ai-save-b` | `-Save qa_ai_mid`: prints beliefs and `ai_has_radio()` after load; `k_restored`; the parser compares with R4a |
| R5 `ai-levels` | The existing `quest_teleport` scenario (two level changes) with `ai_stats` after each load: registries reset, no errors |
| R6 `ai-newgame` | `ai_newgame` (after K4; it replaced the `newgame_start` scenario, which never set a start faction): the start level is reached, every spawned NPC resolves a profile (`profile_default` only for communities without a section), no errors |

Since K12 the R1 and R4a probes keep every creature of the level within 300 m of the actor offline (switch
flags, nothing released), R2 those within 90 m of a fight; R4a's target moves 95-140 m away and the save is at 80 s, and
R4a and R2 end with the night muzzle-flash trial (O and F 150 m apart); the probes' headers give the timelines.

### 7.4 What each lane must emit for these runs

Engine counters of 2.8 (each lane its own), plus the Lua getters of 2.9 the probes poll. In
particular: know `k_blind_shot`, `k_rumour`, `k_restored`, belief getters; sense `s_ident_ms`,
`s_heard_err`, `s_noise_*`, `s_flash`, `s_torch`, `ai_awareness`; comms `c_sent/recv_*`,
`c_lat_*`, `c_rep_err`, `c_net_lost`, `c_relay`, `c_radio_npcs`, `c_radio_violation`, `ai_has_radio`;
core `ai_is_leader`; tactics `t_shots*`, `t_react_ms`, `t_cover_ms`/`t_combat_ms`,
`t_open_m`, `t_flank`, `t_suppress`, `t_grenade`; morale `m_*`; search `r_*`; core `p_*`, `rays`,
`profile_default`. `Parse-AiLog.py` turns them into the verdict table of 7.5.

### 7.5 Pass criteria

Hard (fairness, compatibility, stability):

| Id | Criterion | Run |
|---|---|---|
| P1 | `k_blind_shot = 0` in every run | all |
| P2 | Wall (0.75 s = one worst-case schedule period of 625 ms + margin, the staleness the C18 re-trace allows): bandits D (`supp_window_s` 0) fire no shot at all once their own `see(T)` has been false for 0.75 s (`t_shots_supp = 0` for them); the merc pair fires after that only shots counted as suppression (`t_shots_supp > 0` allowed) aimed at its own last-seen point or the cover edges next to it; no shooter makes an aimed hit on T while its `see(T)` has been false for 0.75 s; `k_blind_shot = 0` | R4a |
| P3 | Hive mind: squad B (other squad, same smart and agent group, in combat, no LOS, out of voice range) never gets a `report_*`, `witness` or `script` belief on the actor, and any belief it does get is its own `seen`/`heard_*` preceded by its own `see(actor)` or a sound the actor made (the actor makes none); A2-A3 (same squad, in earshot of A1) get a danger at A1 from its pain cry and a `report_voice` belief on the actor only from A1's deliberate callout, no sooner than the 250 ms floor (typical 1-2.5 s: `decide_ms` + bark + `understand_ms`); A4 (same squad, >= 150 m, bandits have no radio) gets nothing but `report_pda`, and that no sooner than 8 s after the hit (`decide_ms` + typing 3 s + 5 s) | R1 |
| P4 | Radio: C4 gets `report_radio` between the sum of the profile minima and the sum of the maxima plus two schedule periods after the hit on C1 (defaults: 1.6-5.5 s = `decide_ms` + key + message + `understand_ms` + 2 x 625 ms); C2-C3 get voice, hushed and/or radio; squad C' gets nothing before `hq_delay_s` (30 s) after C's first report and a `report_hq` after it (by t=90 s) | R1 |
| P5 | Latency floor: `c_lat_min_ms >= 250`; no report belief with `t_received - t_observed < 250 ms` | all |
| P6 | Estimates: heard-shot error at 100 m within 3-25 m; suppressed >= 1.5x unsuppressed; every report error >= its sender's belief error | R1 |
| P7 | Leader death known only by perception or message (`m_leader_known` <= members who saw, heard or received) | R3 |
| P8 | Save: R4b loads; `k_restored` = the R4a records of NPCs still online; NPCs without beliefs in R4a have none after load; every NPC's `ai_has_radio()` is the same before and after the load (`AIT1`); old saves load in every other run | R4 |
| P9 | No `FATAL`, assertion, access violation, Lua error, new recurring warning; registries reset on level change; monsters show no `[ai]` state (`npcs` counts stalkers only) | all |
| P10 | Performance budget 6.3 met in R2 | R2 |
| P11 | Radio is physical: `c_radio_violation = 0` (no radio or hq message sent by or delivered to an NPC without `has_radio`, none across nets); in R2 no merc or Duty NPC whose `ai_has_radio()` is false has a `report_radio` belief; army and bandit carriers in `c_radio_npcs` match their profiles (every army NPC, no bandit) | all |

Behaviour (tuned by data, 6.5):

| Id | Criterion | Run |
|---|---|---|
| B1 | Identification (danger state, daylight, target in the open, frontal, <= 80 m): R1 1.0-1.8 s, R2 0.7-1.2, R3 0.45-0.8, R4 0.3-0.6 (+-40 %; a band no run fields is reported "not measured", never "passed"); calm state x2.5; steps: running at 15 m makes a calm loner suspicious, crouching at 15 m on earth does not; night: the crouched unlit target at 40 m is not identified within 10 s, with its torch on within 3 s; the muzzle flash at 150 m gives a `muzzle_flash` estimate within 1 s | R1, R3 |
| B2 | Reaction `t_react_ms` p50 within each profile's `react_ms` +-30 % | R2, R3 |
| B3 | Doctrine: `t_shots_oob = 0` for pistol and shotgun holders; `t_open_m` <= 15 m per shotgun/pistol NPC per fight | R2 |
| B4 | Cover share: army >= 0.6 and >= bandit R1 + 0.15; mercs >= Duty | R2 |
| B5 | Coordination: army and mercs each show `t_suppress >= 1` and `t_flank >= 1`; bandit novices `t_flank` may be 0 | R2 |
| B6 | Morale: bandit novices after 2 of 4 dead -> >= 1 `m_retreat_scatter` within 10 s; Monolith after 2 of 4 -> 0 retreats; army no rout below 55 % losses; mercs withdraw orderly when losing | R2, R3 |
| B7 | Search: after contact loss the searchers visit >= 2 spots; give-up spread `r_giveup_spread_ms >= 3000`; the merc pair searches as a pair and quietly (`r_quiet > 0`), bandits search singly and bark | R4a |
| B8 | Reach: every voice receiver within 60 m of its speaker; army radio receivers beyond 60 m exist; Duty radio receivers > 0 in R2 while a Duty carrier lives; bandit radio receivers = 0 | R1, R2 |

### 7.6 Tuning loop

After the first pass, `core` changes only `dead_air_x64_ai.ltx` values (and bug fixes found by the
failed hard criteria), rebuilds if code changed, and reruns at most two runs (the ones whose
criteria failed). What still fails is decided by 6.5 and written to section 8.

### 7.7 Documentation and handover

- `docs/dead-air/NPC_AI.md` (new active spec): architecture (2.1-2.2), fairness rules, profile keys
  and resolution (2.3), error and comms models (2.5-2.6), planner ids, console and counters, Lua API,
  measured numbers from 7.5 - merged from the lanes' spec texts.
- `SAVE_COMPATIBILITY.md`: `AIK1` and `AIT1` rows and text. `MODDING.md`: an "NPC AI" section
  (per-NPC memory, rumour semantics, the visibility hook as intensity, `.ltxp` patching of
  `dead_air_x64_ai.ltx`, `ai_*` Lua API, `dead_air_x64_ai.subscribe_report`, the belief-distance caps
  of our `xr_combat_ignore.script` copy). `TEST_MATRIX.md`: an "NPC AI" row per 7.3 run with its pass
  criteria. `PROJECT_RULES.md` 13: list `NPC_AI.md`.
- Handover for the lead (section 8): files to deploy (`xrGame.dll`, `xrEngine.dll` + pdb, the overlay
  script/ltx files, the rebuilt `xtra_dead_air_x64.xdb0`) with SHA-256; no version bump.
- When the lead commits, this plan's lasting content already lives in `NPC_AI.md`; the plan is
  then deleted (PROJECT_RULES 12).

### 7.8 Cleanup

Probe scripts and `qa_autoexec.ltx` removed from the rig, rig `user.ltx` restored, `qa_ai_mid` save
removed, only our own engine processes stopped (`Get-Process xrEngine | ? Path -like '*_qa*'`),
locks released, lane status files kept in `_work\ai-npc`.

## 8. Results (filled by `core` during and after the run)

- Per lane: milestones reached, decisions (with the replaced reference behaviour), hook lines added.
- Final stage: 7.1-7.8 outcomes, the 7.5 table with measured values, tuning changes, open items (must
  be empty).
- Handover: deploy list and SHA-256.

### 8.1 K4 (29.09.2026 04:10-05:50): not passed

Full record: `_work\ai-npc\qa\results.md` (every launch, fixes, counter tables, verdicts, SHA-256 list). Lanes: all DONE
with every milestone (status files), no lock held, full build "no work to do", static checks clean.

- **Runs.** Save `foliage` (Cordon) for R1-R5: `fogqa` (marsh) failed the R1 layout (the straight graph walk stops at
  water; the probe helper now takes the graph cell under the spot). R1-R6 and both tuning reruns (R2b, R3b) used, each
  <= ~90 s after the load; four early aborts (DLL init, two void layouts, one crash at t=11 s) are listed there.
- **Fixed at the root during K4** (owners noted): static `shared_str` literals in `Know`/`Comms` config globals crashed
  xrGame's DLL init; `CDangerManager::add` left the selection dangling after a push (FATAL in `best_danger`); blind shots
  came from the weapon's own update one frame after the gate and from the single shot a fire state owes
  (`da_ai_may_fire` in `CWeaponMagazined::state_Fire`); NPC sight was cut to 1/13 by DA's `fog_density = 1` x
  `fog_density_factor 12` (AI stalkers now see to the renderer's fog, `[ai_awareness] fog_sight`), and bodies the camera
  never drew had luminosity 0 (`[ai_awareness] daylight_lum` floor); crouch-with-a-free-mind FATALs (morale `stay`,
  stock `HoldAmbushLocation`/`GetReadyToKillLowCover`, every crouch the lanes added now sets the danger mind first);
  the actor's missing `wounded()` in the executioner wrapper; the vision hook value is cached per target
  (`[ai_awareness] intensity_cache`).
- **Verdicts.** PASS: P1 (0 blind shots in 338 rounds after the fix), P3, P4 (C4 radio 4.19 s, C' HQ 61.9 s after C's
  first report), P5, P7, P8, P9 (last runs clean), P11, B3, B6. FAIL: **P10** (R2b 1.45 ms/frame, sense 0.69, know
  0.43, comms 0.42, morale 0.20; p_max 38 ms; FPS 59 -> 43.5 in the fights), B2 (2.4-3.2 s: the counter includes the
  stock take-cover move before the first round), B5. Partial: B1 (R3b 9 of 12 inside the band). Not measured: P2, P6,
  B1 night/steps/flash, B4, B7 (the first-pass runs were inert until the vision fix; no run left), R6 (DA's intro kept
  the game on fake_start; profiles resolved for every NPC of R1-R5, `profile_default` 0).
- **Open** (must be empty for K4 to pass): P10 performance; one R1, one R2 (F3 now at 55 m) and one R4a run for the
  unmeasured criteria; B2/B5 as above.
- **Handover**: not deployed (0.1); files and SHA-256 in `results.md`.

### 8.2 Lanes

Every lane is DONE with all its milestones: core K0-K3 (K4 run, not passed; K5 below), know N1-N6, sense S1-S7, comms
C1-C6, tactics T1-T6, morale M1-M5, search Q1-Q4, script L1-L5 plus X1 (the stalker binder's offline vertex restore,
added by the lead). Decisions with the reference behaviour each replaces, hook lines and the spec texts are in the
lane status files `_work\ai-npc\lanes\<lane>.md`; their lasting content is merged into `NPC_AI.md`. Hook lines outside
the owner's files: sense 2 (`ai_stalker_fire.cpp`, `hit_memory_manager.cpp` includes), comms 3 (`ai_stalker.cpp`,
`sound_memory_manager.cpp`, `agent_member_manager.cpp` includes); core's K3 and K4 edits in finished lanes' and
unowned files are listed in `core.md` and the owners' inboxes. Deviations from this plan decided by the lanes and
kept: the step hook sits in `CStepManager::update` (the planned `play_next` runs only within 20 m of the camera); the
profile chain is walked in code instead of `[a]:b` parents (a `.ltxp` patch of a parent reaches the children);
armour classes are read from the NPC's visual (DA NPCs wear no outfit item); the dmr rule needs a magazine of 20 or
less; the fire gate is decided on the main thread (`da_ai_update_cl`, review finding R1) and also asked by the
weapon's fire loop (`da_ai_may_fire`, K4); stalker sight ends at the rendered fog (`fog_sight`, K4).

### 8.3 K5 (29.09.2026): docs and handover done

- `docs/dead-air/NPC_AI.md` (new active spec: what changed in play, fairness rules, factions x ranks, senses,
  knowledge and the error model, communication, tactics, morale, search, the Lua side, architecture, planner ids, the
  data file and its patching, save chunks, Lua API, console and counters, budget, measured results, known limits).
- `MODDING.md` "NPC AI: what scripts and configs see now" (per-NPC memory, `make_object_visible_somewhen` as a rumour,
  `get_visible_value` as an intensity, `level.ai_noise`, wrapped base scripts and the `xr_combat_ignore.script` copy,
  `subscribe_report`, the new API, `.ltxp` patching); `SAVE_COMPATIBILITY.md` `AIK1`/`AIT1` rows and text;
  `TEST_MATRIX.md` "NPC AI" (a row per run with its pass criteria and the 29.09 result); `README.md` and
  `packaging\dead-air-x64\README_RU.md` player lines; `PROJECT_RULES.md` 13 lists `NPC_AI.md`.
- The K4 run driver is now `tools\qa\ai\Invoke-AiRun.ps1` (it was a session scratch script), so the open runs are
  reproducible from the tree.
- Handover for the lead: `_work\ai-npc\HANDOVER.md` (deploy list with SHA-256, what changed for the player, known
  limits). Not deployed (0.1).
- 7.8 cleanup: the rig carries the final build (the K4 probe build replaced, hashes checked), no probe script, no
  `qa_autoexec.ltx`, `user.ltx` equal to its pristine copy, no `qa_ai_mid`, no engine running, no lock held; K4's
  temporary pid files removed. The plan stays until K4 passes: the open runs need sections 7.3-7.5. When it is
  deleted, the plan references in five source comments, `dead_air_x64_ai.ltx` (`[ai_awareness]` header) and the
  `tools\qa` headers move to `NPC_AI.md` / `TEST_MATRIX.md`.

### 8.4 K6 (29.09.2026 06:55-08:05): the remaining checks

Four runs (R2, R1, R4a, R6) and two tuning reruns (R1, R4a); record `_work\ai-npc\qa\results.md` "K6", decisions
`_work\ai-npc\lanes\core.md` "K6". Pass: P1-P9, P11, R6 (P2 on its bandit half; P4 after fixing a comms key collision
that made a hit's pain cry swallow its report). Fail: P10 on the R2 load (knowledge 0.197 ms per frame, worst update
7.2 ms; the fixes that followed were not run on that load), B1 night (fixes built after), B5 mercs, B7 (no search in
R4a). Built and not run: work caps per update, device sound preload, candidates from enemy beliefs only, darkness by
the clock (`night_hours`), stale rendered light. Open runs: one R2 (P10, B4, B5) and one R1 (night) of the final build.

### 8.5 Handover after K6 (29.09.2026 08:10): not passed, not deployed

No game run. `Invoke-Ninja` (every target) at 08:09: "no work to do", 0 errors; `Check-AiLtx.py` OK (78 sections, 48
profiles). The 7.5 status stands as K6 left it:

| Result | Criteria |
|---|---|
| Pass | P1-P9, P11, R6; B1 steps and muzzle flash, B2 by the median (except the mercs: 1.36 s from one sample against 0.25-1.04, under fire), B5 army, B6, B8 |
| Fail | **P10** (r2c: 0.329 ms/frame met, knowledge 0.197 > 0.15, worst update 7.2 ms > 0.4; fixes 10-12 not run on R2); B1 night (fixes 13-14 not run); B5 mercs (leader dead 5 s after his first sighting, no order); B7 (R4a bandits never left the stock hold/look-out cycle: a logic question, 6.5) |
| Undecided / not measured | B3 (one Monolith shotgun round at 53 m, ammo unknown), B4 (cover metric fixed after r2c), P2 merc half, B1 torch, B7 pairs and give-up spread |

Open items (7.5 needs them empty; the test budget is spent): one R2 run of the final build (P10, B3, B4, B5) and one
R1 run (night, torch); B7 as a tactics/search change. Deploy list (plan 0.1: the lead deploys; PROJECT_RULES 8: the
whole `runtime-files.txt` list, these are the files of it that differ from `D:\Games\Dead Air`; `xrGame.dll` and
`xrEngine.dll` only together: `Feel::Vision` export ordinals). Details and the recipe: `_work\ai-npc\HANDOVER.md`.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (08:06) | `E6FB470E6EBE1EEF8F646EE04052CF083FD0226393187CB514EE297270399B3D` |
| `bin\x64\Release\xrGame.pdb` | `BF4B8E4CAF43F1CA94E0842396DF5468A9B75FD4B8889D8EA77EDAF7687C4CFA` |
| `bin\x64\Release\xrEngine.dll` (01:20) | `B638735923DB82DE86437E3A17E35BCC334A287C140BD2C5D7603AD7DD207027` |
| `bin\x64\Release\xrEngine.pdb` | `AD98EF7226DFC35CD3DA895BC7CA8CB3518D160C87A3FB258B1272EDB3E4EE0E` |
| `bin\x64\Release\xrEngine.exe` (03:01) | `EC4CD5BA3B237CD5702AA938AB496A627CAB43825282A2FB0CC69DF4A82E5A65` |
| `bin\x64\Release\xrAICore.dll` (01:20) | `100817B34DEC3BF53B25E308FB5AE68F27513714A103C6ECA0157D93C84CD9E4` |
| `bin\x64\Release\xrAICore.pdb` | `163F26884A0329948D86F938F20EB4B0442CA00AE302588272023AFB0F7527E3` |
| `bin\x64\Release\xrPhysics.dll` (01:21) | `695751516AB7718F939A4EB88247689A11F71BED2A0118F9B0834B2F72A0EFE5` |
| `bin\x64\Release\xrUICore.dll` (01:20) | `F983BABB15DCD534A4CC60FD1E3C01FDEA32EC632A1AFB0B271E7678CD3D5A15` |
| `bin\x64\Release\xrParticles.dll` (02:59) | `31DA51615658E1EACD4659A12000D0C101BE4799EB222F701503F435CEA31675` |
| `bin\x64\Release\xrRender_R4.dll` (02:59) | `54ED6D457A0C1B85E8A5CEF2A7CF2BDFB32448CE7D30C3F59A8009F514C8F815` |
| `bin\x64\Release\xrRender_R5.dll` (03:00) | `D91B882AFBDF1C4C84AD663D2EB9121C5972A71ECD32AB5CEDA59380C1862E8D` |
| `gamedata\configs\dead_air_x64_ai.ltx` (new) | `3183E2C29BC5EE0EDF21B9E772146A3B8C6BE4661247AD5FCD5B1B818C261ADC` |
| `gamedata\configs\ai_tweaks\xrs_kill_wounded.ltx` (new) | `2F0FE8DEB5506BE8A01331A91457156A3CEFF6D3811E90999EA14D33F18D1028` |
| `gamedata\scripts\dead_air_x64_ai.script` (new) | `4AF7B1610D43568AC0882ABF997C22D19E6ED1940AD2A9B5CFC5EC285354A2AA` |
| `gamedata\scripts\xr_combat_ignore.script` (new) | `E164278412BE05A9F7B77FC81660BABB9B64D0AD47BD4C326A85F0CF5215A271` |
| `gamedata\scripts\visual_memory_manager.script` (new) | `181B7EF4FC864B93DF7E21794FE9601F7DDAE4599554818EBDA1D55697121D9D` |
| `database\xtra_dead_air_x64.xdb0` (rebuilt 08:09, scratchpad `k7-archive`) | `21710FCFA1A5AB4E702F2CA5B090BB44BACF3F41E28792DB63A60F003CF10563` |

`xrCore.dll` and every other runtime file equal the live game. Nothing committed, git state untouched.

### 8.6 After K6 (29.09.2026 08:15-08:50): B7 and M2, code only, not run

Record `_work\ai-npc\lanes\core.md` "After K6: B7 and M2". **B7**: the stock chain reached the search planner only
at the end of look out -> hold -> detour, and every held-cover change (the T3 score of the held cover grew as the NPC
walked away from it), glimpse (a member's visible bit read between two of its own visual updates) or hit restarted it;
`Search::on_contact_lost` was never called. Now: `DaContactLost` (0x00DA0005) after the profile's `search_delay_s`
without a sighting, sound or report, `DaContactLost = false` on the chain, `DaSearchHandover` (0x00DA0105) into the
search sub-planner, the doctrine yields for a lost contact, the held cover is judged from the cover itself, and
`visible_now` / `visible_right_now` of a live creature need the NPC's own sighting within the still-visible time.
**M2**: mercs were not in `[ai_modifiers] leader_handover`, so a merc squad whose leader died stayed leaderless to the
end of the fight; `killer` added. Built and linked (`xrGame.dll` 08:46 `CB422778...77AE`, ltx `2311FBDF...24DD`);
8.5's hashes of these two files and the compatibility archive are stale. Owed: one R4a run (B7) and the R2 run of 8.5
(B5 mercs).

### 8.7 K7 (29.09.2026 08:50-09:35): the owed runs, not passed

Three runs (R2, R1, R4a) and the one rerun (R2); record `_work\ai-npc\qa\results.md` "K7", decisions
`_work\ai-npc\lanes\core.md` "K7". Pass: P1-P5, P9, P11 (P2 now with both halves in sight), B2, B4, B6, B7 (the R4a
bandits search 8 s after losing T), B8, B1 steps; M2 seen working (the next merc in rank gave orders after the leader's
death). Fail: **P10** (sense 0.181 ms/frame over 0.15, worst update 7.5 ms; the average 0.335 and knowledge 0.115 pass),
B3 (one pistol round at 41 m against 40), B5 (army and mercs 0/0). Not measured: B1 night and torch, muzzle flash, P6
(an army squad fought next to the night trial). Fixed from the runs: the After-K6 own-sight guard made every live target
invisible a frame after the NPC's visual update (6 rounds in the whole first R2 run); `see` could stay true for
seconds without an own sighting (now at most `sight_hold_ms` 0.75 s); out-of-band rounds are refused at the weapon;
`vertex_search_m` 22 and `occlusion_cache_ms` 2000 for P10 (the last three built after the last run). Deploy list: 8.5
with `xrGame.dll`, `xrGame.pdb` and the ltx replaced by the K7 hashes in `results.md`; the archive has to be rebuilt.

### 8.8 Handover after K7 (29.09.2026 09:40): not passed, not deployed

No game run. `Invoke-Ninja` (every target) at 09:39: "no work to do", 0 errors; `Check-AiLtx.py` OK (78 sections, 48
profiles). The 7.5 status after K7:

| Result | Criteria |
|---|---|
| Pass | P1-P9, P11, R6; B1 steps (and the muzzle flash, K6), B2, B4, B6, B7 (search and quiet merc pair), B8 |
| Fail | **P10** (r2e: 0.335 ms/frame and knowledge 0.115 met; sense 0.181 > 0.15, worst update 7.5 ms > 0.4; K7 fixes 18-19 not run); B3 (one pistol round at 41 m against 40; fix 17 not run); B5 (army and mercs 0 suppressions, 0 flanks in r2e: the fights were decided early; M2 succession seen working) |
| Not measured | B1 night and torch, the muzzle flash and P6 on the final build (r1f's night trial broke; probe fixed), P2's merc suppression at the wall, B7 give-up spread; P2 with `sight_hold_ms` (fix 16 built before r2e, not run in R4a) |

Open items (7.5 needs them empty; the test budget of this stage is spent): one R2 (P10, B3, B5), one R1 with the fixed
probe (B1 night, torch, flash, P6), one R4a (P2 with fix 16, B7 give-up spread). Deploy list (plan 0.1: the lead
deploys; PROJECT_RULES 8: the whole `runtime-files.txt` list, of which these differ from `D:\Games\Dead Air`;
`xrGame.dll` and `xrEngine.dll` only together: `Feel::Vision` export ordinals). 8.5's table is superseded. Recipe and
details: `_work\ai-npc\HANDOVER.md`.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (09:29) | `7FCA5C501616398B264B5DAD06E14718AEDBC2F0CB5480735F37DF78E3DBD4BE` |
| `bin\x64\Release\xrGame.pdb` (09:29) | `2886C7146A13968C0691FF1BCDDCFF7EA38B0BA267DC75F553D152EC2C90F244` |
| `bin\x64\Release\xrEngine.dll` (01:20) | `B638735923DB82DE86437E3A17E35BCC334A287C140BD2C5D7603AD7DD207027` |
| `bin\x64\Release\xrEngine.pdb` | `AD98EF7226DFC35CD3DA895BC7CA8CB3518D160C87A3FB258B1272EDB3E4EE0E` |
| `bin\x64\Release\xrEngine.exe` (03:01) | `EC4CD5BA3B237CD5702AA938AB496A627CAB43825282A2FB0CC69DF4A82E5A65` |
| `bin\x64\Release\xrAICore.dll` (01:20) | `100817B34DEC3BF53B25E308FB5AE68F27513714A103C6ECA0157D93C84CD9E4` |
| `bin\x64\Release\xrAICore.pdb` | `163F26884A0329948D86F938F20EB4B0442CA00AE302588272023AFB0F7527E3` |
| `bin\x64\Release\xrPhysics.dll` (01:21) | `695751516AB7718F939A4EB88247689A11F71BED2A0118F9B0834B2F72A0EFE5` |
| `bin\x64\Release\xrUICore.dll` (01:20) | `F983BABB15DCD534A4CC60FD1E3C01FDEA32EC632A1AFB0B271E7678CD3D5A15` |
| `bin\x64\Release\xrParticles.dll` (02:59) | `31DA51615658E1EACD4659A12000D0C101BE4799EB222F701503F435CEA31675` |
| `bin\x64\Release\xrRender_R4.dll` (02:59) | `54ED6D457A0C1B85E8A5CEF2A7CF2BDFB32448CE7D30C3F59A8009F514C8F815` |
| `bin\x64\Release\xrRender_R5.dll` (03:00) | `D91B882AFBDF1C4C84AD663D2EB9121C5972A71ECD32AB5CEDA59380C1862E8D` |
| `gamedata\configs\dead_air_x64_ai.ltx` (new, 09:26) | `BF9CCA7361CE91C86E05512DD29A50C26BAA3644CBF4FA7176863F1387670336` |
| `gamedata\configs\ai_tweaks\xrs_kill_wounded.ltx` (new) | `2F0FE8DEB5506BE8A01331A91457156A3CEFF6D3811E90999EA14D33F18D1028` |
| `gamedata\scripts\dead_air_x64_ai.script` (new) | `4AF7B1610D43568AC0882ABF997C22D19E6ED1940AD2A9B5CFC5EC285354A2AA` |
| `gamedata\scripts\xr_combat_ignore.script` (new) | `E164278412BE05A9F7B77FC81660BABB9B64D0AD47BD4C326A85F0CF5215A271` |
| `gamedata\scripts\visual_memory_manager.script` (new) | `181B7EF4FC864B93DF7E21794FE9601F7DDAE4599554818EBDA1D55697121D9D` |

`xrCore.dll/.pdb` and every other runtime file equal the live game; the other 368 overlay files too. **The
compatibility archive `database\xtra_dead_air_x64.xdb0` is rebuilt at the deploy** by the lead's deploy recipe
(`New-DeadAirCompatibilityArchive`, verified by its own unpack): the ltx changed after the K6 archive, so the scratchpad
`k7-archive` (`21710FCF...`) and `k4-archive` are stale. Nothing committed, git state untouched.

### 8.9 K8 (29.09.2026 10:30-10:50): the final tactics round's R2 check, not passed, not deployed

Record: `_work\ai-npc\lanes\core.md` "K8", `_work\ai-npc\qa\results.md` "K8". `r2g` (build `7D1486A4`) crashed at
48.6 s: a critically wounded stalker without a firearm in hand dereferenced a null weapon while choosing the wound pose
(stock code, debug-only guard; fixed: the wound ends). The one tuning rerun `r2h` (build `7D8B1EBD`: the crash fix,
counters `t_flank_role` / `t_flank_noroute`, `flank_open_limit_m` 10 -> 20) ran 85 s clean. The budget is spent;
nothing changed after r2h.

| Result | Criteria (R2 only) |
|---|---|
| Pass | P1, P5, P9 (r2h), P11, B6, B8; suppression by order now happens (army 5 and 3, Monolith 6, Duty 3, mercs 2) |
| Fail | P10 (0.244 ms/frame, every part within budget; worst update 2.66 ms in `Sense::pre_memory`); B2 mercs (1.50x the required reaction: cover first); B3 `t_open_m` 92 against 75 (`t_shots_oob` 0: the army's short-gun holder crossed ~38 m in 6 m bounds and ~50 m in the contact-lost search); B4 (army 0.28 / bandits 0.02 in r2h, 1.00 / 0.99 in r2g: the layout's spots; mercs 0.98 in both); B5 flanks (6 orders: 4 without a covered route, 2 with one never started) |

Decided (core.md K8): the user's shotgun rule wins over the B7 search (short guns must search by covered bounds or
overwatch, owed); a flanker with a route must leave at once (owed); B4 needs more than one random layout. Deploy list
as 8.8 with three hashes replaced (the rest unchanged; full list `_work\ai-npc\HANDOVER.md`):

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (10:43) | `7D8B1EBD95EDB91513BF258AD0ECD2A24D73B6B24B10E8ED77C0A4CE173C4635` |
| `bin\x64\Release\xrGame.pdb` (10:43) | `C3F5EB4556AEC20E92BCDDCD5EA70F924498EA0FD1386CE142D415EDEEB852BC` |
| `gamedata\configs\dead_air_x64_ai.ltx` (new, 10:41) | `92BBB3CCACEF4B8286712EECF7DAEB0EFDD61290F7E9AACAC17B6A4705007511` |

### 8.10 Closing round (29.09.2026 ~11:00-11:45): code and one profiling run, not deployed

Record: `_work\ai-npc\lanes\core.md` "Closing round". Built: the leader finds his flanker's covered route before he
orders a flank (both sides, two legs through a cover), and with none picks a grenade flush, an advance under
suppression or hold (counted `t_alt_*`); the flanker with the route leaves at once (`DaFlankRole` / `DaFlank`, fire on
the move by the new profile key `flank_fire`); short guns cross at most a 2 m gap toward the enemy per move (6 m for a
trained one under a mate's suppression) and 12 m per fight, and search by covered bounds or watch from cover; drilled
profiles return fire from the cover they have too; an estimate far off the graph snaps back toward its observer (the
`Sense::pre_memory` spike); the flank search tests geometry before the agent checks. The one profiling run `k9p`
(R2 layout, sampler, build `9485167B` = everything but the last cover-loop change) ran 87 s clean: P1, P5, P9, P11,
B3 (9 open metres over 9 holders), B6, B8 pass; `pre` at most 274 us (2633 before); one flank each for army, mercs,
Duty, Monolith, 5 other choices; mercs' reaction 0.89/0.76 s with 8 return-fire exchanges; P10's worst update
1.21 ms was the flank route search (capped after the run, not measured); B4 0.96 / 0.98 (layout); B5 misses the
mercs' suppression in that layout. Owed: one R2 verdict run of this build, R1 (night, torch, flash, P6), R4a (P2,
give-up spread).

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (11:48) | `12A0BF573F32FBA59E6ECF6EBA8AF9550B80AD4F7D5DF4F312B99A1EA8574C97` |
| `bin\x64\Release\xrGame.pdb` (11:48) | `2932E0C7D8DB0C8B18BA997E1920159E5EF6B65BE05FA0A166C86299B4F5418C` |
| `gamedata\configs\dead_air_x64_ai.ltx` (11:42) | `F830C312C70254C7F42F708CCA619F3AE8FC8C1D9F66C28A6DC53F84EF7E69DC` |

### 8.11 K9 (29.09.2026 11:50-12:10): the closing round's verdict runs, not passed, not deployed

Record: `_work\ai-npc\lanes\core.md` "K9", `_work\ai-npc\qa\results.md` "K9". R2 `r2i`, R1 `r1g`, R4a `r4a5` and the one
rerun, R1 `r1h` with the probe fixed (the night target carried a torch DA hands its NPCs at night; step noises on the
ground; a closer bait for the flash shooter). All four ran the handed-over build below; no crash, four clean logs; no
game file changed in K9. `r2i`'s layout broke at 55 s (the broken bandits drifted towards the Monolith's fight) and is
judged on the data before the break (the parser now cuts at the probe clock).

| Result | Criteria (final status of 7.5) |
|---|---|
| Pass | P1-P6, P9, P11 on this build; P7, P8 (K4), R6 (K6) not re-run; B1 (steps; night: the unlit crouched target at 40 m not identified in 10 s, with its torch in 0.67 s; the muzzle flash last measured at K6), B3 (`t_shots_oob` 0, 9 open metres over 7 short-gun holders), B6, B8 |
| Fail | P10 on its absolute maximum only (0.210 ms per frame, every part within budget, the stimulus spike gone; worst update 0.81 ms in the squad tick and comms); B2 army (1.40x the reaction its gate required; mercs 1.23x now); B4 army 0.44 (the layout); B5 (suppression Duty 4, mercs 3, Monolith 3, army 0; no flank order: two leaders found no covered route and advanced under suppression, the mercs' leader ordered none); B7 (nobody searched: the bandits kept sight of the target at its away spot, the merc pair lost him and never searched, cause not found; give-up spread not exercised) |

Decided (core.md K9): no game change after the closing round's build, so the handed-over build is the one all four runs
ran; the rerun went to R1 (the night owed since K4, void in r1g by a probe defect); B5, B7, B2 army, B4 and P10's
maximum recorded by 6.5. Deploy list as 8.10 (unchanged; full list `_work\ai-npc\HANDOVER.md`): against the live r2h
deploy only these three differ, and the compatibility archive has to be rebuilt.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (11:48) | `12A0BF573F32FBA59E6ECF6EBA8AF9550B80AD4F7D5DF4F312B99A1EA8574C97` |
| `bin\x64\Release\xrGame.pdb` (11:48) | `2932E0C7D8DB0C8B18BA997E1920159E5EF6B65BE05FA0A166C86299B4F5418C` |
| `gamedata\configs\dead_air_x64_ai.ltx` (11:42) | `F830C312C70254C7F42F708CCA619F3AE8FC8C1D9F66C28A6DC53F84EF7E69DC` |

### 8.12 After K9 (29.09.2026 ~12:20-13:00): B7, B2 army, P10 maxima, code only, not run, not deployed

Record: `_work\ai-npc\lanes\core.md` "After K9". **B7**: r4a5's merc pair left engine combat at ~14 s holding a heard
(`heard_voice`) belief of T; out of combat only visual-class beliefs made enemy candidates, so neither the fight nor
the search came back (the stock visual memory kept a seen enemy a candidate). Fixed: a belief on an enemy the NPC has
seen itself stays a candidate out of combat; a sneak attack whose contact is lost ends so the handover runs. What ended
the fight is not in the log: counters `r_drop`, `r_handover` and the QA console switch `ai_trace` (drop and handover
lines) record it; the R4a probe turns the trace on and logs the shooters' planner changes. **B2**: the fire decision
waited for the next brain update after the reaction ran out (up to a schedule period, longest far from the player):
the gate now gives a waiting aimed command the frame the reaction ends and the head faces the target; a released
ambusher in sight fires at once; a flanker's ordered hold is not a reaction sample. **P10**: one spot plan per frame
over all squads (`plan_builds_per_frame`), the search's LKP from the belief's cell and bounded vertex searches; comms
shares per update (`deliveries_per_update` 3 with witness cues, `sounds_per_update` 2, `transmits_per_update` 2);
a lamp look capped in rays (the beams' spot rays included) and carrier checks (`torch_scan_checks` 8). Owed: R4a (B7),
R2 (B2 army, P10), R1 night (P10).

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (13:04) | `93BD7F49F184DF3460346EDA69817FBCCBEE1D0B5AE0081DBF098D930187380A` |
| `bin\x64\Release\xrGame.pdb` (13:04) | `31A41550E0DFFEE7C6BD5594496EA01737DB4E0D394FE0A94D92D924B6D784D1` |
| `gamedata\configs\dead_air_x64_ai.ltx` (12:52) | `5BDF6DF3EB9E093AD5B9E71A8A165056891952748E6E953C8CB88F6CF5879659` |

### 8.13 K10 (29.09.2026 13:05-13:45): the tails round's runs, not passed, not deployed

Record: `_work\ai-npc\lanes\core.md` "K10", `_work\ai-npc\qa\results.md` "K10". R4a `r4a6` and R2 `r2j` on the
after-K9 build, the one rerun R4a `r4a7` with the probe fixed (it crashed), and a fourth run R4a `r4a8` on the fixed
build (a crash is a hard failure: 6.5; the user's cap is four runs per stage). **B7's misses in r4a5 and r4a6 were a
dead target**: DA's binder turns an NPC's invulnerability off every 500 ms (`stalker_generic.update_invulnerability`),
one hard hit killed the probe's T, and the "search" watched a corpse (the bandits looted it; it never reached the away
spot); the after-K9 analysis of that miss rested on it. The probe now empties every hit on T and logs a death; the
parser voids B7 on one. **Crash (r4a7, fixed)**: the agent's enemy list kept an enemy after its object was released,
and the cover check of F2 read it (use after free in `Know::knows`); a released object now leaves the list. Latent
since F2, so the live K9 deploy carries it.

| Result | Criteria (final status of 7.5) |
|---|---|
| Pass | P1, P2 (r4a8: all five saw T, no stale round, 3 hits all in sight), P5, P9 (on the final build after the fix; r2j, r4a6, r4a8 clean), P11; P3, P4, P6 (K9), P7, P8 (K4), R6 (K6) not re-run; B2 (army 1.16x its gate, K9 1.40x; every faction within), B3 (`t_shots_oob` 0, 4 open metres over 5 short-gun holders), B6, B8; B1 (K9) |
| Fail | P10 on its maximum only (0.147 ms per frame, every part within budget; worst update 0.66 ms: comms 0.61, tactics 0.56; the squad tick's search-start peak is gone); B7 by 0.2 m (every shooter searched, bandits 4.4-6.5 s and mercs 7.3-8.2 s after losing T, the merc pair together and quiet, the bandits loud; the pair's median distance 15.2 m against the parser's 15; give-up spread not exercised); B4 army 0.09 (the layout); B5 army flank (no covered route: advanced under suppression; mercs 2, Monolith 2, Duty 1 flanks) |

Decided (core.md K10): the rerun to R4a with the probe fixed; the fourth run for the crash; B7's pair bound not loosened
after the fact (tuning candidate `[ai_search] overwatch_dist` 10 -> 8, no rerun left); P10, B4, B5 recorded by 6.5.
R2 ran `93BD7F49`; the handed-over build differs from it only by the crash fix, and r4a8 ran it. Against the live K9
deploy only these three differ, and the compatibility archive has to be rebuilt (full list `_work\ai-npc\HANDOVER.md`).
Owed: R1 night on the final build (the lamp look's caps, B1's torch), P10's comms and tactics peaks, B7's pair spacing
and give-up spread, B5 on open ground, B4 over several layouts, the muzzle flash.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (13:37) | `7CD6DD3393B3A7BE885EDBB6A011259D4BEA7BA8F427C6BE3EC7B378A25D032D` |
| `bin\x64\Release\xrGame.pdb` (13:37) | `678AB8E93CDA6F1D13F03EBB4E81F6D67CD0D2AE1C0DDCB8BC7002846AE4CC29` |
| `gamedata\configs\dead_air_x64_ai.ltx` (12:52) | `5BDF6DF3EB9E093AD5B9E71A8A165056891952748E6E953C8CB88F6CF5879659` |

### 8.14 After K10 (29.09.2026 ~13:55-14:50): B1 torch, path spam, P6, P10 peaks, B7, code only, not run, not deployed

Record: `_work\ai-npc\lanes\core.md` "After K10". The night run `r1i` on the K10 build passed P1, P3-P5, P9, P11, B8
and failed B1 (the lit target after 6.17 s) and P6. **B1** was the trial's geometry: the meter's lamp term never goes
through the lamp look's caps, and the lamp glowed at the observer at a tenth of the frontal rate (the target kept its
spawn heading; its spot was tested at 1.6 m while it crouched); the probe now turns the target to the observer and asks
for a clear line to its crouched head, and the lamp look judges the lamps facing the NPC first (caps unchanged).
**P6** was a parser false alarm (a throttled report callback hid the real sender, A4, error 0); the parser attributes
by channel and time now and still flags a real violation. **The `DetailPathManager` spam** is the probes' hold command
(a "stand" movement to the NPC's own spot, every frame); the holds walk now. **P10**: comms' peaks were the snapping of
described spots off the graph (full contour distances in every window cell, marching toward the sender: r4a8's 2.0 ms),
tactics' a flank leg judged in one update; the window search skips cells by a flat lower bound (same result), a
described spot snaps toward the sender's own estimate, the open-ground walk stops at its limit, a leg's covers and walks
are split over updates (`flank_agent_checks` 32, `flank_walks_per_update` 4), and new counters `p_max_comms_tx_us`,
`p_max_comms_rx_us`, `p_max_flank_us` name the rest. **B7**: `overwatch_dist` 10 -> 8. Owed: R1 (B1 night, P6, P9),
R2 (P10's worst update, B5), R4a (B7).

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (14:46) | `0B3F21D04F5FEC01266F2BB18931D44E30D09D029FF16D5E7E864CB07A8F971D` |
| `bin\x64\Release\xrGame.pdb` (14:46) | `180C582498F393C220A7DF578E976DBE33DFF7998E706C35A922002C7536CD07` |
| `gamedata\configs\dead_air_x64_ai.ltx` (14:19) | `893BE321136E8702644E074C3D408CA968E562078F695C9C53A4948DD1D2FA4B` |

### 8.15 K11 (29.09.2026 14:51-15:20): the after-K10 build's runs, not passed, not deployed

Record: `_work\ai-npc\lanes\core.md` "K11", `_work\ai-npc\qa\results.md` "K11". R1 `r1j` and R2 `r2k` on the after-K10
build, the one rerun R2 `r2l` with three P10 fixes; no crash, three clean logs. **The after-K10 fixes hold**: B1's lit,
frontal target identified 0.22 s after its torch (r1i 6.17 s), no report more precise than its sender's, and no
`DetailPathManager` line in any run (r1i ~3000). r1j's layout broke at 41.8 s (an army squad drifted toward squad A),
and the level's loners pulled the shooters of the hearing trial into a fight: P4's HQ half void, P6's heard shot not
measured on this build (both passed on the K10 build in r1i). **The parser misread P10 since K9**: the log file keeps
1023 characters of a line and drops its line break, the global counter line (split at 1500) ran into the next one, and
the parser let a community's values overwrite the global ones; corrected, K9 had 0.311 and K10 0.316 ms per frame (not
0.210 and 0.147), and most 5 s periods had an update over 0.4 ms. The parser splits such lines now and the engine
splits its tables at 900 characters. P10 fixes of this round: a grid walk for the open-ground measure (the flank step
770 -> 492 us), a heap instead of a full sort in the spot plan (search 621 -> 401 us), one vertex search per described
spot.

| Result | Criteria (final status of 7.5) |
|---|---|
| Pass | P1, P3, P5, P9 (0 `DetailPathManager` lines), P11; P4 radio half and P6 report half (their other halves: K10 build); P2 (K10), P7, P8 (K4), R6 (K6) not re-run; B1 night (0.22 s; steps not judged, E was no longer calm; flash not measured), B2, B3, B6, B8 |
| Fail | P10 (r2k 0.281 ms per frame, worst update 0.89 ms; r2l 0.393 ms per frame with knowledge 0.195 and senses 0.163 over 0.15, worst update 1.34 ms, a comms sending of 1.28 ms; 13-14 of 17 periods over 0.4 ms); B4 in r2l (army 0.85 against bandits 0.81 + 0.15; passed in r2k); B5 on the mercenaries in both (no suppression order; no covered route); B7 (K10, not re-run) |

Decided (core.md K11): the rerun to R2 (P10 was the one failing hard criterion; r1j answered the R1 items); P10's
earlier figures corrected, not re-judged; the knowledge and senses slice rates (6.5) not tuned without a run; B4 and
B5 recorded by 6.5. The print split (fix 72) was built after the last run: the handed-over `xrGame.dll` differs from
r2l's by it alone. Against the live K10 deploy only these three differ, and the compatibility archive has to be rebuilt
(full list `_work\ai-npc\HANDOVER.md`). Owed: one R2 for P10 (slice rates, comms sending, flank gather), the night
stimulus step (1.19 ms in r1j), R1's HQ half, heard shot, steps and muzzle flash, R4a's B7, B5 mercenaries, B4 layouts.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (15:16) | `622AD3BC3D41BD655FCF5712A2ECC373656668902A4052AA7448A2364A99E31C` |
| `bin\x64\Release\xrGame.pdb` (15:16) | `FB318AEE578C3A0BB49AEF7888CDAA1A773E67DA39883A358825073A136EDC84` |
| `gamedata\configs\dead_air_x64_ai.ltx` (14:19) | `893BE321136E8702644E074C3D408CA968E562078F695C9C53A4948DD1D2FA4B` |

### 8.16 K12 (29.09.2026 15:50-17:40): closing round, four runs, not passed, not deployed

Record: `_work\ai-npc\lanes\core.md` "K12", `_work\ai-npc\qa\results.md` "K12" (fixes 73-94, the final table),
verdicts `_work\ai-npc\qa\verdict-k12.md`. **P10 by level of detail and at the root, not by budgets**: knowledge's
share was the script's enemy callback (a Lua call per candidate and second) and senses' the occlusion rays; distant
NPCs (beyond `[ai_config] lod_near_m` 60 m of the camera) ask the callback every 3 s and keep occlusion rays 4 s, the
ray cache holds 16 sources with a tolerance growing with distance, an identified target costs no vision hook, the
vertex search stays inside a column of the graph. The comms peak (1.28 ms) was the start of sounds: every emitter
zero-allocated a second of prefill blocks; xrSound now allocates them when an emitter first renders, and a device sound
nobody could hear is not started (the sound starts of an update: 58-216 us -> 6-57). Time budgets bound the flank and
spot-plan steps, comms' sending and taking in, and (after the last run) the sense pre step. **The squelch** is the base
game's radio clicks (`ambient\special\marsh_click_*`): no "Missing ogg-comment" line. **B4/B5 had bugs** besides the
ground: the probe's cover measure read a quarter of all directions wrong, an outranged NPC could not back off into
cover, the cover score's aggression term was inverted, a merc leader never tried a rifleman for a flank; fixed. **The
probes** keep the level's creatures offline around R1/R4a (and within 90 m of R2's fights), R4a's target moves out of
every search ring so give-ups happen within the run, and R4a and R2 end with a night muzzle-flash trial.

| Result | Criteria (final status of 7.5) |
|---|---|
| Pass | P1, P2, P3, P4 (both halves: HQ 55.6 s), P5, P6 (both halves: 15.5 m, suppressed 32.0 m), P9 (0 ogg-comment warnings), P11; P7, P8 (K4), R6 (K6) not re-run; B1 steps and night (lit 0.47 s), B2, B3, B6, B7 (give-up spread 5.05 s, pair median 8.1 m), B8 |
| Fail | P10 by one update (r2n: 0.185 ms per frame, knowledge 0.053, senses 0.074, every part within; one update of 442 us, its pre step 412, 1 of 17 periods over 0.4 ms; K11 14 of 17, worst 1.34 ms; fix 94 `pre_budget_us` built after, not measured); B1's muzzle flash (r4a9: pair placed, F fired, O heard the rounds but processed no flash; cause not isolated; r1k and r2n found no pair); B4 on the army (r2n: 0.04, its spot on open ground; mercs 1.00 >= Duty 0.98); B5 on the army (r2n: suppression ordered, but the bandits stayed in sight: aimed fire, no suppression round, no flank; mercs 1/1) |

Decided (core.md K12): LOD for per-target refresh work only (identification and reaction unchanged); sound starts cut
at the root; B4 judged on the contact share (the member saw its enemy within 3 s), threshold unchanged; B5 counts
suppression fire, not orders; no deploy, no commit. Owed: one R2 for fix 94, the flash on this build, B4/B5 on the
army on ground that allows them.

| File | SHA-256 |
|---|---|
| `bin\x64\Release\xrGame.dll` (17:46) | `8AB1BF290E24943C5696B3BE10F6169B1205B5A34160491AC0A391A92FB698C5` |
| `bin\x64\Release\xrGame.pdb` (17:46) | `FF73918B743E890CC3BE0F4D7F761BD44AFDD0E789E8D255E47FBD0A444C53AA` |
| `bin\x64\Release\xrSound.dll` (17:07) | `1BD7F4CB8B325CDF70F930BE4043EFCAA6AC51F24DBBE2CD7AA9FB4054A8AC97` |
| `gamedata\configs\dead_air_x64_ai.ltx` (17:44) | `1168EDD5DA11CDB48B7BE428ECB55BD0F06353E2D8442E0D56CCFDECBEED0B5C` |

## 9. Review (adversarial pass, 28.09.2026)

Checked against the five research files, the task with its addendum and the engine sources. Changed
in this plan:

- **Radio trait was not stable for life** (C22, 2.3.4, 2.11). The engine commander is the lowest-id
  alive member (`alife_online_offline_group.cpp:356-361`), and the bonus was evaluated at every
  spawn, so a successor gained the dead leader's radio after an offline switch or a load. Now the
  trait is rolled once per NPC lifetime and saved in the new `.scov` chunk `AIT1`, owned by core.
  The AI leader is the highest band alive (`SquadState::leader`, `ai_is_leader()`). P8 checks the
  trait across save/load.
- **Silent tiers were mute for involuntary cries** (C23). `dummy_silent` also blanks death, pain,
  wounded and grenade-alarm lines. The hushed set now covers them. Involuntary cries carry no
  target estimate; only deliberate callouts carry the speaker's estimate (F7, 2.6).
- **Physical comms gaps** (2.6, comms C1-C4). Added the relay between radio carriers and
  non-carriers, with one hop. A bark is decoded once, although the engine re-emits AI sound events
  every 0.5 s, and no NPC decodes its own voice. Voice, hushed speech and gestures reach
  non-hostile NPCs of any squad, so garrisons still talk. Radio sounds are audible to enemies. A
  report's error now grows with the distance between the sender and the spot it describes, which
  gives sector-level detail. New counters `c_relay` and `c_radio_violation`, and the hard criterion
  P11.
- **Leaks the plan missed.** Dangers were built from pool entries that carry the last writer's
  position and time. `process_enemies` and the voice visitor copied a friend's danger object
  (C28). `xr_combat_ignore.is_enemy` range caps read `enemy:position()`; this is fixed by an
  overlay copy (C25, L3). The stock cover fire read the agent's detour list; it now needs a
  message or sight (T4). Morale read unseen enemies' equipment (M1). Grenades could be thrown at
  reported-only positions (T5, 2.5). All of these are in the cheat map.
- **Planner.** `DaSuppressFire` and `DaHoldAmbush` now also need `ShouldThrowGrenade f` and
  `PlayerOnThePath f`. Without them, their cheap direct path to `PureEnemy f` would always win
  and grenades would never be thrown. The get-distance loop has a progress rule (T1), because
  otherwise `WaitInCover` sends the NPC back to the same cover forever. Fanatics fall back only
  sideways or forward. With no acceptable cover in reach, an NPC falls back away from the enemy.
- **Engine realities.**
  - Estimates carry a valid level vertex (2.5).
  - The Lua list getters return references, so they use a per-NPC scratch vector.
  - The report callback cannot be a `RegisterScriptCallback` name because DA's `intercepts`
    table is closed (C24).
  - Weapon classes use DA's slot 4 for pistols. `ammo_9x18` is shared by pistols and SMGs (C27).
  - The noise bus goes through the same hearing function as engine sounds: occlusion, gunfire
    masking, rain. Each stimulus kind has exactly one source.
  - Friendly-fire and vision rays that only change flags are not counted against the ray budget.
    Muzzle flash is throttled per shooter.
  - The `AIK1` capture follows the existing Begin/Continue phase pattern.
- **Faction lore beyond radios** (2.4.1). Added values by faction and band for the keys that 2.4
  left at the default: hearing, aim bone, lead error, light discipline, stamina, courage,
  fanaticism, surrender, bounding, search style and Freedom's idle reaction. Also added
  `fire_on_order`: army, Duty R2+ and mercs spring an ambush on the leader's order. Armour classes
  are data-driven (2.3.2).
- **Tests** (7.3, 7.5).
  - R1 and R2 had NPCs 250-400 m from the actor, but ALife switches NPCs offline at 250 m (C26).
    Layouts are now rings within 200 m, and a run with broken geometry is void.
  - R1 puts A and B in one smart and keeps B in combat, so the stock pooling paths are actually
    exercised. It sets the hit direction, adds a second army squad for the HQ check, and makes the
    night observer a loner that the HQ relay cannot contaminate.
  - R4a used an army pair next to bandits, but the two factions are hostile. It is now a merc pair.
  - Timing windows are recomputed from the profile ranges and schedule periods: PDA at least 8 s,
    radio 1.6-5.5 s, wall staleness 0.75 s.
  - B8 now covers Duty radios.
  - The done marker is the harness's `DA_WATER_PROBE_DONE`.
  - Ownership of `tools/qa/ai` is no longer shared between two lanes.
  - Missing marker blocks were added: tactics in `Hit`, and know in the script list getters.

Checked and left as they were:
- C5 merge order (`x_ray.cpp:211-230`).
- Scheduler threading: the scheduler is serial. **Corrected 29.09 (review finding R1):** the object handler is not.
  With `mt_object_handler` (on by default) `CAI_Stalker::UpdateCL` queues `update_object_handler` through
  `add_to_seq_parallel` with the stalker as concurrency key, so the object handlers of different stalkers run at the
  same time on TaskScheduler workers, next to `DoRender`; `Exec_Visibility` is queued without a key (serial within
  the queue, still off the main thread). Nothing the object handler reaches may run Lua, touch another NPC's state,
  advance `::Random` or fill a shared cache: the fire gate is decided on the main thread every frame
  (`da_ai_update_cl` -> `Tactics::prepare_fire`, before the object handler is queued) and `fire_continues` only reads
  it; weapon lookups for the gate and for other NPCs use `DaAi::held_weapon` (hands, else the last choice), never
  `best_weapon()`.
- Evaluators save nothing (`property_evaluator.h:39-40`), so replacing them keeps the save stream.
- The `NODEFAULT` switch.
- Sub-planner storage format.
- `stage_chunks` runs before the ALife load.
- The explosive manager's own `visible_now` gate.
- The test budget: 7 runs + 2 reruns.
