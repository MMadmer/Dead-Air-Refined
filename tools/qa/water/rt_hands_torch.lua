-- Ray traced sun shadows indoors: the hands take the sun the eye sees, and the hand flashlight lights.
-- A/B in one run: RT shadows on, then off, each with the torch off and on.
--
--   tools\qa\Run-WaterQa.ps1 -Name rt -Scenario rt_hands_torch -Save rainhouse -KeepShaderCache -Seconds 120 -ExtraArgs "-r5"
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

-- The hand flashlight of the left hand (Dead Air lights it through the actor's torch).
local function torch(on)
	if on then db.actor:restore_detector(false) else db.actor:hide_detector(false) end
	local t = db.actor:item_in_slot(9)
	say("[qa] flashlight " .. tostring(t and t:section()) .. " -> " .. tostring(on))
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
		alife():create("device_flashlight", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		alife():create("wpn_ak74", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		cmd("r__rt_shadows 1")
		t0 = now stage = 1
	elseif stage == 1 and after(3000) then
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "device_flashlight" then db.actor:move_to_slot(item, 9) end
			if item:section() == "wpn_ak74" then db.actor:move_to_slot(item, 3) end
		end, nil)
		t0 = now stage = 11
	elseif stage == 11 and after(800) then
		db.actor:activate_slot(3)
		t0 = now stage = 12
	elseif stage == 12 and after(2500) then
		shot("rt on, torch off")
		t0 = now stage = 2
	elseif stage == 2 and after(1500) then
		torch(true)
		t0 = now stage = 3
	elseif stage == 3 and after(2500) then
		shot("rt on, torch on")
		t0 = now stage = 4
	elseif stage == 4 and after(800) then
		cmd("r__rt_shadows 0")
		t0 = now stage = 5
	elseif stage == 5 and after(2000) then
		shot("rt off, torch on")
		torch(false)
		t0 = now stage = 6
	elseif stage == 6 and after(1500) then
		shot("rt off, torch off")
		t0 = now stage = 7
	elseif stage == 7 and after(1000) then
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
