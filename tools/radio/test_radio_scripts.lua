-- The radios' music: headless tests of xms_radio.script (test tooling, not shipped).
--
--   python tools\nq\run_lua.py tools\radio\test_radio_scripts.lua [-v] [--stock <scripts folder>]
--
-- Without --stock the pool, the stations, the playlists, the streamer mode, the bindings of one
-- radio, the save and the options row run against the script alone. A --stock folder holds the
-- game's own ph_sound.script, xr_sound.script, sound_theme.script and xrs_dyn_music.script
-- (unpacked from the game's archives): the radio scheme, the theme pickers and the combat music are
-- then the stock code itself, driven through stand-ins of the engine's sound objects, with the
-- wrappers installed over it. The engine's calls (xms.radio, xms.streamer_mode, ...) are stand-ins
-- throughout.

local HERE = string.match(arg[0], "^(.*)[\\/][^\\/]+$") or "."
local REPO = HERE .. "\\..\\.."
local SCRIPT = os.getenv("XMS_RADIO_SCRIPT")
	or (REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\scripts\\xms_radio.script")

local verbose, stock_dirs = false, {}
do
	local i = 1
	while i <= #arg do
		if (arg[i] == "-v") then verbose = true
		elseif (arg[i] == "--stock") then i = i + 1; stock_dirs[#stock_dirs + 1] = arg[i]
		end
		i = i + 1
	end
end

local passed, failed = 0, 0
local function check(cond, msg)
	if (cond) then
		passed = passed + 1
		if (verbose) then io.write("  ok   ", msg, "\n") end
	else
		failed = failed + 1
		io.write("  FAIL ", msg, "\n")
	end
	return cond
end
local function section(name) io.write("== ", name, "\n") end

-- ============================================================================ the engine, faked
local G = _G
local LOG = {}
function printf(fmt, ...)
	local args, i = { ... }, 0
	local s = (select("#", ...) == 0) and tostring(fmt) or string.gsub(tostring(fmt), "%%s", function()
		i = i + 1
		return tostring(args[i])
	end)
	LOG[#LOG + 1] = s
	if (verbose) then io.write("     log: ", s, "\n") end
end
log = printf
local function log_has(text)
	for _, l in ipairs(LOG) do
		if (string.find(l, text, 1, true)) then return true end
	end
	return false
end

-- an ini over { [section] = { key = value } }; lines come back sorted, as the engine hands them
local function make_ini(data)
	local ini = {}
	local function lines(s)
		local keys = {}
		for k in pairs(data[s] or {}) do keys[#keys + 1] = k end
		table.sort(keys)
		return keys
	end
	function ini:section_exist(s) return data[s] ~= nil end
	function ini:line_exist(s, k) return data[s] ~= nil and data[s][k] ~= nil end
	function ini:line_count(s) return #lines(s) end
	function ini:r_line(s, i)
		local k = lines(s)[i + 1]
		return k ~= nil, k, data[s][k]
	end
	function ini:r_string(s, k) return data[s][k] end
	function ini:r_string_ex(s, k) return data[s] and data[s][k] or nil end
	function ini:r_bool_ex(s, k, def)
		local v = data[s] and data[s][k]
		if (v == nil) then return def end
		return v == true or v == "true"
	end
	function ini:r_float_ex(s, k) return data[s] and tonumber(data[s][k]) or nil end
	return ini
end

local FILES = {}
function ini_file(name) return make_ini(FILES[name] or {}) end

-- the game's radio themes, as plugins\xr_sound.ltx has them (fewer tracks)
local GAME_RADIO = {}
for n = 1, 6 do GAME_RADIO[#GAME_RADIO + 1] = string.format("radio\\01\\music_%02d", n) end
local function radio_section()
	local s = {}
	for _, n in ipairs(GAME_RADIO) do s[n] = "" end
	return s
end
FILES["plugins\\xr_sound.ltx"] = {
	common_radio = radio_section(),
	esc_sidorovich_radio = radio_section(),
	scientific_radio = { ["radio\\02\\marsh_beep_1"] = "", ["radio\\02\\marsh_beep_2"] = "" },
	static = { ["soundtrack\\static\\static_01"] = "" },
	silence = { none = "" },
}

alun_utils = {
	collect_section = function(ini, section)
		local t = {}
		if (section and ini:section_exist(section)) then
			for i = 0, ini:line_count(section) - 1 do
				local _, id = ini:r_line(section, i, "", "")
				t[#t + 1] = id
			end
		end
		return t
	end,
}

-- the mode, the game's copyrighted list and its replacements, as the engine will know them
local ENGINE = { streamer = false, rights = {}, replace = {} }
local function lower(s) return string.lower(string.gsub(s, "/", "\\")) end
local function engine_copyrighted(name)
	name = lower(name)
	for key in pairs(ENGINE.rights) do
		local k = lower(key)
		if (string.sub(k, -1) == "*") then
			if (string.sub(name, 1, #k - 1) == string.sub(k, 1, -2)) then return true, key end
		elseif (k == name) then
			return true, key
		end
	end
	return false
end

-- a sound object: plays unless the engine refuses a copyrighted one in the streamer mode
local PLAYED = {}
local LIVE = {}
local snd_mt = { __index = {} }
local snd_methods = snd_mt.__index
function snd_methods:play_at_pos()
	if (ENGINE.streamer and engine_copyrighted(self.name)) then
		self.refused = true
		self.on = false
		return
	end
	self.on = true
	PLAYED[#PLAYED + 1] = self.name
	LIVE[#LIVE + 1] = self
end
snd_methods.play = snd_methods.play_at_pos
function snd_methods:playing() return self.on == true end
function snd_methods:stop() self.on = false; self.stopped = true end
function snd_methods:set_position() end
function snd_methods:length() return 1000 end
sound_object = setmetatable({ s2d = 1, s3d = 0, looped = 2 }, {
	__call = function(_, name) return setmetatable({ name = name, volume = 1 }, snd_mt) end,
})
local function end_all()
	for _, s in ipairs(LIVE) do s.on = false end
	LIVE = {}
end

-- what the game's config keeps (axr_options.ltx)
local CONFIG = {}
axr_main = { config = {
	r_value = function(self, s, k, t, def)
		local v = CONFIG[s .. "." .. k]
		if (v == nil) then return def end
		if (t == 1) then return v == true or v == "true" end
		return v
	end,
	w_value = function(self, s, k, v) CONFIG[s .. "." .. k] = v end,
	save = function(self) CONFIG.saved = (CONFIG.saved or 0) + 1 end,
} }

local CALLBACKS = {}
function RegisterScriptCallback(name, fn)
	CALLBACKS[name] = CALLBACKS[name] or {}
	CALLBACKS[name][fn] = true
end
function UnregisterScriptCallback(name, fn) if (CALLBACKS[name]) then CALLBACKS[name][fn] = nil end end
local function send(name, ...)
	for fn in pairs(CALLBACKS[name] or {}) do fn(...) end
end

game = { translate_string = function(id) return "T:" .. tostring(id) end }

local NOW = 100000
function time_global() return NOW end
function device() return { time_global = function() return NOW end } end

local STORY = {}
function get_story_object_id(sid) return STORY[sid] end
local SERVER = {}
function alife()
	return { object = function(self, key) return SERVER[key] end }
end

-- a radio object of the world: its id and its custom data
local function vec() return { distance_to_sqr = function() return 0 end } end
local function make_object(id, custom)
	return {
		_id = id,
		id = function(self) return self._id end,
		position = function() return vec() end,
		spawn_ini = function() return custom and make_ini(custom) or nil end,
	}
end

db = { storage = {}, actor = nil }

-- the engine's xms table: built per case
local SAVED = {}
local function make_xms(opt)
	opt = opt or {}
	local x = {}
	x.modules = function() return {} end
	if not (opt.old_engine) then
		x.radio = function() return { only = opt.only, tracks = opt.tracks or {}, playlists = opt.playlists or {} } end
		x.streamer_mode = function() return ENGINE.streamer end
		x.sound_copyrighted = function(name)
			if (engine_copyrighted(name)) then return true end
			for _, t in ipairs(opt.tracks or {}) do
				if (lower(t.sound) == lower(name) and t.copyrighted) then return true end
			end
			return false
		end
		x.sound_replacement = function(name)
			local yes, key = engine_copyrighted(name)
			local rep = yes and ENGINE.replace[key] or nil
			if not (rep) then
				for _, t in ipairs(opt.tracks or {}) do
					if (lower(t.sound) == lower(name) and t.copyrighted) then rep = t.streamer end
				end
			end
			if (rep and x.sound_copyrighted(rep)) then return nil end
			return rep
		end
	end
	if not (opt.no_save) then
		x.save_data = function(id, blob)
			if (opt.refuse_save) then return false end
			SAVED[id] = blob
			return true
		end
		x.load_data = function(id) return SAVED[id] end
	end
	return x
end

-- a script loaded the way the game loads a namespace
local function load_script(ns, path)
	local env = setmetatable({}, { __index = G })
	G[ns] = env
	local chunk = assert(loadfile(path))
	setfenv(chunk, env)
	chunk()
	return env
end

-- luabind's class, as the stock scripts use it
function class(name)
	local cls = {}
	cls.__index = cls
	setmetatable(cls, { __call = function(c, ...)
		local o = setmetatable({}, c)
		if (c.__init) then c.__init(o, ...) end
		return o
	end })
	getfenv(2)[name] = cls
	return function() end
end

-- a fresh Lua state with the engine's data as given
local function new_state(opt)
	for _, ns in ipairs({ "xms_radio", "xr_sound", "ph_sound", "sound_theme", "xrs_dyn_music" }) do G[ns] = nil end
	CALLBACKS, LOG, PLAYED, LIVE = {}, {}, {}, {}
	CONFIG = { ["mm_options.enable_radio_mod"] = true }
	ENGINE.streamer = false
	db.storage, db.actor = {}, { position = function() return vec() end }
	xms = make_xms(opt)
	return load_script("xms_radio", SCRIPT)
end

local function count(list)
	local c = {}
	for _, v in ipairs(list) do c[v] = (c[v] or 0) + 1 end
	return c
end

local function picks(xr, key, own, n)
	local out = {}
	for _ = 1, n do
		local name = xr.next_track(key, own)
		out[#out + 1] = name or "<silence>"
	end
	return out
end

local function no_repeats(list)
	for i = 2, #list do
		if (list[i] == list[i - 1]) then return false end
	end
	return true
end

local MOD_A = { module = "mod_a", id = "rain", sound = "xms_radio\\mod_a\\rain", copyrighted = false, volume = 0.5, weight = 1 }
local MOD_B = { module = "mod_b", id = "wind", sound = "xms_radio\\mod_b\\wind", copyrighted = false, volume = 1, weight = 1 }
local MOD_B2 = { module = "mod_b", id = "dust", sound = "xms_radio\\mod_b\\dust", copyrighted = false, volume = 1, weight = 1 }

local function playlist_row(module, id, order, tracks)
	return { module = module, id = id, title = "st_" .. id, order = order, tracks = tracks }
end

-- ============================================================================ the pool
math.randomseed(7)
section("the common music")
do
	local xr = new_state()
	local list = picks(xr, 1, GAME_RADIO, 400)
	local c = count(list)
	local all = true
	for _, n in ipairs(GAME_RADIO) do all = all and (c[n] or 0) > 20 end
	check(all and c["<silence>"] == nil, "no module: every game track plays")
	check(no_repeats(list), "never the same track twice in a row")
	check(xr.stock_wanted(1) == true, "no module, Everything, mode off: the stock picker")

	xr = new_state({ tracks = { MOD_A, MOD_B } })
	c = count(picks(xr, 1, GAME_RADIO, 800))
	check((c[MOD_A.sound] or 0) > 40 and (c[MOD_B.sound] or 0) > 40 and (c[GAME_RADIO[1]] or 0) > 40,
		"module tracks join the game's")
	check(xr.stock_wanted(1) == false, "a module track: the pool is ours")

	-- a weight counts among the others: never twice in a row keeps the heavy track under half
	local heavy = { module = "mod_a", id = "heavy", sound = "xms_radio\\mod_a\\heavy", weight = 8 }
	local lights = {}
	for i = 1, 4 do lights[i] = { module = "mod_a", id = "l" .. i, sound = "xms_radio\\mod_a\\l" .. i, weight = 1 } end
	xr = new_state({ tracks = { heavy, lights[1], lights[2], lights[3], lights[4] }, only = "mod_a" })
	c = count(picks(xr, 1, GAME_RADIO, 4000))
	local heavier = true
	for i = 1, 4 do heavier = heavier and (c[heavy.sound] or 0) > 2.5 * (c[lights[i].sound] or 0) end
	check(heavier, "weights: " .. tostring(c[heavy.sound]) .. " against " .. tostring(c[lights[1].sound]))
	local zero = { module = "mod_a", id = "z", sound = "xms_radio\\mod_a\\z", weight = 0 }
	xr = new_state({ tracks = { MOD_A, zero }, only = "mod_a" })
	c = count(picks(xr, 1, GAME_RADIO, 200))
	check(c[zero.sound] == nil and c[MOD_A.sound] == 200, "weight 0 never plays")

	xr = new_state({ tracks = { MOD_A, MOD_B, MOD_B2 }, only = "mod_b" })
	c = count(picks(xr, 1, GAME_RADIO, 300))
	check(c[MOD_A.sound] == nil and c[GAME_RADIO[1]] == nil and (c[MOD_B.sound] or 0) > 50 and (c[MOD_B2.sound] or 0) > 50,
		"only: one module's tracks are the whole pool")
	xr = new_state({ tracks = { MOD_A }, only = "mod_b" })
	check(xr.next_track(1, GAME_RADIO) == nil, "only with no tracks of that module: silence")
end

section("the stations")
do
	local p_list = playlist_row("mod_a", "road", "list", { MOD_A.sound, "xms_radio\\mod_a\\x", MOD_B.sound })
	local p_shuf = playlist_row("mod_a", "mix", "shuffle", { "a\\1", "a\\2", "a\\3", "a\\4", "a\\5" })
	local opt = { tracks = { MOD_A, MOD_B }, only = "mod_b", playlists = { p_list, p_shuf } }

	local xr = new_state(opt)
	check(xr.station() == "all", "the default station is Everything")
	CONFIG["mm_options.radio_station"] = "game"
	local c = count(picks(xr, 1, GAME_RADIO, 200))
	check(c[MOD_A.sound] == nil and c[MOD_B.sound] == nil and (c[GAME_RADIO[2]] or 0) > 10,
		"The game's music: the game's tracks alone, whatever only says")

	CONFIG["mm_options.radio_station"] = "mod_a.road"
	local list = picks(xr, 1, GAME_RADIO, 7)
	check(table.concat(list, "|") == table.concat({ MOD_A.sound, "xms_radio\\mod_a\\x", MOD_B.sound,
		MOD_A.sound, "xms_radio\\mod_a\\x", MOD_B.sound, MOD_A.sound }, "|"), "a playlist in its order, round and round")

	CONFIG["mm_options.radio_station"] = "mod_a.mix"
	list = picks(xr, 2, GAME_RADIO, 15)
	local passes_ok = true
	for pass = 0, 2 do
		local seen = {}
		for i = 1, 5 do seen[list[pass * 5 + i]] = true end
		for _, t in ipairs(p_shuf.tracks) do passes_ok = passes_ok and seen[t] == true end
	end
	check(passes_ok, "a shuffled playlist: every track once a pass")
	check(no_repeats(list), "a shuffled playlist does not repeat across passes")

	CONFIG["mm_options.radio_station"] = "mod_gone.road"
	check(xr.station() == "all" and xr.station_setting() == "mod_gone.road", "a missing playlist: Everything, the setting kept")
	c = count(picks(xr, 3, GAME_RADIO, 100))
	check((c[MOD_B.sound] or 0) > 0 and c[GAME_RADIO[1]] == nil, "a missing playlist plays the common music")
end

section("the streamer mode")
do
	local cr = { module = "mod_a", id = "hit", sound = "xms_radio\\mod_a\\hit", copyrighted = true, weight = 1,
		streamer = "xms_radio\\mod_a\\free" }
	local cn = { module = "mod_a", id = "hit2", sound = "xms_radio\\mod_a\\hit2", copyrighted = true, weight = 1 }
	local free = { module = "mod_a", id = "ok", sound = "xms_radio\\mod_a\\ok", copyrighted = false, weight = 1 }
	local p = playlist_row("mod_a", "set", "list", { free.sound, cr.sound, cn.sound, free.sound })
	local xr = new_state({ tracks = { cr, cn, free }, playlists = { p } })
	ENGINE.rights = { ["radio\\01\\*"] = true }
	ENGINE.replace = {}
	ENGINE.streamer = true
	local c = count(picks(xr, 1, GAME_RADIO, 300))
	check(c[GAME_RADIO[1]] == nil and c[cn.sound] == nil and c[cr.sound] == nil, "nothing copyrighted is chosen")
	check((c["xms_radio\\mod_a\\free"] or 0) > 0 and (c[free.sound] or 0) > 0, "a replacement plays in a track's place")
	check(xr.stock_wanted(1) == false, "the streamer mode: the pool is ours")

	CONFIG["mm_options.radio_station"] = "mod_a.set"
	local list = picks(xr, 2, GAME_RADIO, 6)
	check(table.concat(list, "|") == table.concat({ free.sound, "xms_radio\\mod_a\\free", free.sound,
		free.sound, "xms_radio\\mod_a\\free", free.sound }, "|"),
		"a playlist keeps its order: the replacement in place, the one without skipped")

	ENGINE.streamer = false
	list = picks(xr, 2, GAME_RADIO, 4)
	check(list[1] == free.sound and list[2] == cr.sound and list[3] == cn.sound, "mode off: the playlist goes on with the originals")

	local xr2 = new_state({ tracks = { cn }, only = "mod_a" })
	ENGINE.rights = { ["radio\\01\\*"] = true }
	ENGINE.streamer = true
	check(xr2.next_track(1, GAME_RADIO) == nil, "nothing free: silence")
	ENGINE.rights = { ["radio\\01\\music_01"] = true }
	ENGINE.replace = { ["radio\\01\\music_01"] = "radio\\01\\music_01" }
	xr2 = new_state({})
	ENGINE.streamer = true
	c = count(picks(xr2, 1, GAME_RADIO, 200))
	check(c[GAME_RADIO[1]] == nil, "a replacement that is copyrighted itself does not play")
	ENGINE.rights, ENGINE.replace = {}, {}
end

section("one radio's own playlist")
do
	local p = playlist_row("mod_a", "road", "list", { MOD_A.sound, MOD_B.sound })
	local xr = new_state({ tracks = { MOD_A, MOD_B }, playlists = { p } })
	local own_data = make_object(10, { xms_radio = { playlist = "mod_a.road" } })
	local off_data = make_object(11, { xms_radio = { playlist = "off" } })
	local game_data = make_object(12, { xms_radio = { playlist = "game" } })
	local typo = make_object(13, { xms_radio = { playlist = "not a playlist" } })
	db.storage[10], db.storage[11], db.storage[12], db.storage[13] = { object = own_data }, { object = off_data },
		{ object = game_data }, { object = typo }
	check(xr.next_track(10, GAME_RADIO, own_data) == MOD_A.sound and xr.next_track(10, GAME_RADIO, own_data) == MOD_B.sound,
		"custom data: the radio plays its playlist")
	check(xr.next_track(11, GAME_RADIO, off_data) == nil, "custom data off: silent")
	local c = count(picks(xr, 12, GAME_RADIO, 50))
	check(c[MOD_A.sound] == nil and c[MOD_B.sound] == nil, "custom data game: the game's music")
	check(xr.choice_of(13, typo) == "all" and log_has("is no playlist"), "custom data that names nothing: the station, said once")

	-- by script
	STORY["radio_story"] = 20
	SERVER["radio_name"] = { id = 21 }
	check(xr.set("radio_story", "mod_a.road") == true and xr.choice_of(20) == "mod_a.road", "set by story id")
	check(xr.set("radio_name", "off") == true and xr.choice_of(21) == "off", "set by object name")
	check(xr.set(make_object(22), "game") == true and xr.choice_of(22) == "game", "set by game object")
	check(xr.set(23, "nonsense value") == false, "a value that names nothing is refused")
	check(xr.set("nobody", "off") == false, "a radio that is not there is refused")
	xr.set(10, "off")
	local b, from, now = xr.get(10)
	check(b == "off" and from == "script" and now == "off", "a script binding wins over the data")
	xr.set(10, nil)
	b, from, now = xr.get(10)
	check(b == "mod_a.road" and from == "data" and now == "mod_a.road", "nil gives the radio back to its data")
	xr.set(22, nil)
	b, from, now = xr.get(22)
	check(b == nil and now == "all", "nil gives the radio back to the station")

	-- a playlist whose module is away
	xr.set(24, "mod_gone.road")
	b, from, now = xr.get(24)
	check(b == "mod_gone.road" and now == "all", "a module away: the station plays, the binding is kept")

	local list = xr.playlists()
	check(#list == 1 and list[1].key == "mod_a.road" and list[1].tracks == 2 and list[1].order == "list", "playlists()")

	-- at once or at the next track
	local g = xr.gen_of(10)
	xr.set(10, "game")
	check(xr.gen_of(10) == g + 1, "a switch is at once by default")
	xr.set(10, "off", true)
	check(xr.gen_of(10) == g + 1, "at the next track: nothing is cut")
end

section("the save")
do
	SAVED = {}
	local xr = new_state({})
	db.actor = nil
	xr.set(30, "mod_a.road")
	check(SAVED["xms.radio"] == nil, "before the save is read nothing is staged")
	db.actor = { position = function() return vec() end }
	-- the actor exists: the save is read, the pending binding staged
	local b = xr.get(30)
	check(b == "mod_a.road" and SAVED["xms.radio"] == "v=1\n30=mod_a.road", "staged: " .. tostring(SAVED["xms.radio"]))
	xr.set(31, "off")
	check(SAVED["xms.radio"] == "v=1\n30=mod_a.road\n31=off", "every change is staged")

	-- a new Lua state: a load or a level change
	xr = new_state({})
	db.actor = nil
	check(xr.get(30) == nil, "before the actor: nothing read yet")
	db.actor = { position = function() return vec() end }
	check(xr.get(30) == "mod_a.road" and xr.get(31) == "off", "after a load the radios find their bindings")
	xr.set(30, nil)
	xr.set(31, nil)
	check(SAVED["xms.radio"] == nil, "nothing bound: nothing in the save")

	SAVED = { ["xms.radio"] = "v=9\n30=off" }
	xr = new_state({})
	check(xr.get(30) == nil and log_has("version 9"), "a newer save's bindings are left out, said")

	SAVED = {}
	xr = new_state({ refuse_save = true })
	xr.set(40, "off")
	check(xr.get(40) == "off" and log_has("does not take xms.radio"), "an engine that refuses the blob: kept for the session, said")

	SAVED = {}
	xr = new_state({})
	xr.get(1)
	check(next(SAVED) == nil, "a game that binds nothing writes nothing")
end

section("the options row")
do
	local function combo()
		local c = { items = {}, current = 0 }
		function c:AddItem(text, id) self.items[#self.items + 1] = { text = text, id = id } end
		function c:SetCurrentID(id) self.current = id end
		function c:CurrentID() return self.current end
		return c
	end
	local xml = { InitComboBox = function(self, node) local c = combo(); c.node = node; return c end }
	local p = playlist_row("mod_a", "road", "list", { MOD_A.sound })

	local xr = new_state({})
	local handler = {}
	check(xr.options_init({}, xml, handler, "sound_page") == false and handler.xms_radio_station == nil,
		"no module radio music: no station row")

	xr = new_state({ tracks = { MOD_A }, playlists = { p } })
	CONFIG["mm_options.radio_station"] = "mod_a.road"
	handler = {}
	check(xr.options_init({}, xml, handler, "sound_page") == true, "a module brings radio music: the row is built")
	local c = handler.xms_radio_station
	check(c and c.node == "sound_page:list_radio_station" and #c.items == 3 and c.items[1].text == "T:st_da_x64_radio_all"
		and c.items[2].text == "T:st_da_x64_radio_game" and c.items[3].text == "T:st_road", "Everything, the game's music, the playlist")
	check(c.current == 2, "the current station is selected")
	c.current = 1
	send("opt_menu_on_accept", handler)
	check(CONFIG["mm_options.radio_station"] == "game" and (CONFIG.saved or 0) == 1, "Accept keeps the station with the script options")
	CONFIG["mm_options.radio_station"] = "mod_gone.x"
	send("opt_menu_on_set_values", handler)
	check(c.current == 0, "a station that is gone shows Everything")

	xr = new_state({ old_engine = true })
	handler = {}
	check(xr.options_init({}, xml, handler, "sound_page") == false and xr.stock_wanted(1) == true and xr.streamer_mode() == false,
		"an engine without the calls: no row, the stock pickers, no streamer mode")
end

-- ============================================================================ the stock scripts
local function run_stock(dir)
	section("the stock scripts of " .. dir)
	local function path(name) return dir .. "\\" .. name .. ".script" end

	-- what the stock scripts reach for
	stalker_ids = { sound_script = 0 }
	parse_names = function(s)
		local t = {}
		for w in string.gmatch(s or "", "[^,%s]+") do t[#t + 1] = w end
		return t
	end
	utils = { load_var = function() return false end, save_var = function() end }
	xr_logic = {
		try_switch_to_another_section = function() return false end,
		pick_section_from_condlist = function() return "1" end,
		parse_condlist = function() return {} end,
	}
	closecaption = { sound = function() return true end }
	news_manager = { send_sound = function() end }
	local FS_FILES = {}
	getFS = function() return { exist = function(_, _, p) return FS_FILES[lower(p)] end } end
	local CONSOLE = {}
	get_console = function()
		return { get_float = function() return 0.5 end, execute = function(_, c) CONSOLE[#CONSOLE + 1] = c end }
	end

	local function load_stock(opt)
		local xr = new_state(opt)
		load_script("xr_sound", path("xr_sound"))
		load_script("ph_sound", path("ph_sound"))
		load_script("sound_theme", path("sound_theme"))
		load_script("xrs_dyn_music", path("xrs_dyn_music"))
		return xr
	end

	-- the radio scheme, driven: one radio, its storage as set_scheme leaves it
	local function make_radio(id, custom)
		local obj = make_object(id, custom)
		db.storage[id] = { object = obj }
		local st = { theme = "esc_sidorovich_radio", random = true, looped = false, pause_min = 0, pause_max = 0,
			volume = {}, signals = {} }
		local src = ph_sound.snd_source(obj, st)
		src:reset_scheme(false)
		return src, st
	end
	local function tick(src, ms)
		NOW = NOW + (ms or 60)
		src:update(0)
	end
	local function play_through(src, n)
		local before = #PLAYED
		for _ = 1, n do
			tick(src)
			end_all()
			tick(src)
		end
		local out = {}
		for i = before + 1, #PLAYED do out[#out + 1] = PLAYED[i] end
		return out
	end

	-- stock behaviour: the same picks and the same random numbers as the unwrapped picker
	do
		local xr = load_stock({})
		local base = xr_sound.get_sound_object
		math.randomseed(42)
		local a = {}
		for _ = 1, 50 do a[#a + 1] = base("esc_sidorovich_radio", "random").name end
		local after_a = math.random(1, 1000000)
		xr_sound.clear_all_sound_object()
		xr.install()
		check(xr_sound.get_sound_object ~= base, "the picker is wrapped")
		math.randomseed(42)
		local b = {}
		for _ = 1, 50 do b[#b + 1] = xr_sound.get_sound_object("esc_sidorovich_radio", "random").name end
		local after_b = math.random(1, 1000000)
		check(table.concat(a, "|") == table.concat(b, "|") and after_a == after_b,
			"no module, Everything, mode off: the stock picks, the stock random numbers")
		xr_sound.clear_all_sound_object()
		local src = make_radio(1)
		math.randomseed(5)
		local stock_seq = play_through(src, 10)
		check(#stock_seq == 10, "the stock scheme plays track after track")
	end

	-- module tracks, a playlist on one radio, the cut, the volume
	do
		local p = playlist_row("mod_a", "road", "list", { MOD_A.sound, MOD_B.sound })
		local xr = load_stock({ tracks = { MOD_A, MOD_B }, playlists = { p } })
		xr.install()
		local src = make_radio(1)
		local c = count(play_through(src, 200))
		check((c[MOD_A.sound] or 0) > 10 and (c[GAME_RADIO[1]] or 0) > 10, "the scheme plays the game's and the modules' tracks")

		local bound = make_radio(2, { xms_radio = { playlist = "mod_a.road" } })
		local seq = play_through(bound, 4)
		check(table.concat(seq, "|") == table.concat({ MOD_A.sound, MOD_B.sound, MOD_A.sound, MOD_B.sound }, "|"),
			"a radio bound by its custom data plays its playlist in order")
		tick(bound)
		local playing = bound.played_sound
		check(playing and playing.name == MOD_A.sound and math.abs(playing.volume - 0.5) < 1e-6,
			"a track's volume scales the scheme's")
		tick(bound)
		tick(bound)
		check(math.abs(bound.played_sound.volume - 0.5) < 1e-6, "and stays scaled")

		xr.set(2, "game")
		tick(bound)
		check(playing.stopped == true and bound.played_sound ~= playing and bound.played_sound:playing(),
			"a switch cuts the track at once")
		local now = bound.played_sound.name
		check(string.find(now, "radio\\01\\", 1, true) == 1, "and the game's music plays: " .. tostring(now))
		local cur = bound.played_sound
		xr.set(2, "off", true)
		tick(bound)
		check(bound.played_sound == cur and cur:playing(), "at the next track: the track plays on")
		end_all()
		tick(bound)
		tick(bound)
		check(bound.played_sound.xms_silent == true, "then off: silent")
	end

	-- the streamer mode in the scheme: refused, silent, back
	do
		local xr = load_stock({})
		xr.install()
		local src = make_radio(1)
		tick(src)
		local first = src.played_sound
		check(first and first:playing(), "a track plays")
		ENGINE.rights = { ["radio\\01\\*"] = true }
		ENGINE.streamer = true
		first.on = false			-- the engine stops a copyrighted sound when the mode comes on
		tick(src)
		tick(src)
		check(src.played_sound.xms_silent == true, "the mode on, nothing free: the radio is silent")
		for _ = 1, 20 do tick(src) end
		local refused = 0
		for _, s in ipairs(LIVE) do if (s.refused) then refused = refused + 1 end end
		check(refused == 0, "nothing the engine would refuse is tried")
		ENGINE.streamer = false
		tick(src)
		tick(src)
		check(src.played_sound.xms_silent ~= true and src.played_sound:playing(), "the mode off: the radio plays again, no stall")

		ENGINE.rights = { ["radio\\01\\music_01"] = true, ["radio\\01\\music_02"] = true }
		ENGINE.replace = { ["radio\\01\\music_01"] = "radio\\free\\one" }
		ENGINE.streamer = true
		local c = count(play_through(src, 200))
		check(c[GAME_RADIO[1]] == nil and c[GAME_RADIO[2]] == nil and (c["radio\\free\\one"] or 0) > 0 and (c[GAME_RADIO[3]] or 0) > 0,
			"free plays, copyrighted with a replacement plays the replacement, without is skipped")
		local t = xr_sound.get_sound_object("static", "random")
		check(t and t.name == "soundtrack\\static\\static_01", "a theme with nothing copyrighted: the stock picker")
		ENGINE.rights = { ["soundtrack\\static\\*"] = true }
		t = xr_sound.get_sound_object("static", "random")
		check(t and t.xms_silent == true, "a theme that is no radio, all copyrighted: silent")
		ENGINE.rights, ENGINE.replace, ENGINE.streamer = {}, {}, false
	end

	-- the workbench radio: sound_theme's 3d theme
	do
		for n = 1, 5 do FS_FILES[lower("music\\radio\\music_" .. n .. ".ogg")] = true end
		local p = playlist_row("mod_a", "road", "list", { MOD_A.sound, MOD_B.sound })
		local xr = load_stock({ tracks = { MOD_A }, playlists = { p } })
		xr.install()
		local snd_ini = make_ini({ play_radio_dar2 = { type = "3d", path = "music\\radio\\music_", shuffle = "rnd", idle = "1,1,100" } })
		local theme = sound_theme.object_sound(snd_ini, "play_radio_dar2")
		check(#theme.sound == 5, "the 3d theme reads the game's radio tracks")
		local obj = make_object(50)
		db.storage[50] = { object = obj }
		local function press()
			NOW = NOW + 5000
			local r = theme:play(50)
			end_all()
			theme:callback(50)
			NOW = NOW + 5000
			return r
		end
		CONFIG["mm_options.radio_station"] = "mod_a.road"
		local before = #PLAYED
		press(); press(); press()
		check(PLAYED[before + 1] == MOD_A.sound and PLAYED[before + 2] == MOD_B.sound and PLAYED[before + 3] == MOD_A.sound,
			"the workbench radio plays the station's playlist")
		check(#theme.sound == 5 and theme.shuffle == "rnd", "the theme's own list is left as it was")
		NOW = NOW + 5000
		theme:play(50)
		local again = theme:play(50)
		check(again == false, "a play the stock refuses takes no track")
		end_all()
		theme:callback(50)

		CONFIG["mm_options.radio_station"] = "all"
		ENGINE.rights = { ["music\\radio\\*"] = true }
		ENGINE.streamer = true
		before = #PLAYED
		for _ = 1, 20 do press() end
		local c = count({ unpack(PLAYED, before + 1) })
		check(c[MOD_A.sound] == 20, "the mode on: only the free module track plays")
		ENGINE.rights, ENGINE.streamer = {}, false
	end

	-- the combat music
	do
		local xr = load_stock({})
		xr.install()
		local started = {}
		xrs_stereo = { stereo_sound = function()
			return { initialize = function(_, name) started[#started + 1] = name end, play = function() return NOW end,
				update = function() end, playing = function() return true end }
		end }
		local own = { { "music\\combat\\a1", "music\\combat\\a2" }, { "music\\combat\\b1" } }
		xrs_dyn_music.themes = own
		ENGINE.rights = { ["music\\combat\\a*"] = true }
		ENGINE.streamer = true
		for _ = 1, 10 do xrs_dyn_music.theme = nil; xrs_dyn_music.start_theme() end
		local c = count(started)
		check(c["music\\combat\\b1"] == 10, "the mode on: the combat music picks among what may play")
		ENGINE.rights = { ["music\\combat\\*"] = true }
		started = {}
		CONSOLE = {}
		xrs_dyn_music.start_theme()
		check(#started == 0 and #CONSOLE == 0, "nothing free: no combat music, the usual music not muted")
		ENGINE.streamer, ENGINE.rights = false, {}
		started = {}
		for _ = 1, 30 do xrs_dyn_music.theme = nil; xrs_dyn_music.start_theme() end
		c = count(started)
		check(xrs_dyn_music.themes == own and (c["music\\combat\\a1"] or 0) + (c["music\\combat\\a2"] or 0) > 0,
			"the mode off: the game's own table again")
	end
end

for _, dir in ipairs(stock_dirs) do
	local ok, err = pcall(run_stock, dir)
	check(ok, "the stock run of " .. dir .. " ended: " .. tostring(err))
end
if (#stock_dirs == 0) then io.write("   (no --stock folder: the stock scripts were not driven)\n") end

io.write(string.format("%d passed, %d failed\n", passed, failed))
if (failed > 0) then error("radio script tests failed", 0) end
