-- Interaction: rings from impacts and the wake a walking actor leaves. Shot at a grazing angle,
-- which is where a ripple is actually visible.
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
	elseif stage == 1 and now - t0 > 3500 then
		cmd("qa_water_state")
		cmd("screenshot da_rip_0_quiet")   -- before anything touches it
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 400 then
		cmd("qa_water_ring 1.6")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 250 then
		cmd("qa_water_ring 2.4")
		cmd("screenshot da_rip_1_rings")   -- two fronts, one behind the other
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 700 then
		cmd("screenshot da_rip_2_spread")  -- the same fronts, further out
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 1800 then
		cmd("screenshot da_rip_3_decay")   -- and dying, not looping
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 600 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 7
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end

