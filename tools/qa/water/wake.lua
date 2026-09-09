-- Wading wake: the actor walks through shin-deep water on the engine's own input, so the feet,
-- the step manager and the wake slots are the shipped path and not a stand-in. Frames
-- mid-stride (the bow wave ahead of the legs), a second after the feet stop (the rings they
-- leave) and three seconds on; the field and the slots are read back beside each.
local stage, t0, t1, frames = 0, 0, 0, 0
local fwd = nil
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
		cmd("qa_water_goto -0.85 0.35")   -- shin-deep water, fifty degrees down: from half a metre out
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3500 then
		cmd("qa_water_state")
		shot("da_wake_0_quiet")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 400 then
		fwd = level.action_id("forward")
		printf(string.format("DA_WATER_T %d walk", time_global()))
		t0 = now t1 = now stage = 3
	elseif stage == 3 then
		-- a movement key is a HOLD action in the actor's input: it has to be fed every frame
		level.hold_action(fwd)
		if now - t1 > 500 then
			cmd("qa_water_state")           -- the wake slots and the feet, as the engine feeds them
			t1 = now
		end
		if now - t0 > 2000 then
			shot("da_wake_1_stride")       -- mid-stride: the bow wave, the V behind
			cmd("r__water_ripple_stats")
			t0 = now stage = 4
		end
	elseif stage == 4 then
		level.hold_action(fwd)
		if now - t0 > 500 then
			printf(string.format("DA_WATER_T %d stop", time_global()))
			t0 = now stage = 5
		end
	elseif stage == 5 and now - t0 > 1000 then
		shot("da_wake_2_stop")             -- the troughs sprung back into rings
		cmd("r__water_ripple_stats")
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 3000 then
		shot("da_wake_3_late")             -- the rings metres out
		t0 = now stage = 7
	elseif stage == 7 and now - t0 > 5000 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
