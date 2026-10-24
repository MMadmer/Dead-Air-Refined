# Debug menu (1.6.1)

A window over the running game (it does not pause it) in the look of the main menu's own windows - the palette of
the Mods window (`sys_*` of `ui_theme.xml`): black panel, thin light lines, graffiti and letterica fonts. Off by
default: Options -> Game -> Other -> "Debug mode" (`dar_debug_menu`, saved). The key is the action `debug_menu`
(Options -> Controls, common group); turning the mode on binds it to F7 when the action has no key and F7 is free.
Esc or the key closes the window.

The same switch gates the other developer keys (1.6.2, `g_dar_debug_menu` of `XR_IOConsole.h`): the console's key (in
the game, the main menu and before either), the editor's (F10), the keys bound to console commands (`bind_console`)
and the `alife_command` action. Commands from user.ltx and scripts run regardless; turning the mode off closes an open
console. The engine's raw developer keys (F7 reload, NumPad 5 demo record) exist in debug builds only, and Dead Air's
scripted `debug_*` keys (`axr_keybind`) ship commented out. QA: `tools/qa/water/dev_mode.lua` (the runner presses the
console's key on the hidden desktop and captures the window).

Sources: `src/xrGame/ui/UIDebugMenuWnd.{h,cpp}` (the window), `gamedata/scripts/dead_air_x64_debug_menu.script`
(what the tabs list and do), `configs/ui/ui_debug_menu.xml` and `_16` (written by `tools/ui/debug_menu_layout.py`),
`configs/text/<lang>/dead_air_x64_debug_menu.xml`. The window belongs to the game UI (`CUIGameSP::DebugMenu`), made
on its first opening and dropped with the level or a UI rebuild.

## The window

Every tab has the same three panes:

- **Categories** on the left, "All" first where the tab has it.
- **The list** with its **search**: rows show the game name and, in a smaller font under it, the internal name
  (section, info portion, console command); a state on the right in the accent colour and a small note (price,
  distance, members). The list is virtual: the rows on screen are filled from the entries under the scroll
  position, so thousands of items cost the same as ten. Wheel, a drag of the bar, Up/Down/PgUp/PgDn move the pick;
  Enter or a double click presses the main button; Ctrl+Tab and Ctrl+Shift+Tab walk the tabs.
- **The card** of the picked entry: an inventory icon for items, the name, the internal name, a scrolling
  description, a two-way choice (into the inventory / in front), a count with - and + (a tenth at a time over ten)
  and up to three buttons; a line over them says what the last button did.

The search matches every word of the query against the game name (the start of a word, inside it, one typo in a
word of five letters or more - the Mod Browser's rule, ModSearch) and against the internal name (its start, the
start of a part after an underscore, inside it, its letters in order: "wpnak" finds "wpn_ak74"), in the layout it
was typed in, in the other one (QWERTY <-> JCUKEN) and transliterated from Cyrillic ("medkit" typed in Cyrillic).
An exact name or section ranks first; a tie goes to the shorter section (the item before its scene's copy).

While the window is open it takes every press, hold, wheel and look (`CUIDialogWnd::TakesAllInput`, checked first
in `Level_input.cpp`): neither the scripts' key callbacks (`on_key_press`, where the game's and the mods' hotkeys
live) nor the game nor the actor see them, so a name typed into the search draws, throws and reloads nothing.
Releases still go on (a key held as it opened lets go); the screenshot key works.

## Tabs (the script's)

| Tab | Categories | Entries | Buttons |
| --- | --- | --- | --- |
| Items | weapons, ammo, outfits, helmets, artefacts, medicine, food, devices, quest, other (by `class`) | every section with `inv_name`, `visual` and inventory cells | Spawn - into the inventory or in front, N items or N boxes of ammo |
| Characters | factions (`st_faction_*`), mutants | squad sections (`faction` + `npc_random`/`npc`); simulation squads first, named by faction and rank | Spawn ahead / beside (`create_npc` + `setup_squad_and_group`, they come online at once) |
| Teleport | levels (`level_maps_single`, the current first) | smart terrains (`smart_names`), level changers | Teleport: `set_actor_position` on this level, `ChangeLevel` to another; the window closes |
| Player | modes, condition, money, character | god mode, unlimited ammo, ignored by AI, no clipping, full recovery, money, weight limit | Turn on/off, Apply, Give/Take, Set |
| World | time, weather, events, season, time speed | the 24 hours, every weather file, climate weather, emission and psy storm start/stop, the months and the PC calendar, x1..x100 | Apply (a weather pins the sky until "Climate weather") |
| Story | the `[info_portions]` files, undeclared | every declared info portion and those the actor holds | Give / Remove |
| Nearby | stalkers, mutants, items, other | online objects within 80 m, nearest first | Teleport to it, Kill, Remove |
| Loadouts (1.6.2) | standard, own, actions | the sets of `configs/dead_air_x64_loadouts.ltx`, then the player's own; a new set from the worn gear, the deleted standard ones back | Give / Replace (the choice), Equip, Edit contents (+ Add / - Remove / Set amount), Delete |

## Loadouts

A set is a name and a list of sections (`section:count`, ammo in boxes) with `ammo_boxes`: boxes of the first ammo of each
weapon the list gives no ammo for. The shipped ones are `configs/dead_air_x64_loadouts.ltx` (`[loadouts] order`); the
player's own and their edits of the shipped ones live in `<appdata>/dead_air_x64_loadouts.ltx` in the same form (a section
of a shipped id replaces it, `[hidden]` lists the deleted shipped ones). The script ini cannot remove a section, so a
deleted own set keeps `deleted = 1`; and its parse cache forgets a file only when that file's own object saves it, so
every write goes through the one object that read it.

