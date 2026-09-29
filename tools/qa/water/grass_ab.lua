-- Mesh-shader grass against the vertex path in one session (docs/dead-air/dx12-features/mesh.md 9.3).
-- After the load the wind is reseeded and frozen; then, as the header asks:
--   qa_grass_shots = true  lossless shots (r__screenshot_every 1 with -ss_tga), each announced as
--                          "[qa] grass shot <n> <label> ...": "off1", "off2" (r__grass_mesh 0), "on1"
--                          (r__grass_mesh 1), "off3" (0 again). off1-off3 bracket on1 in time, so they
--                          are the noise floor on1 is judged against:
--                          tools\qa\Compare-Frames.py off1 off2 on1 --ref off3;
--   qa_grass_ab = true     the in-frame A/B (r__dbg_grass_ab 1) at three headings (0, 120, 240 degrees
--                          from the save's): the engine draws the frame's grass through both paths
--                          over the same state and logs "[grass-ab] ..." lines a few frames later.
--   qa_grass_gpu = true    r__grass_mesh alternates 0/1 every 3 s, four times each way, under r__gpu_log 10;
--                          each phase is announced as "[qa] grass gpu phase <n> mesh=<0|1>", so the
--                          [gpu] lines between the markers are the in-session GPU cost of each path.
-- Every "r__grass_stats" also schedules the row check: "[grass-mesh] row check ... -> PASS|FAIL" a few
-- frames later compares the GPU's pool rows and row list with what the engine uploaded.
--   qa_grass_cmds = { ... } console commands after the load (qa_render_cmds too).
-- qa_tga = true is render_check's parity mode: game time stops and the weather clock is pinned on the
-- first frame, the header's weather is forced, the HUD weapon is hidden, eye adaptation is instant.
-- Shots are at least 1.2 s apart: the screenshot's file name has a one-second resolution.
local c = get_console()
local stage, t0, frames, shots, step, yaw0, heading, phase
local adaptation

-- label, r__grass_mesh, milliseconds before the shot
-- (qa_grass_shot_list replaces it: { { label, r__grass_mesh, ms }, ... })
local shot_list = qa_grass_shot_list or { { "off1", 0, 3000 }, { "off2", 0, 1200 }, { "on1", 1, 1500 }, { "off3", 0, 1200 } }
local headings = { 0, 120, 240 }

local function cmd(s) c:execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if qa_tga and frames == 1 then
		level.set_time_factor(0.0001)
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		cmd("hud_weapon 0")
		adaptation = c:get_float("r2_tonemap_adaptation")
		cmd("r2_tonemap_adaptation 10")
	end
	if qa_tga and frames == 2 and qa_water_weather then level.set_weather(qa_water_weather, true) end
	if frames < 120 then return end
	local now = time_global()
	local function go(next_stage) stage, t0 = next_stage, now end

	if stage == "wait" then
		printf("[qa] grass level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " caps " .. c:get_string("r__caps"))
		cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7))
		cmd("wind_freeze 1")
		-- The mesh path is what this scenario looks at (the switch's default may still be 0).
		cmd("r__grass_mesh 1")
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		for _, s in ipairs(qa_grass_cmds or {}) do cmd(s) end
		cmd("r__grass_stats")
		yaw0 = db.actor:direction():getH()
		if qa_grass_shots then
			step = 1
			cmd("r__grass_mesh " .. shot_list[1][2])
			go("shots")
		else
			go(qa_grass_gpu and "gpu" or "ab")
		end
	elseif stage == "shots" then
		local s = shot_list[step]
		if not s then
			cmd("r__grass_mesh 1")
			go(qa_grass_gpu and "gpu" or qa_grass_ab and "ab" or "done")
			return
		end
		if now - t0 > s[3] then
			shots = shots + 1
			printf("[qa] grass shot " .. shots .. " " .. s[1] .. " level=" .. level.name() .. " grass_mesh=" ..
				c:get_string("r__grass_mesh"))
			-- Only this frame's render takes the shot: the next update switches it off.
			cmd("r__screenshot_every 1")
			go("shot")
		end
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		step = step + 1
		local s = shot_list[step]
		if s then cmd("r__grass_mesh " .. s[2]) end
		go("shots")
	elseif stage == "gpu" then
		-- Eight phases of 3 s, the paths alternating; the first second of each is the switch settling.
		phase = (phase or 0) + 1
		if phase > 8 then
			cmd("r__gpu_log 0")
			cmd("r__grass_mesh 1")
			go(qa_grass_ab and "ab" or "done")
			return
		end
		cmd("r__grass_mesh " .. (phase % 2 == 0 and "1" or "0"))
		cmd("r__gpu_log 10")
		go("gpu_settle")
	elseif stage == "gpu_settle" and now - t0 > 1000 then
		printf("[qa] grass gpu phase " .. phase .. " mesh=" .. c:get_string("r__grass_mesh"))
		cmd("r__grass_stats")
		go("gpu_run")
	elseif stage == "gpu_run" and now - t0 > 2000 then
		printf("[qa] grass gpu phase end " .. phase)
		go("gpu")
	elseif stage == "ab" then
		-- Next heading: turn, let the slots refresh and the frame settle, then one A/B frame.
		heading = (heading or 0) + 1
		local deg = headings[heading]
		if not deg then
			go("done")
			return
		end
		db.actor:set_actor_direction(-(yaw0 + deg * math.pi / 180))
		printf("[qa] grass heading " .. deg .. " deg")
		go("ab_settle")
	elseif stage == "ab_settle" and now - t0 > 1500 then
		cmd("r__grass_stats")
		cmd("r__dbg_grass_ab 1")
		go("ab_wait")
	elseif stage == "ab_wait" and now - t0 > 1500 then
		go("ab")
	elseif stage == "done" then
		cmd("wind_freeze 0")
		cmd("hud_weapon 1")
		cmd("r2_tonemap_adaptation " .. tostring(adaptation or 1))
		printf("[qa] grass DONE level=" .. level.name() .. " shots=" .. shots)
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = "end"
	end
end

function on_game_start()
	stage, t0, frames, shots, heading, phase = "wait", 0, 0, 0, 0, 0
	RegisterScriptCallback("actor_on_update", on_update)
end
