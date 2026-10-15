-- Spent cases: a burst from the player's rifle, the cases flying out of the port (a 30 fps capture), then the
-- camera turned down to the ground where they lie, then a walk through them.
--
--   tools\qa\Run-WaterQa.ps1 -Name shells -Scenario shells -Save rainhouse -KeepShaderCache -Seconds 120
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
		if not db.actor:item_in_slot(3) then
			for _, sec in ipairs({ "wpn_ak74", "ammo_5.45x39_fmj", "ammo_5.45x39_fmj" }) do
				alife():create(sec, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
			end
		end
		t0 = now stage = 11
	elseif stage == 11 and after(1500) then
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "wpn_ak74" and not db.actor:item_in_slot(3) then db.actor:move_to_slot(item, 3) end
		end, nil)
		t0 = now stage = 12
	elseif stage == 12 and after(700) then
		db.actor:activate_slot(3)
		t0 = now stage = 1
	elseif stage == 1 and after(3000) then
		cmd("r__capture 2 30")
		act("wpn_fire", true)
		t0 = now stage = 2
	elseif stage == 2 and after(900) then
		act("wpn_fire", false)
		t0 = now stage = 3
	elseif stage == 3 and after(2500) then
		cmd("qa_shells")
		-- down to the ground on the right, where they went
		cmd("cam_yaw_rotate 60")
		cmd("cam_pitch_rotate 55")
		t0 = now stage = 4
	elseif stage == 4 and after(1200) then
		shot("cases on the ground")
		cmd("qa_shells")
		t0 = now stage = 5
	elseif stage == 5 and after(800) then
		cmd("cam_yaw_rotate -60")
		act("right", true)
		t0 = now stage = 6
	elseif stage == 6 and after(1400) then
		act("right", false)
		t0 = now stage = 7
	elseif stage == 7 and after(1500) then
		cmd("cam_yaw_rotate 60")
		t0 = now stage = 8
	elseif stage == 8 and after(1000) then
		shot("after walking through")
		cmd("qa_shells")
		t0 = now stage = 9
	elseif stage == 9 and after(1000) then
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 10
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
