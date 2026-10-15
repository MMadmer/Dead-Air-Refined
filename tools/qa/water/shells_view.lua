-- Spent cases seen up close: a burst from the player's rifle on hard ground in daylight, a 60 fps capture of
-- the cases leaving the port, then the script camera over the spot where they came down.
--
--   tools\qa\Run-WaterQa.ps1 -Name shellsview -Scenario shells_view -Save pripyat -KeepShaderCache -Seconds 150
local stage, t0, frames = 0, 0, 0
local shot_frame = -1
local shots = 0

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function shot(label)
	shots = shots + 1
	say(string.format("[qa] SHOT %02d %s", shots, label))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function act(name, on)
	if on then level.press_action(level.action_id(name)) else level.release_action(level.action_id(name)) end
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		for _, sec in ipairs({ "wpn_ak74", "ammo_5.45x39_fmj", "ammo_5.45x39_fmj" }) do
			alife():create(sec, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		end
		t0 = now stage = 1
	elseif stage == 1 and after(1500) then
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "wpn_ak74" then db.actor:move_to_slot(item, 3) end
		end, nil)
		t0 = now stage = 2
	elseif stage == 2 and after(700) then
		db.actor:activate_slot(3)
		t0 = now stage = 3
	elseif stage == 3 and after(3000) then
		cmd("r__capture 0.8 60")
		act("wpn_fire", true)
		t0 = now stage = 4
	elseif stage == 4 and after(600) then
		act("wpn_fire", false)
		t0 = now stage = 5
	elseif stage == 5 and after(3000) then
		cmd("qa_shells")
		-- the grass laid flat and the weapon out of the frame, or the cases hide under the blades
		cmd("r__detail_height 0.1")
		cmd("hud_weapon 0")
		cmd("qa_shells look 0.5")
		t0 = now stage = 6
	elseif stage == 6 and after(1200) then
		shot("camera 0.5 m over the cases")
		cmd("qa_shells")
		t0 = now stage = 7
	elseif stage == 7 and after(500) then
		cmd("qa_shells look 1.2")
		t0 = now stage = 8
	elseif stage == 8 and after(1200) then
		shot("camera 1.2 m over the cases")
		t0 = now stage = 10
	elseif stage == 10 and after(800) then
		level.remove_cam_custom_position_direction()
		cmd("r__detail_height 1")
		cmd("hud_weapon 1")
		cmd("qa_shells")
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 11
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
