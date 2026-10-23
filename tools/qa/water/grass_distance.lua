-- The grass distance tiers (docs/dead-air/GRASS.md, "Distance"): for each quality preset of qa_tiers the preset and
-- the grass row of the same tier are set, the video restarts (the detail radius is read at the details' reload), and
-- the eye and a high view each log five r__gpu_stats samples and r__grass_blades_stats, announced as
-- "[qa] gdist <tier> <view>", and take a shot. A pinned July noon in clear weather, so two builds compare.
--   qa_tiers = "2,3,4"   the tiers, 0..4 (all five by default)
--   qa_dist_up = 35      how far above the eye the high view is (m)
--   qa_variants = "r__detail_radius 266;r__detail_radius 300"   after the last tier, each command in turn (live,
--                        no restart) and the two views again, announced as tier "v<n>"
--   qa_plan = "4;r__detail_radius 300|r__dbg_grass 256;3"   instead of both: tiers and variants in any order, a
--                        variant's commands split by "|"
--
--   tools\qa\Run-WaterQa.ps1 -Name gdist-new -Scenario grass_distance -Save qa_meadow -KeepShaderCache -Seconds 150
local c = get_console()
local presets = { [0] = "Minimum", "Low", "Default", "High", "Extreme" }
local tiers_tokens = { [0] = "minimum", "low", "medium", "high", "maximum" }
local stage, t0, frames = "start", 0, 0
local eye, dir0
local up = qa_dist_up or 35
local plan, step, samples = {}, 0, 0

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function place(view)
	if view == "eye" then
		level.set_cam_custom_position_direction(eye, vector():set(dir0:getH(), -0.08, 0), 0, false, false)
	else
		level.set_cam_custom_position_direction(vector():set(eye.x, eye.y + up, eye.z),
			vector():set(dir0:getH(), -0.2, 0), 0, false, false)
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
		level.set_weather(qa_water_weather or "af3_slight_clear_steady_3", true)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_time_factor(0.0001)
		cmd("g_god on")
		cmd("hud_weapon 0")
		cmd("r__season july")
		cmd("wind_force 0.3")
		to_hour(13)
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		local n = 0
		if qa_plan then
			for item in string.gmatch(qa_plan, "[^;]+") do
				if string.match(item, "^%s*[0-9]%s*$") then
					plan[#plan + 1] = tonumber(item)
				else
					n = n + 1
					plan[#plan + 1] = { label = "v" .. n, cmd = item }
				end
			end
		else
			for t in string.gmatch(qa_tiers or "0,1,2,3,4", "[0-9]") do
				plan[#plan + 1] = tonumber(t)
			end
			for v in string.gmatch(qa_variants or "", "[^;]+") do
				n = n + 1
				plan[#plan + 1] = { label = "v" .. n, cmd = v }
			end
		end
		say("[qa] gdist level " .. level.name() .. " renderer " .. c:get_string("renderer"))
		go("tier")
	elseif stage == "tier" then
		step = step + 1
		local t = plan[step]
		if not t then
			go("done")
			return
		end
		if type(t) == "table" then
			say("[qa] gdist variant " .. t.label .. ": " .. t.cmd)
			for one in string.gmatch(t.cmd, "[^|]+") do
				cmd(one)
			end
			go("restarted")
			return
		end
		cmd("_preset " .. presets[t])
		-- The build under test knows one of the two rows; the other answers "unknown command".
		cmd("r__grass_density grass_density_" .. tiers_tokens[t])
		cmd("r__grass_distance grass_distance_" .. tiers_tokens[t])
		-- A tier starts clean of the variants before it.
		cmd("r__dbg_grass 0")
		cmd("vid_restart")
		go("restarted")
	elseif stage == "restarted" and now - t0 > 2500 then
		place("eye")
		go("settle_eye")
	elseif (stage == "settle_eye" or stage == "settle_high") and now - t0 > 3500 then
		samples = 0
		go(stage == "settle_eye" and "sample_eye" or "sample_high")
	elseif (stage == "sample_eye" or stage == "sample_high") and now - t0 > 400 then
		samples = samples + 1
		cmd("r__gpu_stats")
		t0 = now
		if samples >= 5 then
			local view = stage == "sample_eye" and "eye" or "high"
			local t = plan[step]
			say("[qa] gdist " .. (type(t) == "table" and t.label or t) .. " " .. view)
			cmd("r__grass_blades_stats")
			cmd("r__screenshot_every 1")
			go(view == "eye" and "shot_eye" or "shot_high")
		end
	elseif stage == "shot_eye" or stage == "shot_high" then
		cmd("r__screenshot_every 0")
		if stage == "shot_eye" then
			place("high")
			go("settle_high")
		else
			go("tier")
		end
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		say("[qa] gdist DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
