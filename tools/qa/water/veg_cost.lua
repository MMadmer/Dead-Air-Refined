-- The vegetation's cost at the eye (docs/dead-air/VEGETATION_GOT.md): at the save's eye, a few headings a quarter turn
-- apart, each mode of the plan in turn (an A/B in one run, the modes interleaved at every heading so the laptop's clocks
-- drift alike over both). A mode settles, then "[qa] veg begin <view> <mode>" ... "[qa] veg end <view> <mode>" brackets
-- the r__gpu_log lines that average it, followed by the populations' stats and a shot. A pinned July noon in clear
-- weather and a light wind.
--   qa_veg_modes = "base:;nogrid:r__trees_occlusion 0|r__grass_occlusion 0"   label:commands (split by "|"), in order
--   qa_veg_turns = 4       how many headings, a quarter turn apart from the save's
--   qa_veg_up = 0          how far above the eye the camera stands (m)
--   qa_veg_pitch = -0.05   the camera's pitch (rad)
--   qa_veg_hour = 13       the hour of the day (a low sun: the soil's and the leaves' sheen against it)
--
--   tools\qa\Run-WaterQa.ps1 -Name veg-base -Scenario veg_cost -Save trees -KeepShaderCache -Seconds 150
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
		cmd("wind_force 0.3")
		to_hour(qa_veg_hour or 13)
		cmd("r__gpu_log 30")
		cmd("r__fps_log 1")
		eye = vector():set(device().cam_pos)
		eye.y = eye.y + (qa_veg_up or 0)
		dir0 = vector():set(device().cam_dir)
		local modes = {}
		for item in string.gmatch(qa_veg_modes or "base:", "[^;]+") do
			local label, cmds = string.match(item, "^%s*([^:]+):(.*)$")
			modes[#modes + 1] = { label = label, cmds = cmds or "" }
		end
		for v = 0, (qa_veg_turns or 4) - 1 do
			for _, m in ipairs(modes) do
				plan[#plan + 1] = { view = "h" .. (v * 90), yaw = dir0:getH() + v * math.pi * 0.5, mode = m }
			end
		end
		say("[qa] veg level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset"))
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		level.set_cam_custom_position_direction(eye, vector():set(s.yaw, qa_veg_pitch or -0.05, 0), 0, false, false)
		for one in string.gmatch(s.mode.cmds, "[^|]+") do
			cmd(one)
		end
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		local s = plan[step]
		say("[qa] veg begin " .. s.view .. " " .. s.mode.label)
		go("measure")
	elseif stage == "measure" and now - t0 > 3000 then
		local s = plan[step]
		say("[qa] veg end " .. s.view .. " " .. s.mode.label)
		cmd("r__trees_stats")
		cmd("r__shrubs_stats")
		cmd("r__grass_blades_stats")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		-- The screenshot's file name has a one-second resolution.
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		cmd("r__gpu_log 0")
		cmd("r__fps_log 0")
		say("[qa] veg DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
