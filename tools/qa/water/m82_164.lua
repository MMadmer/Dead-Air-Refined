-- Player report: drawing the M82 of Revolution II stops the game ("Old skinned model version unsupported" on its hud
-- model). The rifle is spawned, put in its slot and drawn; if the run reaches the marker the model loads.
--
--   tools\qa\Run-WaterQa.ps1 -Name m82 -Scenario m82_164 -Save glowsq -KeepShaderCache -Seconds 60 -ExtraArgs "-r5"
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
		say("[qa] m82 section " .. tostring(system_ini():section_exist("wpn_m82")))
		alife():create("wpn_m82", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		stage, t0 = "slot", now
	elseif stage == "slot" and now - t0 > 2000 then
		local rifle = db.actor:object("wpn_m82")
		say("[qa] m82 spawned " .. tostring(rifle ~= nil))
		if rifle then
			db.actor:move_to_slot(rifle, 3)
			db.actor:activate_slot(3)
		end
		stage, t0 = "held", now
	elseif stage == "held" and now - t0 > 6000 then
		local a = db.actor:active_item()
		say("[qa] m82 active " .. (a and a:section() or "-") .. " slot " .. db.actor:active_slot())
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
