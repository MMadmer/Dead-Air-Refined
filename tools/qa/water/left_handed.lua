-- The left-handed hud (hud_left_handed): a rifle drawn (a frame), three shots (a frame of the cases
-- leaving on the left), a detector beside it (a frame: in the right hand, its screen as it reads),
-- the 3D PDA (a frame: in the other hand, its screen as it reads), then the same rifle and
-- detector with the flag off for the side-by-side look. Window frames: tools\qa\Watch-QaShots.ps1.
--
--   tools\qa\Run-WaterQa.ps1 -Name left-handed -Scenario left_handed -Save qa_meadow -KeepShaderCache
--        -ExtraArgs '-no_shader_pack' -PreCommands 'rs_always_active on'
local stage, t0, frames = 0, 0, 0
local pass = 1
local function cmd(s) get_console():execute(s) end
local function act(name) return level.action_id(name) end
local function shot(name)
	printf("[qa] SHOT %s_%s", pass == 1 and "left" or "right", name)
	cmd("flush")
end
local function tap(name)
	level.press_action(act(name))
	level.release_action(act(name))
end
local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end
local function spawn(section)
	alife():create(section, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	device():pause(false)
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_clear_4", true)
		cmd("qa_climate_force clear_steady 0 20")
		to_hour(13)
		level.set_time_factor(0.0001)
		cmd("g_god on")
		cmd("hud_left_handed 1")
		spawn("wpn_ak74")
		spawn("detector_elite")
		t0 = now stage = 1
	elseif stage == 1 and after(800) then
		local w = db.actor:object("wpn_ak74")
		if w then db.actor:move_to_slot(w, 3) end
		local d = db.actor:object("detector_elite")
		if d then db.actor:move_to_slot(d, 9) end
		db.actor:activate_slot(3)
		t0 = now stage = 2
	elseif stage == 2 and after(2500) then
		shot("rifle")
		t0 = now stage = 3
	elseif stage == 3 and after(1500) then
		level.press_action(act("wpn_fire"))
		t0 = now stage = 4
	elseif stage == 4 and after(260) then
		level.release_action(act("wpn_fire"))
		shot("fire")
		t0 = now stage = 5
	elseif stage == 5 and after(1500) then
		tap("show_detector")
		t0 = now stage = 6
	elseif stage == 6 and after(2500) then
		cmd("qa_hands_state")
		shot("detector")
		t0 = now stage = 7
	elseif stage == 7 and after(1500) then
		tap("show_detector")
		t0 = now stage = 8
	elseif stage == 8 and after(1500) then
		if pass == 1 then
			local pda = ActorMenu.get_pda_menu()
			if pda then pda:ShowDialog(true) end
		end
		t0 = now stage = 9
	elseif stage == 9 and after(2500) then
		if pass == 1 then
			cmd("qa_hands_state")
			shot("pda")
		end
		t0 = now stage = 10
	elseif stage == 10 and after(1500) then
		if pass == 1 then get_hud():HidePdaMenu() end
		t0 = now stage = 11
	elseif stage == 11 and after(2000) then
		if pass == 1 then
			pass = 2
			cmd("hud_left_handed 0")
			stage = 2
			t0 = now
		else
			printf("DA_WATER_PROBE_DONE") cmd("flush")
			stage = 12
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
