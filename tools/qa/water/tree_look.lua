-- The trees' crowns at distance (docs/dead-air/TREES.md, "Drawing"): from the save's eye the camera moves along its view
-- (flat, a little over the eye's height) and takes a shot per step of qa_tree_plan. A step is "<metres forward>" and
-- then console commands, split by "|" ("-20" backs off, "20|r__trees_force_tier 2" forces the thinned quads there);
-- every step starts from the tiers and the antialiasing the save had. Each step settles, then "[qa] tree begin <n>" ...
-- "[qa] tree shot <n> <step>" brackets the r__gpu_log lines that average it; r__trees_stats and the shot follow. A pinned July noon in clear weather, no wind.
--   qa_tree_plan = "0;10;20;30;20|r__trees_force_tier 1;20|r__trees_force_tier 2;20|r__trees_force_tier 3"
--   qa_tree_up = 0.3   metres over the eye's height
--
--   tools\qa\Run-WaterQa.ps1 -Name tree-look -Scenario tree_look -Save trees -KeepShaderCache -Seconds 120
local c = get_console()
local stage, t0, frames = "start", 0, 0
local eye, dir0, aa0
local plan, step, shots = {}, 0, 0

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function place(fwd)
	local flat = vector():set(dir0.x, 0, dir0.z):normalize()
	local pos = vector():set(eye.x + flat.x * fwd, eye.y + (qa_tree_up or 0.3), eye.z + flat.z * fwd)
	level.set_cam_custom_position_direction(pos, vector():set(dir0:getH(), -0.03, 0), 0, false, false)
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
		cmd("wind_force 0")
		to_hour(13)
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		aa0 = c:get_token("r__aa")
		cmd("r__gpu_log 30")
		for item in string.gmatch(qa_tree_plan or "0;10;20;30;20|r__trees_force_tier 1;20|r__trees_force_tier 2;20|r__trees_force_tier 3", "[^;]+") do
			local s = { cmds = {}, label = item }
			for part in string.gmatch(item, "[^|]+") do
				if not s.fwd then s.fwd = tonumber(part) or 0 else s.cmds[#s.cmds + 1] = part end
			end
			plan[#plan + 1] = s
		end
		say("[qa] tree level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset") .. " aa " .. aa0)
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		cmd("r__trees_force_tier -1")
		cmd("r__aa " .. aa0)
		for _, x in ipairs(s.cmds) do cmd(x) end
		place(s.fwd)
		go("settle")
	elseif stage == "settle" and now - t0 > 3000 then
		say(string.format("[qa] tree begin %02d", shots + 1))
		go("measure")
	elseif stage == "measure" and now - t0 > 2000 then
		shots = shots + 1
		say(string.format("[qa] tree shot %02d %s", shots, plan[step].label))
		cmd("r__trees_stats")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		cmd("r__trees_force_tier -1")
		cmd("r__aa " .. aa0)
		cmd("r__gpu_log 0")
		level.remove_cam_custom_position_direction()
		say("[qa] tree DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
