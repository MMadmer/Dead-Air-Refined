# What the scripts save about a module

`SAVE_COMPATIBILITY.md` describes the save group and how a module's objects live in `.scov`,
behind inert placeholders in `.scop`. This document is the script side of the same rule:

- **Nothing a stock script writes into `.scop` or `.scoc` names a module's object, task or
  id** in a place where the original Dead Air 0.98b would stumble over it. Such entries are
  taken out of what the stock function writes, kept with the module's data in `.scov`, and
  put back when the save is loaded with the module there.
- **Nothing of a module's own script state is lost** while the module is away (switched off,
  removed from the disk, not for this game's mode - the same case) or while the save visits
  the original game.
- **A game without modules, and a save that holds nothing of a module, are written byte for
  byte as before.** Every wrapper below hands the stock function its own arguments untouched
  when nothing of a module is involved, and none of them is installed without the engine's
  ownership calls (below).

The code is `gamedata\scripts\xms_saves.script` (one of the `[common]` scripts of
`configs\script.ltx`) and the quest runtime (`xms_nq*.script`). The harness proof is
`tools\nq\test_examples.lua`, sections `(z30)`.

## 1. What the engine answers

The scripts ask the engine; each call is feature-tested, and without `xms.object_owner` no
stock function is wrapped at all.

| Call | Answer |
| --- | --- |
| `xms.object_owner(id)` | The module an ALife object belongs to, `nil` for the game's own. For the placeholder that holds the id of an object of a module that is away, that module. |
| `xms.own_object(id, module)` | Declares an object the module's: it is then saved with the module's data. |
| `xms.module_present(id)` | The module is enabled and loaded now (falls back to `xms.is_loaded`). |
| `xms.section_owner(section, file)` | The module that made a section of `file` (not one it only changed); used for `misc\task_manager.ltx`. Without it the task records of 3.4 stay as they are. |
| `xms.kept_modules()` | The modules this game keeps content of; informational. |

`xms_forget <module>` drops a module's chunks and objects in the engine and calls
`xms_saves.forget(<module>)`, which drops what the quest runtime keeps for it (section 4).

## 2. The module's chunk

