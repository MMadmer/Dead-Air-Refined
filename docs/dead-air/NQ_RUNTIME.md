# NQ — the node quest graph runtime

Dead Air: Refined interprets quest graphs at runtime. A module ships a
declarative description of a quest; the game reads it, validates it and runs it.
Nothing is compiled, generated or copied into `gamedata` — the editor that
authors these files never emits Lua, XML or LTX for them, and a runtime fix in a
game update fixes every module at once.

This document is the game-side contract: what a module ships, what the format
means, how the runtime executes it, and what freezes when the editor reaches its
first public release. The authoring side (editor UI, MCP tools, Russian kind
titles) is documented in XFined Editor's `docs/nq/NQ_FORMAT.md`.

Related specs: `MODDING.md` (module layout), `XMS_ARCHITECTURE.md` (the module
system NQ rides on), `SAVE_COMPATIBILITY.md` (the save chunk NQ writes into).

---

## 1. What NQ is and what a module ships

One quest is one file:

```
<game root>/modules/<mod.id>/
  quests/wolf_debt.nqasset      ; anywhere inside the module, any folder depth
```

The runtime scans **the whole module root recursively** for `*.nqasset`
(`xms.list_files(id, "", "*.nqasset", true)`), so the folder layout is the
author's business. There is no registry, no manifest key, no index file: the
folder is the only source of truth.

A quest gets a **uid** of `<module id>.<quest id>`. Everything a quest creates in
the engine is namespaced under it, so two modules can ship a quest called
`wolf_debt` without ever colliding:

| Thing | Id in the engine |
|---|---|
| dialog | `nq.<module>.<quest>.<topic node>` |
| PDA task | `nq.<module>.<quest>.<task>` |
| story id of a quest-created object | `nq.<module>.<quest>.<ref>` |

A module may also extend the kind catalog (§4) with
`gamedata/configs/nq/kinds/<mod>.ltx` plus implementations registered from its
`scripts/register.script`. That is the only way to add new node kinds; the
`.nqasset` format itself never changes shape.

Quests are gated exactly like everything else a module ships: the scan only
looks at modules that are enabled and pass `xms.module_applies(id)`, so a quest
written for one game mode never runs in another.

---

## 2. The `.nqasset` format

### 2.1 Rules

- UTF-8 without BOM, `\n` line endings, extension `.nqasset`.
- The content is **one `return { … }`** and nothing else. The file is loaded with
  `loadstring` + `setfenv(chunk, {})` — an empty environment with no libraries
  and no globals. Anything that is code (a function call, an operator on a
  global, `require`, `local`, a second return value) fails at load; the quest is
  rejected with `E003` and its neighbours are untouched.
- Custom Lua is allowed in exactly one place: the string parameter `code` of the
  `lua` kind, used as an extra action or as a condition (§9).
- Table keys are written as identifiers (`id = …`). Keys that are Lua reserved
  words must use bracket form: `["else"] = …` (the `else` pin of `flow.branch`)
  and `["not"] = true` (condition inversion). `["repeat"] = …` is only needed
  when a table constructor spells the `trigger.when` parameter out as a key —
  inside `params = { … }` it is written `["repeat"] = true`.
- `pos` on a node is **optional**. The runtime ignores it entirely; it exists so
  the editor can remember canvas coordinates. A hand-written or AI-written quest
  never needs it.
- `comment` on a node is likewise editor-only and ignored by the game.
- The editor rewrites the file canonically on save and prepends a generated
  comment block with the quest outline. The runtime skips comments like any Lua
  chunk does — the header carries no meaning.

### 2.2 Schema

```lua
return {
  nq         = 1,                     -- format version (required, must be 1)
  id         = "wolf_debt",           -- [a-z0-9_], unique inside the module
  title      = "Долг вежливости",     -- text
  activation = "auto",                -- "auto" (default) | "manual"
  vars       = { <name> = <bool|number|string default>, … },
  tasks      = { <task id> = { title = <text>, descr = <text>,
                               type = "additional"|"storyline",
                               target = <target_ref|place>, icon = <string> }, … },
  nodes      = { <node>, <node>, … },
}

<node> = {
  id       = "meet_wolf",             -- [a-z0-9_], unique inside the quest
  kind     = "dialog.topic",          -- a catalog kind usable as main or trigger
  once     = true,                    -- optional; the catalog gives the default
  params   = { … },                   -- the kind's parameters
  cond     = { <cond>, … },           -- optional AND-list
  on_enter = { <action>, … },         -- optional, run in order before the main action
  on_exit  = { <action>, … },         -- optional, run in order after it
  out      = { <pin> = "<node id>" | { "<id>", "<id>" }, … },
  pos      = { 0, 160 },              -- editor only
  comment  = "…",                     -- editor only
}

<action> = { kind = "item.give", params = { section = "wpn_pm", count = 1 } }
<action> = { kind = "lua",       params = { code = [[ nq.news("Привет") ]] } }
<cond>   = { kind = "var", params = { name = "agreed", op = "eq", value = true } }
<cond>   = { kind = "has_money", params = { amount = 500 }, ["not"] = true }
```

Everything the catalog describes has the same shape: `{ kind, params }`.
Conditions additionally accept `["not"] = true`, which inverts the result after
evaluation (including the "was it this event?" test).

Unknown keys anywhere are ignored; unknown *kinds* are errors.

### 2.3 Value types

The catalog declares a type per parameter. These are the types the loader knows
(`canon_param`); anything else is treated as a plain string.

| Type | Written as | Notes |
|---|---|---|
| `text` | `"строка"` or `{ rus = "…", eng = "…" }` | UTF-8; placeholders `{var:name}`, `{actor}` |
| `string` | `"…"` | also `item_section`, `squad_section`, `level`, `smart`, `story_id`, `profile`, `community`, `info`, `var_name`, `task_id`, `quest_id`, `ref_name`, `signal_name`, `spot_type`, `node_id` |
| `int` | `5` | integer; `min`/`max` from the catalog are enforced |
| `float` | `2.5` | `min`/`max` enforced |
| `bool` | `true` / `false` | must be a real boolean |
| `enum` | `"actor"` | must be one of the catalog's `enum=a\|b\|c` |
| `relation` | `"enemy"` \| `"neutral"` \| `"friend"` | |
| `value` | bool, number or string | for `var.set` / `var` |
| `count_or_all` | integer ≥ 1 or `"all"` | `item.take` |
| `duration` | `{ seconds = N }` real time, `{ game_minutes = N }`, `{ game_hours = N }`, `{ game_seconds = N }` game time, or a plain number (real seconds) | |
| `npc_ref` | `{ story = "esc_2_12_stalker_wolf" }` \| `{ ref = "guard" }` \| `{ profile = "…" }` \| `{ community = "stalker", level = "l01_escape" }` | §7 |
| `target_ref` | any `npc_ref`, or `{ ref = "boars" }` (squad/object), or `{ smart = "…" }` | |
| `kill_target` | `{ story = … }` \| `{ ref = … }` \| `{ spawn = <spawn_spec> }` | only `objective.kill` |
| `place` | `{ level = "l01_escape", pos = {x,y,z}, radius = 5 }` \| `{ restrictor = "zone_name" }` \| `{ smart = "…" }` | `radius` defaults to 5 |
| `spawn_spec` | `{ section = "simulation_boar", smart = "…", ref = "boars", hold = true }` | `section` and `smart` are required |
| `cases_cond` | `{ { name = "yes", cond = { … } }, … }` | case names become pins |
| `cases_weight` | `{ { name = "a", weight = 3 }, … }` | weight defaults to 1, must be > 0 |
| `cond_list` | `{ <cond>, … }` | the `of` parameter of `any` |
| `lua` | `[[ … ]]` | compiled once at load |

