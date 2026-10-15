-- Two reports of 1.5.0 in one short run:
--   1. the bolt of a first-person weapon moves on every shot (a zero-accrue shot cuts in, it is not
--      blended in over 0.2 s): a burst captured at 60 fps;
--   2. the night sky does not sweep: from 22:30 over midnight at 600x, a frame every 2 s.
--
--   tools\qa\Run-WaterQa.ps1 -Name boltmoon -Scenario bolt_moon -Save fogqa -KeepShaderCache -Seconds 120
local stage, t0, frames = 0, 0, 0
local shots, shot_frame = 0, -1

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot()
	shots = shots + 1
	say(string.format("[qa] SHOT %02d at %02d:%02d", shots, level.get_time_hours(), level.get_time_minutes()))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function fire(on)
	if on then level.press_action(level.action_id("wpn_fire")) else level.release_action(level.action_id("wpn_fire")) end
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		local gun = db.actor:item_in_slot(3) and 3 or (db.actor:item_in_slot(2) and 2 or 0)
		say("[qa] weapon slot " .. gun .. ": " .. tostring(db.actor:item_in_slot(gun) and db.actor:item_in_slot(gun):section()))
		db.actor:activate_slot(gun)
		t0 = now stage = 1
	elseif stage == 1 and after(2500) then
		cmd("r__capture 1.2 60")
		fire(true)
		t0 = now stage = 2
	elseif stage == 2 and after(700) then
		fire(false)
		t0 = now stage = 3
	elseif stage == 3 and after(2000) then
		-- 2. the night: forward to 22:30 the way sleep does it, then let the clock run
		local h, m = level.get_time_hours(), level.get_time_minutes()
		local dh = (22 - h) % 24
		local dm = 30 - m
		if dm < 0 then dm = dm + 60 dh = dh - 1 end
		if dh < 0 then dh = dh + 24 end
		level.change_game_time(0, dh, dm)
		level.set_weather(level.get_weather(), true)
		db.actor:activate_slot(0)
		cmd("time_factor 600")
		t0 = now stage = 4
	elseif stage >= 4 and stage < 24 and after(2000) then
		shot()
		t0 = now stage = stage + 1
	elseif stage == 24 then
		cmd("time_factor 1")
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 25
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
