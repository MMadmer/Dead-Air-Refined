-- The 3D PDA in the outfit's own hands. For each hands rig - the save's own, the SEVA gloves, the
-- exoskeleton - the device is drawn (a frame mid-draw), held (a frame at rest), raised to the face
-- with the fire key held (a frame in the zoom), the headlamp is switched (a frame of the
-- acknowledgement) and the device is put away. Every frame is a window shot for a side-by-side
-- look at the fingers; the log is checked for motions the hands lack.
--
--   tools\qa\Run-WaterQa.ps1 -Name pda-hands -Scenario pda_hands -Save qa_meadow -KeepShaderCache
--        -PreCommands 'rs_always_active on'      (window frames: tools\qa\Watch-QaShots.ps1)
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local rigs, ri = { "", "actor_hud_loner_seva", "actor_hud_exo" }, 0
local tag, holding = "", false

local function cmd(s) get_console():execute(s) end

local function to_hour(hour)
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local delta = ((hour - h) * 60 - m + 24 * 60) % (24 * 60)
	level.change_game_time(0, math.floor(delta / 60), delta % 60)
end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local function shot(name)
	printf("[qa] SHOT %s_%s", tag, name)
	cmd("flush")
end

-- The PDA key, as CUIGameCustom::ShowPdaMenu takes it.
local function pda_key()
	local pda = ActorMenu.get_pda_menu()
	if not (pda and pda:IsShown()) then get_hud():HideActorMenu() end
	if pda then pda:ShowDialog(true) end
end

local function pda_up()
	return db.actor:active_slot() == 13
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	device():pause(false)
	local now = time_global()
	local function after(ms) return now - t0 > ms end
	if holding then level.hold_action(level.action_id("wpn_fire")) end

	if stage == 0 then
		if ri == 0 then
			-- Daylight and a still sky: the hands are what is looked at.
			level.set_weather(qa_water_weather or "af3_bright_clear_4", true)
			cmd("qa_climate_force clear_steady 0 20")
			to_hour(13)
			level.set_time_factor(0.0001)
			cmd("g_god on")
		end
		ri = ri + 1
		local rig = rigs[ri]
		tag = (rig == "") and "own" or rig:gsub("^actor_hud_", "")
		if rig ~= "" then cmd("qa_hands_rig " .. rig) end
		cmd("qa_hands_state")
		t0 = now stage = 1
	elseif stage == 1 and after(1200) then
		pda_key()
		t0 = now stage = 2
	elseif stage == 2 and after(450) then
		shot("draw")
		t0 = now stage = 3
	elseif stage == 3 and after(2200) then
		check(tag .. "-raised", pda_up(), "slot " .. tostring(db.actor:active_slot()))
		cmd("qa_hands_state")
		shot("idle")
		t0 = now stage = 31
	-- The window grab lands a moment after the marker: nothing moves until it is surely taken.
	elseif stage == 31 and after(1500) then
		level.press_action(level.action_id("wpn_fire"))
		holding = true
		t0 = now stage = 4
	elseif stage == 4 and after(1400) then
		cmd("qa_hands_state")
		shot("zoom")
		t0 = now stage = 5
	elseif stage == 5 and after(1500) then
		holding = false
		level.release_action(level.action_id("wpn_fire"))
		t0 = now stage = 6
	elseif stage == 6 and after(1200) then
		level.press_action(level.action_id("torch"))
		level.release_action(level.action_id("torch"))
		t0 = now stage = 7
	elseif stage == 7 and after(450) then
		shot("flash")
		t0 = now stage = 8
	elseif stage == 8 and after(1500) then
		level.press_action(level.action_id("torch"))
		level.release_action(level.action_id("torch"))
		get_hud():HidePdaMenu()
		t0 = now stage = 9
	elseif stage == 9 and after(1800) then
		check(tag .. "-lowered", not pda_up(), "slot " .. tostring(db.actor:active_slot()))
		if ri < #rigs then
			stage = 0
		else
			printf(string.format("[qa] DONE %d/%d", passed, total))
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 10
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