A missing parameter takes the catalog default; a missing parameter marked
`required` is `E006`. A wrong type is `E006` and the parameter is dropped.

### 2.4 A complete quest

```lua
return {
  nq = 1,
  id = "wolf_debt",
  title = "Долг вежливости",
  activation = "auto",
  vars = { agreed = false },
  tasks = {
    kill_boars = { title = "Отстрел кабанов",
                   descr = "Волк просил перебить кабанов у деревни новичков.",
                   type = "additional", target = { ref = "boars" } },
  },
  nodes = {
    { id = "start", kind = "trigger.start", out = { next = "meet" } },
    { id = "meet", kind = "dialog.topic", once = true,
      params = { npc = { story = "esc_2_12_stalker_wolf" },
                 text = "Слышал, у тебя проблемы с кабанами." },
      out = { next = "wolf_1" } },
    { id = "wolf_1", kind = "dialog.npc_phrase",
      params = { text = "Лезут из леса. Перебьёшь — не обижу." },
      out = { next = { "accept", "decline" } } },
    { id = "accept", kind = "dialog.actor_phrase", params = { text = "Помогу." },
      on_exit = { { kind = "var.set", params = { name = "agreed", value = true } },
                  { kind = "task.give", params = { task = "kill_boars" } } },
      out = { next = "kill" } },
    { id = "decline", kind = "dialog.actor_phrase", params = { text = "Не моё дело." } },
    { id = "kill", kind = "objective.kill",
      params = { target = { spawn = { section = "simulation_boar",
                                      smart = "esc_smart_terrain_3_16",
                                      ref = "boars", hold = true } } },
      out = { done = "finish" } },
    { id = "finish", kind = "flow.end", params = { status = "completed" } },
  },
}
```

---

## 3. The execution model

### 3.1 Quest lifecycle

`inactive → active → completed | failed`.

- `activation = "auto"` — activated on the first safe moment of every game and
  every load (`actor_on_first_update`), if the module applies and the persisted
  state holds no record for the quest. This means a quest reaches an **existing
  save** as soon as the module is installed: it simply starts on the next load,
  the same way additive spawn ops do.
- `activation = "manual"` — activated by another quest's `quest.activate` action
  or by the console `nq activate <uid>`.
- Activating creates the state record, copies `vars` from their defaults and
  arms every trigger. `trigger.start` fires immediately.
- The quest ends when a `flow.end` node is entered (status taken from its
  `status` parameter), or **implicitly** when no token is left anywhere and no
  trigger is still armed — that case is recorded as `completed` with a log line.
  A quest whose `trigger.when` has `["repeat"] = true` therefore stays active
  forever until an explicit `flow.end`; that is how a background quest is built.
- `flow.end` with `restart = true` finishes the quest, then activates it again:
  tokens, `done`, `fired`, `joins`, `timers` and `trig` state are dropped and
  `vars` go back to their defaults, while `refs` and PDA task states survive.
- `activate` refuses a quest whose record is already `active`, `completed` or
  `failed` (it logs a warning). The console's `nq activate` drops the record
  first, which is why it works on a finished quest.
- `quest_status` of a quest that has no record — never activated, module removed,
  module not applicable — reads `inactive`.

### 3.2 Node lifecycle and tokens

A **token** means "the quest currently stands on this node". A node holds at
most one token.

```
enter(node):
  flow.join only: count this arrival; return until every incoming edge arrived
  token already present            -> ignore
  node.once and node already done  -> ignore
  tokens[node] = { t0 = <game seconds>, r = 0, w = {}, k = <kind> }
  run on_enter actions, in order, each inside pcall
  res = kind.begin(ctx)
    res == "wait"                  -> the token stays; the node is wired for polls/events
    res == { pin = "…" }           -> complete(node, pin)
    res == { finish = { … } }      -> complete(node, nil) then finish the quest
    anything else                  -> complete(node, nil)

complete(node, pin):
  kind.finish(ctx, pin, external)  (waiting kinds only)
  run on_exit actions, in order
  drop the token; if node.once then mark the node done
  for every target of node.out[pin], in file order: enqueue enter(target)
```

- A waiting kind is driven by `poll(ctx)` (every 250 ms) and/or
  `on_event(ctx, evt)`; returning a pin name completes the node. `begin` always
  makes the first check itself, so a node whose condition is already satisfied
  (target already dead, item already carried, actor already at the place)
  completes the moment it is entered.
- A pin with no targets simply ends that branch. A pin with several targets
  activates **all** of them — that is the fork.
- `flow.join` counts arrivals against the number of incoming edges declared in
  the graph and passes the token on only when all of them have arrived, then
  resets its counter. Arrivals are keyed by `<source node>/<pin>`.
- Loops (an edge pointing backwards) are legal. `once` is what stops a node from
  being re-entered.
- `cancel(ctx)` is called instead of `finish` when a token is removed without
  completing (quest finished, reset, `nq jump`) so the kind can clean up map
  spots, anchors and subscriptions.

### 3.3 Ordering, the tick and the deferred queue

- Everything runs on one thread. The tick is `actor_on_update`; events come from
  the game's script callbacks (`actor_on_item_take/drop/use`,
  `actor_on_info_callback`, `npc_on_death_callback`, `monster_on_death_callback`,
  `squad_on_npc_death`, `squad_on_unregister`, `squad_on_register`,
  `actor_on_leave_dialog`, `actor_on_trade`, `on_level_changing`).
- **Every transition goes through a deferred queue.** `enter` is never called
  synchronously — not from a completion, not from a dialog callback, not from an
  event. The queue is drained once per frame; transitions produced during a drain
  are appended to the tail, so the drain never re-enters itself.
  `on_enter`/`on_exit` actions of a node, by contrast, run synchronously and in
  order.
  This is not decoration: changing quest state inside `CPhraseDialog::SayPhrase`
  can filter away every continuation of the phrase the engine is about to offer
  and trip its `R_ASSERT2("No available phrase to say")`. The queue moves that
  work to the next frame.
- Polls run at most every 250 ms, over the list of waiting tokens and armed
  triggers that asked for polling.
- Iteration order is deterministic everywhere: quests by uid, nodes by their
  order in the file, event subscribers by (uid, node index).
- The queue is capped at 20000 pending operations; overflow drops the operation
  with an error line rather than growing without bound.

### 3.4 `once`

`once` is per node and its default comes from the catalog (`dialog.topic` and
`trigger.start` default to `true`, everything else to `false`). A node marked
`once` that has already completed is never entered again, and a `once` phrase is
filtered out of a dialog after it has been said.

### 3.5 Error containment

Every boundary is wrapped in `pcall`: asset load, `begin`/`poll`/`on_event`/
`finish`/`cancel`/`save`/`load`, every action, every condition, every dialog
callback.

- An action that throws is logged as
  `! [nq] <uid>/<node>: <slot>#<i> <kind>: <error>` and skipped. The remaining
  actions and the transition still happen.
