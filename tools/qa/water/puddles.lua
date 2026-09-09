-- Puddles and wet ground after a while of rain: where the water sits, how the ground darkens, and
-- whether the ripples on it read as rain rather than as wind chop.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_rain", true)
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3000 then
		cmd("screenshot da_pud_0_dry")     -- the accumulator has barely started
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 40000 then
		cmd("qa_water_state")
		cmd("screenshot da_pud_1_wet")     -- fully soaked: the fill map should be visible
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1500 then
		cmd("screenshot da_pud_2_wet_b")
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

