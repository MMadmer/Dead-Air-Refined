-- Water seen from close above, looking down: the refracted tap near the bottom of the frame (the
-- bottom band turned into vertical streaks when the bend reached past the frame's edge). The actor
-- stands in the deepest water near the save's spot, looks down, and the steps switch one thing each:
-- the settings the run was started with, ray traced reflections off, the upscaler off. One shot per
-- step, in step order ("[qa] step <name> ..." in the log). The refraction itself (r2_soft_water) cannot
-- be a step: vid_restart keeps the compiled shaders, so its macro changes only with a new session.
--   tools\qa\dx12\Invoke-Dx12Stand.ps1 -Name x -Scenario refract_edge -Save fogqa -ExtraArgs '-r5 -always_active -ss_tga'
-- Header globals: qa_only = "user,dlss_off" keeps the named steps; qa_pitch (default -0.8 rad, down)
-- and qa_wade (default 0.6 m, the water the actor looks for first) frame the surface.
local c = get_console()

local steps = {
	{ name = "user" },
	{ name = "rt_off", cmds = { "r__rt_reflections 0" }, restart = true },
	{ name = "dlss_off", cmds = { "r__rt_reflections 1", "r__dlss st_dlss_off" }, restart = true },
}
local only = qa_only
if only then
	local keep = {}
	for _, s in ipairs(steps) do
		if string.find("," .. only .. ",", "," .. s.name .. ",", 1, true) then keep[#keep + 1] = s end
	end
	steps = keep
end

local i, phase, t0, frames = 1, "start", 0, 0
local function cmd(s) c:execute(s) end
local function flag(name)
	local ok, v = pcall(function() return c:get_bool(name) end)
	return ok and tostring(v) or "?"
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames == 1 then
		-- The frame held still between steps: game time, weather clock, wind, eye adaptation, no weapon.
		level.set_time_factor(0.0001)
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		cmd("hud_weapon 0")
		cmd("r2_tonemap_adaptation 10")
		cmd("wind_seed 1833.7")
		cmd("wind_freeze 1")
	end
	if frames < 150 then return end
	local now = time_global()
	if phase == "start" then
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		-- Shallow water first, then the deepest near it: a deeper column bends the bed further.
		cmd(string.format("qa_water_goto %.2f 0.25", qa_pitch or -0.8))
		cmd(string.format("qa_water_goto %.2f %.2f", qa_pitch or -0.8, qa_wade or 0.6))
		phase = "apply"
	elseif phase == "apply" then
		local s = steps[i]
		if not s then
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		for _, x in ipairs(s.cmds or {}) do cmd(x) end
		if s.restart then cmd("vid_restart") end
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 3500 then
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		local s = steps[i]
		printf(string.format("[qa] step %s rt_refl=%d dlss=%s aa=%s soft_water=%s", s and s.name or "?",
			c:get_integer("r__rt_reflections"), c:get_string("r__dlss"), c:get_string("r__aa"), flag("r2_soft_water")))
		cmd("flush")
		-- 1.2 s between shots: the screenshot's file name has a one-second resolution.
		phase, t0 = "gap", now
	elseif phase == "gap" and now - t0 > 1200 then
		i, phase = i + 1, "apply"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
