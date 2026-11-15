# The radios' music and the streamer mode

The radios of the world play the game's own music, the radio music of every module, a playlist the
player tunes them to, or a playlist a module binds to one radio. The streamer mode keeps everything
copyrighted from playing. The script side is `scripts\xms_radio.script`; the engine side (the
manifest's tracks and playlists, the copyrighted flags, the refusal before a sound starts) is
described in `MODDING.md`, "Screens and music" and "Streamer mode".

## For a module author

### A track on the radio

A track of the manifest joins the radio with `radio` in its `play`:

```ini
[track.night_road]
file        = music\night_road.ogg
play        = radio          ; with loading or menu as well, if it plays there too
weight      = 2              ; how often, against the other tracks of the radio (a game track weighs 1)
volume      = 0.8            ; scales the radio's own volume, which its logic may set
copyrighted = true           ; a commercial recording: not played in the streamer mode
streamer    = night_road_free ; optional: a track of this module (or a sound name) played instead
```

The radios of the world shuffle such tracks among the game's own radio music. A track never plays
twice in a row on one radio while there is another, so a weight counts among the others: a track of
weight 8 among four of weight 1 plays a little under half the time.

`[radio] music = only` makes this module's radio tracks the whole common music (the last module of
the load order that says so); `mix`, the default, adds them to the game's. The player can still pick
the game's music alone, or a playlist.

### A playlist

A playlist is a station of its own: a named list of the module's tracks.

```ini
[playlist.road_songs]
title  = st_my_mod_road_songs  ; a string id, or plain text
tracks = night_road, rain, wind
order  = list                  ; list: in this order, round and round; shuffle (the default): every
                               ; track once a pass, in a new order each pass
```

A track a playlist names plays in it whatever its own `play` says. Every playlist of every enabled
module is offered under Options > Sound > Radio station, by its title. When the player picked a
playlist whose module is switched off later, the radios play everything until it is back.

### Copyrighted

A sound is copyrighted when its description says `copyrighted = true` (a `.sndasset`), when its
manifest track says so, or when the game's own list names it (below). In the streamer mode the engine
refuses to start such a sound by itself, from any script and from the engine; the radio does not even
try it: a copyrighted track with a replacement (`streamer =` of the track or of the description, a
sound that is not copyrighted itself) plays the replacement in its place, so a playlist keeps its
length and its order, and one without is passed over. A radio with nothing left is silent and starts
again as soon as something may play - the mode switched off, the station changed. The same choice
holds wherever a script picks music from a list: the other themes of the radios, the 3d themes of
`script_sound.ltx` that pick at random (the guitar and harmonica themes among them), the combat music.

### A radio of your own

A radio is a physics object whose logic runs the stock `ph_sound` scheme on the radio theme
`common_radio` (or a theme that inherits it unchanged). Its custom data may bind it to a playlist:

```ini
[xms_radio]
playlist = my_mod.road_songs   ; <module id>.<playlist id>: this playlist, whatever the station
;playlist = game               ; the game's radio music alone
;playlist = off                ; silent
; no playlist line: the player's station
```

A worked example: the section in `gamedata\configs\xms\my_mod.ltx`

```ini
[my_mod_camp_radio]:physic_destroyable_object
$spawn      = "dynamic_objects\my_mod\camp_radio"
visual      = dynamics\equipments\trade\radio.ogf
fixed_bones = link
custom_data = scripts\my_mod\camp_radio.ltx
```

its logic `gamedata\configs\scripts\my_mod\camp_radio.ltx`

```ini
[logic]
active = ph_sound@play

[ph_sound@play]
snd      = common_radio
looped   = false
random   = true
min_idle = 300
max_idle = 500
volume   = {=dist_to_actor_le(10)} 1, {=dist_to_actor_le(20)} 0.5, 0
on_info  = {=surge_started} ph_sound@surge

[ph_sound@surge]
snd     = broadcast_disrupt
random  = true
on_info = {=surge_complete} ph_sound@play

[xms_radio]
playlist = my_mod.road_songs
```

and the object placed by the module's spawn layer (`spawn\*.xspawn`):

```ini
[obj:camp_radio]
op       = add
section  = my_mod_camp_radio
level    = l01_escape
position = 12.5, 3.1, -40.2
story_id = my_mod_camp_radio
```

The surge section plays the stock noise (`broadcast_disrupt` is no radio theme), so the playlist
waits out the surge. A playlist bound to a radio is filtered by the streamer mode like any other.

### Switching a radio from a script

