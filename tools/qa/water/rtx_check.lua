-- Walks the ray tracing settings of the RTX category in one session (docs/dead-air/dx12-features/dxr.md 9):
-- each step sets its console values, restarts the renderer when the effects change (the rows are
-- depend="vid"), waits for the frame to settle, takes its shots and measures the frame rate
-- (tools\qa\Run-WaterQa.ps1 -Scenario rtx_check -ExtraArgs "-r5 -always_active"). The log carries
-- "[qa] step <name> rt=... fps <x> caps <r__caps>" per step and the [gpu] lines of r__gpu_log
-- (rt_scene, rt_sun, rt_ao, rt_refl). RT steps are skipped where r__caps says rt=0 (DirectX 11, no
-- DXR 1.1, -dx12_no_rt), and the DLSS step where there is no DLSS.
--
-- Header globals:
--   qa_only = "rt_off,shadows"    keeps just the named steps, in their order here;
--   qa_tga = true                 parity mode as render_check (game time and weather pinned, HUD weapon
--                                 hidden, wind reseeded and frozen);
--   qa_sway = true                the rtx-foliage sway check: the wind keeps blowing and the rt_off and
--                                 shadows steps take a second shot 1.2 s after the first;
--   qa_debug = true               adds the r__rt_debug views (sun mask, occlusion, reflections, albedo).
local c = get_console()

local function caps(key)
	return string.find(";" .. c:get_string("r__caps") .. ";", ";" .. key .. "=1;", 1, true) ~= nil
end

local rt_all_off = { "r__rt_reflections 0", "r__rt_shadows 0", "r__rtao 0" }
local function with(list, extra)
	local out = {}
	for _, x in ipairs(list) do out[#out + 1] = x end
	for _, x in ipairs(extra) do out[#out + 1] = x end
	return out
end

local steps = {
	{ name = "rt_off", cmds = with(rt_all_off, { "r__rt_quality st_rt_medium" }), restart = true, sway = true },
	{ name = "rtao", cmds = { "r__rtao 1" }, restart = true, need = "rt" },
	{ name = "shadows", cmds = { "r__rtao 0", "r__rt_shadows 1" }, restart = true, need = "rt", sway = true },
	{ name = "refl", cmds = { "r__rt_shadows 0", "r__rt_reflections 1" }, restart = true, need = "rt" },
	{ name = "all", cmds = { "r__rt_reflections 1", "r__rt_shadows 1", "r__rtao 1" }, restart = true, need = "rt" },
	{ name = "all_low", cmds = { "r__rt_quality st_rt_low" }, restart = true, need = "rt" },
	{ name = "all_high", cmds = { "r__rt_quality st_rt_high" }, restart = true, need = "rt" },
	{ name = "all_dlss_q", cmds = { "r__rt_quality st_rt_medium", "r__dlss st_dlss_quality" }, restart = true, need = "rt", also = "dlss" },
	{ name = "back", cmds = with(rt_all_off, { "r__dlss st_dlss_off", "r__rt_quality st_rt_medium", "r__rt_debug 0" }), restart = true },
}
if qa_debug then
	local debug_steps = {
		{ name = "debug_sun", cmds = { "r__rt_reflections 1", "r__rt_shadows 1", "r__rtao 1", "r__rt_debug 1" }, restart = true, need = "rt" },
		{ name = "debug_ao", cmds = { "r__rt_debug 2" }, need = "rt" },
		{ name = "debug_refl", cmds = { "r__rt_debug 3" }, need = "rt" },
		{ name = "debug_albedo", cmds = { "r__rt_debug 4" }, need = "rt" },
	}
	-- Before "back", which switches everything off again.
	local back = table.remove(steps)
	for _, s in ipairs(debug_steps) do steps[#steps + 1] = s end
	steps[#steps + 1] = back
end

local only = qa_only
if only then
	local keep = {}
	for _, s in ipairs(steps) do
		if string.find("," .. only .. ",", "," .. s.name .. ",", 1, true) then keep[#keep + 1] = s end
	end
	steps = keep
end

local i, phase, t0, frames, fps_frames = 1, "start", 0, 0, 0

local function cmd(s) c:execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if qa_tga and frames == 1 then
		level.set_time_factor(0.0001)
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		if not qa_keep_hud then cmd("hud_weapon 0") end
		cmd("r2_tonemap_adaptation 10")
	end
	if qa_tga and frames == 2 and qa_water_weather then level.set_weather(qa_water_weather, true) end
	if frames < 150 then return end
	local now = time_global()

	if phase == "start" then
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		cmd("r__gpu_log 60")
		if qa_tga and not qa_sway then
			cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7))
			cmd("wind_freeze 1")
		end
		phase, t0 = "apply", now
	elseif phase == "apply" then
		local s = steps[i]
		if not s then
			cmd("r__gpu_log 0")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		if (s.need and not caps(s.need)) or (s.also and not caps(s.also)) then
			printf("[qa] step %s skipped (no %s)", s.name, (s.need and not caps(s.need)) and s.need or s.also)
			i = i + 1
			return
		end
		for _, x in ipairs(s.cmds) do cmd(x) end
		if s.restart then cmd("vid_restart") end
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 3000 then
		-- Only this frame's render takes the shot: the next update switches it off.
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		if qa_sway and steps[i].sway then
			phase, t0 = "sway", now
		else
			phase, t0, fps_frames = "measure", now, 0
		end
	elseif phase == "sway" and now - t0 > 1200 then
		-- The second shot of the sway check: plant shadows must have moved, the static ones not. At least
		-- 1.2 s after the first: the screenshot's file name has a one-second resolution.
		cmd("r__screenshot_every 1")
		phase, t0 = "sway_shot", now
	elseif phase == "sway_shot" then
		cmd("r__screenshot_every 0")
		phase, t0, fps_frames = "measure", now, 0
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > 6000 then
			printf(string.format("[qa] step %s rt=%d,%d,%d quality=%s debug=%s dlss=%s FPS %.1f caps %s", steps[i].name,
				c:get_integer("r__rt_reflections"), c:get_integer("r__rt_shadows"), c:get_integer("r__rtao"),
				c:get_token("r__rt_quality"), tostring(c:get_integer("r__rt_debug")), c:get_string("r__dlss"),
				fps_frames * 1000 / (now - t0), c:get_string("r__caps")))
			cmd("flush")
			i, phase, t0 = i + 1, "apply", now
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
