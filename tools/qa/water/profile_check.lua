-- The game thread's time shares under a renderer setting (tools\qa\dx12\Invoke-Dx12Stand.ps1 -Scenario
-- profile_check): after the load, qa_render_cmds, a 3 s settle, then the engine's sampling profiler
-- (dar_profile_start, xrGame da_profiler.cpp) over qa_profile_ms (10 s) with the frame rate over the same
-- window, the self/inclusive top list and, per entry of qa_profile_callers, the share of the thread's
-- samples that contain it ("appears in N of M sample(s) - X% of the thread"). A render-thread A/B is two
-- runs with different -PreCommands; the shares are comparable because both runs sample the same frames
-- of the same view.
local stage, t0, frames, frames0 = 0, 0, 0, 0

local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()

	if stage == 0 then
		local c = get_console()
		printf("[qa] renderer %s", tostring(c:get_string("renderer")))
		printf("[qa] caps %s", tostring(c:get_string("r__caps")))
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3000 then
		cmd("dar_profile_start " .. tostring(qa_profile_hz or 1000))
		frames0, t0 = frames, now
		stage = 2
	elseif stage == 2 and now - t0 > (qa_profile_ms or 10000) then
		printf(string.format("[qa] FPS %.1f over %d frames", (frames - frames0) * 1000 / (now - t0), frames - frames0))
		cmd("dar_profile_stop " .. tostring(qa_profile_top or 30))
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1000 then
		for _, needle in ipairs(qa_profile_callers or {}) do cmd("dar_profile_callers " .. needle) end
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 4
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