Everything the scripts keep for a module rides its own per-module chunk of `.scov`
(`0x584D0000|ns`, `XMS_ARCHITECTURE.md` section 9) - the chunk `xms.save_data(id, str)` has
always written. No new chunk id is needed, and the chunk of a module that is away is the
engine's to keep byte for byte, through any number of saves and a trip to the original game.
The script side cannot touch it: `xms.save_data` and `xms.load_data` refuse a module that is
not enabled (the engine resolves only an enabled module's id), nothing else in the scripts
writes that channel, and the wrappers below write only for a module that is here.

The chunk has two forms:

- **The module's own string, byte for byte**, while that is all there is - exactly what
  `xms.save_data(id, str)` wrote before.
- **Parts**, once there is more: `\0 X M S \1`, then each part as a tag byte, its length in
  decimal, `:`, its bytes. `r` is the module's own string, `s` its storage (section 5), `k`
  the records kept for it (section 3). A part with another tag is kept as it is. `s` and `k`
  start with a version byte (`1`); a part this game cannot read is kept unchanged for the
  session and never overwritten.

`xms.save_data(id, str)` and `xms.load_data(id)` are wrapped so a module keeps reading and
writing its own string as before, beside its storage and its records; `nil` or `""` erases
only that part. A module script that kept the engine's own function from before the wrap and
writes around it is noticed: its string becomes the `r` part, the other parts stay. A script
that reads the chunk with such a kept function sees the parts; call `xms.save_data` /
`xms.load_data` through the `xms` table when you use them.

The values of `s` and `k` are plain data: booleans, finite numbers, strings, game times
(`CTime`) and tables of them with string, number or boolean keys, written with sorted keys
and read without running anything.

## 3. Records of the game's own objects

### 3.1 What the stock scripts write, and what the original game does with it

The audit of every stock writer into `.scop` (`STATE_Write` of the server classes, the
binders' `save`) and `.scoc` (`save_state(m_data)` handlers, pstor). *Original* is what
0.98b's own reader does when the named thing is absent or is the placeholder; the same holds
in Refined while the module is away.

| Place | Writes | Original / module away | Verdict | Done |
| --- | --- | --- | --- | --- |
| `smart_terrain` `STATE_Write` | ids of the NPCs it expects (`arriving_npc`) and holds (`npc_info`, with job section, begin, need) | a held placeholder is dropped by `init_npc_after_load` (not a creature); an expected one stays listed, at most one `fill_npc_info` line | harmless | taken out, kept, merged back (3.2) |
| `smart_terrain` respawn `already_spawned` | a module's squad sections a smart respawns | `STATE_Read` makes an empty entry | harmless | none |
| `sim_squad_scripted` `STATE_Write` | `current_target_id`, `assigned_target_id`, `smart_id`, `respawn_point_id` (+ section) | a placeholder target: a generic squad picks another; a squad with `target_random_smart` hands it to `specific_update`, which calls `am_i_reached` on the placeholder (`sim_squad_scripted:274`) | **stop** | written as none, kept, restored (3.2) |
| squad members (engine part of the group) | member ids | the engine resolves them on load | engine | named to the engine (section 7) |
| `se_stalker`, `se_monster` (engine part) | `m_smart_terrain_id` of a module's smart | the brain follows it to a smart terrain that is not there (`CALifeMonsterBrain::smart_terrain`); `on_unregister` / `on_death` call `unregister_npc` / `clear_dead` on the placeholder (`se_stalker:105`, `se_monster:97`, `smart_terrain:1502`) | **stop** | written as none, kept, restored (3.2) |
| `se_stalker`, `se_monster` (script part) | the offline section, here a section of a logic file the quest runtime made | `determine_section_to_activate` checks the section exists | harmless | written as none: the runtime gives the step back |
| `xr_logic.save_obj` (packet) and the binders' `save_state` (`game_object[id].xr_logic`) | an NPC's logic file: `*<tag>` of a behaviour step or a standing order | an NPC outside a smart terrain loads a nil logic file: `configure_schemes` indexes it (`xr_logic:35`) | **stop** | a game NPC is saved with its own logic (3.3) |
| `task_manager.save_state` (`task_info`, `task_objects`) | a task section a module made | `CGeneralTask(id)` reads no target: `pick_section_from_condlist(.., nil, nil)` indexes a nil NPC (`xr_logic:358`) during the load | **stop** | taken out, kept, merged back (3.4) |
| `axr_companions` (`companion_squads`, `non_task_companions`) | a module's squad and NPC ids | the placeholder never registers as a squad: `get_test_member` indexes `false` (`axr_companions:507`, the "move to point" key) | **stop** | taken out, kept, merged back (3.4) |
| `alife_storage_manager` `se_object`, `game_object`, `actor_contacts` | the state of a module's objects by id (its NPCs' pstor) | never asked for an id the placeholder holds, name-checked when asked, written back as it was | harmless | none: the module NPC's state survives both trips there |
| `se_heli` | `m_smart_terrain_id`, `respawn_point_id` of a heli a module's smart spawned | `on_unregister` indexes the placeholder (`se_heli:34`, `:40`) | **stop** | not changed (7) |
| `level_weathers` | `weather_fx` while a module's weather effect plays | `start_weather_fx_from_time` asserts on an unknown effect | **stop** | not changed (7) |
| `azazel_mode` | the actor's visual (a module outfit's mesh) | the mimic spawns with a model that is not there | **stop** | not changed (7) |
| `bind_anomaly_zone` | ids of artefacts it spawned | ids only compared; `turn_off` / `cleanup` release them - here the placeholder | harmless in 0.98b | not changed (7) |
| `xr_statistic` | weapon and artefact sections, smart and level names | membership and counters; the best weapon's name reads as empty | harmless | none |
| `coc_treasure_manager`, `axr_task_manager`, `trade_manager`, `sr_psy_antenna`, `bind_container`, `bind_campfire`, `pda`, `surge_manager`, `xr_sound`, `m_mines`, `heli_combat`, `release_item_manager`, `safe_release_manager`, `xr_zones`, `_g` | ids or names of a module's things | read per object, re-validated, or never read back | harmless | none |

Every other stock writer writes its own object only, stock names or numbers.

### 3.2 Smart terrains, squads, NPCs

For an object of the game (`xms.object_owner(self.id)` is nil) the writer of
`se_smart_terrain`, `sim_squad_scripted`, `se_stalker` and `se_monster` is wrapped:

1. the entries that name a module's object are taken out of the tables (or set to none) for
   the call, and put back right after it - the object itself never notices;
