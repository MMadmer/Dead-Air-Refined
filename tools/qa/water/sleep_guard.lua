-- A sleep that would kill is refused, and one that would not is slept to the health it was
-- forecast. On the stand's save (7 % radiation, radioactive artefacts carried):
--   1. the 4 h forecast is lethal, cause radiation, and the player's 4 h attempt - the dialog's own
--      button method - leaves the clock where it was;
--   2. three seconds of a radiation field turn the 1 h forecast lethal from the exposure alone;
--   3. radiation cleared and the field long gone, the 1 h forecast survives and a real 1 h sleep
--      (sleep_forced, the same time jump) wakes with the health the forecast named.
local stage, frames, t0 = 0, 0, 0
local fc = nil
local function cmd(s) get_console():execute(s) end
local function say(s) printf("SLEEPG %s", s) cmd("flush") end
local function state(tag)
	local a = db.actor
	say(string.format("%s alive %s health %.4f rad %.4f sat %.3f psy %.3f bleed %.4f time %d:%02d", tag,
		tostring(a:alive()), a.health, a.radiation, a.satiety, a.psy_health, a.bleeding,
		level.get_time_hours(), level.get_time_minutes()))
end
local function forecast(tag, hours)
	local h, cause = dead_air_x64_sleep.forecast(hours)
	say(string.format("%s forecast %d h -> min health %.4f cause %d", tag, hours, h, cause))
	return h
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		state("start")
		forecast("start", 4)
		-- The dialog stand-in: only what OnButtonSleep reads.
		local fake = {
			time_track = { GetIValue = function() return 4 end },
			IsShown = function() return false end,
			HideDialog = function() end,
		}
		t0 = now
		fc = level.get_time_minutes() + level.get_time_hours() * 60
		ui_sleep_dialog.sleep_dialog.OnButtonSleep(fake)
		stage = 1
	elseif stage == 1 and now - t0 > 2500 then
		local m = level.get_time_minutes() + level.get_time_hours() * 60
		say(string.format("attempt: clock %s, sleeping info %s", (m == fc or m == fc + 1) and "unchanged" or "MOVED",
			tostring(db.actor:has_info("actor_is_sleeping"))))
		t0 = now stage = 2
	elseif stage == 2 then
		if now - t0 < 3000 then
			local h = hit()
			h.type = hit.radiation
			h.power = 0.001
			h.impulse = 0
			h.direction = vector():set(0, 0, 1)
			h.draftsman = db.actor
			db.actor:hit(h)
		else
			forecast("field", 1)
			t0 = now stage = 3
		end
	elseif stage == 3 and now - t0 > 12000 then
		db.actor.radiation = -5
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 1500 then
		state("clean")
		fc = forecast("clean", 1)
		ui_sleep_dialog.sleep_forced(1)
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 25000 then
		state("woke")
		say(string.format("forecast said %.4f", fc or -1))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 6
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