- `begin` that throws leaves the token in place, records the error in the state
  (`nq state` shows it) and is retried no more often than every 5 seconds.
  A `poll` that throws is retried the same way, and the error is only logged once
  per 5 s window.
- A condition that throws reads as `false`.
- An invalid asset is not loaded at all: the errors are printed, the quest shows
  up as `INVALID` in `nq list`, and every other quest is unaffected.
- A kind that the catalog declares but nobody implemented does **not** invalidate
  the quest. The quest loads, the missing kinds are listed once at scan time, and
  a node of that kind errors when it is entered.
- A game crash caused by the *content* of a `.nqasset` is a runtime bug, not an
  author error.

### 3.6 Zero cost without quests

`on_game_start` registers exactly one callback: `actor_on_first_update`. If the
scan finds no quest in any applicable module, none of the other callbacks are
registered at all and the log says so. No quests means no polls, no event
dispatch and no save blob.

With **no module at all** the engine builds no `xms` table, so the runtime cannot
even read its own blob. It then does nothing and stays completely silent: a game
without modules must not gain a single `[nq]` line. A warning is printed only when
`xms` exists but has no `list_files`/`read_file`, because that means the scripts
and the engine disagree about the API.

---

## 4. The catalog and how a module extends it

### 4.1 Format

The catalog is `configs\nq\catalog.ltx` plus every `configs\nq\kinds\*.ltx`
(read in sorted order). LTX was chosen because both sides read it natively — the
game through `ini_file`, the editor through `CInifile` — and because a module can
drop a file into the mounted VFS to extend it.

One kind is one section named `nq.<kind>`:

```ini
[nq.catalog]                       ; only in the core catalog.ltx
version = 1                        ; catalog version
api     = 1                        ; version of the kind implementation contract

[nq.item.give]                     ; section = "nq." .. kind
group   = items                    ; group in the editor dropdown
title   = Выдать предмет           ; cp1251, what the author sees
desc    = Даёт игроку предметы секции
use     = extra                    ; comma list of: trigger, main, extra, cond
params  = section, count           ; ordered parameter names
section = item_section, required   ; <type>[, required][, default=v][, min=v][, max=v][, enum=a|b|c]
count   = int, default=1, min=1
pins    =                          ; main/trigger: pin list; "cases" = pins come from params.cases
wait    = false                    ; main: is this a waiting kind?
once    = false                    ; main/trigger: default of node.once
event   = false                    ; cond: true for edge-triggered event.* conditions
impl    = core                     ; core | <module id> — who registers the implementation
since   = 1                        ; catalog version the kind appeared in
alias   = old.kind                 ; optional, comma list of retired names still accepted
```

The file is **cp1251**, like every other game config. `title` and `desc` are the
only human-language fields; everything else is ASCII.

A kind whose section is defined twice wins from the file read later, and the
duplicate is logged.

### 4.2 Extending it from a module

```
<module>/gamedata/configs/nq/kinds/mymod.ltx     ; new [nq.*] sections
<module>/scripts/register.script                 ; the implementations
```

```lua
-- register.script of the module
local reg = xms.registry.get("nq.kinds")
reg:add("mymod.brew_tea", {
    begin = function(ctx) … end,
    poll  = function(ctx) … end,
})
```

Module registrars run at bootstrap, before the NQ runtime initialises, so an
entry a module installed is **kept**: extensions win over the core, and the core
logs a warning instead of overwriting. The core registers its own kinds through
the same registry from `xms_nq_kinds`, `xms_nq_dialog`, `xms_nq_task` and
`xms_nq_world`.

### 4.3 The kind implementation contract

An implementation is a plain table. Every field is optional except the one the
kind's position requires (`begin` for main, `arm`/`poll`/`on_event` for a
trigger, `run` for extra, `test` for cond).

| Field | Position | Signature and meaning |
|---|---|---|
| `begin(ctx)` | main | Entry point. Return `{ pin = "next" }` to complete at once, `"wait"` to keep the token, or `{ finish = { status = "completed"\|"failed", restart = <bool> } }` to end the quest. Anything else completes the node with no pin (the branch stops). |
| `poll(ctx)` | main, trigger | Called at most every 250 ms while the token waits / the trigger is armed. Return a pin name to complete (or, for a trigger, to fire); `nil` to keep waiting. `ctx.dt` is the seconds since the previous poll. |
| `on_event(ctx, evt)` | main, trigger | Called for every event the node is subscribed to. Same return contract as `poll`. |
| `finish(ctx, pin, external)` | main | Called on **every** completion, before `on_exit` runs. `external` is true when the completion was queued from outside the kind — a console `nq fire`, or a `once` `dialog.topic` passed by its leaf phrase. Only called if `begin` returned `"wait"`. |
| `cancel(ctx)` | main | Called when the token is removed without completing (quest finished/reset, `nq jump`). Only called if `begin` returned `"wait"`. |
| `save(ctx)` | main | Return a plain-data table to persist alongside the token. Called for every waiting token right before the blob is staged. |
| `load(ctx, w)` | main | Called after a load for every waiting token, with the table `save` returned. Re-create anything the engine does not persist (map spots, subscriptions). |
| `events` | main, trigger | Array of event names to subscribe to unconditionally, e.g. `{ "npc_killed", "squad_dead" }`. |
| `subscribe(ctx)` | main, trigger | Return an array of extra event names, computed from the node's parameters. |
| `wants_poll(ctx)` | main, trigger | Return `false` to stay out of the poll list (pure event-driven node). When absent, the node polls exactly when `poll` exists. |
| `arm(ctx, existed)` | trigger | Called whenever the trigger is (re)armed. `existed` is true when the trigger already had persisted state. Return a pin name to fire immediately, `nil` to wait. |
| `run(ctx, params)` | extra | Instant operation. Throw to report failure. |
| `test(ctx, params[, evt])` | cond | Return a boolean. Event conditions receive the matching `evt` and are only called when `evt.name` matches. |

`ctx` carries:

| Field | Meaning |
|---|---|
| `ctx.quest`, `ctx.uid` | quest uid |
| `ctx.def`, `ctx.quest_def` | the canonical quest definition |
| `ctx.qs` | the persisted quest record |
| `ctx.node` | the node table; `ctx.node_id` its id |
| `ctx.params` | canonical parameters of the node |
| `ctx.tok` | the token; `ctx.w` its persisted sub-state (write plain data only) |
| `ctx.trig` | a trigger's persisted sub-state (triggers only) |
| `ctx.vars` | proxy over the quest variables (assignment writes through) |
| `ctx.refs` | the quest's ref table |
| `ctx.npc` | the dialog partner, or `nil` |
| `ctx.actor` | `db.actor` |
| `ctx.now` | `{ g = <game seconds>, r = <real ms> }` |
| `ctx.dt` | seconds since the previous poll (poll only) |
| `ctx.log(fmt, …)` | logs `* [nq] <uid>/<node>: …` |
| `ctx.emit(evt)` | queues an event for every subscriber |

Anything a kind puts into `ctx.w` or `ctx.trig` must be plain data
(booleans, numbers, strings, nested tables of those): it is serialized into the
save blob.

---

## 5. Catalog v1

