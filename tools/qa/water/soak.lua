-- A report's save left to run (docs/dead-air/TEST_MATRIX.md): the save loads, the game runs unattended for a while with
-- the AI and the simulation going, then the scenario ends - a crash on the way is the finding.
--   qa_soak_ms = 90000     how long the level runs after the first frames
--   qa_soak_inventory = false   run it with the inventory open (the actor menu's per-frame work)
--
--   tools\qa\Run-WaterQa.ps1 -Name army -Scenario soak -Save qa_army -Seconds 200
local frames, t0 = 0, nil

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if not t0 then
		t0 = now
		get_console():execute("g_god on")
		if qa_soak_inventory then
			local hud = get_hud()
			if hud then hud:ShowActorMenu() end
		end
		printf("[qa] soak start on " .. level.name())
		get_console():execute("flush")
		return
	end
	if now - t0 > (qa_soak_ms or 90000) then
		local am = ActorMenu.get_actor_menu()
		printf("[qa] soak done after " .. tostring(math.floor((now - t0) / 1000)) .. " s, inventory " ..
			tostring(am ~= nil and am:IsShown()))
		printf("DA_WATER_PROBE_DONE")
		get_console():execute("flush")
		UnregisterScriptCallback("actor_on_update", on_update)
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
