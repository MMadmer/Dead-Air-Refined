-- The hour before midnight: the weather mixer of 23:00 and 00:00 ran the clock backwards through the
-- day, so the dynamic sun (the moon's flare, moonlight shadows) rose and set inside that hour. Frames at
-- 22:30, 23:20, 23:40 and 00:20 must all be night.
--
--   tools\qa\Run-WaterQa.ps1 -Name moon -Scenario moon_midnight -Save fogqa -KeepShaderCache -Seconds 120
local stage, t0, frames = 0, 0, 0
local shot_frame = -1
local times = { { 22, 30 }, { 23, 20 }, { 23, 40 }, { 0, 20 } }

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function go_to(h, m)
	-- the save carries some radiation, and a jumped hour applies it at once
	db.actor.radiation = -1
	db.actor.health = 1
	local ch, cm = level.get_time_hours(), level.get_time_minutes()
	local delta = ((h * 60 + m) - (ch * 60 + cm)) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
	level.set_weather(level.get_weather(), true)
end

local function on_update()
	if not db.actor then return end
	if not db.actor:alive() then
		say("[qa] the actor died")
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		return
	end
	device():pause(false)
	frames = frames + 1
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
	if frames < 90 then return end
	local now = time_global()

	local i = math.floor(stage / 2) + 1
	if stage % 2 == 0 and times[i] and now - t0 > 1000 then
		go_to(times[i][1], times[i][2])
		t0 = now stage = stage + 1
	elseif stage % 2 == 1 and now - t0 > 2500 then
		say(string.format("[qa] SHOT %d at %02d:%02d", i, level.get_time_hours(), level.get_time_minutes()))
		cmd("r__screenshot_every 1")
		shot_frame = frames
		t0 = now stage = stage + 1
	elseif not times[i] and now - t0 > 1500 then
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		times = {}
		stage = -100
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
