-- One spot, one heading, every ray traced effect on its own (docs/dead-air/dx12-features/dxr.md 9): the frame with
-- nothing traced, then each effect alone, then all of them, so a shot says what each one changes. The log has
-- "[qa] SHOT <config>" before each shot (the run's NN.jpg in that order) and the [gpu] lines.
--
--   tools\qa\Run-WaterQa.ps1 -Name ab-house -Scenario rtx_ab -Save rainhouse -KeepShaderCache -Seconds 300
--        -ExtraArgs '-r5 -always_active' -Globals 'qa_ab_keep_time = true'
--
-- Header globals: qa_ab_quality (st_rt_ultra), qa_ab_heading (degrees from the save's, 0), qa_ab_hands ("rifle",
-- "none"), qa_ab_hour / qa_ab_weather (13, af3_slight_clear_steady_3; qa_ab_keep_time = true keeps the save's),
-- qa_ab_torch = true (the head torch on), qa_ab_sets = { "off", "gi", ... } (a subset of the configs below),
-- qa_ab_debug = { 6, 7, 8 } (r__rt_debug views shot after the configs, with everything on), qa_ab_qualities = {
-- "st_rt_low", ... } (everything on at each quality, after the configs), qa_ab_measure = true (a frame-rate window after
-- every shot: "[qa] fps <config> <x>" with the [gpu] lines of r__gpu_log 60).
local c = get_console()
local function cmd(s) c:execute(s) end
local function act(name) return level.action_id(name) end
local function tap(name)
	level.press_action(act(name))
	level.release_action(act(name))
end
local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end
local function spawn(section)
	alife():create(section, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
end

local quality = qa_ab_quality or "st_rt_ultra"
local function set(refl, shadows, ao, gi)
	return { "r__rt_reflections " .. refl, "r__rt_shadows " .. shadows, "r__rtao " .. ao, "r__rt_gi " .. gi,
		"r__rt_quality " .. quality }
end
local all = {
	off = set(0, 0, 0, 0), shadows = set(0, 1, 0, 0), ao = set(0, 0, 1, 0), gi = set(0, 0, 0, 1),
	reflections = set(1, 0, 0, 0), on = set(1, 1, 1, 1),
}
local order = qa_ab_sets or { "off", "shadows", "ao", "gi", "reflections", "on" }
local configs = {}
for _, n in ipairs(order) do
	if all[n] then table.insert(configs, { name = n, cmds = all[n] }) end
end
for _, q in ipairs(qa_ab_qualities or {}) do
	table.insert(configs, { name = "on_" .. q, cmds = { "r__rt_reflections 1", "r__rt_shadows 1", "r__rtao 1", "r__rt_gi 1",
		"r__rt_quality " .. q } })
end
for _, d in ipairs(qa_ab_debug or {}) do
	local cmds = {}
	for _, x in ipairs(all.on) do table.insert(cmds, x) end
	table.insert(cmds, "r__rt_debug " .. d)
	table.insert(configs, { name = "debug" .. d, cmds = cmds, keep = true })
end

local ci, phase, t0, frames, armed, fps_frames = 1, "start", 0, 0, false, 0
local last_cmds

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	device():pause(false)
	if frames == 1 then
		level.set_time_factor(0.0001)
		if not qa_ab_keep_time then
			to_hour(qa_ab_hour or 13)
			level.set_weather(qa_ab_weather or "af3_slight_clear_steady_3", true)
			cmd("qa_climate_force clear_steady 0 20")
		end
		cmd("g_god on")
		if (qa_ab_hands or "rifle") == "rifle" then spawn("wpn_ak74") end
	end
	if frames < 150 then return end
	local now = time_global()

	if phase == "start" then
		if (qa_ab_hands or "rifle") == "rifle" then
			local w = db.actor:object("wpn_ak74")
			if w then db.actor:move_to_slot(w, 3) db.actor:activate_slot(3) end
		else
			db.actor:activate_slot(0)
		end
		if qa_ab_torch then
			local t = db.actor:object("device_torch")
			if t then
				t:enable_torch(true)
				t:enable_torch2(true)
				if itms_manager then itms_manager.Torch2 = true end
			end
			printf("[qa] torch %s", t and "on" or "missing")
		end
		db.actor:set_actor_direction(-(db.actor:direction():getH() + math.rad(qa_ab_heading or 0)))
		cmd("r__gpu_log 60")
		phase, t0 = "config", now
	elseif phase == "config" then
		local cfg = configs[ci]
		if not cfg then
			cmd("r__rt_debug 0")
			cmd("r__gpu_log 0")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		cmd("r__rt_debug 0")
		for _, x in ipairs(cfg.cmds) do cmd(x) end
		-- A debug view keeps the scene of the config before it; an effect toggle rebuilds the renderer's targets.
		if not cfg.keep then cmd("vid_restart") end
		phase, t0, armed = "settle", now, false
	elseif phase == "settle" and not armed and now - t0 > 1500 then
		-- The restart lowers the weapon: up again before the shot.
		armed = true
		if (qa_ab_hands or "rifle") == "rifle" then db.actor:activate_slot(3) end
	elseif phase == "settle" and now - t0 > 7000 then
		printf(string.format("[qa] SHOT %s", configs[ci].name))
		cmd("flush")
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		if qa_ab_measure then
			phase, t0, fps_frames = "measure", now, 0
		else
			ci, phase, t0 = ci + 1, "config", now
		end
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > 6000 then
			printf(string.format("[qa] fps %s %.1f", configs[ci].name, fps_frames * 1000 / (now - t0)))
			cmd("flush")
			ci, phase, t0 = ci + 1, "config", now
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