Generated from `configs\nq\catalog.ltx` (version 1, api 1). Parameter notation:
`name: type` — required parameters are in **bold**, defaults follow `=`.

### 5.1 Triggers (`use = trigger`)

A trigger is a root node: it has no input, and it is armed as long as the quest
is active. An edge may never point at a trigger (`E007`).

| Kind | Params | Pins | `once` default | What it does |
|---|---|---|---|---|
| `trigger.start` | — | `next` | `true` | Fires once, the moment the quest is activated. |
| `trigger.when` | `repeat: bool = false`, `cooldown: duration` | `next` | `false` | Fires on the rising edge of the node's `cond` list. Arming counts as "false", so conditions that are already true fire it on the first poll. Without `repeat` it fires once per activation; with `repeat` it re-arms after the conditions go false again, no more often than `cooldown`. |

Conditions live in the node's `cond`, not in `params`. When the list contains an
event condition (`event.*`) the trigger stops polling and is evaluated purely on
that event: the event condition is true exactly at the moment the event arrives
and the rest of the list is checked right then.

### 5.2 Main actions (`use = main`)

`wait = true` marks a kind that keeps its token until something happens.

| Kind | Params | Pins | wait | `once` | What it does |
|---|---|---|---|---|---|
| `dialog.topic` | **`npc: npc_ref`**, **`text: text`**, `initiator: enum(actor\|npc) = actor` | `next`, `done` | ✔ | `true` | A conversation topic on an NPC. While the token sits here and the node's `cond` holds, the topic is in the talk list. `next` leads to the phrases; `done` fires when a leaf phrase has been said. |
| `dialog.npc_phrase` | **`text: text`** | `next` | | `false` | A line the NPC says. The node's `cond` is its display condition; the first declared line whose condition holds is the one spoken. |
| `dialog.actor_phrase` | **`text: text`** | `next` | | `false` | A reply the player can pick. The node's `cond` is its display condition. |
| `objective.kill` | **`target: kill_target`**, `by_actor: bool = false` | `done` | ✔ | `false` | Waits for the target to die: an NPC by story id, an object remembered under a `ref`, or a squad from `spawn` (created on entry and remembered under its `ref`). `by_actor` additionally demands that the player landed the kill. |
| `objective.fetch` | **`section: item_section`**, `count: int = 1` | `done` | ✔ | `false` | Waits until the player's inventory holds `count` items of the section. |
| `objective.reach` | **`place: place`**, `map_spot: bool = true`, `spot_text: text` | `done` | ✔ | `false` | Waits until the player is inside the place. With `map_spot` a secondary map marker is put on it (on an anchor restrictor for a bare position) and removed when the node ends. |
| `wait.timer` | **`duration: duration`** | `done` | ✔ | `false` | Waits for the given time — game time for `game_*`, real time for `seconds`. |
| `wait.when` | `timeout: duration` | `done`, `timeout` | ✔ | `false` | Waits until the node's `cond` list becomes true; `timeout` gives up through the other pin. |
| `wait.any` | **`cases: cases_cond`** | one per case | ✔ | `false` | Several named condition sets; the first one that becomes true (in declared order) wins and its case name is the pin. |
| `flow.branch` | **`cases: cases_cond`** | one per case, `else` | | `false` | Instant: the first case whose conditions hold, otherwise `else`. Event conditions are not allowed here (`E020`). |
| `flow.random` | **`cases: cases_weight`** | one per case | | `false` | Instant weighted random pick. |
| `flow.join` | — | `next` | ✔ | `false` | Waits until a token has arrived along every incoming edge, then passes one on and resets. |
| `flow.step` | — | `next` | | `false` | An empty main action: a stage that exists only for its `on_enter`/`on_exit`. |
| `flow.end` | `status: enum(completed\|failed) = completed`, `restart: bool = false` | — | | `false` | Terminal. Cancels every token, disarms the triggers and records the status. `restart` resets and activates the quest again. |

Instant world operations — spawning, giving, rewarding — are deliberately **not**
main actions. A node is a meaningful stage of the quest; the rest is the
`on_enter`/`on_exit` wrapping around it. Use `flow.step` when you want a stage
that does not wait.

### 5.3 Extra actions (`use = extra`, all instant)

| Kind | Params | What it does |
|---|---|---|
| `item.give` | **`section: item_section`**, `count: int = 1` | Gives items to the player. Inside a dialog the transfer goes through the NPC so the talk window shows it. Ammo sections are created with the proper box size. |
| `item.take` | **`section: item_section`**, `count: count_or_all = 1` | Takes items away; `"all"` takes every one. Inside a dialog it goes through the NPC. |
| `item.spawn` | **`section: item_section`**, `place: place`, `into: target_ref`, `ref: ref_name` | Creates an item at a place, or inside a container/NPC when `into` is given (`into` wins over `place`). `ref` remembers it. |
| `money.give` | **`amount: int ≥ 1`** | Gives money (through the NPC inside a dialog). |
| `money.take` | **`amount: int ≥ 1`** | Takes money. |
| `info.give` | **`info: info`** | Gives an info portion — the interop channel with vanilla logic. An unknown id does not assert. |
| `info.disable` | **`info: info`** | Removes an info portion. |
| `var.set` | **`name: var_name`**, **`value: value`** | Assigns a quest variable. |
| `var.add` | **`name: var_name`**, **`delta: float`** | Adds to a numeric quest variable (a non-number reads as 0). |
| `signal.send` | **`name: signal_name`** | Emits a signal for `event.signal` conditions, including those of other quests. |
| `quest.activate` | **`quest: quest_id`** | Activates another quest (`"<module>.<quest>"` or a bare `"<quest>"` meaning this module). Errors if there is no such quest. |
| `task.give` | **`task: task_id`** | Creates the PDA task from the quest's `tasks` declaration. |
| `task.complete` | **`task: task_id`** | Marks it completed. |
| `task.fail` | **`task: task_id`** | Marks it failed. |
| `task.remove` | **`task: task_id`** | Closes it quietly and forgets it (`task_status` goes back to `none`). |
| `task.set_target` | **`task: task_id`**, **`target: target_ref`** | Moves the task's map marker onto another object. |
| `task.set_text` | **`task: task_id`**, `new_title: text`, `new_descr: text` | Rewrites the task's title and/or description. |
| `spawn.squad` | **`section: squad_section`**, **`smart: smart`**, `ref: ref_name`, `hold: bool = true` | Creates a squad on a smart terrain. `hold` pins it there; `ref` remembers it. |
| `spawn.object` | **`section: string`**, **`place: place`**, `ref: ref_name` | Creates an arbitrary object at a place. |
| `squad.move` | **`target: target_ref`**, `smart: smart`, `follow_actor: bool = false` | Sends a squad to a smart terrain, or after the player. One of the two is required. |
| `squad.remove` | **`target: target_ref`** | Removes a squad from the world. |
| `npc.remove` | **`npc: npc_ref`** | Removes an NPC from the world. |
| `npc.kill` | **`npc: npc_ref`** | Kills an NPC (online), releases it otherwise. |
| `relation.set` | **`who: npc_ref`**, **`value: relation`** | Makes an NPC, a squad or a whole faction an enemy, a neutral or a friend of the player. A `{ community = … }` ref goes the faction route. |
| `relation.goodwill` | **`who: npc_ref`**, **`delta: int`** | Adds to (or subtracts from) goodwill towards the player. |
| `news.tip` | **`text: text`**, `sender: npc_ref`, `icon: string`, `duration: float = 5` | A PDA news message; `sender` picks whose portrait shows. |
| `map.spot` | **`target: target_ref`**, `text: text`, `spot: spot_type = secondary_task_location` | Puts a marker on the map. |
| `map.unspot` | **`target: target_ref`**, `spot: spot_type = secondary_task_location` | Removes it. |
| `dialog.force` | **`npc: npc_ref`**, `allow_break: bool = true` | Forces the talk window open with an NPC (who must be online and near). |
| `dialog.break` | — | Closes the current conversation. |
| `actor.teleport` | **`place: place`** | Moves the player inside the current level. A place on another level is `E031` in the editor and a runtime error in game — there is no scripted cross-level teleport in Dead Air, level changers are the way. |
| `sound.play` | **`theme: string`** | Plays a sound theme on the player. |
| `lua` | **`code: lua`** | Custom Lua (§9). |

