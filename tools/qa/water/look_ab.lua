-- The look's contributors one at a time, in one run at the eye under a pinned clear sky at 16:00 (the halo round the
-- sun, the washed-out shadows, the far haze of the 1.6.0 reports): all on, then the screen-space shafts off
-- (r2_sss_enable 0), the volumetric air off (r__froxel_strength 0), the bloom off (r__bloom 0), each announced as
-- "[qa] SHOT nn <label>" and switched back after its shot. qa_look_extra = { { label, on, off }, ... } adds pairs.
--
--   tools\qa\Run-WaterQa.ps1 -Name look -Scenario look_ab -Save qa_meadow -KeepShaderCache -Seconds 240
local stage, t0, frames = "start", 0, 0
local shots, shot_frame = 0, -1
local restore, restore_frame

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local plan = {
	{ "all_on", nil, nil },
	{ "sss_off", "r2_sss_enable 0", "r2_sss_enable 1" },
	{ "froxel_off", "r__froxel_strength 0", "r__froxel_strength 1" },
	{ "bloom_off", "r__bloom 0", "r__bloom 0.3" },
}
for _, e in ipairs(qa_look_extra or {}) do plan[#plan + 1] = e end

local step = 0

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	-- The switch goes back only after the shot's frame is drawn.
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
		restore_frame = frames
	end
	if restore and restore_frame and frames > restore_frame then
		cmd(restore)
		restore, restore_frame = nil, nil
	end
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		cmd("g_god on")
		level.set_time_factor(0.0001)
		cmd("hud_weapon 0")
		cmd("r2_tonemap_adaptation 10")
		to_hour(qa_look_hour or 16)
		cmd("qa_climate_force clear_steady 0 20")
		level.set_weather("af3_slight_clear_steady_3", true)
		say("[qa] look renderer " .. get_console():get_string("renderer") .. " preset " .. get_console():get_token("_preset"))
		stage, t0 = "next", now
	elseif stage == "next" and now - t0 > 2500 then
		step = step + 1
		local s = plan[step]
		if not s then
			say("[qa] look DONE shots=" .. shots)
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		if s[2] then cmd(s[2]) end
		stage, t0 = "settle", now
	elseif stage == "settle" and now - t0 > 2000 then
		local s = plan[step]
		shots = shots + 1
		say(string.format("[qa] SHOT %02d %s", shots, s[1]))
		cmd("r__screenshot_every 1")
		shot_frame = frames
		restore = s[3]
		stage, t0 = "next", now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
