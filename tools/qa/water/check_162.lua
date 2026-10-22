-- Fixes of 1.6.2 in one short run: a storm at the wind's ceiling (how far the trees lean, the wet ground under them),
-- the trees' statistics (the GPU cut's elements with no room), then January's snow with a rifle lying in it, seen low
-- from four sides. Each shot is announced as "[qa] SHOT nn <label>".
--
--   tools\qa\Run-WaterQa.ps1 -Name chk162 -Scenario check_162 -Save qa_square2 -KeepShaderCache -Seconds 150 -ExtraArgs "-r5"
local stage, t0, frames = "start", 0, 0
local shots, shot_frame = 0, -1
local eye, dir0, rifle_id

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

-- The camera d metres off the rifle along heading h, a little over the snow, looking at it.
local function at_rifle(h, d)
	local obj = rifle_id and level.object_by_id(rifle_id)
	if not obj then return end
	local p = obj:position()
	cam(vector():set(p.x + math.sin(h) * d, p.y + 0.45, p.z + math.cos(h) * d), h + math.pi, -0.2)
end

-- A rifle on the ground ahead: qa_rifle_m metres along the AI grid (a save's actor may stand on a roof), 2.5 by default.
local function drop_rifle()
	local flat = vector():set(dir0.x, 0, dir0.z):normalize()
	local lv = level.vertex_in_direction(db.actor:level_vertex_id(), flat, qa_rifle_m or 2.5)
	local ahead = vector():set(level.vertex_position(lv))
	ahead.y = ahead.y + 0.4
	local se = alife():create("wpn_ak74", ahead, lv, db.actor:game_vertex_id())
	rifle_id = se and se.id
end

local plan = {
	{ "storm_wide_a", function() cam(vector():set(eye.x, eye.y + 6, eye.z), dir0:getH(), -0.05) end, 4000 },
	{ "storm_wide_b", function() end, 700 },
	{ "storm_wide_c", function() end, 700 },
	{ "storm_ground", function() cam(eye, dir0:getH(), -0.6); cmd("r__trees_stats") end, 1500 },
	{ "snow_wide", function()
		cmd("qa_climate_force clear_steady 0 -8")
		cmd("r__season january")
		cmd("wind_force -1")
		cam(vector():set(eye.x, eye.y + 2, eye.z), dir0:getH(), -0.25)
	end, 4000 },
	{ "snow_rifle_dropped", function() drop_rifle() end, 4000 },
	{ "rifle_0", function() at_rifle(0.0, 1.6) end, 1500 },
	{ "rifle_90", function() at_rifle(math.pi * 0.5, 1.6) end, 1200 },
	{ "rifle_180", function() at_rifle(math.pi, 1.6) end, 1200 },
	{ "rifle_270", function() at_rifle(math.pi * 1.5, 1.6) end, 1200 },
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
		to_hour(13)
		cmd("qa_climate_force storm 0.8 15")
		cmd("wind_force 1.25")
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		say("[qa] chk162 level " .. level.name() .. " renderer " .. get_console():get_string("renderer") .. " preset " ..
			get_console():get_token("_preset"))
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
		level.remove_cam_custom_position_direction()
		say("[qa] chk162 DONE shots=" .. shots)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
