-- The grass under a sprint (docs/dead-air/dx12-features/mesh.md 9.3 "Sprint"): the actor is carried
-- along its heading at qa_run_speed (7 m/s) for qa_run_ms (10 s) over the level's AI nodes (a heading
-- that runs out of nodes turns by 90 degrees), then stands. "r__grass_stats" is logged every second
-- while running and every half second for 4 s after the stop: the decompression backlog must be 0
-- within 2 s after the stop and no part may go to the vertex path after the first 2 s. A capture
-- (r__capture) at the stop shows the grass around the new place.
-- Header globals: qa_run_speed, qa_run_ms, qa_run_cmds = { ... } (console commands after the load).
local c = get_console()
local stage, t0, frames, last_stats, heading, pos
local speed, duration

local function cmd(s) c:execute(s) end

local function stats(tag)
	printf("[qa] grass run " .. tag .. " t=" .. (time_global() - t0))
	cmd("r__grass_stats")
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()

	if stage == "wait" then
		printf("[qa] grass run level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " caps " .. c:get_string("r__caps"))
		-- The mesh path under the sprint (the switch's default may still be 0); qa_run_cmds may turn it off.
		cmd("r__grass_mesh 1")
		for _, s in ipairs(qa_run_cmds or {}) do cmd(s) end
		speed = qa_run_speed or 7
		duration = qa_run_ms or 10000
		heading = db.actor:direction():getH()
		pos = db.actor:position()
		stage, t0, last_stats = "settle", now, now
		stats("settle")
	elseif stage == "settle" then
		if now - t0 > 2000 then
			stage, t0, last_stats = "run", now, now
			stats("start")
		end
	elseif stage == "run" then
		local dt = math.min(device().time_delta, 100) / 1000
		local dir = vector():set(-math.sin(heading), 0, math.cos(heading))
		local next_pos = vector():set(pos.x + dir.x * speed * dt, pos.y, pos.z + dir.z * speed * dt)
		local vid = level.vertex_id(next_pos)
		if vid and vid < 4294967295 then
			local ground = level.vertex_position(vid)
			-- A node far off the straight line means the way is blocked: turn instead.
			if math.abs(ground.x - next_pos.x) + math.abs(ground.z - next_pos.z) < 1.5 then
				pos = vector():set(next_pos.x, ground.y, next_pos.z)
				db.actor:set_actor_position(pos)
			else
				heading = heading + math.pi / 2
			end
		else
			heading = heading + math.pi / 2
		end
		if now - last_stats >= 1000 then
			last_stats = now
			stats("running")
		end
		if now - t0 > duration then
			stage, t0, last_stats = "stop", now, now
			stats("stop")
			cmd("r__capture 1 2")
		end
	elseif stage == "stop" then
		if now - last_stats >= 500 then
			last_stats = now
			stats("stopped")
		end
		if now - t0 > 4000 then
			printf("[qa] grass run DONE")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = "end"
		end
	end
end

function on_game_start()
	stage, t0, frames = "wait", 0, 0
	RegisterScriptCallback("actor_on_update", on_update)
end
