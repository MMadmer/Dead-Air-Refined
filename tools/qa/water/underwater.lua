-- Under the surface: the medium itself, the ceiling seen from below (Snell's window), and the bed
-- under sun. Nothing in the game puts the camera under water on purpose, so this leans on
-- qa_water_dive.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		if qa_water_weather then level.set_weather(qa_water_weather, true) end
		cmd("qa_water_goto -0.12")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3500 then
		cmd("screenshot da_uw_0_above")   -- the surface from above, for the pair
		cmd("qa_water_dive 0.8")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 2500 then
		cmd("qa_water_state")
		cmd("screenshot da_uw_1_under")   -- the medium: absorption, scatter, warp
		cmd("qa_water_goto -0.95 0.10")          -- straight down at the bed: caustics
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1500 then
		cmd("screenshot da_uw_2_bed")
		cmd("qa_water_dive 1.4")
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 2000 then
		cmd("screenshot da_uw_3_deep")
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 800 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 6
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end

