-- One look at the renderer: which API runs, what the adapter supports, frames through r__capture
-- and the frame rate over a few seconds (tools\qa\Run-WaterQa.ps1 -Scenario render_check
-- [-ExtraArgs "-r5"]). A wrapper scenario may define qa_render_cmds (console commands run first).
-- qa_tga = true (with -ss_tga) is the parity mode of PLAN.md 0.6. On the first frame game time
-- stops, the weather clock is pinned to the save's ten minutes, the header's weather is forced, the HUD weapon is hidden (its idle motion never repeats;
-- qa_keep_hud = true keeps it) and eye adaptation turns instant; the wind service restarts from a
-- pinned seed (qa_wind_seed) so a following wind_freeze holds the same trees, grass and cloud deck
-- in every run; one frame of r__screenshot_every 1 gives a lossless shot before the capture starts.
local stage, t0, frames = 0, 0, 0
local fps_frames = 0
local adaptation

local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	-- Parity mode: game time stops on the level's first frame, so every run shoots the weather at
	-- the save's own minute (the descriptors no longer blend while the scenario waits).
	if qa_tga and frames == 1 then
		level.set_time_factor(0.0001)
		-- The weather clock itself is pinned to the save's ten minutes: the level's clock still
		-- moved a fraction of a second more in one run than in another before it stopped.
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		-- qa_env_pin (h:mm, Invoke-Dx12Stand -Tga) holds the pin of the references: a slow load can
		-- carry the level clock across a ten-minute boundary before this frame.
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		if not qa_keep_hud then cmd("hud_weapon 0") end
		adaptation = get_console():get_float("r2_tonemap_adaptation")
		cmd("r2_tonemap_adaptation 10")
	end
	-- The forced weather picks its descriptors by the (now pinned) weather clock.
	if qa_tga and frames == 2 and qa_water_weather then level.set_weather(qa_water_weather, true) end
	if frames < 120 then return end
	local now = time_global()

	if stage == 0 then
		local c = get_console()
		printf("[qa] renderer %s", tostring(c:get_string("renderer")))
		printf("[qa] caps %s", tostring(c:get_string("r__caps")))
		printf("[qa] aa %s fsr %s dlss %s fg %s", tostring(c:get_string("r__aa")), tostring(c:get_string("r__fsr")),
			tostring(c:get_string("r__dlss")), tostring(c:get_string("r__framegen")))
		if qa_tga then cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7)) end
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 3000 then
		if qa_tga then
			-- Where the camera stood: the first suspect when two parity runs disagree near the view.
			local p, d = device().cam_pos, device().cam_dir
			printf(string.format("[qa] camera %.5f %.5f %.5f dir %.6f %.6f %.6f", p.x, p.y, p.z, d.x, d.y, d.z))
			local y, mo, dd, h, mi, sec, ms = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
			printf(string.format("[qa] game time %d.%d.%d %d:%02d:%02d.%03d", y, mo, dd, h, mi, sec, ms))
			-- Only this frame's render takes the shot: the next update switches it off.
			cmd("r__screenshot_every 1")
			stage = 5
		else
			cmd("r__capture 1 2")
			t0 = now stage = 2
		end
	elseif stage == 5 then
		cmd("r__screenshot_every 0")
		if not qa_keep_hud then cmd("hud_weapon 1") end
		cmd("r2_tonemap_adaptation " .. tostring(adaptation or 1))
		cmd("r__capture 1 2")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 2000 then
		fps_frames, t0 = 0, now
		stage = 3
	elseif stage == 3 then
		fps_frames = fps_frames + 1
		if now - t0 > (qa_fps_ms or 12000) then
			printf(string.format("[qa] FPS %.1f over %d frames", fps_frames * 1000 / (now - t0), fps_frames))
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 4
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
