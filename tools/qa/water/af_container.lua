-- Does a quest see an artefact that is sitting in a container, and does taking it leave the
-- container behind? Run on the rig with the ordinary game scripts loaded, so the answers come
-- from the real check and the real removal and not from a copy of them.
--
-- Counts are deltas against what the save already carries: the rig's actor is a real character
-- with a real inventory, and a lead_box he already owns is not this test's business.
--
-- Not covered here, and checked by reading instead: the belt. A filled container can be belted,
-- and both halves of the rule exclude the belt - the engine scans m_ruck (da_artefact_container.cpp)
-- and the script asks is_on_belt - but Lua has no move_to_belt binding to drive it with.
local ART = "af_medusa"
local CON = "lead_box"
local FULL = ART .. "_" .. CON

local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local base = {}

local function cmd(s) get_console():execute(s) end

local function count(section)
	local n = 0
	db.actor:iterate_inventory(function(_, item)
		if (item and item:section() == section) then n = n + 1 end
		return false
	end, db.actor)
	return n
end

-- Everything is measured against the inventory as the save left it.
local function delta(section) return count(section) - (base[section] or 0) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("AFQA: %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local function spawn(section)
	local a = db.actor
	alife():create(section, a:position(), a:level_vertex_id(), a:game_vertex_id(), a:id())
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 60 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		for _, s in ipairs({ART, CON, FULL}) do base[s] = count(s) end
		printf(string.format("AFQA-info: baseline %s=%d %s=%d %s=%d",
			ART, base[ART], CON, base[CON], FULL, base[FULL]))
		spawn(FULL)
		t0 = now stage = 1

	elseif stage == 1 and after(3000) then
		-- The engine's stand-in: a quest asks for the artefact and gets the container holding it.
		local found = db.actor:object(ART)
		check("sees-through-container", found ~= nil, "object(" .. ART .. ")")
		check("stands-in-as-container", found ~= nil and found:section() == FULL,
			found and found:section() or "nil")
		check("in-the-rucksack", found ~= nil and not db.actor:is_on_belt(found), "is_on_belt=false")
		-- The counters, which walk the inventory themselves and never saw it before.
		check("counter-sees-it",
			xr_conditions.actor_has_item_count(db.actor, nil, {ART, "1"}) == true, "actor_has_item_count")
		check("count_all",
			dead_air_x64_af_container.count_all(ART) - (base[ART] or 0) == 1,
			tostring(dead_air_x64_af_container.count_all(ART)))
		t0 = now stage = 2

	elseif stage == 2 and after(500) then
		-- A quest takes it.
		xr_effects.remove_item(nil, nil, {ART, 1})
		t0 = now stage = 3

	elseif stage == 3 and after(3000) then
		check("container-emptied", delta(FULL) == 0, "filled delta " .. delta(FULL))
		check("container-kept", delta(CON) == 1, "empty delta " .. delta(CON))
		check("artefact-gone", delta(ART) == 0, "loose delta " .. delta(ART))
		check("no-longer-seen", db.actor:object(ART) == nil, "object(" .. ART .. ")")
		-- Now the priority: one loose and one packed, and only one asked for.
		spawn(ART)
		spawn(FULL)
		t0 = now stage = 4

	elseif stage == 4 and after(3000) then
		check("both-present", delta(ART) == 1 and delta(FULL) == 1,
			"loose " .. delta(ART) .. " packed " .. delta(FULL))
		xr_effects.remove_item(nil, nil, {ART, 1})
		t0 = now stage = 5

	elseif stage == 5 and after(3000) then
		check("loose-goes-first", delta(ART) == 0, "loose delta " .. delta(ART))
		check("packed-untouched", delta(FULL) == 1, "packed delta " .. delta(FULL))
		-- The hand-over: what a dialogue does, with somebody on the other end. The receiver here
		-- is the actor himself, which is the same code path and needs no NPC standing about.
		dead_air_x64_af_container.take_from_containers(ART, 1, db.actor:id())
		t0 = now stage = 6

	elseif stage == 6 and after(3000) then
		check("handed-over", delta(ART) == 1, "loose delta " .. delta(ART))
		check("handover-emptied", delta(FULL) == 0, "packed delta " .. delta(FULL))
		check("handover-kept-container", delta(CON) == 2, "empty delta " .. delta(CON))
		local art = db.actor:object(ART)
		check("handover-is-an-artefact",
			art ~= nil and art:section() == ART and art:clsid() == clsid.artefact_s,
			art and art:section() or "nil")
		-- Tidy up: the rig's save is reused.
		xr_effects.remove_item(nil, nil, {ART, 1})
		t0 = now stage = 7

	elseif stage == 7 and after(3000) then
		check("cleanup", delta(FULL) == 0 and delta(ART) == 0,
			"packed " .. delta(FULL) .. " loose " .. delta(ART))
		printf(string.format("AFQA: DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
