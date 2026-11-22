-- A look at the ray traced effects around one save's spot (docs/dead-air/dx12-features/dxr.md 9): with every RTX
-- effect off, then on (sun shadows, global illumination, reflections, occlusion) at the quality given, the player turns
-- through four headings and is shot at each - a rifle drawn at the first two, the rifle and a detector at the third,
-- empty hands at the fourth - after the denoisers have settled. Each config ends with a frame-rate window; the log has
-- "[qa] SHOT <config>_<view>" before each shot (the run's NN.jpg in that order), "[qa] fps <config> <x>" and the
-- [gpu] lines (rt_scene, rt_sun, rt_gi, rt_refl, rt_gloss, rt_ao, rt_lamps).
--
--   tools\qa\Run-WaterQa.ps1 -Name tour-meadow -Scenario rtx_tour -Save qa_meadow -KeepShaderCache -Seconds 400
--        -ExtraArgs '-r5 -always_active' -Globals 'qa_tour_quality = "st_rt_ultra"'
--
-- Header globals: qa_tour_quality (st_rt_ultra), qa_tour_hour (13; nil keeps the save's time with qa_tour_keep_time),
-- qa_tour_weather (af3_slight_clear_steady_3 with the hour), qa_tour_keep_time = true (night saves),
-- qa_tour_torch = true (the player's torch on: its traced shadow).
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

local quality = qa_tour_quality or "st_rt_ultra"
local configs = {
	{ name = "off", cmds = { "r__rt_reflections 0", "r__rt_shadows 0", "r__rtao 0", "r__rt_gi 0" } },
	{ name = "on", cmds = { "r__rt_reflections 1", "r__rt_shadows 1", "r__rtao 1", "r__rt_gi 1", "r__rt_quality " .. quality } },
}
-- qa_tour_effects = { shadows = 1, gi = 0, ... }: the "on" config with only these effects (the others off).
if qa_tour_effects then
	local e = qa_tour_effects
	configs[2].cmds = { "r__rt_reflections " .. (e.reflections or 0), "r__rt_shadows " .. (e.shadows or 0),
		"r__rtao " .. (e.ao or 0), "r__rt_gi " .. (e.gi or 0), "r__rt_quality " .. quality }
end
-- heading (degrees from the save's), hands: rifle, rifle+detector, none
local views = { { 0, "rifle" }, { 90, "rifle" }, { 180, "detector" }, { 270, "none" } }

local ci, vi, phase, t0, frames, fps_frames = 1, 1, "start", 0, 0, 0
local heading0 = 0
local detector_up = false

local function hands(kind)
	if kind == "none" then
		if detector_up then tap("show_detector") detector_up = false end
		db.actor:activate_slot(0)
		return
	end
	db.actor:activate_slot(3)
	if kind == "detector" and not detector_up then
		tap("show_detector")
		detector_up = true
	elseif kind ~= "detector" and detector_up then
		tap("show_detector")
		detector_up = false
	end
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	device():pause(false)
	if frames == 1 then
		level.set_time_factor(0.0001)
		if not qa_tour_keep_time then
			to_hour(qa_tour_hour or 13)
			level.set_weather(qa_tour_weather or "af3_slight_clear_steady_3", true)
			cmd("qa_climate_force clear_steady 0 20")
		end
		cmd("g_god on")
		spawn("wpn_ak74")
		spawn("detector_elite")
	end
	if frames < 150 then return end
	local now = time_global()

	if phase == "start" then
		local w = db.actor:object("wpn_ak74")
		if w then db.actor:move_to_slot(w, 3) end
		local d = db.actor:object("detector_elite")
		if d then db.actor:move_to_slot(d, 9) end
		if qa_tour_torch then
			-- The head torch shines through its second beam (itms_manager's kTORCH toggle).
			local t = db.actor:object("device_torch")
			if t then
				t:enable_torch(true)
				t:enable_torch2(true)
				if itms_manager then itms_manager.Torch2 = true end
			end
			printf("[qa] torch %s", t and "on" or "missing")
		end
		heading0 = db.actor:direction():getH()
		printf("[qa] renderer %s caps %s", c:get_string("renderer"), c:get_string("r__caps"))
		cmd("r__gpu_log 60")
		phase, t0 = "config", now
	elseif phase == "config" then
		local cfg = configs[ci]
		if not cfg then
			cmd("r__gpu_log 0")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		for _, x in ipairs(cfg.cmds) do cmd(x) end
		cmd("vid_restart")
		vi = 1
		phase, t0 = "view", now
	elseif phase == "view" and now - t0 > 2500 then
		local v = views[vi]
		db.actor:set_actor_direction(-(heading0 + math.rad(v[1])))
		hands(v[2])
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 3500 then
		local v = views[vi]
		printf(string.format("[qa] SHOT %s_%d_%s", configs[ci].name, v[1], v[2]))
		cmd("flush")
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		vi = vi + 1
		if views[vi] then
			phase, t0 = "view", now
		else
			phase, t0, fps_frames = "measure", now, 0
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
