-- Typing into the open debug menu does not play the game: the runner presses keys bound to weapon slots, the
-- inventory and a few letters on "[qa] dmt READY" (Send-HiddenDesktopKey.ps1) and the scenario checks the hands, the
-- inventory and the scripts' key callbacks (on_key_press, where the game's and the mods' hotkeys live) after, then
-- closes the window and checks that the same keys draw a weapon and reach the scripts again:
-- "[qa] dmt <check> PASS|FAIL".
--
--   tools\qa\Run-WaterQa.ps1 -Name dmt -Scenario debug_menu_typing -Save qa_meadow -KeepShaderCache -Seconds 60
local stage, t0, frames, presses = "start", 0, 0, 0
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function on_key_press()
	presses = presses + 1
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		cmd("dar_debug_menu 1")
		db.actor:activate_slot(0)
		stage, t0 = "open", now
	elseif stage == "open" and now - t0 > 2500 then
		cmd("dar_debug_menu_open")
		stage, t0 = "ready", now
	elseif stage == "ready" and now - t0 > 1500 then
		presses = 0
		say("[qa] dmt READY slot " .. db.actor:active_slot())
		stage, t0 = "typed", now
	elseif stage == "typed" and now - t0 > 5000 then
		cmd("dar_debug_menu_do state")
		local window = ActorMenu.get_actor_menu():IsShown()
		say("[qa] dmt open_typing " .. ((db.actor:active_slot() == 0 and not window and presses == 0) and "PASS" or "FAIL") ..
			" slot " .. db.actor:active_slot() .. " inventory " .. tostring(window) .. " script_presses " .. presses)
		cmd("dar_debug_menu_open")
		stage, t0 = "closed", now
	elseif stage == "closed" and now - t0 > 1500 then
		presses = 0
		say("[qa] dmt READY2")
		stage, t0 = "after", now
	elseif stage == "after" and now - t0 > 4000 then
		say("[qa] dmt closed_keys " .. ((db.actor:active_slot() ~= 0 and presses > 0) and "PASS" or "FAIL") .. " slot " ..
			db.actor:active_slot() .. " script_presses " .. presses)
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
	RegisterScriptCallback("on_key_press", on_key_press)
end
