-- The shrubs in a local light's shadow map (docs/dead-air/SHRUBS.md, "The GPU cut"): at night a hand flashlight
-- (device_flashlight, a shadowed spot) lies on the ground a few metres ahead of the save's eye; for each mode of the plan
-- the populations' stats ("... shadowed local lights cast vegetation this frame ..., N chunks of shrubs") and a shot.
-- With the GPU cut on, the bushes in the spot's reach must cast as many chunks as with it off.
--   qa_light_modes = "cut:r__shrubs_cut 1;nocut:r__shrubs_cut 0"   label:commands (split by "|")
--
--   tools\qa\Run-WaterQa.ps1 -Name veg-light -Scenario veg_light -Save trees -KeepShaderCache -Seconds 120
local stage, t0, frames = "start", 0, 0
local plan, step = {}, 0
local lamp

local function cmd(s) get_console():execute(s) end
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
		cmd("g_god on")
		cmd("hud_weapon 0")
		level.set_time_factor(0.0001)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_weather("af3_slight_clear_steady_3", true)
		to_hour(23)
		local eye, dir = vector():set(device().cam_pos), vector():set(device().cam_dir)
		dir.y = 0
		dir:normalize()
		local at = vector():set(eye.x + dir.x * 3, eye.y - 1.0, eye.z + dir.z * 3)
		lamp = alife():create("device_flashlight", at, level.vertex_id(at), db.actor:game_vertex_id())
		for item in string.gmatch(qa_light_modes or "cut:r__shrubs_cut 1;nocut:r__shrubs_cut 0", "[^;]+") do
			local label, cmds = string.match(item, "^%s*([^:]+):(.*)$")
			plan[#plan + 1] = { label = label, cmds = cmds or "" }
		end
		say(string.format("[qa] veg_light level %s lamp at %.1f %.1f %.1f", level.name(), at.x, at.y, at.z))
		go("next")
	elseif stage == "next" and now - t0 > 2000 then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		for one in string.gmatch(s.cmds, "[^|]+") do
			cmd(one)
		end
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		say("[qa] veg_light " .. plan[step].label)
		cmd("r__shrubs_stats")
		cmd("r__trees_stats")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		-- (The screenshot's file name has a one-second resolution.)
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		say("[qa] veg_light DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
