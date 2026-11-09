-- Player reports of 1.6.3: the torch key clicks and nothing lights, and a treasure spawned under a stale box id crashes.
-- One night run with the actor's own camera. Every step logs "[qa] torch <label>" and takes a shot; the keys go the way
-- a real press does, the script's raw-key callback first and the actor's input after it. The last step spawns an item
-- into a grenade launcher, as coc_treasure_manager did for an id that outlived its stash: the run has to go on.
--
--   tools\qa\Run-WaterQa.ps1 -Name torch164 -Scenario torch_164 -Save glowsq -KeepShaderCache -Seconds 120 -ExtraArgs "-r5"
local stage, t0, frames = "start", 0, 0
local step, shots, shot_frame = 0, 0, -1
local torch_key, torch_slot, gl_id

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function first_of(sec)
	local found
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then found = item end end, nil)
	return found
end

local function give(sec)
	if not first_of(sec) then
		alife():create(sec, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
	end
end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function press_torch()
	itms_manager.on_key_press(torch_key)
	level.press_action(key_bindings.kTORCH)
	level.release_action(key_bindings.kTORCH)
end

local function detector_up()
	db.actor:activate_slot(0)
	db.actor:restore_detector(true)
end

local function shot(label)
	shots = shots + 1
	local torch = db.actor:object("device_torch")
	local det = db.actor:active_detector()
	say(string.format("[qa] torch SHOT %02d %s torch2 %s type %s enabled %s detector %s state %s", shots, label,
		tostring(itms_manager.Torch2), tostring(itms_manager.TorchType), tostring(torch and torch:torch_enabled()),
		det and det:section() or "-", det and tostring(det:get_state()) or "-"))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local plan = {
	{ "dark", function() end, 2000 },
	{ "headlamp_on", press_torch, 2000 },
	{ "headlamp_off", press_torch, 2000 },
	{ "hand_slot", function() db.actor:move_to_slot(first_of("device_flashlight"), 9) end, 1000 },
	{ "hand_draw", detector_up, 1500 },
	{ "hand_on", detector_up, 3000 },
	-- Black, then the animation on and off in one tick: the order the hand lamp's scripts run in when it stayed dark.
	{ "hand_race", function()
		local torch = db.actor:object("device_torch")
		torch:torch_set_color_r(0)
		torch:torch_set_color_g(0)
		torch:torch_set_color_b(0)
		torch:torch_set_animation("light_torch_01")
		torch:torch_set_animation("empty")
	end, 1500 },
	{ "hand_away", function() db.actor:move_to_ruck(first_of("device_flashlight")) end, 3000 },
	{ "headlamp_on_again", press_torch, 2000 },
	{ "torch_in_ruck", function() db.actor:move_to_ruck(db.actor:object("device_torch")) end, 2000 },
	{ "torch_back", function() db.actor:move_to_slot(db.actor:object("device_torch"), torch_slot) end, 2000 },
	{ "launcher_spawn", function()
		alife():create("bandage", db.actor:position(), 0, 0, gl_id, false)
	end, 3000 },
}

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
	if frames < 120 then return end
	local now = time_global()

	if stage == "start" then
		cmd("g_god on")
		level.set_time_factor(0.0001)
		cmd("r2_tonemap_adaptation 10")
		to_hour(23)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_weather("af3_slight_clear_steady_3", true)
		for k = 0, 255 do
			if dik_to_bind(k) == key_bindings.kTORCH then torch_key = k break end
		end
		give("device_torch")
		give("wpn_upd")
		give("device_flashlight")
		give("wpn_ak74")
		give_info("enable_device_torch")
		stage, t0 = "items", now
	elseif stage == "items" and now - t0 > 2000 then
		local upd = db.actor:object("wpn_upd")
		if upd then upd:set_condition(1.0) end
		local torch = db.actor:object("device_torch")
		for s = 0, 15 do
			local it = db.actor:item_in_slot(s)
			if torch and it and it:id() == torch:id() then torch_slot = s end
		end
		if torch and not torch_slot then
			db.actor:move_to_slot(torch, 8)
			torch_slot = 8
		end
		local gl = first_of("wpn_ak74")
		gl_id = gl and gl:id()
		say(string.format("[qa] torch level %s renderer %s key %s slot %s launcher %s", level.name(),
			get_console():get_string("renderer"), tostring(torch_key), tostring(torch_slot), tostring(gl_id)))
		stage, t0 = "next", now
	elseif stage == "next" and now - t0 > 1000 then
		step = step + 1
		local s = plan[step]
		if not s then
			stage = "done"
			return
		end
		s[2]()
		stage, t0 = "settle", now
	elseif stage == "settle" and now - t0 > plan[step][3] then
		shot(plan[step][1])
		stage, t0 = "next", now
	elseif stage == "done" then
		say("[qa] torch DONE shots=" .. shots)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
