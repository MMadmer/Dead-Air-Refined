-- Player reports of 1.6.1: the black square under a carried lamp, the player's own shadow from it. One short run at
-- night with a glowstick in the left hand: ahead, at the feet, to the side, then the feet again with each suspect
-- switched off in turn (the lamp shadow cache, the lamp's detail casters, the player's shadow model). Each shot is
-- announced as "[qa] SHOT nn <label>".
--
--   tools\qa\Run-WaterQa.ps1 -Name sq162 -Scenario square_162 -Save qa_square -KeepShaderCache -Seconds 150 -ExtraArgs "-r5"
local stage, t0, frames = "start", 0, 0
local shots, shot_frame = 0, -1
local eye, dir0
local glow = false

-- The glowstick and the lighter shine through the actor's torch (xr_actor.UpdateTorch, TorchType 3): a point lamp of
-- 8 m at the hand. Set straight on the torch, so the run does not depend on the detector slot's scenes.
local function glow_torch()
	local torch = db.actor:object("device_torch")
	if not torch then return end
	if not torch:torch_enabled() then torch:enable_torch(true) end
	torch:torch_switch_spot(false)
	torch:torch_set_range(8)
	torch:torch_set_radius(60)
	torch:torch_set_texture("internal\\torch_gas")
	torch:torch_set_animation("empty")
	torch:torch_set_color_r(0.0)
	torch:torch_set_color_g(0.4)
	torch:torch_set_color_b(0.3)
	torch:torch_set_color_a(1.0)
	torch:torch_set_offset_y(-0.1)
	torch:torch_set_offset_z(0.35)
end

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot(label)
	shots = shots + 1
	say(string.format("[qa] SHOT %02d %s", shots, label))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function cam(pos, h, p)
	level.set_cam_custom_position_direction(pos, vector():set(h, p, 0), 0, true, false)
end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function clear_sky()
	cmd("qa_climate_force clear_steady 0 20")
	level.set_weather("af3_slight_clear_steady_3", true)
end

-- label, setup, milliseconds to settle
local plan = {
	{ "glow_ahead", function()
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "device_glowstick" then db.actor:move_to_slot(item, 9) end
		end, nil)
		db.actor:restore_detector(false)
		glow = true
	end, 4000 },
	{ "glow_ahead_later", function() end, 2500 },
	{ "custom_ahead", function() cam(eye, dir0:getH(), dir0:getP()) end, 2000 },
	{ "glow_feet", function() cam(eye, dir0:getH(), -1.25) end, 2000 },
	{ "glow_side", function() cam(eye, dir0:getH() + 1.2, -0.7) end, 2000 },
	{ "feet_no_cache", function() cam(eye, dir0:getH(), -1.25); cmd("r__light_shadow_cache 0") end, 2000 },
	{ "normal_again", function() cmd("r__light_shadow_cache 1"); level.remove_cam_custom_position_direction() end, 2500 },
	{ "normal_no_actor_shadow", function() cmd("r__actor_shadow 0") end, 2000 },
}

local step = 0

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
	if glow and frames % 30 == 0 then
		if not qa_no_torch then glow_torch() end
		-- the detector shows only beside a free or one-handed item; retried until it is up
		if not db.actor:active_detector() then
			db.actor:activate_slot(0)
			db.actor:restore_detector(true)
		end
	end

	if stage == "start" then
		cmd("g_god on")
		level.set_time_factor(0.0001)
		cmd("r2_tonemap_adaptation 10")
		cmd("r__light_shadow_cache 2")
		to_hour(23)
		clear_sky()
		-- qa_no_torch: no head torch, so the glowstick lights with its own lamp (CCustomDetector::m_light)
		if qa_no_torch then
			local torch = db.actor:object("device_torch")
			if torch then alife():release(alife_object(torch:id()), true) end
		end
		alife():create("device_glowstick", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		say("[qa] sq162 level " .. level.name() .. " renderer " .. get_console():get_string("renderer") .. " preset " ..
			get_console():get_token("_preset"))
		stage, t0 = "next", now
	elseif stage == "next" and now - t0 > 1500 then
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
		say("[qa] time " .. level.get_time_hours() .. ":" .. level.get_time_minutes() .. " glow " ..
			tostring(db.actor:item_in_slot(9) and db.actor:item_in_slot(9):section()) .. " active_detector " ..
			tostring(db.actor:active_detector() ~= nil) .. " torch " .. tostring(db.actor:object("device_torch") ~= nil))
		stage, t0 = "next", now
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] sq162 DONE shots=" .. shots)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
