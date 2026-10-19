-- Player reports of 1.6.0, shot in one short run (fixes of 1.6.1), under a pinned clear sky at 16:00:
--   meadow at the eye, both ways (the grass's shadows), from 30 m up toward the horizon (tree LODs, the sky, bloom,
--   the far field), the nearest house from inside at four headings (sun through the roofs; the spot is found by rays:
--   a roof over it and walls round it), then the night with a glowstick in the left hand, looking down at the feet
--   and ahead (the black square under the player). Each shot is announced as "[qa] SHOT nn <label>".
--
--   tools\qa\Run-WaterQa.ps1 -Name r161 -Scenario reports_161 -Save qa_meadow -KeepShaderCache -Seconds 300
local stage, t0, frames = "start", 0, 0
local shots, shot_frame = 0, -1
local eye, dir0, house

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot(label)
	shots = shots + 1
	say(string.format("[qa] SHOT %02d %s", shots, label))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function cam(pos, h, p, hud)
	level.set_cam_custom_position_direction(pos, vector():set(h, p, 0), 0, hud or false, false)
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

local function pick(pos, dir, range)
	local r = rq_result()
	if level.ray_pick(pos, dir, range, rq_target.rqtStatic, r, nil) then return r.range end
	return nil
end

-- The nearest spot under a roof 2.2-6 m over the floor with walls within 7 m on all four sides.
local function find_house()
	local down, up = vector():set(0, -1, 0), vector():set(0, 1, 0)
	local best, best_d
	for ix = -40, 40 do
		for iz = -40, 40 do
			local x, z = eye.x + ix * 3, eye.z + iz * 3
			local top = vector():set(x, eye.y + 60, z)
			local r1 = pick(top, down, 120)
			if r1 then
				local roof = top.y - r1
				local r2 = pick(vector():set(x, roof - 0.3, z), down, 10)
				if r2 then
					local floor = roof - 0.3 - r2
					local hgt = roof - floor
					if hgt > 2.2 and hgt < 6 then
						local p = vector():set(x, floor + 1.6, z)
						local walls = 0
						for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
							if pick(p, vector():set(d[1], 0, d[2]), 7) then walls = walls + 1 end
						end
						local up_hit = pick(p, up, 6)
						local dist = math.sqrt((x - eye.x) ^ 2 + (z - eye.z) ^ 2)
						if walls == 4 and up_hit and (not best_d or dist < best_d) then
							best, best_d = p, dist
						end
					end
				end
			end
		end
	end
	return best, best_d
end

-- label, setup, milliseconds to settle
local plan = {
	{ "meadow_eye", function() level.remove_cam_custom_position_direction() end, 2500 },
	{ "meadow_back", function() cam(eye, dir0:getH() + math.pi, -0.05) end, 2500 },
	{ "high_far", function() cam(vector():set(eye.x, eye.y + 30, eye.z), dir0:getH(), -0.08) end, 3500 },
	{ "house_0", function() if house then cam(house, math.pi * 0.0, 0.1) end end, 3500 },
	{ "house_45", function() if house then cam(house, math.pi * 0.25, 0.1) end end, 1500 },
	{ "house_90", function() if house then cam(house, math.pi * 0.5, 0.1) end end, 1500 },
	{ "house_135", function() if house then cam(house, math.pi * 0.75, 0.1) end end, 1500 },
	{ "house_180", function() if house then cam(house, math.pi * 1.0, 0.1) end end, 1500 },
	{ "house_225", function() if house then cam(house, math.pi * 1.25, 0.1) end end, 1500 },
	{ "house_270", function() if house then cam(house, math.pi * 1.5, 0.1) end end, 1500 },
	{ "house_315", function() if house then cam(house, math.pi * 1.75, 0.1) end end, 1500 },
	{ "night_glow_ahead", function()
		level.remove_cam_custom_position_direction()
		to_hour(23)
		clear_sky()
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "device_glowstick" then db.actor:move_to_slot(item, 9) end
		end, nil)
	end, 1000 },
	{ "night_glow_ahead_on", function() db.actor:restore_detector(false) end, 4000 },
	{ "night_glow_feet", function() cam(eye, dir0:getH(), -1.2, true) end, 2000 },
	{ "night_glow_side", function() cam(eye, dir0:getH() + 1.2, -0.6, true) end, 2000 },
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

	if stage == "start" then
		cmd("g_god on")
		level.set_time_factor(0.0001)
		cmd("hud_weapon 0")
		cmd("r2_tonemap_adaptation 10")
		to_hour(16)
		clear_sky()
		alife():create("device_glowstick", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		local d
		house, d = find_house()
		say("[qa] r161 level " .. level.name() .. " renderer " .. get_console():get_string("renderer") .. " preset " ..
			get_console():get_token("_preset") .. " house " .. (house and string.format("%.1f %.1f %.1f at %.0f m", house.x, house.y, house.z, d) or "none"))
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
		say("[qa] weather " .. tostring(level.get_weather()) .. " time " .. level.get_time_hours() .. ":" .. level.get_time_minutes() ..
			" glow " .. tostring(db.actor:item_in_slot(9) and db.actor:item_in_slot(9):section()) .. " active_detector " ..
			tostring(db.actor:active_detector() ~= nil))
		stage, t0 = "next", now
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] r161 DONE shots=" .. shots)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