```lua
xms_radio.set(radio, "my_mod.road_songs")  -- this playlist, at once
xms_radio.set(radio, "game", true)         -- the game's music from the next track on
xms_radio.set(radio, "off")                -- silent
xms_radio.set(radio, nil)                  -- back to its custom data, then to the player's station
local binding, from, now = xms_radio.get(radio)  -- the binding, "script" or "data", what it plays
for _, p in ipairs(xms_radio.playlists()) do     -- what exists now: key, module, id, title, order,
	printf("%s %s", p.key, p.title)              -- tracks (a count)
end
```

`radio` is the radio's game object or server object, its ALife id, or its story id or object name -
the last two are turned into the id when `set` runs. The id is what is kept: it is stable for the
objects of a level and of a module alike, so a binding finds the same radio after a load and a level
change. A switch cuts the track playing unless the third argument is `true`. A binding to a playlist
whose module is away is kept; until the module is back the radio plays the player's station. What
scripts set is kept in the save, in the module-independent core channel of the `.scov`
(`xms.save_data("xms.radio", ...)`): `v=1`, then one `<id>=<binding>` line per radio. A game that
binds nothing writes nothing there.

### Switching a radio from a quest

The quest runtime has the same two moves as actions (catalog v22): `radio.play` - which radio, which
playlist, at once or at the next track - and `radio.reset`, which gives the radio back to its own
data and the player's station.

```lua
{ kind = "radio.play", params = { radio = { story = "my_mod_camp_radio" }, playlist = "road_songs" } },
{ kind = "radio.play", params = { radio = { ref = "bar_radio" }, playlist = "game", next_track = true } },
{ kind = "radio.play", params = { radio = { story = "esc_radio_01" }, playlist = "off" } },
{ kind = "radio.reset", params = { radio = { story = "my_mod_camp_radio" } } },
```

`playlist` is `<module>.<playlist>`, the id alone for a playlist of the quest's own module, `game`
or `off`. `radio` is an object the quest made (`ref`), a story id, or - written as `story` too - the
object name of a radio the level itself has. What a quest sets is kept exactly like a script's.

## For the game's keeper

### The game's copyrighted list

`configs\dead_air_x64_sound_rights.ltx`, `[copyrighted]`: one line per sound name (its path under
`sounds`, without the extension) or mask with `*`, optionally `= <replacement>`, a sound that is not
copyrighted played in its place. The engine reads it once when the sound engine starts; a module's
`#file` patch adds to it. Today it names the radio's commercial recordings: `music\radio\*` (the
workbench radio), `radio\01\*` (the level radios: the same 45 recordings, byte for byte) and the
four tracks under `sounds\radio` named after their performers and titles.

### The options

Options > Sound, the subsection Music (`dead_air_x64_sound_options.ltx`, `[group_music]`):

- **Streamer mode** - the check `streamer_mode`, bound to the engine's `snd_streamer_mode` and saved
  with the other sound options. A game without the variable leaves the row out.
- **Radio station** - the list `radio_station`, a script row built by `xms_radio.options_init` (the
  node `list_radio_station`, placed by `place_radio_station`). It is shown only when a module brings
  radio music. Everything (the default), The game's music, then every playlist by its title; the
  choice is `radio_station` of `[mm_options]` in `axr_options.ltx`, beside the game's other script
  options: `all`, `game` or `<module>.<playlist>`. A radio playing when the choice changes moves on
  at its next track.

### Which themes are the radio

- `plugins\xr_sound.ltx`: `common_radio` and every section with exactly its tracks - every level
  radio of the stock spawn (`esc_sidorovich_radio`, `bar_visitors_radio`, ...), played by `ph_sound`
  through `xr_sound.get_sound_object`. `scientific_radio`, `static`, `broadcast_disrupt`,
  `zombie_radio` and `silence` are noise, not music, and stay stock.
- `misc\sound\script_sound*.ltx`: a 3d theme whose `path` is under `music\radio\` -
  `play_radio_dar2`, the workbench radio, one track per press (`sound_theme.object_sound`). A switch
  of such a radio by script takes effect at its next press.

Both pickers are wrapped, not replaced: with no module radio music, the station at Everything, no
binding and the streamer mode off, the stock code picks exactly as before, with the same random
numbers. The `enable_radio_mod` option of the game still switches every level radio off.

### Not covered by the scripts

Music a script does not pick from a list - the camp guitar and harmonica of the NPCs' own sound
collections, a sound a script names directly, the ambient and menu music of the engine - is left to
the engine's own refusal in the streamer mode.

### Tests

`python tools\nq\run_lua.py tools\radio\test_radio_scripts.lua [--stock <scripts folder>]`: the pool,
the stations, the playlists, the streamer mode, the bindings, the save and the options row; with the
game's unpacked `ph_sound`, `xr_sound`, `sound_theme` and `xrs_dyn_music` the stock scheme and pickers
themselves, including the stock-identical case.
