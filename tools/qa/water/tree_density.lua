-- The crowns' density from tier to tier (docs/dead-air/TREES.md, "Drawing"): the camera stands still (qa_tree_fwd metres
-- along the save's view, qa_tree_up over its eye, pitched qa_tree_pitch) and r__trees_lod_scale walks the tiers' distances through qa_tree_scales,
-- so every crown in view runs its tiers through at one size on the screen: the share of the sky its foliage covers in a
-- box should hold from one tier to the next. A shot a step, announced as "[qa] dens shot <n> <scale>". A pinned July noon
-- in clear weather, no wind.
--   qa_tree_fwd = 15
--   qa_tree_up = 0.3
--   qa_tree_pitch = 0.12
--   qa_tree_scales = "4,3,2.5,2,1.7,1.4,1.2,1,0.85,0.7,0.6,0.5,0.42,0.35,0.3,0.25,0.2,0.16,0.13,0.1"   (a step may carry
--                    commands after its scale, split by "|": "0.6|r__trees_bb_rebake")
--
--   tools\qa\Run-WaterQa.ps1 -Name tree-dens -Scenario tree_density -Save trees -KeepShaderCache -Seconds 150
local c = get_console()
local stage, t0, frames = "start", 0, 0
local plan, step, shots = {}, 0, 0

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
		cmd("wind_force 0")
		to_hour(13)
		local eye = vector():set(device().cam_pos)
		local dir0 = vector():set(device().cam_dir)
		local flat = vector():set(dir0.x, 0, dir0.z):normalize()
		local fwd = qa_tree_fwd or 15
		level.set_cam_custom_position_direction(vector():set(eye.x + flat.x * fwd, eye.y + (qa_tree_up or 0.3), eye.z + flat.z * fwd),
			vector():set(dir0:getH(), qa_tree_pitch or 0.12, 0), 0, false, false)
		for v in string.gmatch(qa_tree_scales or "4,3,2.5,2,1.7,1.4,1.2,1,0.85,0.7,0.6,0.5,0.42,0.35,0.3,0.25,0.2,0.16,0.13,0.1", "[^,]+") do
			plan[#plan + 1] = v
		end
		say("[qa] dens level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " .. c:get_token("_preset"))
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		-- A step may carry commands after its scale ("0.6|r__trees_bb_rebake").
		local first = true
		for part in string.gmatch(s, "[^|]+") do
			cmd(first and ("r__trees_lod_scale " .. part) or part)
			first = false
		end
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		shots = shots + 1
		say(string.format("[qa] dens shot %02d %s", shots, plan[step]))
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		cmd("r__trees_lod_scale 1")
		level.remove_cam_custom_position_direction()
		say("[qa] dens DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
