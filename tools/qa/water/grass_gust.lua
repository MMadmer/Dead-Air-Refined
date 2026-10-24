-- The meadow's motion under the wind (docs/dead-air/GRASS.md, "Wind"): at the save's eye, raised by qa_gust_up, a few
-- seconds of r__capture for each mode of the plan, so the far field's gusts can be measured frame to frame (the
-- temporal spread of its pixels). A pinned July noon in clear weather and a fresh wind.
--   qa_gust_modes = "base:r__dbg_grass 1024;gust:r__dbg_grass 0"   label:commands (split by "|")
--   qa_gust_wind = 0.6     wind_force
--   qa_gust_up = 1.5       the camera above the eye (m)
--
--   tools\qa\Run-WaterQa.ps1 -Name grass-gust -Scenario grass_gust -Save qa_meadow -KeepShaderCache -Seconds 150
local c = get_console()
local stage, t0, frames = "start", 0, 0
local eye, dir0
local plan, step = {}, 0

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

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
		level.set_weather(qa_water_weather or "af3_slight_clear_steady_3", true)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_time_factor(0.0001)
		cmd("g_god on")
		cmd("hud_weapon 0")
		cmd("r__season july")
		cmd("r__clouds_cover 0")
		cmd("wind_seed 1833.7")
		cmd("wind_force " .. tostring(qa_gust_wind or 0.6))
		to_hour(13)
		eye = vector():set(device().cam_pos)
		eye.y = eye.y + (qa_gust_up or 1.5)
		dir0 = vector():set(device().cam_dir)
		for item in string.gmatch(qa_gust_modes or "base:r__dbg_grass 1024;gust:r__dbg_grass 0", "[^;]+") do
			local label, cmds = string.match(item, "^%s*([^:]+):(.*)$")
			plan[#plan + 1] = { label = label, cmds = cmds or "" }
		end
		level.set_cam_custom_position_direction(eye, vector():set(dir0:getH(), -0.12, 0), 0, false, false)
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		for one in string.gmatch(s.cmds, "[^|]+") do
			cmd(one)
		end
		-- Every mode from the same wind: the reseed restarts all its clocks, so two modes' captures differ by the
		-- mode alone (a capture a few seconds later caught another gust).
		cmd("wind_seed 1833.7")
		go("settle")
	elseif stage == "settle" and now - t0 > 2000 then
		say("[qa] gust capture " .. plan[step].label)
		cmd("r__capture 3 30")
		go("capture")
	elseif stage == "capture" and now - t0 > 4000 then
		go("next")
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] gust DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
