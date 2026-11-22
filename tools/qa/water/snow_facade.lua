-- The snow by the walls of tall buildings (docs/dead-air/SNOW.md): a deep snowpack, the player turned to the
-- save's heading plus qa_sf_heading degrees, one shot per r__dbg_snow view ("[qa] SHOT dbg<N>", NN.jpg in order).
--
--   tools\qa\Run-WaterQa.ps1 -Name snow-facade -Scenario snow_facade -Save pripyat -KeepShaderCache -Seconds 200
--        -ExtraArgs '-r5 -always_active' -Globals 'qa_sf_depth = 0.6'
--
-- Header globals: qa_sf_depth (0.6 m), qa_sf_ice (still water's ice, m; none), qa_sf_heading (90),
-- qa_sf_views ({ 0, 1, 3, 4 }).
local c = get_console()
local function cmd(s) c:execute(s) end
local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local views = qa_sf_views or { 0, 1, 3, 4 }
local vi, phase, t0, frames = 1, "start", 0, 0

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	device():pause(false)
	if frames == 1 then
		level.set_time_factor(0.0001)
		to_hour(13)
		level.set_weather("af3_slight_clear_steady_3", true)
		cmd("qa_climate_force clear_steady 0 20")
		cmd("g_god on")
	end
	if frames == 30 then
		cmd(string.format("qa_snowpack %.2f 0", qa_sf_depth or 0.6))
		if qa_sf_ice then cmd(string.format("qa_ice %.2f", qa_sf_ice)) end
		db.actor:set_actor_direction(-(db.actor:direction():getH() + math.rad(qa_sf_heading or 90)))
		db.actor:activate_slot(0)
	end
	if frames < 300 then return end
	local now = time_global()
	if phase == "start" then
		phase, t0 = "view", now
	elseif phase == "view" then
		local v = views[vi]
		if not v then
			cmd("r__dbg_snow 0")
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		cmd("r__dbg_snow " .. v)
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 2500 then
		printf(string.format("[qa] SHOT dbg%d", views[vi]))
		cmd("flush")
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		vi = vi + 1
		phase, t0 = "view", now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
