-- Player reports of 1.6.1: the game went down opening the inventory or picking a sort type (an ellipsis that overran
-- its buffer in a narrow combo box row). One short run: the inventory opened, held, closed and opened again.
--
--   tools\qa\Run-WaterQa.ps1 -Name inv162 -Scenario inv_162 -Save qa_inv -KeepShaderCache -Seconds 120
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
		cmd("g_god on")
		get_hud():ShowActorMenu()
		say("[qa] inv162 inventory open")
		stage, t0 = "open", now
	elseif stage == "open" and now - t0 > 3000 then
		get_hud():HideActorMenu()
		stage, t0 = "closed", now
	elseif stage == "closed" and now - t0 > 1000 then
		get_hud():ShowActorMenu()
		say("[qa] inv162 inventory open again")
		stage, t0 = "again", now
	elseif stage == "again" and now - t0 > 3000 then
		get_hud():HideActorMenu()
		say("[qa] inv162 DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
