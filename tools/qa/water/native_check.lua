-- r__native_selftest in game (docs/dead-air/dx12-features/infra.md 3.7, 9.3; Invoke-Dx12Stand
-- -Scenario native_check). After the level settles it prints the renderer and r__caps, asks for the
-- self-test (the renderer runs it at the start of the next frame and logs "[qa] native selftest: ..."),
-- lets a few seconds of frames run on top of it (the frames after the test must render as before) and
-- ends with DA_WATER_PROBE_DONE. The R4 module answers "r__native_selftest: DirectX 12 only".
-- Header globals: qa_render_cmds (console commands run first), qa_native_after_ms (3000).
local stage, t0, frames, after_frames = 0, 0, 0, 0

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
		cmd("r__native_selftest")
		t0, stage = now, 1
	elseif stage == 1 then
		after_frames = after_frames + 1
		if now - t0 > (qa_native_after_ms or 3000) then
			printf(string.format("[qa] %d frames after the self-test", after_frames))
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 2
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
