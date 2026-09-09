-- Rain: what it looks like outdoors, and whether it still falls indoors. The cover test is the
-- headline fix here, so the indoor frame is the one that matters.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_storm", true)
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 6000 then
		cmd("qa_water_state")
		cmd("screenshot da_rain_0_open")
		cmd("qa_rain_shelter")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 2500 then
		cmd("screenshot da_rain_1_indoors")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1800 then
		cmd("screenshot da_rain_2_indoors_b")
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

