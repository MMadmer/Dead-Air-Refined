-- The hang watchdog (src/xrEngine/da_watchdog.cpp): the main thread blocks 20 s in a C call (os.execute waiting on a
-- process), the watchdog should log "! [watchdog] no frame" with its stack and write dar-report-hang-*.zip; the game
-- then goes on and stays 30 s more so the report is finished.
--
--   tools\qa\Run-WaterQa.ps1 -Name watchdog -Scenario watchdog_hang -Save qa_meadow -KeepShaderCache -Seconds 60
local stage, frames, t_back = "start", 0, 0
local function say(s) printf(s) get_console():execute("flush") end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 or stage == "end" then return end
	if stage == "start" then
		say("[qa] watchdog blocking 20 s")
		os.execute("ping -n 21 127.0.0.1 >nul")
		say("[qa] watchdog back")
		stage, frames = "wait", 120
		t_back = time_global()
	elseif stage == "wait" and time_global() - t_back > 30000 then
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
