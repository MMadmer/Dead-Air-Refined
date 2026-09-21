-- Drives the engine's optimization benchmarks once the level is actually up, because the
-- benchmarks need a loaded collision model and level graph and user.ltx runs long before
-- either exists. Prints the marker the probe waits for, so a pass costs what the benches
-- cost and not a fixed ceiling.
local stage, t0, frames = 0, 0, 0

local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	-- Let the level settle: the first frames are still streaming geometry, and a benchmark
	-- that races the loader measures the loader.
	if frames < 120 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		printf("OPTBENCH: begin level=%s", tostring(level.name()))
		cmd("flush")
		t0 = now stage = 1

	elseif stage == 1 and after(1000) then
		cmd("dar_bench_cdb " .. tostring(qa_optbench_rays or 20000))
		cmd("flush")
		t0 = now stage = 2

	elseif stage == 2 and after(1000) then
		cmd("dar_bench_path " .. tostring(qa_optbench_paths or 200))
		cmd("flush")
		t0 = now stage = 3

	elseif stage == 3 and after(1000) then
		-- Frame sampling is opt-in. The probe runs the engine on a hidden desktop without
		-- -always_active, and an unfocused engine throttles its frame loop, so a frame-time
		-- number collected here would measure the throttle and not the game. Ask for it only
		-- from a run that can keep the window active.
		local frames = qa_optbench_frames or 0
		if frames > 0 then
			cmd("dar_bench_frame " .. tostring(frames))
			cmd("flush")
		else
			printf("~ [optbench] frame skipped")
		end
		printf("OPTBENCH: DONE")
		cmd("flush")
		stage = 4
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
