-- An upscaler through display size changes: FSR quality, then the resolution down and back up,
-- one shot after each renderer reset (tools\qa\Run-WaterQa.ps1 -Scenario resize_check
-- [-ExtraArgs "-r5"]). The log carries the render and display sizes each step ended with.
local c = get_console()
local steps = {
	{ "r__dlss st_dlss_off", "r__fsr st_fsr_quality", "vid_restart" },
	{ "vid_mode 1280x720 (60Hz)", "vid_restart" },
	{ "vid_mode 1920x1080 (60Hz)", "vid_restart" },
	{ "r__fsr st_fsr_off", "vid_restart" },
}
local i, phase, t0, frames = 0, "next", 0, 0

local function cmd(s) c:execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 150 then return end
	local now = time_global()
	if phase == "next" then
		i = i + 1
		local s = steps[i]
		if not s then
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		for _, x in ipairs(s) do cmd(x) end
		phase, t0 = "shot", now
	elseif phase == "shot" and now - t0 > 4000 then
		printf("[qa] step %s vid_mode=%s fsr=%s", tostring(i), c:get_string("vid_mode"), c:get_string("r__fsr"))
		cmd("r__screenshot_every 1")
		phase, t0 = "stop", now
	elseif phase == "stop" and now - t0 > 1100 then
		cmd("r__screenshot_every 0")
		cmd("flush")
		phase, t0 = "next", now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
