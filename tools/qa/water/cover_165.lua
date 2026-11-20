-- Player reports of 1.6.3: five crashes on a level change in CCoverEvaluatorCloseToEnemy::setup - a script's
-- npc:best_cover got a vector luabind no longer held, as a null reference. The cover calls take pointers now and refuse
-- a missing position. One stalker near the actor is asked for covers with real vectors, then without them; the run
-- has to reach the marker and log "[qa] cover ...".
--
--   tools\qa\Run-WaterQa.ps1 -Name cover165 -Scenario cover_165 -Save glowsq -KeepShaderCache -Seconds 60 -ExtraArgs "-r5"
local stage, frames = "start", 0
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function nearest_stalker()
	local best, best_d
	for id = 1, 65534 do
		local o = level.object_by_id(id)
		if o and IsStalker(o) and o:id() ~= db.actor:id() and o:alive() then
			local d = o:position():distance_to_sqr(db.actor:position())
			if not best_d or d < best_d then best, best_d = o, d end
		end
	end
	return best, best_d
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 150 or stage ~= "start" then return end
	stage = "end"
	local npc, d = nearest_stalker()
	if not npc then
		say("[qa] cover no stalker online")
	else
		say(string.format("[qa] cover npc %s at %.0f m", npc:name(), math.sqrt(d)))
		local real = npc:best_cover(npc:position(), db.actor:position(), 20, 1, 30)
		say("[qa] cover real " .. tostring(real and real:level_vertex_id()))
		local no_enemy = npc:best_cover(npc:position(), nil, 20, 1, 30)
		say("[qa] cover no_enemy " .. tostring(no_enemy))
		local no_pos = npc:safe_cover(nil, 20, 1)
		say("[qa] cover safe_no_pos " .. tostring(no_pos))
		local found = npc:find_best_cover(nil)
		say("[qa] cover find_no_pos " .. tostring(found))
	end
	say("DA_WATER_PROBE_DONE")
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