- **Give** spawns the set into the inventory and wears what it can: the first item of each of the knife, rifle (3, then
  2), pistol, outfit, detector, helmet, binoculars and backpack slots goes in through `move_to_slot` (past the wear
  scenes: the set dresses at once), what was worn going to the backpack. A spawn lands a few frames later; the slots
  are taken on `actor_on_update` within 5 s. An outfit with its own helmet takes none; a SEVA takes a backpack only as
  its `*_backpack` variant.
- **Replace** first releases everything but the base equipment (`wpn_upd` - the power bank, the PDA, the torch the
  lamps shine through, the bolt) and quest items.
- **Edit contents** opens the set in the tab: its items first (each with **Set amount** and **- Remove**), the ammo
  boxes per weapon, then the whole item catalogue by category with **+ Add** (the count field picks how many). Every
  change is saved at once; a shipped set is copied to the appdata file on its first change. **New empty loadout**
  creates `own_<n>` and opens it the same way; **New loadout from current gear** records what is worn and carried
  (ammo in whole boxes, knives' "ammo" left out) named after the outfit and the main weapon. Names are edited in the
  appdata file. A card's count field may ask to start from its own default on every pick (`count.reset`).

## Script contract (`dead_air_x64_debug_menu.script`)

```lua
tabs()                               -- { { id = "items", caption = "st_dbg_tab_items" }, ... }
build(tab)                           -- { all = true, categories = { caption, ... },
                                     --   entries = { { key, name, sec, badge, note, cat }, ... } }
describe(tab, key)                   -- { title, sub, body, icon = <item section>, choice = { a, b }, choice_default,
                                     --   count = { min, max, default }, primary, second, third }
act(tab, key, button, count, choice) -- { msg, rebuild = true, close = true } or the message alone
add_tab(id, caption, provider)       -- a mod's own tab: provider.build(), describe(key), act(key, button, count, choice)
```

Captions are string ids or text in the UI code page. Every tab is read again when the window opens (what is near and
what the player holds change while it is closed) and after a button asks for it.

## QA

- `dar_debug_menu_open` toggles the window whatever the option says; `dar_debug_menu_do <command>` drives the open
  window: `tab <id>`, `category <n>`, `search <text>`, `pick <n>`, `pick_key <key>`, `count <n>`, `choice <1|2>`,
  `press <primary|second|third>`, `state` (logged as `* [debug menu] ...`).
- `tools/qa/water/debug_menu.lua` (Run-WaterQa): every tab driven and checked in the game's state - spawn into the
  inventory and in front, a squad, a mode, money, the time, the story, nearby, a teleport.
- `tools/qa/water/debug_menu_typing.lua`: slot keys, the inventory and letters pressed into the open window
  (`Send-HiddenDesktopKey.ps1`) change no slot, open nothing and reach no `on_key_press`; closed, they do again.
- `tools/qa/water/loadouts.lua`: a set given (worn and carried), one replacing the inventory (the base kept), one
  created from the worn gear and deleted, a standard one deleted and restored; `qa_lo_hold` holds the window open for
  `Capture-HiddenDesktopWindow.ps1`.
