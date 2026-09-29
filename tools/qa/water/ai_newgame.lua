-- R6 ai-newgame (docs\dead-air\ai\PLAN.md 7.3): a new game. Dead Air starts it on fake_start and moves the player to
-- the start the new-game screen picked ([character_creation] new_game_faction / new_game_faction_position of the rig's
-- axr_options.ltx, which Invoke-AiRun.ps1 -NewGame sets). The probe polls the game itself until that level change
-- (with a ceiling), then lets the start level spawn its stalkers and prints the AI counters and every online stalker's
-- resolved profile: profile_default only for communities without a section, and a clean log.
--
--   tools\qa\ai\Invoke-AiRun.ps1 -Name r6-ai-newgame -Scenario ai_newgame -NewGame
--
-- The script runs twice: on fake_start, where it waits, and on the start level (a level change restarts the scripts).
local function cmd(s) get_console():execute(s) end

local t0
local function t_now() return t0 and (time_global() - t0) / 1000 or 0 end

local function say(fmt, ...)
	printf("%s", string.format("[qa-ai] t=%.2f ", t_now()) .. string.format(fmt, ...))
end

-- fake_start: with a start picked the move follows the first update by half a second; without one the actor keeps
-- the plain "actor" community and nothing will ever move him.
local FAKE_START_MAX_S, NO_FACTION_S, WAIT_LOG_S = 120, 15, 10
-- Start level: the online stalkers have spawned and resolved their profiles by then.
local SETTLE_S = 20

local on_fake_start, finished, quit_at, reported
local next_wait_log = 0

local function finish(verdict)
	if finished then return end
	finished = true
	say("done %s", verdict)
	printf("%s", "DA_WATER_PROBE_DONE")
	cmd("flush")
	quit_at = time_global() + 3000
end

local function actor_community()
	return db.actor and db.actor:character_community() or "-"
end

local function wait_step()
	local t = t_now()
	if t >= next_wait_log then
		next_wait_log = t + WAIT_LOG_S
		say("waiting level=%s actor_community=%s", level.name(), actor_community())
	end
	if t >= NO_FACTION_S and not string.find(actor_community(), "^actor_") then
		finish("fail no_start_faction")
	elseif t >= FAKE_START_MAX_S then
		finish("fail still_on_fake_start")
	end
end

local function online_stalkers()
	local list = {}
	for id = 1, 65534 do
		local obj = level.object_by_id(id)
		if obj and IsStalker(obj) and obj:alive() then list[#list + 1] = obj end
	end
	return list
end

-- Profile values go out without blanks ("700, 1000" -> "700,1000") so the parser reads them as one field.
local function profile_key(npc, key)
	local value = npc:ai_profile(key) or ""
	value = string.gsub(value, "%s+", "")
	return value ~= "" and value or "-"
end

local function report_step()
	if reported or t_now() < SETTLE_S then return end
	reported = true
	local list = online_stalkers()
	local per_community = {}
	for _, npc in ipairs(list) do
		local community = npc:character_community()
		per_community[community] = (per_community[community] or 0) + 1
		say("profile npc=%d comm=%s rank=%d react_ms=%s roles=%s return_fire_m=%s radio=%d", npc:id(), community,
			npc:character_rank(), profile_key(npc, "react_ms"), profile_key(npc, "roles"),
			profile_key(npc, "return_fire_m"), npc:ai_has_radio() and 1 or 0)
	end
	local parts = {}
	for community, count in pairs(per_community) do parts[#parts + 1] = community .. ":" .. count end
	table.sort(parts)
	say("newgame level=%s online_stalkers=%d communities=%s", level.name(), #list,
		#parts > 0 and table.concat(parts, ",") or "-")
	cmd("ai_stats")
	finish("complete")
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	device():pause(false)
	cmd("ai_log_period 5000")
	t0 = time_global()
	on_fake_start = level.name() == "fake_start"
	say("start scenario=ai_newgame level=%s actor_community=%s", level.name(), actor_community())
end

local function on_update()
	device():pause(false)
	if not t0 then return end
	if quit_at then
		if time_global() >= quit_at then
			quit_at = nil
			cmd("quit")
		end
		return
	end
	if finished then return end
	local ok, err = pcall(on_fake_start and wait_step or report_step)
	if not ok then
		say("error lua=%s", tostring(err))
		finish("error")
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_first_update", on_first_update)
	RegisterScriptCallback("actor_on_update", on_update)
end