2. what was taken out is the record of that object in the owning module's `k` part:
   `smart[<id>] = { name, a = { ids }, i = { [id] = { j = job, b = begin, n = need } } }`,
   `squad[<id>] = { name, <field> = <id>, ... }`, `npc[<id>] = { name, smart = <id> }`; a
   module that held a record of the object and has nothing of it now loses the record;
3. the reader, after the stock reader ran, gives back what the module that is here keeps for
   an object of that id and name: the expected and held NPCs of a smart terrain (its own
   `init_npc_after_load` then checks them as for any NPC), a squad's fields that read as none
   (its smart through `assign_squad_to_smart`, as the stock reader does), an NPC's smart
   terrain while it reads as none. Whatever the object found for itself while the module was
   away stays; a record of another object's name is not applied.
4. at registration, when every object of the save is in the registry, an id given back that
   names nothing any more - the engine dropped the module's object with its section - or no
   smart terrain where one is meant, is set to none again with one line: the engine follows
   an NPC's smart terrain id to an object it must find (`CALifeMonsterBrain::smart_terrain`).

An object of a module is written whole into `.scov` by the engine, so its writer is not
touched.

### 3.3 A game NPC under a behaviour step or a standing order

The NPC is saved with its own logic: `<customdata>`, section `logic`, no active section - read
back, it chooses its section anew, or its smart terrain gives it its job. After a load the
quest runtime gives it the step or the order again (`heal_applied`, the orders' tick), with
the module there; with the module away the NPC simply runs its own logic. An NPC of a module
keeps its record as it is.

### 3.4 Tasks and companions in `.scoc`

After every `save_state` handler has written its part and before the table is encoded
(`CALifeStorageManager_capture_prepare_begin`), the task ids a module made and the module's
companion squads and NPCs are taken out (the companions script hands over its live table: a
copy goes into the save instead) and kept as `task.manager` and `companions.actor`. On load,
before any `load_state` handler reads the table (`CALifeStorageManager_load` sees the
`load_state` callback first), what a module that is here keeps goes back in.

## 4. The quest runtime and a module that is away

Records are judged after every scan (`xms_nq.script`, `sort_records`):

- **A module that is not here** (not enabled, not loaded, or not applying to this game's
  mode): every record of it is **kept**. Its module is the record's `mod` (older records: the
  uid before its last dot); a behaviour instance whose starter is kept is kept too.
  - Not run, not ticked, not reconciled or migrated: no token is wired, no trigger armed, no
    poll, no event, no job of `jobs` applied, no timer counted.
  - Invisible: `quest_state` gives nothing, `quest_status` reads `inactive`, its global
    variables read as unset and cannot be written (one line), `nq list` prints one line per
    module (`kept for <module>: <n> record(s)`), `nq state` says it is kept, `nq globals` counts
    them, `nq dump` marks them.
  - Written back as it was at every save, inside the runtime's blob (`0x584DFF01`).
  - Withdrawn from the live game: its PDA tasks are closed silently once (the engine offers no
    way to take a task out of the actor's registry; the record still says `active`); no story
    id of its refs is registered; an NPC of the game one of its steps or orders drove is let go
    to its smart terrain's job or its own logic, and its orders and hostility wait untouched;
    dialogs are registered only for quests that run. Nothing it keeps is changed by the live
    game: a quest that removes an object does not clear a kept quest's ref to it.
- **The module is back**: the ordinary restore path - a changed asset is reconciled as after
  any update (`NQ_RUNTIME.md` 11.4), refs are registered again, waiting tokens wired, a task
  the PDA lost is recreated from the declaration, a behaviour instance restored and its NPCs
  given their steps. A ref whose object now has another section (an id a game object gave up
  and something else took while the module was away) is dropped with a line rather than
  followed.
- **The module is here but has no such quest any more** (every asset of it was listed and
  read): the record is dropped with one line; its PDA tasks are closed and its anchors
  released. A record whose asset did not load stays, listed as "asset not loaded".
- **Forgetting**: `xms_forget <module>` -> `xms_saves.forget(<module>)` ->
  `xms_nq.forget_module(<module>)` drops the kept quests, instances, global variables and the
  orders its quests gave; `nq forget <module>` is the same runtime part from the console.
  Refused for a module that is here.

What a quest or a behaviour graph puts into the world - an NPC, a squad and every member it
was made with, a prop, an anchor restrictor, a body, items it lays out on the ground or in a
container - is declared its module's (`xms.own_object`), so the engine saves it with the
module's data. What `item.give` hands the player, or an item put into the player's own
pockets, is the player's ordinary property.

## 5. A module's own state: where to keep it

| What | Where | Lives |
| --- | --- | --- |
| Anything your scripts track: counters, flags, tables | `xms.storage("<module id>")` | your chunk of `.scov`; read when a save is loaded, written back at every save; kept while the module is away |
| A quest's own state | the quest's `vars` (`nq.vars`, `var.set`) | the quest runtime's blob, with the quest |
| What several quests or behaviour graphs of the module share | `mod.ltx [vars]` (`global.*`, `{global:name}`, `nq.global`) | the quest runtime's blob, per module |
| A string you encode yourself | `xms.save_data(id, str)` / `xms.load_data(id)` | the same chunk, beside the storage |

```lua
-- any script of the module, any time after the game is up (on_game_load, first update, ...)
local s = xms.storage("my_mod")
s.visits = (s.visits or 0) + 1
s.last = { level = level.name(), when = game.get_game_time() }
```

- `xms.storage(id)` returns the same table every time in a Lua state (`xms.storage_api == 1`
  says it is there). A new game starts it empty. It is there in every Lua state the engine
  makes - a new game, a load, a level change. A table read before the save is (in
  `on_game_start`) is refreshed in place when the save is read, so a reference kept from
  then stays valid.
- It is written back by itself at every save, after every `save_state` handler ran - what you
  put into it there is in that save. Nothing is written when nothing changed.
- Plain values only (section 2). A value that cannot be saved - a function, a game object, a
  table that contains itself - is left out with one line naming its key; the rest is saved.
- For a module that is away it returns `nil`; its data stays where it is.

Why not the game's own channels: `save_state(m_data)` handlers, pstor (`save_var`, `se_save_var`)
and the packets of `STATE_Write` / `save` are the original game's format. What a module puts
there is harmless to the original game, but it is not kept apart from the module: it is not
dropped by `xms_forget`, not kept in the module's chunk, and names things the original game
does not have. Keep a module's state in the module's own places.

## 6. Proof

`python tools\nq\run_lua.py tools\nq\test_examples.lua`, sections `(z30)`, through the mock
engine (`tools\nq\mock_engine.lua`, whose `xms.object_owner`, `own_object`, `module_present`
and `section_owner` stand in for the engine's): a quest saved mid-way and loaded without its
module (kept, untouched, not run, nothing listed but the kept line, its task closed), saved
again, loaded with the module (on the node it was at, global back, task back, finishes); the
removed quest dropped with one line; `nq forget`; a behaviour instance kept and the game NPC
it drove let go and given its step again; quest-spawned squads, members and props declared
the module's, an item given to the player not; `xms.storage` saved, loaded, beside
`xms.save_data`, kept while the module is away, a refused value, an early read; the wrappers
over stand-ins of the stock writers - a module's entries taken out and put back, stock output
identical when nothing of a module is involved, records restored only when nothing changed
meanwhile and set to none at registration when the module's object is gone, the logic of a
game NPC on a standing order saved as its own in the packet and in `.scoc`, the task and
companion tables of `.scoc`, and nothing wrapped without the engine's calls. The original 0.98b is not on this
machine: what it does is read from its scripts (the unpacked 0.98b and Revolution II trees).

## 7. Limits

What the scripts do not change, with the reason:

- `se_heli` of a module's smart terrain, `level_weathers` while a module's weather effect
  plays, `azazel_mode` with a module outfit's mesh: each needs a way to tell a module's
  effect, model or smart from the game's that the scripts do not have, and each is rare.
- `bind_anomaly_zone` and `xr_zones` may release a placeholder that holds an artefact's id;
  the original game is fine, but the id is then free. A module's artefact in a stock zone's
  list is the case.
- A stock squad whose member list holds a module's NPC, and the actor's task registry holding
  the quest runtime's tasks (`nq.<module>.<quest>.<task>`), are written by the engine; the
  tasks are shown in the original game's PDA and, while the module is away, closed rather
  than taken out.
- A module's own smart terrain that held an NPC of the game may still list that NPC after it
  moved elsewhere while the module was away (the smart's record is the module's, the NPC's
  the game's).
- An NPC of the game saved by an older build while it ran a step keeps the step's file name
  in its save until it is saved again; the runtime keeps putting such files back before the
  world spawns, so it loads.
