-- The water surface from three fixed pitches: down into it, a middling angle, and edge-on. The
-- last one is where every depth-driven term used to fall apart, so it is the one that matters.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		if qa_water_weather then level.set_weather(qa_water_weather, true) end
		cmd("qa_water_goto -0.95 0.10")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 4000 then
		cmd("qa_water_state")
		cmd("screenshot da_w_down")
		cmd("qa_water_goto -0.12")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 1800 then
		cmd("screenshot da_w_mid")
		cmd("qa_water_goto -0.12")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1800 then
		cmd("screenshot da_w_flat")
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 600 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 5
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end

