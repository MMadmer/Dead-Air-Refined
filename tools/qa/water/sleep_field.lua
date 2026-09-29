-- Field exposure in the sleep forecast. A radiation field for three seconds (strong enough to get
-- through the stand's suit), the 1 h forecast right after it and again once the field has been
-- gone for twelve seconds: the difference is what the field alone would do over the night if the
-- player lay down inside it. Then a psy field, lighter and shorter.
local stage, frames, t0, last = 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end
local function say(s) printf("SLEEPF %s", s) cmd("flush") end
local function state(tag)
	local a = db.actor
	say(string.format("%s health %.4f rad %.4f psy %.4f", tag, a.health, a.radiation, a.psy_health))
end
local function forecast(tag, hours)
	local h, cause = dead_air_x64_sleep.forecast(hours)
	say(string.format("%s forecast %d h -> min health %.4f cause %d", tag, hours, h, cause))
end
local function field(kind, power)
	local h = hit()
	h.type = kind
	h.power = power
	h.impulse = 0
	h.direction = vector():set(0, 0, 1)
	h.draftsman = db.actor
	db.actor:hit(h)
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()
	if stage == 0 then
		db.actor.radiation = -5
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 1500 then
		state("before")
		forecast("before", 1)
		t0 = now stage = 2
	elseif stage == 2 then
		if now - t0 < 3000 then
			field(hit.radiation, 0.2)
		else
			state("rad field")
			forecast("rad field", 1)
			t0 = now stage = 3
		end
	elseif stage == 3 and now - t0 > 12000 then
		state("rad gone")
		forecast("rad gone", 1)
		db.actor.radiation = -5
		t0 = now stage = 4
	elseif stage == 4 then
		-- A hit every 100 ms for 1.5 s: one per frame kills the stand's actor inside the window.
		if now - t0 < 1500 then
			if now - last >= 100 then
				last = now
				field(hit.telepatic, 0.01)
			end
		else
			state("psy field")
			forecast("psy field", 1)
			t0 = now stage = 5
		end
	elseif stage == 5 and now - t0 > 12000 then
		state("psy gone")
		forecast("psy gone", 1)
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 6
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
