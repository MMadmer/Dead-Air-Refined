-- Caustics: the sun on the bed through a wind-ruffled surface. A sunny cycle is a calm one by
-- its wind profile, so the ceiling is pinned to a fresh breeze instead (wind_force), and the
-- frames wait out the sea's own low-pass before they are taken.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end
local function shot(name)
	printf(string.format("DA_WATER_T %d %s", time_global(), name))
	cmd("screenshot " .. name)
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		if qa_water_weather then level.set_weather(qa_water_weather, true) end
		cmd("wind_force 0.40")
		cmd("qa_water_goto -0.95 0.10")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 12000 then
		cmd("qa_water_state")
		shot("da_cau_0_down")             -- the bed at the feet, straight down
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 1500 then
		cmd("qa_water_goto -0.45 0.10")   -- same spot, the bed a few metres out
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 3000 then
		shot("da_cau_1_out")
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 5000 then
		cmd("wind_force -1")
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 5
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
