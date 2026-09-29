-- Walks the anti-aliasing, upscaler and frame generation settings in one session: each step sets
-- its console values, restarts the renderer when it has to, waits for the frame to settle, takes
-- one shot and measures the frame rate (tools\qa\Run-WaterQa.ps1 -Scenario upscale_check
-- [-ExtraArgs "-r5 -always_active"]). The log carries "[qa] step <name> ..." per step; the shots
-- come out in step order, one per step. Frame generation steps only run where r__caps says it is
-- supported. qa_tga = true (with -ss_tga) is the parity mode of render_check: game time stops, the
-- header's weather is forced, the HUD weapon is hidden (qa_keep_hud keeps it) and eye adaptation turns
-- instant on the first frame; the wind is reseeded and frozen before the steps.
local c = get_console()
local r5 = c:get_string("renderer") == "renderer_r5"

local function caps(key)
	return string.find(";" .. c:get_string("r__caps") .. ";", ";" .. key .. "=1;", 1, true) ~= nil
end

local steps = {
	{ name = "smaa", cmds = { "r__aa st_aa_smaa", "r__fsr st_fsr_off", "r__dlss st_dlss_off", "r__framegen st_fg_off" }, restart = true },
	{ name = "taa", cmds = { "r__aa st_aa_taa" } },
	{ name = "vectors", cmds = { "r__motion_vectors 1" } },
	{ name = "vectors_off", cmds = { "r__motion_vectors 0" } },
	{ name = "fsr_quality", cmds = { "r__fsr st_fsr_quality" }, restart = true, need = "fsr" },
	{ name = "fsr_performance", cmds = { "r__fsr st_fsr_performance" }, restart = true, need = "fsr" },
	{ name = "fsr_fg", cmds = { "r__framegen st_fg_fsr" }, restart = true, need = "fsrfg" },
	{ name = "fsr_fg_off", cmds = { "r__framegen st_fg_off" }, restart = true, need = "fsrfg" },
	{ name = "dlss_quality", cmds = { "r__fsr st_fsr_off", "r__dlss st_dlss_quality" }, restart = true, need = "dlss" },
	{ name = "dlaa", cmds = { "r__dlss st_dlss_dlaa" }, restart = true, need = "dlss" },
	{ name = "dlss_fsr_fg", cmds = { "r__dlss st_dlss_quality", "r__framegen st_fg_fsr" }, restart = true, need = "fsrfg" },
	{ name = "dlss_fg", cmds = { "r__dlss st_dlss_quality", "r__framegen st_fg_dlss" }, restart = true, need = "dlssg" },
	{ name = "back", cmds = { "r__dlss st_dlss_off", "r__fsr st_fsr_off", "r__framegen st_fg_off", "r__aa st_aa_smaa" }, restart = true },
}

-- qa_only (header global, comma separated) keeps just the named steps, in their order here.
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
		-- The weather clock itself is pinned to the save's ten minutes: the level's clock still
		-- moved a fraction of a second more in one run than in another before it stopped.
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		-- qa_env_pin (h:mm, Invoke-Dx12Stand -Tga) holds the pin of the references: a slow load can
		-- carry the level clock across a ten-minute boundary before this frame.
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		if not qa_keep_hud then cmd("hud_weapon 0") end
		cmd("r2_tonemap_adaptation 10")
	end
	-- The forced weather picks its descriptors by the (now pinned) weather clock.
	if qa_tga and frames == 2 and qa_water_weather then level.set_weather(qa_water_weather, true) end
	if frames < 150 then return end
	local now = time_global()

	if phase == "start" then
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		if qa_tga then
			cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7))
			cmd("wind_freeze 1")
		end
		phase, t0 = "apply", now
	elseif phase == "apply" then
		local s = steps[i]
		if not s then
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		if s.need and not caps(s.need) then
			printf("[qa] step %s skipped (no %s)", s.name, s.need)
			i = i + 1
			return
		end
		for _, x in ipairs(s.cmds) do cmd(x) end
		if s.restart then cmd("vid_restart") end
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 4000 then
		-- Only this frame's render takes the shot: the next update switches it off.
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		phase, t0, fps_frames = "measure", now, 0
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > 5000 then
			printf(string.format("[qa] step %s aa=%s fsr=%s dlss=%s fg=%s FPS %.1f", steps[i].name, c:get_string("r__aa"),
				c:get_string("r__fsr"), c:get_string("r__dlss"), c:get_string("r__framegen"), fps_frames * 1000 / (now - t0)))
			cmd("flush")
			i, phase, t0 = i + 1, "apply", now
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
