-- The bug report window opened and closed, then the game quits: the exit destroys the main menu with the window in
-- it. No marker - the probe sees the process end; a crash leaves its stack in the log.
--
--   tools\qa\Run-WaterQa.ps1 -Name bugexit -Scenario bugreport_exit -Save puddle -KeepShaderCache -Seconds 60
local stage, t0, frames = 0, 0, 0

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		cmd("dar_ui_dialog bug_report")
		t0 = now stage = 1
	elseif stage == 1 and after(2500) then
		-- closed as the player closes it, unless the scenario asks to quit with it shown
		if not qa_keep_shown then
			local hud = get_hud()
			if hud then hud:HideActorMenu() end
			level.press_action(level.action_id("quit"))
			level.release_action(level.action_id("quit"))
		end
		t0 = now stage = 2
	elseif stage == 2 and after(1500) then
		say("[qa] quitting")
		cmd("quit")
		stage = 3
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
