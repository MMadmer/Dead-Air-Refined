-- The save indicator (CUISaveIndicator) in its three looks: a real save ("[qa] ind SAVE": saving, then the success),
-- three seconds of saving from dar_save_indicator_demo ("[qa] ind SPIN"), then a failure ("[qa] ind FAIL"). The runner
-- shoots the window in bursts on each marker (Capture-HiddenDesktopWindow.ps1).
--
--   tools\qa\Run-WaterQa.ps1 -Name ind -Scenario save_indicator -Save qa_meadow -KeepShaderCache -Seconds 60
local stage, t0, frames = "start", 0, 0
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		stage, t0 = "save", now
	elseif stage == "save" and now - t0 > 3000 then
		say("[qa] ind SAVE")
		cmd("save qa_indicator")
		stage, t0 = "spin", now
	elseif stage == "spin" and now - t0 > 9000 then
		say("[qa] ind SPIN")
		cmd("dar_save_indicator_demo saving")
		stage, t0 = "fail", now
	elseif stage == "fail" and now - t0 > 9000 then
		say("[qa] ind FAIL")
		cmd("dar_save_indicator_demo failed")
		stage, t0 = "done", now
	elseif stage == "done" and now - t0 > 8000 then
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
