-- Player report of 1.6.1: a flashlight dropped on the ground lights the ground behind the walls of houses. One short
-- night run: a hand flashlight (device_flashlight, a shadowed 80 m spot) dropped inside the nearest house, the house
-- shot from four sides outside, then again with the lamp shadow cache off. Each shot is "[qa] SHOT nn <label>".
--
--   tools\qa\Run-WaterQa.ps1 -Name fl162 -Scenario flashlight_162 -Save qa_square2 -KeepShaderCache -Seconds 150 -ExtraArgs "-r5"
local stage, t0, frames = "start", 0, 0
local shots, shot_frame = 0, -1
local eye, house, lamp_obj

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot(label)
	shots = shots + 1
	say(string.format("[qa] SHOT %02d %s", shots, label))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function cam(pos, h, p)
	level.set_cam_custom_position_direction(pos, vector():set(h, p, 0), 0, false, false)
end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
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


-- A point outside the house, d metres off along heading h, at eye height, looking back at the house.
local function outside(h, d)
	local p = vector():set(house.x + math.sin(h) * d, house.y, house.z + math.cos(h) * d)
	return p, h + math.pi
end

local plan = {
	{ "inside", function() cam(vector():set(house.x, house.y + 0.8, house.z), 0.0, -0.6) end, 2500 },
	{ "out_0", function() local p, h = outside(0.0, 9); cam(p, h, -0.25) end, 2000 },
	{ "out_90", function() local p, h = outside(math.pi * 0.5, 9); cam(p, h, -0.25) end, 2000 },
	{ "out_180", function() local p, h = outside(math.pi, 9); cam(p, h, -0.25) end, 2000 },
	{ "out_270", function() local p, h = outside(math.pi * 1.5, 9); cam(p, h, -0.25) end, 2000 },
	{ "nocache_0", function() cmd("r__light_shadow_cache 0"); local p, h = outside(0.0, 9); cam(p, h, -0.25) end, 2000 },
	{ "nocache_90", function() local p, h = outside(math.pi * 0.5, 9); cam(p, h, -0.25) end, 2000 },
	{ "nocache_180", function() local p, h = outside(math.pi, 9); cam(p, h, -0.25) end, 2000 },
	{ "nocache_270", function() local p, h = outside(math.pi * 1.5, 9); cam(p, h, -0.25) end, 2000 },
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
		cmd("r2_tonemap_adaptation 10")
		cmd("hud_weapon 0")
		to_hour(23)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_weather("af3_slight_clear_steady_3", true)
		eye = vector():set(device().cam_pos)
		local d
		house, d = find_house()
		if not house then
			say("[qa] fl162 no house")
			stage = "done"
			return
		end
		lamp_obj = alife():create("device_flashlight", vector():set(house.x, house.y - 1.0, house.z), level.vertex_id(house), db.actor:game_vertex_id())
		say("[qa] fl162 level " .. level.name() .. " renderer " .. get_console():get_string("renderer") .. string.format(" house %.1f %.1f %.1f at %.0f m", house.x, house.y, house.z, d))
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
		stage, t0 = "next", now
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] fl162 DONE shots=" .. shots)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
