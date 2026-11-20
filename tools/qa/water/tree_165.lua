-- Player report of 1.6.3: a fallen tree on Cordon swayed in the wind, the butt of its trunk too. The reporter's save, a
-- gale, the camera as it was saved; three shots 0.7 s apart ("[qa] tree SHOT nn"). Lying wood must hold still between
-- them while the standing trees around it move. The "[wind-veg] ... rigid, N lying" line counts what was taken out.
--
--   tools\qa\Run-WaterQa.ps1 -Name tree165 -Scenario tree_165 -Save tree165 -KeepShaderCache -Seconds 90 -ExtraArgs "-r5"
local stage, t0, frames, shots, shot_frame = "start", 0, 0, 0, -1
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot()
	shots = shots + 1
	say(string.format("[qa] tree SHOT %02d", shots))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
	if frames < 150 then return end
	local now = time_global()

	if stage == "start" then
		cmd("g_god on")
		cmd("hud_weapon 0")
		cmd("wind_force 1.25")
		level.set_cam_custom_position_direction(device().cam_pos,
			vector():set(device().cam_dir:getH(), device().cam_dir:getP(), 0), 0, false, false)
		stage, t0 = "settle", now
	elseif stage == "settle" and now - t0 > 4000 then
		shot()
		stage, t0 = "shots", now
	elseif stage == "shots" and now - t0 > 700 then
		shot()
		t0 = now
		if shots >= 3 then
			level.remove_cam_custom_position_direction()
			say("DA_WATER_PROBE_DONE")
			stage = "end"
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
