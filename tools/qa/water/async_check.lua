-- Async compute in one session (docs/dead-air/dx12-features/async.md 9.3): the header's weather (a
-- storm gives the march its clouds and the ripple its rain drops), r__async_verify (its port and
-- schedule "[async] verify" lines land in the log), then per phase a capture and the frame rate:
-- "[qa] async <name> path=<r__async_path> setting=<r__async_compute> FPS <f> over <n> frames".
-- Header globals: qa_async_phases (a list of { name = ..., cmds = { ... } } replacing the default
-- off/on pair), qa_render_cmds (console commands run first), qa_fps_ms (10000), qa_async_verify
-- (false skips the verify), qa_verify_ms (8000: both verify stages wait up to 90 frames each for a
-- ripple step, so a slow session - GPU-based validation - needs more).
local c = get_console()

local phases = qa_async_phases or {
	{ name = "off", cmds = { "r__async_compute st_async_off" } },
	{ name = "on", cmds = { "r__async_compute st_async_on" } },
}
local measure_ms = qa_fps_ms or 10000
local verify_ms = qa_verify_ms or 8000

local i, phase, t0, frames, fps_frames = 1, "start", 0, 0, 0

local function cmd(s) c:execute(s) end

local function switches()
	return string.format("path=%s setting=%s", c:get_string("r__async_path"), c:get_string("r__async_compute"))
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 150 then return end
	local now = time_global()

	if phase == "start" then
		level.set_weather(qa_water_weather or "af3_bright_storm", true)
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		phase, t0 = "weather", now
	elseif phase == "weather" and now - t0 > 6000 then
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		printf("[qa] async %s", switches())
		if qa_async_verify ~= false then cmd("r__async_verify") end
		phase, t0 = "verify", now
	elseif phase == "verify" and now - t0 > (qa_async_verify ~= false and verify_ms or 0) then
		phase = "apply"
	elseif phase == "apply" then
		local p = phases[i]
		if not p then
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		for _, x in ipairs(p.cmds) do cmd(x) end
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 2000 then
		cmd("r__capture 1 2")
		phase, t0, fps_frames = "measure", now, 0
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > measure_ms then
			printf(string.format("[qa] async %s %s FPS %.1f over %d frames", phases[i].name, switches(),
				fps_frames * 1000 / (now - t0), fps_frames))
			cmd("flush")
			i, phase = i + 1, "apply"
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
