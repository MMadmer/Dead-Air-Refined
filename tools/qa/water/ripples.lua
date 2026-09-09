-- Interaction: rings from impacts and the wake a walking actor leaves. Shot at a grazing angle,
-- which is where a ripple is actually visible.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function shot(name)
	-- the clock goes into the log beside the frame, so a ring's radius can be read against
	-- the time it had to run - the screenshot files themselves are stamped when the encoder
	-- finishes, seconds later on the rig
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
		cmd("qa_water_goto -0.95 0.10")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3500 then
		cmd("qa_water_state")
		shot("da_rip_0_quiet")            -- before anything touches it
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 400 then
		cmd("qa_water_ring 1.6")
		printf(string.format("DA_WATER_T %d ring1", time_global()))
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 250 then
		cmd("qa_water_ring 2.4")
		printf(string.format("DA_WATER_T %d ring2", time_global()))
		shot("da_rip_1_rings")            -- two fronts, one behind the other
		cmd("r__water_ripple_stats")
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 2000 then
		shot("da_rip_2_spread")           -- the same fronts, further out
		cmd("r__water_ripple_stats")
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 1000 then
		cmd("qa_water_goto -0.45 0.10")   -- same spot, looking further out
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 5000 then
		shot("da_rip_3_late")             -- ten seconds on: still there, metres out
		t0 = now stage = 7
	elseif stage == 7 and now - t0 > 5000 then
		-- the encoder needs its seconds before the run is cut
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end