### 5.4 Conditions (`use = cond`)

Usable anywhere a `cond` list is: node conditions, phrase display conditions,
`flow.branch` / `wait.any` cases.

| Kind | Params | True when |
|---|---|---|
| `has_info` | **`info: info`** | the player holds the info portion (works offline) |
| `has_item` | **`section: item_section`**, `count: int = 1` | the player carries at least `count` of them |
| `has_money` | **`amount: int`** | the player has at least that much money |
| `var` | **`name: var_name`**, `op: enum(eq\|ne\|lt\|le\|gt\|ge) = eq`, **`value: value`** | the quest variable compares that way (ordering comparisons need numbers) |
| `node_done` | **`node: node_id`** | that `once` node has already completed |
| `quest_status` | **`quest: quest_id`**, **`is: enum(inactive\|active\|completed\|failed)`** | another quest is in that state |
| `task_status` | **`task: task_id`**, **`is: enum(none\|active\|completed\|failed)`** | this quest's PDA task is in that state |
| `actor_on_level` | **`level: level`** | the player is on that level |
| `actor_in_place` | **`place: place`** | the player is inside the place |
| `npc_alive` | **`npc: npc_ref`** | the NPC exists and is alive |
| `npc_dead` | **`npc: npc_ref`** | the NPC is dead or gone |
| `squad_alive` | **`target: target_ref`** | the squad still exists |
| `actor_community` | **`community: community`** | the player belongs to that faction |
| `relation` | **`who: npc_ref`**, **`is: relation`** | that NPC/faction is enemy, neutral or friend of the player |
| `time_of_day` | **`from: int 0..23`**, **`to: int 0..23`** | the game hour is in the range (a range crossing midnight works) |
| `elapsed` | `from: enum(quest\|node) = quest`, `node: node_id`, **`duration: duration`** | at least that much time passed since the quest was activated / the node was entered |
| `chance` | **`percent: int 0..100`** | a random roll succeeds — for a phrase, rolled on every show |
| `any` | **`of: cond_list`** | at least one nested condition is true |
| `lua` | **`code: lua`** | the code returns a truthy value (§9) |

### 5.5 Event conditions (`use = cond`, `event = true`)

These are edge-triggered: they are true only in the instant the event arrives.
They are legal **only** inside `trigger.when`, `wait.when` and `wait.any`
(`E020` everywhere else, including phrase conditions and `flow.branch` cases).

| Kind | Params | Fires on |
|---|---|---|
| `event.item_taken` | `section: item_section` | the player picked an item up (empty section = any) |
| `event.item_dropped` | `section: item_section` | the player dropped one |
| `event.item_used` | `section: item_section` | the player used one |
| `event.npc_killed` | `who: npc_ref`, `by_actor: bool = false` | an NPC died (empty `who` = any); `by_actor` restricts it to the player's kills |
| `event.squad_dead` | **`target: target_ref`** | a squad (by `ref` or `story`) stopped existing |
| `event.info_given` | **`info: info`** | the player received an info portion |
| `event.level_entered` | **`level: level`** | the player arrived on that level (also on the very first update of a new game) |
| `event.talk_started` | `npc: npc_ref` | the talk window opened with that NPC |
| `event.talk_ended` | `npc: npc_ref` | it closed |
| `event.signal` | **`name: signal_name`** | `signal.send` emitted that signal — matched bare or as `<module>.<name>` |
| `event.trade_done` | `npc: npc_ref` | the player traded |

### 5.6 Rules for extending the catalog

- A kind that has shipped never changes meaning. New kinds and new **optional**
  parameters may be added freely with `since = <version>`.
- Removing a kind is forbidden; the implementation stays and the editor marks it
  deprecated.
- Renaming is forbidden; add `alias = old.kind` instead and the runtime resolves
  the old name.
- A parameter cannot become required after the fact.

---

## 6. Dialogs

### 6.1 How the graph maps onto the engine

A `dialog.topic` node is **one engine dialog** with the id
`nq.<module>.<quest>.<node>`. Its root phrase (`"0"`) is the topic caption. Every
`dialog.*_phrase` node reachable from it through `next` edges is a phrase of that
dialog, keyed by the node id, and the `next` edges between phrases are the
dialog's edges.

The graph is built once per dialog id per process, through the engine's
`init_func` path for virtual dialogs (§8). `nq reload` and a re-init drop the
cached graph so the next conversation rebuilds it from the current assets.

**Alternation.** The engine flips the speaker on every edge: even depth from the
root is the initiator, odd depth is the partner. So with `initiator = actor` the
first phrase level must be `dialog.npc_phrase`, the second `dialog.actor_phrase`,
and so on. The validator enforces this along **every** path (`E010`). Two lines
in a row from the same side are written as two nodes with a filler line of the
other side between them.

**Deterministic NPC choice.** Phrase goodwill is `-10000 - <index among the
phrase children>`, so the engine's "highest goodwill first" ordering degrades to
"first declared wins". For the NPC that means the first declared line whose
condition holds is the one spoken; for the player it is the order the replies
appear in.

**The automatic fallback leaf.** If a non-leaf phrase has children that can all
be filtered away, the two sides of the engine fail differently: the player's turn
asserts (`No available phrase to say`), the NPC's turn used to read past the end
of its empty candidate list. The runtime therefore appends a hidden unconditional
leaf — `Пока.` when it is the player's turn, `...` when it is the NPC's — with the
lowest goodwill so it is picked last. It is added whenever **any** child carries a
condition, not only when they all do: an unconditional sibling is not proof the
group survives, because the engine refuses to re-add a phrase id it already knows
and a revisited line can therefore be missing from the group. The editor warns
about the risky shape with `W011`. A phrase counts as unconditional only when it
has no `cond` **and** is not `once`; a `once` phrase carries a precondition so it
disappears after it has been said.

**Two engine fixes this feature required** (both in `AI_PhraseDialogManager::AnswerPhrase`,
both crash fixes that leave stock dialogs behaving exactly as before):

* the NPC's reply selection walked its candidate list with a frozen index — it
  read `PhraseList()[phrase_num]` inside both loops instead of `[i]`, so the
  goodwill under test never changed. Every stock dialog gives all of its phrases
  the same goodwill (`-10000`), which hides the bug; a dialog with distinct
  goodwill levels — exactly what NQ builds to make the choice deterministic —
  ended up with an empty candidate list and the pick read past its end;
