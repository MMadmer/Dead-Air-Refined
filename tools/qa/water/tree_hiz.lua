-- The trees' cut against the depth drawn so far (docs/dead-air/TREES.md, "Occlusion"): at the eye, four headings a quarter
-- turn apart, each with r__trees_occlusion 0 (the cut by the frustum and the wire rule alone, as before) and 1 (and by the
-- depth: the opaque world, then the near trees for the far ones). A mode settles, then "[qa] hiz begin <view> <mode>" ...
-- "[qa] hiz end <view> <mode>" brackets the r__gpu_log lines that average it, followed by r__trees_stats (the cut's words
-- by phase come a frame or two later) and a shot. A pinned July noon in clear weather.
--   qa_hiz_modes = "0,1"   the modes, in this order at every heading
--   qa_hiz_turns = 4       how many headings, a quarter turn apart from the save's
--
--   tools\qa\Run-WaterQa.ps1 -Name tree-hiz -Scenario tree_hiz -Save trees -KeepShaderCache -Seconds 120
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
		cmd("r__season july")
		cmd("wind_force 0.3")
		to_hour(13)
		cmd("r__gpu_log 30")
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		for v = 0, (qa_hiz_turns or 4) - 1 do
			for m in string.gmatch(qa_hiz_modes or "0,1", "[0-9]") do
				plan[#plan + 1] = { view = "h" .. (v * 90), yaw = dir0:getH() + v * math.pi * 0.5, mode = m }
			end
		end
		say("[qa] hiz level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset"))
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		level.set_cam_custom_position_direction(eye, vector():set(s.yaw, -0.05, 0), 0, false, false)
		cmd("r__trees_occlusion " .. s.mode)
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		local s = plan[step]
		say("[qa] hiz begin " .. s.view .. " " .. s.mode)
		go("measure")
	elseif stage == "measure" and now - t0 > 2500 then
		local s = plan[step]
		say("[qa] hiz end " .. s.view .. " " .. s.mode)
		cmd("r__trees_stats")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		-- The screenshot's file name has a one-second resolution.
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		cmd("r__gpu_log 0")
		cmd("r__trees_occlusion 1")
		say("[qa] hiz DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
