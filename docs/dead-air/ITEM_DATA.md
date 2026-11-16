# Item data: the catalog, craft recipes, start kits and upgrade costs

What a module writes as data so that its items reach the game's own windows, and the catalog of
every config key the game reads of an item. Nothing here replaces a stock script: the craft window,
the new-game screen and the upgrade functions are the ones the game ships (Dead Air's or Revolution
II's, whichever is installed), and Refined's `scripts\xms_items.script` adds the module's data to
them. A game whose system config holds none of this data runs those windows exactly as before.

## 1. The item catalog

`configs\item_catalog.ltx` lists every config key the engine and the game's scripts read of an
item's section, of its HUD section, of its upgrade sections and of the sections those name. The
game itself reads none of it: it is for tools and for people writing an item.

### Format

Plain LTX, ASCII, comma lists, one section per thing.

```ini
[catalog]
version = 1

[scopes]
order = item, hud, upgrade, immunities, absorbation, bones, sound_layers, model, global

[scope.hud]
title = HUD Section
help = The section the item's hud key names: ...

[families]
order = item, eatable, medicine, weapon, shotgun, launcher, knife, grenade, ammo, scope, ...

[family.weapon]
title = Firearm
classes = WP_AK74, WP_GROZA, ...        ; the `class =` values of the family
like = wpn_ak74, wpn_pm, wpn_svd        ; sections of the game a new item of the family starts from
help = ...                              ; when the family needs a word

[groups]
order = general, icon, inventory, economy, use, protection, weapon, ...

[group.economy]
title = Economy

[item.cost]                             ; [<scope>.<name>]
families = *                            ; or a comma list of families
type = float
group = economy
title = Cost
unit = RU
required = true
common = true
help = The base price, which traders scale by their own factors.
```

A key's section is `[<scope>.<name>]`. The scopes say which section the key lives in:

| Scope | The section |
|---|---|
| `item` | the item's own section |
| `hud` | the section the item's `hud` key names |
| `upgrade` | an upgrade's settings section (the `section` key of an upgrade) |
| `immunities` | the section `immunities_sect` names |
| `absorbation` | the section an artefact's `hit_absorbation_sect` names |
| `bones` | the section `bones_koeff_protection` names |
| `sound_layers` | a section a sound key names instead of a file |
| `model` | the `torch_definition` section of a torch model's user data |
| `global` | a section of the system config every item of a kind reads (`section` says which) |

The fields of a key, all but `families`, `type` and `group` optional:

| Field | Means |
|---|---|
| `families` | the families whose classes read the key; `*` is every family |
| `type` | `bool` `int` `float` `string` `vec2` `vec3` `vec4` `color` `list` `section` `visual` `texture` `sound` `particles` `motion` `string_id` `functor` `token` |
| `group` | where the key is shown, one of `[groups] order` |
| `title` | the key's name for people; one made from the key when nobody wrote one |
| `unit` | `RU` `kg` `m` `m/s` `s` `ms` `deg` `rpm` `%` `x` (a factor) |
| `required` | the game stops without the key, after inheritance: `true` for every class that reads it, or the comma list of the classes that stop without it |
| `default` | the code's default when the key is left out and the code states a literal one |
| `values` | the words a `token` takes |
| `common` | the key belongs on the surface of a form, the rest under More |
| `presence` | the game asks only whether the line is there; its value is not read |
| `section` | a `global` key's section |
| `pattern` | the key is a series, not one line: `use<N>_functor` is `use1_functor`, `use2_functor` ...; `anm_<name>` any `anm_` key |
| `help` | one sentence of what the key does |

Families follow the engine class (`class =`) of the item. Dead Air's medkits and drugs are of the
food class, so they are `eatable`; its backpacks, plates and pouches are of the artefact class;
its artefact containers have no class of their own. There is no family of containers for that
reason.

### Regenerating

The catalog is generated, and regenerating reproduces it byte for byte:

```
python tools\items\make_item_catalog.py                     write configs\item_catalog.ltx
python tools\items\make_item_catalog.py --check             check the inputs, and that the file is what they make
python tools\items\make_item_catalog.py --check --game <configs folder>
                                                            also: every family's `like` sections exist there
                                                            and are of the family's classes
python tools\items\make_item_catalog.py --calibrate --game <configs folder>
                                                            narrow `required` in item_keys.tsv by the
                                                            game's real items, then write the catalog again
```

`required` starts from the code: a key read without a default. The code reads many keys only under
a condition no extraction sees - a launcher's keys only when the weapon has a launcher - so
`--calibrate` settles it by the data: a key stays required for a class when every real item of that
class in the game's configs has it after inheritance (for a HUD key: every HUD section those items
name), and a class of which one real item lives without it is taken out. A class the game has no
item of keeps what the code says. So the flag never says more than the game's own items prove: a
key that is required only under a condition (a launcher's class of grenades) is not marked.

The inputs are in `tools\items\`: `item_keys.tsv` (every key the code reads, extracted from the
engine and the scripts: scope, key, type, required, default, families, the source line),
`item_classes.tsv` (the item classes and their family) and `item_catalog_notes.ltx` (what no
extraction knows: the families, the groups, titles, help, units, which keys are common, and the
corrections of the extraction). A key nobody wrote notes for is still in the catalog, with its
type, families and required flag from the code. The check also refuses a type, a unit, a group or
a family that is not declared, a class in two families, a `required` naming no class, and any
value the game's LTX reader would cut (a `;`) or misread.

## 2. Craft recipes from data

A module adds recipes to the craft window of a tool or a workbench with lines in the system
config's `[xms_craft]`, from a `.ltxp` patch or a `configs\xms\*.ltx` overlay:

```ini
[xms_craft]
my_mod_silencer = itm_repairkit_tier_1, my_mod_silencer, 1, jar, textile_patch_m, rope, flashlight_broken
my_mod_rounds   = verstak_ammo, ammo_9x18_fmj, 40, ammo_bad, ammo_bad
```

- One key a recipe: `<key> = at, result, count, part, part, part, part`. The key is any name that
  is unique in the section; begin it with the module's id, so two modules never write the same one.
- `at` is the section the craft window is opened with: a tool (`itm_repairkit_tier_1`,
  `itm_repairkit_tier_2`, `itm_repairkit_tier_3`, `itm_drugkit`) or a workbench of Revolution II
  (`verstak_other`, `verstak_weapon`, `verstak_ammo`, ...). A recipe for a section the window is
  never opened with is simply never shown.
- `count` is how many of the result are made, 1 when left empty, 1000 at most. A result whose section name
  holds `ammo_` is made as that many rounds, in boxes, like the window's own ammo recipes.
- Up to four parts. `-`, an empty part or a part left out at the end is no part.
- A recipe whose result or part is no section of the system config, whose count is no whole
  number from 1 to 1000, or that has more than four parts is left out with one log line,
  `! xms_items: craft recipe <key> is left out - <why>`.

The recipes are listed after the window's own ones, in the order of their keys (the engine hands
a section's lines back sorted). They behave like the window's own: the row shows the result's
name and the count, picking it shows the four parts with their icons, found ones in green and
missing ones in red, each item counted once; Craft makes the result into the player's inventory,
takes exactly the parts it counted, plays the tool's effect and closes the window. A part with an
icon sheet of its own (`icons_texture`) is drawn from it. A data recipe has no perk or
information condition: the window's own lists that open with a perk or a workbench upgrade stay
as they are, and a data recipe is always there.

The section is read once per Lua state. Without a usable recipe in it the craft window is the
stock one. Two modules' `[xms_craft]` merge line by line; within one module write the section in one
overlay file or in `.ltxp` lines, since one section in two overlay files of a module stops the game.
The same holds for `[xms_start_kits]`.

## 3. Start kits of the new-game screen

The new-game screen offers kits for a budget of 100 points: two lists, the kits on offer and the
kits taken, moved between them with a double click. A module adds kits to the offer with data:

```ini
[xms_start_kits]
my_mod_sniper =                 ; a key per kit: the kit's section

[my_mod_sniper]
title = st_my_mod_sniper        ; string id of the row's caption
cost  = 40                      ; points, 0 to 100
items = wpn_mosin, ammo_7.62x54_7h1:3, wpn_toz34|wpn_bm16@0.4-0.6, medkit:2, novice_outfit@0.8
descr = st_my_mod_sniper_descr  ; optional: a string id shown above the item list
after = my_mod_kits.on_given    ; optional: a function called after the items are given
```

- `[xms_start_kits]` names the kit sections, one key each; its value is not read. A module's
  `.ltxp` adds a line with `my_kit =`, an overlay with a bare `my_kit`. Name the kit sections with
  the module's id first.
- `items` is a comma list of rows. A row is a section, `:N` for N of it (ammo: N boxes),
  `@min-max` for a condition drawn between the two (or `@c` for exactly c), and `a|b|c` for one of
  them at random: `wpn_toz34|wpn_bm16:1@0.4-0.6`. A section the game does not have is left out of
  the kit with one log line.
- The row's caption is `title`; the description pane shows the kit's points, `descr` when given,
  and what the kit gives, each item by the name the player reads.
- `after` is called as `fn(<kit section>)` once the items are given, for what data cannot say. It
  is a global function, `<script>.<function>`: a global script of the module, or a function its
  registrar publishes as a global table.
- A kit without a title, with a cost that is no whole number from 0 to 100, or that gives nothing
  (no row left and no `after`) is left out with one log line,
  `! xms_items: start kit <section> is left out - <why>`.

The kits are offered after the screen's own ones, in the order of their keys, and they spend the
same 100 points. The screen packs the kits taken into one number with a bit per row, which only
has room for the screen's own rows; a module kit is stored by its section in
`[character_creation] new_game_xms_kits` instead. A new game gives the kits taken as the game is
loaded, beside the screen's own kits, the way the screen gives its own: the items in the player's
inventory. A loaded save never gets a kit (a key left over by a start that never happened is only
cleared). A module mode with a `[start_kit]` (MODDING.md, World rules) replaces the screen's kits
and these alike: the inventory becomes exactly that kit on the first update.

The offer list scrolls, so the screen takes any number of kits; the 100 points are the limit of
what a player can take. Without a usable kit in `[xms_start_kits]` the screen is the stock one.

## 4. Upgrade costs of a module's upgrades

The stock upgrade functions (`inventory_upgrades.script`) read an upgrade's cost, a property's
name and an upgrade's `value` from `item_upgrades.ltx`, a file they open by name, and that file
holds only the game's own upgrade files. An upgrade section a module adds to the system config is
not in it, and reading its cost there stops the game. In a game with modules those functions are
wrapped: for an upgrade or a property that `item_upgrades.ltx` does not have, the same code runs
over the system config instead, and every upgrade of the file goes to the stock code untouched.
So a module's upgrade needs nothing but its sections in the system config:

```ini
[up_my_mod_barrel_ak74]          ; the upgrade (name, description, icon, section, property, the functors)
...
cost = 1200                      ; read from here: the mechanic's price, shown and paid as for a stock upgrade
```

- The price is `cost` times the mechanic's discount, as for a stock upgrade; a module upgrade
  without `cost` is free, with one log line.
- The mechanic's allowance (`[<mechanic>_upgr] <upgrade> = ...` of `misc\stalkers_upgrade_info.ltx`)
  is read from that file as before; an upgrade it does not name is allowed, as for the game's own.
- A property section a module adds (`prop_*` with `name`) is read from the system config too, and
  a property that sums stock and module upgrades sums both.
- `stalkers_upgrade_info.ltx` and the upgrade window's layout (`ui\inventory_upgrade.xml`) are not
  part of this: a `#file` patch and an `.xmlp` reach them.
- The 4:3 layout the game took from Revolution II did not parse - one scheme was left open - so a
  4:3 screen had no upgrade panel at the mechanic, and it lacked ten schemes of the 16:9 layout
  (the binoculars, the Mosins, the MP-412 and the guns that share its scheme, the SVT-40, the heavy
  respirators among them). The game ships it corrected in its own gamedata, the ten schemes spread
  over the 4:3 columns as the rest are.

A game without modules runs the stock functions untouched and reads `item_upgrades.ltx` once.

## 5. Testing

`python tools\nq\run_lua.py tools\items\test_item_scripts.lua [--stock <scripts folder> ...]` runs
the data readers and the upgrade fallback headless. Every `--stock` folder holds the game's own
`ui_itm_craft.script`, `ui_mm_faction_select.script` and `inventory_upgrades.script` of one script
layer; with it the stock craft window, new-game screen and upgrade functions themselves are driven
through a stand-in of the engine's UI and ALife, with the wrappers over them.