* an empty candidate list is now reported and finishes the dialog instead of
  crashing, so no addon can take the game down through this path again.

**Once.** `dialog.topic` defaults to `once = true`: after a leaf phrase is said,
the topic is gone. `once = false` makes it repeatable — and repeatable topics run
`on_exit` and the `done` pin on every pass while keeping their token.

**`initiator = npc`.** Such a topic is not offered in the topic list; it is
installed as the NPC's start dialog (`set_start_dialog`) while the token is
there, the NPC is alive and the node's `cond` holds. Because `xr_meet` rewrites
the start dialog from its own condlist, the runtime re-applies the value on every
poll and restores the default when the topic ends.

**World exits.** An edge from a phrase to a non-phrase node means "when this
phrase has been said, activate that node" — it goes through the deferred queue
like any transition. A phrase with no phrase children is a leaf: the conversation
ends there and the topic counts as passed, which runs the topic's `on_exit`,
drops the token (when `once`) and fires the `done` pin. Closing the talk window
mid-branch finishes nothing: the topic is still there and starts from the top.

**Availability.** A topic is offered when the quest is active, the token is on
the topic node, the node's `npc` reference matches this partner and the node's
`cond` holds. That query *is* the availability filter; there is no dialog-level
precondition. One NPC can carry any number of topics from any number of quests
and modules — the list is the union over all active tokens, ordered by quest uid
and node order.

**When the NPC is dead, missing or offline** the topic is simply not offered and
the token keeps waiting. Model "what if he dies" as a parallel branch — a
`wait.any` with an `npc_dead` case, or a `trigger.when`.

**Action ordering inside a phrase.** `on_enter` runs first, then the `once` mark,
then `on_exit`, all synchronously inside the engine's phrase action. Because the
engine filters the continuations *after* the action, a `var.set` in the
`on_exit` of a player's line is already visible to the `cond` of the NPC's next
line.

### 6.2 Texts and placeholders

Phrase texts are handed to the engine as raw cp1251 (the engine's `translate()`
returns an unknown key unchanged, so no string table is generated — see §10).

A text containing `{` is registered with the engine's script-text hook and
formatted on every show, so `{var:name}` and `{actor}` reflect the current state.
**The topic caption is the exception**: the engine reads it with no speakers
attached, so it must be static — placeholders in a topic's `text` are substituted
once, when the dialog graph is built, and only change after an invalidate.

### 6.3 The per-dialog init function

`CPhraseDialog` gives Lua no way to ask which dialog is being initialised, so the
runtime binds the id through the *function name*. For each topic it creates a
function named `init_<fnv1a8 of the dialog id>` inside the `xms_nq_dialog`
namespace (with a `_2`, `_3`… suffix if two ids ever hashed the same) and
registers `xms.dialog_register(<dialog id>, "xms_nq_dialog.init_<hash>")`. The
name is content-addressed, so it is stable across Lua states and across saves.

The fixed entry points the engine calls are `xms_nq_dialog.pre`, `.act`, `.text`
and `xms.dialogs_for`; all of them demultiplex on the dialog id and are
`pcall`-protected inside. A precondition that fails reads as `false`.

---

## 7. References and places

