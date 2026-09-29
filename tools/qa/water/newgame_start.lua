-- A new game leaves fake_start for the start the new-game screen picked: the transition's
-- autosave must go through (under protected mode too). Set [character_creation]
-- new_game_faction / new_game_faction_position in the rig's axr_options.ltx, then start
-- server(all/single/alife/new) with this script as qa_water.script.
local first_at = nil

local function finish(ok, detail)
	printf("[qa] newgame-start %s %s", ok and "PASS" or "FAIL", tostring(detail))
	printf("DA_WATER_PROBE_DONE")
	get_console():execute("flush")
end

local function on_first_update()
	local name = level.name()
	printf("[qa] first update on %s", name)
	if name ~= "fake_start" then
		finish(true, name)
	else
		first_at = time_global()
	end
end

-- Still on fake_start long after the start was asked for: the transition was cancelled.
local function on_update()
	if first_at and time_global() - first_at > 25000 then
		first_at = nil
		finish(false, "still on fake_start")
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_first_update", on_first_update)
	RegisterScriptCallback("actor_on_update", on_update)
end
