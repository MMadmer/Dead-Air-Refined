-- The debug menu (docs/dead-air/DEBUG_MENU.md) in one short run: turned on, opened over the game, every tab driven
-- through dar_debug_menu_do (search, pick, count, choice, press) and checked in the game's state; the window's state
-- lines ("* [debug menu] ...") are in the log. Each check prints "[qa] dbg <name> PASS|FAIL <detail>".
--
--   tools\qa\Run-WaterQa.ps1 -Name dbg -Scenario debug_menu -Save qa_meadow -KeepShaderCache -Seconds 240
local stage, t0, frames, step = "start", 0, 0, 0
local passed, total = 0, 0
local mark = {}

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end
local function dbg(s) cmd("dar_debug_menu_do " .. s) end

local function check(name, ok, detail)
	total = total + 1
	if ok then passed = passed + 1 end
	say("[qa] dbg " .. name .. " " .. (ok and "PASS" or "FAIL") .. " " .. tostring(detail or ""))
end

local function count_of(sec)
	local n = 0
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then n = n + 1 end end, nil)
	return n
end

local function near_count(sec, radius)
	local n = 0
	local p = db.actor:position()
	level.iterate_online_objects(function(obj)
		if obj:section() == sec and obj:parent() == nil and obj:position():distance_to(p) < radius then n = n + 1 end
	end)
	return n
end

local function stalkers_near(radius)
	local n = 0
	local p = db.actor:position()
	level.iterate_online_objects(function(obj)
		if IsStalker(obj) and obj:id() ~= db.actor:id() and obj:position():distance_to(p) < radius then n = n + 1 end
	end)
	return n
end

-- "medkit" typed in the Russian layout (vtlrbn) and "aptechka" in Cyrillic, as bytes (the file is UTF-8, the game cp1251)
local medkit_ru = string.char(0xEC, 0xE5, 0xE4, 0xEA, 0xE8, 0xF2)
local aptechka = string.char(0xE0, 0xEF, 0xF2, 0xE5, 0xF7, 0xEA, 0xE0)

local plan = {
	function() cmd("g_god on"); cmd("dar_debug_menu 1"); cmd("dar_debug_menu_open") end,
	function() dbg("state") end,
	function() dbg("search wpn_ak74") end,
	function() dbg("search " .. aptechka) end,
	function() dbg("search " .. medkit_ru) end,
	function() dbg("search medkit"); dbg("pick_key medkit"); dbg("choice 1"); dbg("count 3"); mark.medkits = count_of("medkit") end,
	function() dbg("press primary") end,
	function() check("spawn_inventory", count_of("medkit") == mark.medkits + 3, count_of("medkit") .. " vs " .. mark.medkits)
		mark.ground = near_count("medkit", 6); dbg("choice 2"); dbg("count 2"); dbg("press primary") end,
	function() check("spawn_in_front", near_count("medkit", 6) >= mark.ground + 2, near_count("medkit", 6) .. " vs " .. mark.ground) end,
	function() dbg("tab npc"); dbg("search bandit novice"); dbg("pick 0"); mark.stalkers = stalkers_near(30); dbg("press primary") end,
	function() end,
	function() check("spawn_npc", stalkers_near(30) > mark.stalkers, stalkers_near(30) .. " vs " .. mark.stalkers) end,
	function() dbg("tab player"); dbg("pick_key ammo"); mark.ammo = get_console():get_bool("g_unlimitedammo"); dbg("press primary") end,
	function() check("toggle_ammo", get_console():get_bool("g_unlimitedammo") ~= mark.ammo, tostring(get_console():get_bool("g_unlimitedammo")))
		dbg("press primary"); dbg("pick_key money"); dbg("count 12345"); mark.money = db.actor:money(); dbg("press primary") end,
	function() check("money", db.actor:money() == mark.money + 12345, db.actor:money() .. " vs " .. mark.money) end,
	function() dbg("tab world"); dbg("pick_key time:12"); dbg("press primary") end,
	function() check("time", level.get_time_hours() == 12, level.get_time_hours() .. ":" .. level.get_time_minutes())
		dbg("tab story"); dbg("search escape"); dbg("pick 0"); dbg("state") end,
	function() dbg("tab near"); dbg("state"); dbg("pick 0") end,
	function() dbg("tab teleport"); dbg("pick 3"); dbg("state"); mark.pos = vector():set(db.actor:position()); dbg("press primary") end,
	function() end,
	function() check("teleport", db.actor:position():distance_to(mark.pos) > 5, string.format("moved %.1f m", db.actor:position():distance_to(mark.pos))) end,
	function() cmd("dar_debug_menu_open"); dbg("tab items"); dbg("state") end,
	function() cmd("dar_debug_menu_open") end,
}

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		level.set_time_factor(0.0001)
		stage, t0 = "run", now
	elseif stage == "run" and now - t0 > 1500 then
		step = step + 1
		local f = plan[step]
		if not f then
			say(string.format("[qa] dbg %d/%d passed", passed, total))
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		local ok, err = pcall(f)
		if not ok then say("[qa] dbg step " .. step .. " error " .. tostring(err)) end
		t0 = now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
