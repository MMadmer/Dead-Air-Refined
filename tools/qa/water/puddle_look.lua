-- Wet ground after a rain (docs/dead-air/WATER.md, puddles): a September noon, a shower of qa_rain_s seconds with a
-- fast buildup, then the sky of qa_after; from the save's eye the camera looks flat and from qa_up metres over it
-- steeply down. Each step of qa_plan ("base" or console commands split by "|") takes both shots, announced as
-- "[qa] puddle shot <n> <view> <step>".
--   qa_rain_s = 15
--   qa_after = "foggy"
--   qa_up = 20
--   qa_plan = "base;r__puddles 0;r__puddles_gloss 0;r__puddles_size 0"
--
--   tools\qa\Run-WaterQa.ps1 -Name puddle-look -Scenario puddle_look -Save qa_meadow -KeepShaderCache -Seconds 120
local c = get_console()
local stage, t0, frames = "start", 0, 0
local eye, dir0
local plan, step, shots, view = {}, 0, 0, "eye"

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function place(v)
	if v == "eye" then
		level.set_cam_custom_position_direction(eye, vector():set(dir0:getH(), -0.08, 0), 0, false, false)
	else
		level.set_cam_custom_position_direction(vector():set(eye.x, eye.y + (qa_up or 20), eye.z),
			vector():set(dir0:getH(), -0.75, 0), 0, false, false)
	end
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end

	if stage == "start" then
		level.set_time_factor(0.0001)
		cmd("g_god on")
		cmd("hud_weapon 0")
		cmd("r__season september")
		cmd("wind_force 0.3")
		to_hour(13)
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		for item in string.gmatch(qa_plan or "base;r__puddles 0;r__puddles_gloss 0;r__puddles_size 0", "[^;]+") do
			plan[#plan + 1] = item
		end
		cmd("r__puddles_buildup 5")
		cmd("qa_climate_force rain_steady 1 14")
		say("[qa] puddle level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " .. c:get_token("_preset"))
		go("rain")
	elseif stage == "rain" and now - t0 > (qa_rain_s or 15) * 1000 then
		cmd("qa_climate_force " .. (qa_after or "foggy") .. " 0 14")
		cmd("qa_climate_state")
		cmd("qa_rain_state")
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		if s ~= "base" then
			for part in string.gmatch(s, "[^|]+") do cmd(part) end
		end
		view = "eye"
		place(view)
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		shots = shots + 1
		say(string.format("[qa] puddle shot %02d %s %s", shots, view, plan[step]))
		cmd("qa_rain_state")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then
			if view == "eye" then
				view = "high"
				place(view)
				go("settle")
			else
				-- Each step starts from the build's own values.
				cmd("r__puddles 1")
				cmd("r__puddles_gloss " .. (qa_gloss or "1"))
				cmd("r__puddles_size " .. (qa_size or "1"))
				go("next")
			end
		end
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] puddle DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
