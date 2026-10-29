-- A storm with the climate off (the player's "old weather"): the stock storm cycle pinned at an hour, the rain's and
-- the sky's state reported, shots with the hands and the weapon in view - is it dark, does it rain, does lightning
-- strike, how do the hands look.
--   qa_storm_weather = "af3_dark_storm"   the cycle
--   qa_storm_hour = 14
--   qa_storm_settle = 6000   ms of the storm before the shots (the puddles fill over a minute or two)
--   qa_storm_boot = "wind_seed 1833.7|wind_freeze 1"   commands once the weather is set ("|")
--   qa_storm_shots = ";hud_weapon 0;hud_weapon 1|r2_ssao st_opt_off;hud_weapon 0"
--                    one entry per shot (";"), the commands run before it ("|"); default: four plain shots
--   qa_storm_walk = 0        m/s the actor walks ahead before the shots (moved frame by frame), the rain's wind reported
--                            while he walks: the sheet must not follow him
--
--   tools\qa\Run-WaterQa.ps1 -Name storm -Scenario storm_check -Save qa_meadow -PreCommands 'r__season_weather 0'
local c = get_console()
local stage, t0, frames, shot = "start", 0, 0, 0
local walk_t
local plan = {}

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end
local function run(list) for one in string.gmatch(list or "", "[^|]+") do cmd(one) end end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end

	if stage == "start" then
		cmd("g_god on")
		cmd("r__season_weather 0")
		to_hour(qa_storm_hour or 14)
		level.set_weather(qa_storm_weather or "af3_dark_storm", true)
		db.actor:activate_slot(db.actor:item_in_slot(3) and 3 or 2)
		run(qa_storm_boot)
		local shots = qa_storm_shots or ";;;"
		for item in string.gmatch(shots .. ";", "([^;]*);") do plan[#plan + 1] = item end
		go("settle")
	elseif stage == "settle" and now - t0 > (qa_storm_settle or 6000) then
		say("[qa] storm weather " .. tostring(level.get_weather()))
		cmd("qa_rain_state")
		cmd("qa_snow_state")
		cmd("qa_climate_state")
		go((qa_storm_walk or 0) > 0 and "walk" or "next")
	elseif stage == "walk" then
		-- The ground under him is the physics' (y as it lands), the step along his heading.
		local dt = walk_t and (now - walk_t) * 0.001 or 0
		walk_t = now
		-- (Along the ground, unit: the camera's direction came back unscaled on DirectX 12.)
		local d3 = device().cam_dir
		local len = math.max(math.sqrt(d3.x * d3.x + d3.z * d3.z), 1e-6)
		local dir = vector():set(d3.x / len, 0, d3.z / len)
		local p = db.actor:position()
		db.actor:set_actor_position(vector():set(p.x + dir.x * qa_storm_walk * dt, p.y, p.z + dir.z * qa_storm_walk * dt))
		if now - t0 > 3000 then
			say("[qa] storm walking " .. qa_storm_walk .. " m/s toward " .. string.format("%.2f %.2f", dir.x, dir.z))
			cmd("qa_rain_state")
			-- A walking eye sits a few centimetres off his feet (the head's sway and lean): the camera put 12 cm ahead.
			local h = dir:getH()
			level.set_cam_custom_position_direction(vector():set(p.x + dir.x * 0.12, p.y + 1.7, p.z + dir.z * 0.12),
				vector():set(h, -0.05, 0), 0, false, false)
			go("walk_eye")
		end
	elseif stage == "walk_eye" and now - t0 > 400 then
		say("[qa] storm eye 12 cm ahead of the feet")
		cmd("qa_rain_state")
		go("next")
	elseif stage == "next" then
		shot = shot + 1
		if shot > #plan then
			cmd("qa_rain_state")
			go("done")
			return
		end
		run(plan[shot])
		go("wait")
	elseif stage == "wait" and now - t0 > 1500 then
		-- One frame (the screenshot's file name has a one-second resolution).
		say("[qa] storm shot " .. shot .. " " .. plan[shot])
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" and now - t0 > 1500 then
		say("[qa] storm DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