- **`story`** — a CoC-style string story id (`[story_object] story_id = …` in the
  spawn's `custom_data`, registered in `story_objects`). This is how you point at
  a vanilla NPC or at an NPC your own module placed.
- **`ref`** — a name the quest gave to something it created itself
  (`spawn.squad`, `spawn.object`, `item.spawn`, `objective.kill { spawn = … }`,
  the anchor of `objective.reach`). Refs are stored as
  `refs[name] = { id, kind, section, smart, hold, scripted }` and survive saves.
  The runtime additionally registers the story id
  `nq.<module>.<quest>.<ref>` for every ref (re-registered after every load), so
  custom Lua and vanilla condlists can address the object by name.
- **`profile`** — any NPC with that `character_profile`.
- **`community`** (optionally plus `level`) — any NPC of that faction.

Resolution order for an `npc_ref` is `ref` → `story` → the first online NPC
matching `profile` / `community`. For dialogs the question is asked from the
other side — "does this partner match the ref?" — which is what lets a
`community` reference put a topic on any stalker.

A `place` is one of:

| Form | Meaning |
|---|---|
| `{ level, pos = {x,y,z}, radius }` | a sphere; `radius` defaults to 5; on another level it never contains the actor |
| `{ restrictor = "zone_name" }` | the restrictor zone's own shape |
| `{ smart = "…" }` | the smart terrain's position with `radius` (or its arrive distance, or 25) |

Positions are turned into vertices with `level.vertex_id` on the current level
and `xms.graph_vertex(level, x, y, z)` for anywhere else.

---

## 8. PDA tasks

`task_manager` reads its LTX once when the script loads and offers no
registration API, so NQ drives `CGameTask` directly. A quest declares its tasks
in `tasks`, and the extra actions operate on them.

- `task.give` creates or refreshes the task with the id
  `nq.<module>.<quest>.<task>`, sets type (`additional` / `storyline`), title,
  description, icon (default `ui_pda2_mtask_overlay`), and — when the
  declaration has a `target` — the map location and a blinking spot
  (`secondary_task_location` / `ui_secondary_task_blink`, or the storyline pair).
  A task that is already live only gets its fields refreshed; the engine refuses
  a second `give_task`.
- `task.complete` / `task.fail` set the engine task state and send the PDA news.
- `task.remove` is best effort: the engine cannot delete a task, so the map
  locations are dropped, the task is closed silently and the runtime forgets it
  (`task_status` reads `none` again).
- `task.set_target` re-points the marker; `task.set_text` rewrites title and
  description. Both require the task to be live.
- A `target` of `{ pos = … }` gets an anchor `space_restrictor`, created once and
  kept as a ref, so the marker has an object to hang on.
- The tasks themselves live in the actor's engine registry, which is vanilla
  behaviour. NQ keeps its own view (`active` / `completed` / `failed`) in its blob
  and reconciles it on every init: a task the runtime believes is active but the
  PDA has lost is recreated from the declaration, with a log line.
- `task_manager.task_callback` ignores ids it does not know, so there is no
  conflict with the base game's tasks.

---

## 9. Custom Lua and the `nq` API

Custom Lua exists in exactly two places: an extra action
`{ kind = "lua", params = { code = [[ … ]] } }` and a condition of the same kind,
whose code must `return` a boolean. Main actions are never custom.

The code is compiled **once**, when the quest loads, with the chunk name
`<uid>/<node>/<slot>`. A syntax error is `E050` and the quest does not load at
all. Its environment is `setmetatable({}, { __index = _G })` — full read access
to the game's API, with global writes landing in the sandbox instead of `_G`.
A runtime error behaves like any other action error (§3.5); a condition that
throws reads as `false`.

The environment gains one extra global, `nq`, rebuilt for every call:

| Member | Meaning |
|---|---|
| `nq.quest` | quest uid |
| `nq.node` | the current node id |
| `nq.actor` | `db.actor` |
| `nq.npc` | the dialog partner, or `nil` |
| `nq.vars` | proxy over the quest variables — reading and assigning both work |
| `nq.var(name)` | read one variable |
| `nq.set_var(name, value)` | write one variable (bool/number/string) |
| `nq.ref(name)` | server object behind a ref, or `nil` |
| `nq.ref_go(name)` | game object behind a ref, or `nil` |
| `nq.signal(name)` | emit a signal for `event.signal` |
| `nq.news(text[, sender[, duration]])` | PDA tip; `text` is UTF-8 and converted for you |
| `nq.tr(text)` | UTF-8 → cp1251 |
| `nq.log(fmt, …)` | log line prefixed with the quest and node |
| `nq.give_item(section[, n])` | like `item.give` |
| `nq.take_item(section[, n])` | like `item.take` |
| `nq.has_item(section[, n])` | inventory count ≥ n |
| `nq.money([delta])` | adds `delta` when given, returns the balance |
| `nq.time()` | game time in seconds |
| `nq.done(node_id)` | has that `once` node completed? |

Write UTF-8 in your source and pass it through `nq.tr` / `nq.news`; do not paste
cp1251 bytes into a `.nqasset`.

---

## 10. Texts and localisation

- Assets are UTF-8. A `text` parameter is either a plain string (the default
  language, `rus`) or a table keyed by language: `{ rus = "…", eng = "…" }`.
- At load the runtime picks the language from
  `localization.ltx [string_table] language`, falls back to `rus`, then to any
  string present, and converts **UTF-8 → cp1251** in pure Lua: the full Cyrillic
  block plus `Ё ё — – « » “ ” … № €` and the rest of the code page. Anything with
  no cp1251 equivalent becomes `?` and raises `W040` with the count.
- Converted strings go into the engine raw — `AddPhrase`, `set_title`,
  `send_tip`. `translate()` returns an unknown key unchanged, so no string table
  is generated for a module and nothing has to be registered.
- Placeholders are `{var:name}` (a quest variable, empty when unset) and
  `{actor}` (the player's character name). They are substituted at display time
  for phrases, task text and news; topic captions substitute them once at graph
  build time (§6.2).
- **Byte values are built with `string.char`, never written as decimal escapes.**
  The engine's script loader hands the Lua state `"\208"` as the three characters
  `2`, `0`, `8` (symbolic escapes — `\n`, `\t`, `\\` — behave normally), so a byte
  class written as `"[\128-\255]"` would compile to a class of digits and every
  Cyrillic text would slip through unconverted. The rule holds for anything in the
  runtime that talks about raw bytes; `tools\qa\nq` prints what the engine actually
  does with escapes in its `*.encoding` step on every run.

---

## 11. Persistence

### 11.1 The channel

The runtime keeps everything in one blob:

```lua
xms.save_data("xms.nq", <encoded state>)
xms.load_data("xms.nq")
```

`xms.nq` is a **core pseudo-namespace** (`0xFF01`) on the XMS per-module data
channel, so NQ never competes for a module author's own blob. It lands in the
save's `.scov` sidecar as chunk `0x584DFF01`; the range `0xFF00`–`0xFFFF` is
reserved for engine-owned blobs and is never handed to a module.
`.scop` and `.scoc` are not touched in any way — see `SAVE_COMPATIBILITY.md`.

The payload is tagged: `"M"` followed by a `marshal` blob when the engine has the
`marshal` library, `"L"` followed by Lua source when it does not. An `"L"` blob
decodes on any build; an `"M"` blob needs `marshal` present, and a build without
it reports the blob as unreadable and starts from empty state. An untagged blob
from an older revision is tried both ways.

### 11.2 The restage rule

A blob that was loaded does **not** ride along into the next save by itself —
`GetLoadedBlob` does not stage it. The runtime therefore saves the state
immediately at the end of `init`, and again at the end of any tick where
something changed. Real-time accumulators alone force a restage no more often
than every 5 seconds.

### 11.3 Schema

```lua
state = {
  v = 1,
  debug = false,            -- `nq debug 1`
  pending_level = "l01_escape",  -- set by on_level_changing, drives level_entered
  first_done = true,
  quests = {
    ["madmer.cordon_tales.wolf_debt"] = {
      uid    = "madmer.cordon_tales.wolf_debt",
      status = "active",    -- inactive | active | completed | failed
      hash   = 305419896,   -- FNV-1a of the asset text of the last run
      t0     = <game seconds at activation>,
      r      = <real seconds the quest has been active>,
      tokens = { kill = { t0 = <game secs>, r = <real secs>, k = "objective.kill",
                          b = true,          -- begin() succeeded and returned "wait"
                          w = { … } } },     -- the kind's own sub-state
      vars   = { agreed = true },
      refs   = { boars = { id = 5123, kind = "squad", section = "simulation_boar",
                           smart = "esc_smart_terrain_3_16", hold = true } },
      done   = { meet = true },              -- `once` nodes that have completed
      fired  = { t_item = 2 },               -- trigger fire counters
      joins  = { j1 = { ["a/next"] = true } },
      trig   = { t_item = { last = false, cd_g = … } },
      tasks  = { kill_boars = "active" },
      errors = { kill = "…" },
      timers = { wait_1 = { real_left = 12.5 } },
    },
  },
}
```

A state written by a newer runtime is logged and then read anyway with the
current version stamped in — the schema only ever grows.

### 11.4 Reconciliation

- **The graph changed between sessions** (the module was updated): the stored
  `hash` no longer matches the asset, so the record is reconciled. A token on a
  node that no longer exists, changed its kind, or became a trigger is dropped
  with a log line; `joins`, `timers`, `errors` and `trig` entries for vanished
  nodes are dropped; new `vars` take their defaults; `refs`, `done` and `tasks`
  survive; triggers are re-armed against the new graph. Everything else keeps
  running.
- **The module was removed or stopped applying**: the record simply stays in the
  blob, unread and tiny. `nq list` shows it as "asset not loaded, record kept". If
  the module comes back, the quest carries on where it was. When the *last*
  module goes, XMS itself goes inactive and stages nothing, so the chunk is
  carried into the new save untouched rather than rewritten — the record survives
  either way, but the runtime cannot report it during such a session.
- **A vanilla save, or original Dead Air**: no `.scov`, no chunk, nothing to do.
- **A save that predates NQ**: no blob, so every `auto` quest of every installed
  module activates on the first update, exactly as if the module had just been
  installed.

---

## 12. Engine additions

Everything NQ needed from the engine is additive. No existing contract — saves,
XMS, the Lua API, vanilla dialog behaviour — changed, and with zero modules
mounted none of it runs.

### 12.1 Lua natives and the `xms` bootstrap

`xms.nq_api = 1` is the feature test; check it before using anything below.

| API | Signature | Notes |
|---|---|---|
| `xms.list_files` | `(id, subdir, mask, recursive) → { relpath, … }` | Lists files of an **enabled** module. `subdir = ""` is the module root. Paths are relative to the module root. A bad `subdir` yields `nil`, a missing one an empty list. |
| `xms.read_file` | `(id, relpath) → string \| nil` | Reads a file of an enabled module. Refuses absolute paths, `..`, drive/UNC and control/glob characters, and files above 8 MiB, each with a log line. |
| `xms.module_applies` | `(id) → bool` | The engine's own `mode=` gate, so Lua and C++ agree on which modules apply. |
| `xms.dialog_register` | `(dialog_id, init_func) → bool` | Registers a virtual dialog: an id the engine builds through the named Lua function instead of an XML file. |
| `xms.dialog_unregister` | `(dialog_id)` | Removes it. |
| `xms.dialog_invalidate` | `(dialog_id) → bool` | Drops the cached phrase graph so the next conversation rebuilds it. **Returns false while a talk window is open** (the open dialog holds pointers into that graph); the runtime retries on `actor_on_leave_dialog`. |
| `xms.save_data` / `xms.load_data` | `("xms.nq", blob)` | The core pseudo-namespace, resolved before any module id. |

### 12.2 The `dialogs_for` hook

At the end of `CActor::UpdateAvailableDialogs`, after the partner's own actor
dialogs have been added, the engine calls Lua `xms.dialogs_for(partner, actor)`
and adds every dialog id the returned array holds. The lookup is raw (no
autoload, no logging), it is skipped entirely when XMS is inactive or the
function does not exist, and a Lua failure is reported exactly once per session.

On the Lua side that same call is the source of the `talk_started` event.

### 12.3 The `nq` console command

`nq <args>` forwards the argument string to `xms_nq_console.exec("<args>")`.
Without the runtime loaded it answers `! NQ runtime is not loaded`.

| Subcommand | What it does |
|---|---|
| `nq list` | Every quest: uid, status, title, token count, warning count, runtime errors. Then invalid assets, then records whose module is gone. |
| `nq state <uid>` | One quest: hash and file, tokens (with their sub-state), vars, refs, done, fired, joins, timers, tasks, errors, and the full text outline of the graph. |
| `nq activate <uid>` | Activates it, dropping a finished record first. |
| `nq reset <uid>` | Drops the record entirely (cancelling tokens and closing its PDA tasks); an `auto` quest activates again at once. |
| `nq jump <uid> <node>` | Cancels every token and puts one on that node — activating the quest if needed. |
| `nq fire <uid> <node> [pin]` | Completes a waiting node through `pin`, or through the kind's first declared pin. |
| `nq reload` | Re-reads the catalog and every asset, invalidates the dialog graphs and reconciles the state exactly like a graph change between sessions. |
| `nq debug 0\|1` | Mirrors every transition into PDA news as well as the log. |
| `nq validate` | Re-scans and prints every problem of every asset with its code. Works before the runtime is initialised. |
| `nq dump` | Writes a full JSON report to `$app_data_root$\nq_report.json`. |
| `nq help` | The subcommand list (also the answer to a bare `nq`). |

`<uid>` accepts a bare quest id when it is unambiguous across modules; otherwise
say `<module>.<quest>`.

### 12.4 Log prefixes

Every runtime line is prefixed so the log can be filtered:

```
* [nq] …    information
~ [nq] …    warning
! [nq] …    error
[nq] …      console command output
```

---

## 13. Validation codes

The same rule list runs in the editor (as a build gate) and in the game (at
load). `E` blocks — the build refuses, and the game does not load the quest.
`W` is advisory.

| Code | Rule |
|---|---|
| `E001` | `nq` is missing, or the version is not supported by this runtime |
| `E002` | quest `id` is not `[a-z0-9_]`, or is already used by another quest of the module/project |
| `E003` | the file is not one declarative table (code outside strings, syntax error, nothing returned) |
| `E004` | node `id` is not `[a-z0-9_]`, is missing, or is duplicated |
| `E005` | unknown `kind`, or a kind used in a position it does not declare (`main`/`trigger`/`extra`/`cond`) |
| `E006` | a required parameter is missing, has the wrong type, is out of range or outside its enum — also bad `activation`, `vars`, `tasks`, `once` |
| `E007` | a pin is not declared by the kind; an edge points at a node that does not exist; an edge points into a trigger |
| `E008` | the quest has no trigger, or no `nodes` list at all |
| `E009` | `flow.join` with no incoming edges |
| `E010` | speaker alternation broken (a phrase at the wrong depth parity on some path), or a phrase unreachable from any topic |
| `E011` | a phrase is entered from a non-dialog node |
| `E020` | an event condition outside `trigger.when` / `wait.when` / `wait.any` |
| `E021` | `cases` empty, a case name missing or not `[a-z0-9_]`, duplicate case names, `weight ≤ 0` |
| `E030` | a `var` / `task` / `node` / `quest` reference points at something that is not declared |
| `E031` | `actor.teleport` to a place on another level *(editor only — in game this is a runtime error prefixed `E031:`)* |
| `E050` | syntax error in custom Lua |
| `W011` | a non-leaf phrase has no unconditional continuation — an automatic reply will be added |
| `W012` | a `dialog.topic` with no phrase continuation |
| `W013` | a topic with `once = false` and no conditions: it is offered forever |
| `W020` | a node unreachable from any trigger |
| `W021` | a waiting node with no outgoing edges — the quest will stop there |
| `W030` | a `ref` is used but no node creates it |
| `W031` | `spawn.squad` with `hold = false`: the squad may wander off into the simulation |
| `W040` | a character with no cp1251 equivalent in a text (replaced by `?`) |
| `W050` | custom Lua could not be syntax-checked *(editor only)* |
| `W060` | a value was not found in the linked game's index — story id, section, level, smart, profile, info portion *(editor only)* |
| `W061` | the file name does not match the quest `id` *(editor only)* |
| `W070` | the quest has no `flow.end` and will complete implicitly |

A problem is printed as `<code> <node>[<slot>]: <message>`, where the slot is
`param:<name>`, `cond:<i>`, `enter:<i>`, `exit:<i>`, `out:<pin>` or
`cond:<i>/case:<j>`.

Beyond these, the runtime reports at load — without invalidating the quest —
every kind that the catalog declares but nobody implemented.

---

## 14. Compatibility guarantees

**Classic mods are untouched.** NQ adds engine entry points, it does not change
existing behaviour. `character_dialogs.xml`, `dialogs.xml`, `game_tasks.xml`,
`task_manager.ltx`, info portions, JSGME layers, loose `gamedata`, `xtra_*.xdb0`
and content addons all work exactly as before. With no module mounted the dialog
hook returns immediately, the virtual dialog registry is empty and the NQ
callbacks are never registered.

**The save contract is untouched.** `.scop` stays byte-compatible with original
Dead Air 0.98b and `.scoc` is not written to. NQ's entire footprint is one
optional chunk in the `.scov` sidecar (`0x584DFF01`). An original-Dead-Air save
loads with no NQ state; a Refined save loads in original Dead Air with the
sidecar ignored. Removing a module never bricks a save.

**Quest ids never collide.** Everything is namespaced by module: dialog ids, task
ids, ref story ids and the quest uid itself.

**What freezes when the editor is released.** Until XFined Editor's first public
release the whole contract may be rebuilt without migration paths. From that
release on, the following are frozen and only ever extended additively:

1. The `.nqasset` format (§2) behind the `nq` version field. The game supports
   every released version; migrations live on the game side.
2. Catalog semantics (§4, §5): a released kind never changes meaning, kinds and
   optional parameters are only added, nothing is removed or renamed.
3. The execution model (§3): lifecycle, ordering, `once`, `flow.end` behaviour,
   and the rule that a pin activates all of its targets.
4. The persisted schema (§11.3), migrated through its `v` field.

The game's own obligation is stronger and independent of all of this: a module
built by any released version of the editor keeps working, and old-style modding
keeps working forever.
