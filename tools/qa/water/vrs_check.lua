-- Variable rate shading in one session (docs/dead-air/dx12-features/vrs.md 9.3): each step sets its
-- console values, lets the frame settle, takes one lossless shot (r__screenshot_every 1 with -ss_tga)
-- and measures for a few seconds with r__gpu_log and r__vrs_log on, printing
-- "[qa] vrs step <name> mode=<r__vrs> force=<r__vrs_force> ... FPS <f>". The shots come out in step
-- order.
-- Header globals: qa_vrs_set (a named step set below, "quality" by default), qa_vrs_steps (a list of
-- { name = ..., cmds = { ... } } replacing the set), qa_vrs_settle_ms (1500), qa_vrs_measure_ms (5000),
-- qa_render_cmds (console commands run first), qa_vrs_start_frames (150 frames after the load before
-- anything; fewer under -dx12gbv).
-- A step may also carry settle_ms, measure_ms, restart = true (vid_restart after its commands) and
-- turn = <frames>: the frame settles, then the actor turns by qa_vrs_turn_step radians (0.00873, 30
-- degrees a second at 60 FPS) per frame and the shot is taken on the turn's last frame - the same view
-- under the same motion in every turn step, whatever the frame rate (capture = "<r__capture args>"
-- records the turn as a sequence as well). Every step starts facing the direction the actor had when
-- the steps began. "$vid_mode" in a command is the video mode the session started with.
-- qa_tga = true is the parity mode of render_check: game time stops and the weather clock is pinned on
-- the first frame, the header's weather is forced, eye adaptation is instant, the wind is reseeded and
-- frozen; the HUD weapon stays unless qa_hide_hud is set (then only the dbg step shows it: the weapon
-- must stay untinted in the overlay).
local c = get_console()

local hud_on = qa_hide_hud and "hud_weapon 1" or nil
local hud_off = qa_hide_hud and "hud_weapon 0" or nil
local turn_frames = qa_vrs_turn_frames or 45

local sets = {
	-- The quality table (9.3) and the motion check: cons/aggr/force2 against ref0/ref1, the turns
	-- against turn0/turn0b, the overlay last.
	quality = {
		{ name = "ref0", cmds = { "r__vrs 0", "r__vrs_force 0", "r__vrs_debug 0" } },
		{ name = "ref1", cmds = {} },
		{ name = "cons", cmds = { "r__vrs 1" } },
		{ name = "aggr", cmds = { "r__vrs 2" } },
		{ name = "force2", cmds = { "r__vrs 1", "r__vrs_force 1" } },
		{ name = "turn0", cmds = { "r__vrs 0", "r__vrs_force 0" }, turn = turn_frames },
		{ name = "turn0b", cmds = {}, turn = turn_frames },
		{ name = "turn1", cmds = { "r__vrs 1" }, turn = turn_frames },
		{ name = "turn2", cmds = { "r__vrs 2" }, turn = turn_frames },
		{ name = "dbg", cmds = { "r__vrs 1", "r__vrs_debug 1", hud_on } },
		{ name = "dbga", cmds = { "r__vrs 2" } },
		{ name = "back", cmds = { "r__vrs_debug 0", "r__vrs 0", hud_off } },
	},
	-- The upscalers (9.3): DLSS quality and FSR performance, each against its own two references.
	-- Aggressive runs as conservative under an upscale (the log says mode 1).
	upscale = {
		{ name = "dlss0", cmds = { "r__vrs 0", "r__vrs_force 0", "r__vrs_debug 0", "r__dlss st_dlss_quality" }, restart = true, settle_ms = 3500 },
		{ name = "dlss0b", cmds = {} },
		{ name = "dlss1", cmds = { "r__vrs 1" } },
		{ name = "dlss2", cmds = { "r__vrs 2" } },
		{ name = "fsr0", cmds = { "r__vrs 0", "r__dlss st_dlss_off", "r__fsr st_fsr_performance" }, restart = true, settle_ms = 3500 },
		{ name = "fsr0b", cmds = {} },
		{ name = "fsr1", cmds = { "r__vrs 1" } },
		{ name = "fsr2", cmds = { "r__vrs 2" } },
		{ name = "back", cmds = { "r__vrs 0", "r__fsr st_fsr_off" }, restart = true, settle_ms = 3500 },
	},
	-- Robustness (9.3): forced rates, a display size change there and back, an upscaler switched on
	-- and off; after every restart the log must say the mode again and show coarse tiles.
	robust = {
		{ name = "cons", cmds = { "r__vrs 1", "r__vrs_force 0", "r__vrs_debug 0" } },
		{ name = "aggr", cmds = { "r__vrs 2" } },
		{ name = "force4", cmds = { "r__vrs_force 2" } },
		{ name = "res720", cmds = { "r__vrs_force 0", "r__vrs 1", "vid_mode 1280x720 (60Hz)" }, restart = true, settle_ms = 4000 },
		{ name = "resback", cmds = { "vid_mode $vid_mode" }, restart = true, settle_ms = 4000 },
		{ name = "fsrq", cmds = { "r__fsr st_fsr_quality" }, restart = true, settle_ms = 4000 },
		{ name = "fsrq2", cmds = { "r__vrs 2" } },
		{ name = "native", cmds = { "r__vrs 1", "r__fsr st_fsr_off" }, restart = true, settle_ms = 4000 },
		{ name = "dbg", cmds = { "r__vrs_debug 1", hud_on } },
		{ name = "back", cmds = { "r__vrs_debug 0", "r__vrs 0", hud_off } },
	},
	-- The preset ladder: r__vrs back to the preset, the log names each preset's mode.
	preset = {
		{ name = "high", cmds = { "r__vrs -1", "r__vrs_force 0", "_preset High" }, settle_ms = 3000 },
		{ name = "low", cmds = { "_preset Low" }, settle_ms = 3000 },
		{ name = "extreme", cmds = { "_preset Extreme" }, settle_ms = 3000 },
	},
}

