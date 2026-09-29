-- GPU-driven vegetation in one session (docs/dead-air/dx12-features/gpudriven.md 9.3). After the load
-- the wind is reseeded and frozen, then lossless shots (r__screenshot_every 1 with -ss_tga), each
-- announced as "[qa] gd shot <n> <label> ...", in this order: "off1", "off2" (r__gpu_driven 0),
-- "on1" (r__gpu_driven <qa_gd_mode, 1>), "off3" (0 again). off1-off3 bracket on1 in time, so the three
-- are the noise floor on1 is judged against (tools\qa\Compare-Frames.py off1 off2 on1 --ref off3).
-- Header globals:
--   qa_gd_mode = 2        the "on" shots with the CPU cull and GPU draw (the G1/G2 path) instead of 1;
--   qa_gd_variants = true then "cpucull" (2) and "off5" (0): each against the same floor;
--   qa_gd_toggle = true   then flip r__gpu_driven every 30 frames for qa_gd_toggle_ms (6000), and
--                         shoot "on2" (switch on) and "off4" (switch off): both must equal the floor;
--   qa_gd_motion = true   then wind_freeze 0 and r__capture 3 s at 10 fps with the switch off, then on
--                         (the shadow sway run: "[qa] gd capture <label>" names each sequence);
--   qa_gd_winddbg = true  then wind_dbg 1 for 2 s with the switch on (the GPU oscillator's readback);
--   qa_gd_next = "<save>" then load that save with the switch on; the steps run again on its level
--                         (qa_gd_next_level = its level name tells the two sessions apart);
--   qa_gd_cmds = { ... }  console commands after the load (qa_render_cmds too).
-- qa_tga = true is render_check's parity mode: game time stops and the weather clock is pinned on the
-- first frame, the header's weather is forced, the HUD weapon is hidden, eye adaptation is instant.
-- Shots are at least 1.2 s apart: the screenshot's file name has a one-second resolution.
local c = get_console()
local stage, t0, frames, shots, flips, second, step
local adaptation, capture, phases

local function on_mode() return tostring(qa_gd_mode or 1) end

-- label, console commands, milliseconds before the shot
local function shots_first()
	return { { "off1", { "r__gpu_driven 0" }, 3000 }, { "off2", {}, 1200 },
		{ "on1", { "r__gpu_driven " .. on_mode() }, 1500 }, { "off3", { "r__gpu_driven 0" }, 1200 } }
end
local shots_variants = {
	{ "cpucull", { "r__gpu_driven 2" }, 1500 },
	{ "off5", { "r__gpu_driven 0" }, 1200 },
}
local function shots_toggle()
	return { { "on2", { "r__gpu_driven " .. on_mode() }, 1200 }, { "off4", { "r__gpu_driven 0" }, 1200 } }
end
-- label, switch, capture seconds
local captures = { { "motion-off", "0", 3 }, { "motion-on", "1", 3 } }

local function cmd(s) c:execute(s) end

-- The phases after the first shots, in order (the second level of qa_gd_next runs the first shots only).
local function plan()
	local p = {}
	if not second then
		if qa_gd_variants then p[#p + 1] = "variants" end
		if qa_gd_toggle then p[#p + 1] = "toggle" end
		if qa_gd_motion then p[#p + 1] = "motion" end
		if qa_gd_winddbg then p[#p + 1] = "winddbg" end
	end
	p[#p + 1] = "next"
	return p
end

local function start_shots(list, name)
	step = { list = list, list_name = name, i = 1 }
	for _, s in ipairs(list[1][2]) do cmd(s) end
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames == 1 then
		-- The session after qa_gd_next's load runs the steps on that level and ends the run.
		second = qa_gd_next_level ~= nil and level.name() == qa_gd_next_level
	end
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
		printf("[qa] gd level %s renderer %s caps %s", level.name(), c:get_string("renderer"), c:get_string("r__caps"))
		cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7))
		cmd("wind_freeze 1")
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		for _, s in ipairs(qa_gd_cmds or {}) do cmd(s) end
		cmd("r__gpu_log 30")
		phases = plan()
		start_shots(shots_first(), "first")
		go("shots")
	elseif stage == "shots" then
		local s = step.list[step.i]
		if not s then
			local nxt = table.remove(phases, 1)
			if nxt == "variants" then
				start_shots(shots_variants, "variants")
				go("shots")
			elseif nxt == "motion" then
				capture = 1
				stage, t0 = "motion", 0
			else
				go(nxt)
			end
			return
		end
		if now - t0 > s[3] then
			shots = shots + 1
			printf(string.format("[qa] gd shot %d %s level=%s gpu_driven=%s caps %s", shots, s[1], level.name(),
				c:get_string("r__gpu_driven"), c:get_string("r__caps")))
			-- Only this frame's render takes the shot: the next update switches it off.
			cmd("r__screenshot_every 1")
			go("shot")
		end
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		step.i = step.i + 1
		local s = step.list[step.i]
		if s then for _, x in ipairs(s[2]) do cmd(x) end end
		go("shots")
	elseif stage == "toggle" then
		-- 30 frames each way: the switch moves the nodes between their hierarchies and the registry.
		if frames % 30 == 0 then
			flips = flips + 1
			cmd("r__gpu_driven " .. ((flips % 2 == 1) and on_mode() or "0"))
		end
		if now - t0 > (qa_gd_toggle_ms or 6000) then
			printf(string.format("[qa] gd toggles %d", flips))
			start_shots(shots_toggle(), "toggle_shots")
			go("shots")
		end
	elseif stage == "motion" then
		-- Free wind, one capture per switch position: the crowns and their shadows sway in both.
		-- The capture that runs finishes first (3 s and a margin).
		if t0 > 0 and now - t0 < 4500 then return end
		local cap = captures[capture]
		if not cap then
			cmd("wind_freeze 1")
			go(table.remove(phases, 1))
			return
		end
		cmd("wind_freeze 0")
		cmd("r__gpu_driven " .. (cap[2] == "1" and on_mode() or "0"))
		printf(string.format("[qa] gd capture %s gpu_driven=%s", cap[1], c:get_string("r__gpu_driven")))
		cmd(string.format("r__capture %d 10", cap[3]))
		capture = capture + 1
		t0 = now
	elseif stage == "winddbg" then
		if not step.wind then
			step.wind = true
			cmd("r__gpu_driven 1")
			cmd("wind_freeze 0")
			cmd("wind_dbg 1")
			printf("[qa] gd wind_dbg on")
			t0 = now
			return
		end
		if now - t0 > 2000 then
			cmd("wind_dbg 0")
			cmd("wind_freeze 1")
			cmd("r__gpu_driven 0")
			printf("[qa] gd wind_dbg off")
			go(table.remove(phases, 1))
		end
	elseif stage == "next" then
		if qa_gd_next and not second then
			-- The next level builds its registry at its first frame with the switch on.
			cmd("r__gpu_driven " .. on_mode())
			printf("[qa] gd load %s", qa_gd_next)
			cmd("flush")
			cmd("load " .. qa_gd_next)
			go("loading")
			return
		end
		cmd("r__gpu_log 0")
		cmd("hud_weapon 1")
		cmd("r2_tonemap_adaptation " .. tostring(adaptation or 1))
		printf(string.format("[qa] gd DONE level=%s shots=%d", level.name(), shots))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		go("done")
	end
end

function on_game_start()
	-- Every game start (the save loaded by qa_gd_next too) runs the steps from the top.
	stage, t0, frames, shots, flips, second, capture = "wait", 0, 0, 0, 0, false, 1
	RegisterScriptCallback("actor_on_update", on_update)
end
