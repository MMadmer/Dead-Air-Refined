-- The look of the blade grass at rest (docs/dead-air/GRASS.md, "Disorder"): for each month of
-- qa_look_months the season is pinned and each view of the stand is shot, announced as
-- "[qa] look shot <n> <month> <view>". A pinned noon in clear weather with a light frozen wind, so two builds compare
-- blade for blade.
--   qa_look_months = "june,august,october"   the season pins, in order (qa_season_pin's words or days)
--   qa_look_species = "cal_epi,dac_glo"      every lane grows that species (r__grass_species_pin), each in turn
--                                            through the months; the meadow's own stand without it
--   qa_look_views = "eye,low,down,far"       eye: standing, looking a little down; mid: a metre over the ground,
--                                            into the stand (a photographer's); low: 0.45 m over the ground, level
--                                            through the stand; down: at the feet; far: the meadow at eye level
--   qa_look_wind = 0.15                      the wind's force (frozen after the first second)
--   qa_look_cmds = "r__dbg_grass 2048"       console lines before the first shot, split by ";"
--   qa_look_turn = 0                         degrees added to the save's heading
--
--   tools\qa\Run-WaterQa.ps1 -Name look-new -Scenario grass_look -Save qa_meadow -KeepShaderCache -Seconds 90
local c = get_console()
local stage, t0, frames = "start", 0, 0
local eye, yaw0
local months, views, species, step, shot, pin = {}, {}, {}, 0, 0, 1

-- name, metres over the save's eye, pitch (rad), yaw offset (rad)
local view_def = {
	eye = { 0, -0.32, 0 },
	mid = { -0.7, -0.14, 0.25 },
	low = { -1.2, -0.04, 0.5 },
	down = { -0.2, -1.0, 0.9 },
	far = { 0.2, -0.1, -0.6 },
}

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function place(name)
	local v = view_def[name]
	level.set_cam_custom_position_direction(vector():set(eye.x, eye.y + v[1], eye.z),
		vector():set(yaw0 + v[3], v[2], 0), 0, false, false)
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end

	if stage == "start" then
		level.set_weather(qa_water_weather or "af3_slight_clear_steady_3", true)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_time_factor(0.0001)
		cmd("g_god on")
		cmd("hud_weapon 0")
		cmd("wind_seed 1833.7")
		cmd("wind_force " .. tostring(qa_look_wind or 0.15))
		to_hour(13)
		eye = vector():set(device().cam_pos)
		yaw0 = device().cam_dir:getH() + (qa_look_turn or 0) * math.pi / 180
		for m in string.gmatch(qa_look_months or "june,august,october", "[^,]+") do months[#months + 1] = m end
		for v in string.gmatch(qa_look_views or "eye,low,down,far", "[^,]+") do
			if view_def[v] then views[#views + 1] = v end
		end
		for sp in string.gmatch(qa_look_species or "", "[^,]+") do species[#species + 1] = sp end
		if species[1] then cmd("r__grass_species_pin " .. species[1]) end
		for line in string.gmatch(qa_look_cmds or "", "[^;]+") do cmd(line) end
		say("[qa] look level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset"))
		go("month")
	elseif stage == "month" then
		step = step + 1
		if not months[step] then
			pin = pin + 1
			if not species[pin] then
				go("done")
				return
			end
			cmd("r__grass_species_pin " .. species[pin])
			step = 1
		end
		cmd("qa_season_pin " .. months[step])
		place(views[1])
		shot = 0
		go("latch")
	elseif stage == "latch" and now - t0 > 2500 then
		cmd("wind_freeze 1")
		go("view")
	elseif stage == "view" then
		shot = shot + 1
		if not views[shot] then
			go("month")
			return
		end
		place(views[shot])
		go("settle")
	elseif stage == "settle" and now - t0 > 1300 then
		say("[qa] look shot " .. (((pin - 1) * #months + step - 1) * #views + shot) .. " " .. (species[pin] or "stand") ..
			" " .. months[step] .. " " .. views[shot])
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		go("view")
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		cmd("qa_season_pin off")
		cmd("r__grass_species_pin off")
		cmd("wind_freeze 0")
		cmd("hud_weapon 1")
		say("[qa] look DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