local steps = qa_vrs_steps or sets[qa_vrs_set or "quality"]
local settle_ms = qa_vrs_settle_ms or 1500
local measure_ms = qa_vrs_measure_ms or 5000
local turn_step = qa_vrs_turn_step or 0.00873

local i, phase, t0, frames, fps_frames = 1, "start", 0, 0, 0
local yaw0, yaw, turned = 0, 0, 0
local vid_mode0 = ""

local function cmd(s) c:execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if qa_tga and frames == 1 then
		level.set_time_factor(0.0001)
		local _, _, _, h, mi = game.get_game_time():get(0, 0, 0, 0, 0, 0, 0)
		cmd("env_time_pin " .. (qa_env_pin or string.format("%d:%02d:00", h, mi - mi % 10)))
		if qa_hide_hud then cmd("hud_weapon 0") end
		cmd("r2_tonemap_adaptation 10")
	end
	if qa_tga and frames == 2 and qa_water_weather then level.set_weather(qa_water_weather, true) end
	if frames < (qa_vrs_start_frames or 150) then return end
	local now = time_global()

	if phase == "start" then
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		if not steps then
			printf("[qa] vrs: no step set %s", tostring(qa_vrs_set))
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		if qa_tga then
			cmd("wind_seed " .. tostring(qa_wind_seed or 1833.7))
			cmd("wind_freeze 1")
		end
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		cmd("r__gpu_log 30")
		cmd("r__vrs_log 30")
		yaw0 = db.actor:direction():getH()
		vid_mode0 = c:get_string("vid_mode")
		phase, t0 = "apply", now
	elseif phase == "apply" then
		local s = steps[i]
		if not s then
			cmd("r__gpu_log 0")
			cmd("r__vrs_log 0")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		printf("[qa] vrs step %s begin", s.name)
		for _, x in ipairs(s.cmds) do cmd((string.gsub(x, "%$vid_mode", vid_mode0))) end
		if s.restart then cmd("vid_restart") end
		yaw = yaw0
		db.actor:set_actor_direction(-yaw)
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > (steps[i].settle_ms or settle_ms) then
		local s = steps[i]
		if s.turn then
			if s.capture then cmd("r__capture " .. s.capture) end
			phase, turned = "turn", 0
		else
			-- Only this frame's render takes the shot: the next update switches it off.
			cmd("r__screenshot_every 1")
			phase, t0 = "shot", now
		end
	elseif phase == "turn" then
		turned = turned + 1
		yaw = yaw + turn_step
		db.actor:set_actor_direction(-yaw)
		if turned == steps[i].turn then
			cmd("r__screenshot_every 1")
		elseif turned > steps[i].turn then
			cmd("r__screenshot_every 0")
			printf(string.format("[qa] vrs step %s turned %d frames", steps[i].name, turned - 1))
			phase, t0, fps_frames = "measure", now, 0
		end
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		phase, t0, fps_frames = "measure", now, 0
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > (steps[i].measure_ms or measure_ms) then
			printf(string.format("[qa] vrs step %s mode=%s force=%s passes=%s threshold=%s edge=%s vid_mode=%s FPS %.1f",
				steps[i].name, c:get_string("r__vrs"), c:get_string("r__vrs_force"), c:get_string("r__vrs_passes"),
				c:get_string("r__vrs_threshold"), c:get_string("r__vrs_edge"), c:get_string("vid_mode"),
				fps_frames * 1000 / (now - t0)))
			cmd("flush")
			i, phase, t0 = i + 1, "apply", now
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
