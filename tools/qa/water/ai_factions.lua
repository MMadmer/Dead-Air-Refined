-- R2 ai-factions (docs\dead-air\ai\PLAN.md 7.3): three simultaneous fights around the actor, each within 150 m
-- of him and at least 150 m from the others - army vs bandit novices (two of them on double-barrels), Monolith vs
-- loners, mercs vs Duty - with every squad topped up to 4, so at least 24 stalkers fight. The actor is ignored
-- (ai_ignore_actor 1). This run is also the performance pass (p_* counters, r__fps_log 5).
--
--   tools\qa\Run-WaterQa.ps1 -Name ai-factions -Scenario ai_factions -Save fogqa -Seconds 150 `
--       -ExtraArgs "-force_flushlog -always_active" -Out "D:\Games\Dead Air\_work\ai-npc\qa"
--
-- Logged per side every 5 s: alive count, the combat sub-planner's action histogram, the share of samples in cover
-- (toward the best enemy of any kind, and B4's fight share: toward a living member of the paired side, broken or
-- surrendered members left out), NPCs broken or surrendered; per NPC: its radio, morale changes, every shot with its
-- weapon class and distance, belief changes on the enemies, reports received, deaths, a best enemy outside the probe's
-- squads ("third"); every second the layout (a side farther than 200 m from the actor or a fight drifting into another
-- makes the run void). The fights keep clear of the level's own NPCs where the ground allows. After the fights (85 s)
-- the night muzzle-flash trial of B1 (see "After the fights"): night at 85, F fires at 86.3, done 89.5, quit ~92.5.
-- Lines "[qa-ai] t=<s> <event> key=value ..." feed tools\qa\ai\Parse-AiLog.py.

------------------------------------------------------------------------------------------------------------------------
-- Probe helpers (the same block in every ai_*.lua scenario: the harness copies one file to the rig)
------------------------------------------------------------------------------------------------------------------------
local function cmd(s) get_console():execute(s) end

local t0
local function t_now() return t0 and (time_global() - t0) / 1000 or 0 end

local function say(fmt, ...)
	printf("%s", string.format("[qa-ai] t=%.2f ", t_now()) .. string.format(fmt, ...))
end

local finished, quit_at
local function finish(verdict)
	if finished then return end
	finished = true
	say("done %s", verdict)
	printf("%s", "DA_WATER_PROBE_DONE")
	cmd("flush")
	quit_at = time_global() + 3000
end

local function bearing(deg)
	local r = math.rad(deg)
	return vector():set(math.sin(r), 0, math.cos(r))
end

local function flat_dist(a, b)
	local dx, dz = a.x - b.x, a.z - b.z
	return math.sqrt(dx * dx + dz * dz)
end

-- An open level vertex d metres from vid along the bearing, or nil when the level graph has none there. The graph cell
-- under the wanted spot comes first: a straight walk along the graph stops at every gap (water, a fence), which on a
-- marsh leaves almost no bearing reachable. level.vertex_id answers 4294967296 for "no vertex".
local function vertex_at(vid, deg, d)
	local origin = level.vertex_position(vid)
	local dir = bearing(deg)
	local found = level.vertex_id(vector():set(origin.x + dir.x * d, origin.y + 2, origin.z + dir.z * d))
	if not (found and found < 4294967295 and level.valid_vertex_id(found)) then
		found = level.vertex_in_direction(vid, dir, d)
	end
	if not found or found == vid or not level.valid_vertex_id(found) then return nil end
	local p = level.vertex_position(found)
	if math.abs(flat_dist(p, origin) - d) > math.max(3, d * 0.1) then return nil end
	return found, p
end

-- Static line of sight between two standing heads.
local function los(a, b)
	local from = vector():set(a.x, a.y + 1.6, a.z)
	local dir = vector():set(b.x, b.y + 1.6, b.z):sub(from)
	local dist = dir:magnitude()
	if dist < 0.5 then return true end
	dir:normalize()
	local r = rq_result()
	if level.ray_pick(from, dir, dist, rq_target.rqtStatic, r, nil) then
		return r.range >= dist - 0.3
	end
	return true
end

-- The first spot around the centre vertex (radii in order, bearings in 10 degree steps from start_deg) passing test.
local function find_spot(center_vid, radii, start_deg, test)
	for _, d in ipairs(radii) do
		for k = 0, 35 do
			local deg = (start_deg + k * 10) % 360
			local vid, p = vertex_at(center_vid, deg, d)
			if vid and (not test or test(p, vid)) then return vid, p, deg end
		end
	end
end

-- find_spot with a fallback: when no spot passes `test`, the spot passing `must` that scores highest (the number of
-- sight lines it hides from). The caller logs the relaxed placement; the parser reads the sight flips that follow.
local function find_spot_or_best(center_vid, radii, start_deg, test, must, score)
	local vid, p, deg = find_spot(center_vid, radii, start_deg, test)
	if vid then return vid, p, deg, false end
	local best, best_score
	for _, d in ipairs(radii) do
		for k = 0, 71 do
			local b = (start_deg + k * 5) % 360
			local v, q = vertex_at(center_vid, b, d)
			if v and (not must or must(q, v)) then
				local s = score(q, v)
				if not best_score or s > best_score then best, best_score = {v, q, b}, s end
			end
		end
	end
	if best then return best[1], best[2], best[3], true, best_score end
end

local function hidden_count(p, points)
	local n = 0
	for _, q in ipairs(points) do
		if not los(p, q) then n = n + 1 end
	end
	return n
end

local function far_from(p, points, d)
	for _, q in ipairs(points) do
		if flat_dist(p, q) < d then return false end
	end
	return true
end

local function hidden_from(p, points)
	for _, q in ipairs(points) do
		if los(p, q) then return false end
	end
	return true
end

-- level.high/low_cover_in_direction hand the direction's yaw to the level graph without the engine's angle_normalize,
-- and the graph extrapolates a yaw below zero (x > 0, z > 0: a quarter of all directions) past its quadrant, to values
-- far below 0 or above 1. The engine (tactics' cover_toward) normalizes it: that quarter lies between the values at
-- 3/2 pi and at 0, which are read from directions whose yaw needs no normalizing.
local function cover_in_direction(lookup, vid, dir)
	if dir.x > 0 and dir.z > -1e-6 then
		local share = 1 - math.atan2(dir.x, math.max(dir.z, 0)) / (math.pi / 2)
		local at_zero = lookup(vid, vector():set(0, 0, 1))
		local at_three_halves = lookup(vid, vector():set(1, 0, -1e-3))
		return at_three_halves + (at_zero - at_three_halves) * share
	end
	return lookup(vid, dir)
end

-- The level's own living stalkers and monsters (not the actor): a fight placed next to them becomes theirs (r2k: level
-- loners killed three mercenaries within three seconds), so the layout keeps its spots CROWD_CLEAR_M away from them.
local CROWD_CLEAR_M = 100
local function level_crowd()
	local crowd = {}
	level.iterate_online_objects(function(obj)
		if obj:id() ~= 0 and (IsStalker(obj) or IsMonster(obj)) and obj:alive() then
			crowd[#crowd + 1] = vector():set(obj:position())
		end
		return false
	end)
	return crowd
end

-- The level's own creatures near the fights are kept offline for the run (r2m: dogs and the checkpoint's soldiers fought
-- the bandits and the mercenaries while no layout was clear of the level's 75 NPCs). Every living stalker and monster
-- the probe did not spawn within FIGHT_SWEEP_M of a fight's spot gets the switch flags "may go offline, never online" on
-- its squad (or itself), as ai_fair.lua does for the whole ring: the ALife switch manager takes it offline, nothing is
-- released. Traders and the actor's companions are left alone (kept). The rest of the level stays online, so the think
-- time is still measured with the level's load. Lines "sweep scope=<all|all_ids|online|squads> new=<n> squads=<n>
-- singles=<n> kept=<n> online=<n> nearest=<m>": online counts foreign creatures still online near a fight.
local FIGHT_SWEEP_M, SWEEP_PERIOD_MS, SWEEP_SQUADS_EVERY, SWEEP_HEARTBEAT_MS = 90, 500, 4, 15000
local own_ids = {} -- the probe's squads and their members
local fight_spots = {} -- both spots of every fight
local swept, kept_ids = {}, {}
local sweep_count = {squads = 0, singles = 0, kept = 0}
local sweep_state = {next = 0, passes = 0, logged = 0}

local function foreign_creature(se)
	if not se or se.id == 0 or own_ids[se.id] then return false end
	local c = se:clsid()
	return (IsStalker(nil, c) or IsMonster(nil, c)) and se:alive() and not own_ids[se.group_id]
end

local sweep_level
local function on_this_level(se)
	sweep_level = sweep_level or alife():level_id()
	return game_graph():vertex(se.m_game_vertex_id):level_id() == sweep_level
end

-- The distance to the nearest fight spot.
local function fight_distance(pos)
	local best
	for _, spot in ipairs(fight_spots) do
		local d = pos:distance_to(spot)
		best = best and math.min(best, d) or d
	end
	return best or math.huge
end

-- Flags the creature's squad (or the creature) offline for good; 1 when it was not flagged before.
local function take_offline(se)
	local group = se.group_id
	local id = group and group ~= 65535 and group or se.id
	if swept[id] or kept_ids[id] or own_ids[id] then return 0 end
	local companion = false
	if id ~= se.id then
		local ok, found = pcall(function() return axr_companions.companion_squads[id] end)
		companion = ok and found and true or false
	end
	if companion or (IsStalker(nil, se:clsid()) and se:community() == "trader") then
		kept_ids[id] = true
		sweep_count.kept = sweep_count.kept + 1
		return 0
	end
	local sim = alife()
	sim:set_switch_offline(id, true)
	sim:set_switch_online(id, false)
	swept[id] = true
	if id == se.id then
		sweep_count.singles = sweep_count.singles + 1
	else
		sweep_count.squads = sweep_count.squads + 1
	end
	return 1
end

local function sweep_one(se)
	if foreign_creature(se) and on_this_level(se) and fight_distance(se.position) <= FIGHT_SWEEP_M then
		return take_offline(se)
	end
	return 0
end

-- An online creature of db.storage: new flags, 1 when it is a foreign one still online near a fight, its distance.
local function sweep_online_one(id, st)
	local obj = st.object
	if not obj or id == 0 or own_ids[id] or not (IsStalker(obj) or IsMonster(obj)) or not obj:alive() then return 0, 0 end
	local d = fight_distance(obj:position())
	local se = d <= FIGHT_SWEEP_M and alife():object(id)
	if not se or own_ids[se.group_id] then return 0, 0 end
	local new = take_offline(se)
	local group = se.group_id ~= 65535 and se.group_id or id
	return new, kept_ids[group] and 0 or 1, d
end

local function sweep(now)
	if now < sweep_state.next or #fight_spots == 0 then return end
	sweep_state.next = now + SWEEP_PERIOD_MS
	local new, scope = 0, "online"
	if sweep_state.passes == 0 then
		-- Every ALife object once; by id when the engine's iterate_objects is missing or fails.
		scope = "all"
		local walked = pcall(function()
			alife():iterate_objects(function(se)
				local ok, n = pcall(sweep_one, se)
				if ok and n then new = new + n end
				return false
			end)
		end)
		if not walked then
			scope = "all_ids"
			for id = 1, 65534 do
				local se = alife():object(id)
				if se then
					local ok, n = pcall(sweep_one, se)
					if ok and n then new = new + n end
				end
			end
		end
	elseif sweep_state.passes % SWEEP_SQUADS_EVERY == 0 and SIMBOARD and SIMBOARD.squads then
		scope = "squads"
		for id, sq in pairs(SIMBOARD.squads) do
			if not (own_ids[id] or swept[id] or kept_ids[id]) then
				local ok, n = pcall(function() return sweep_one(alife():object(sq:commander_id())) end)
				if ok and n then new = new + n end
			end
		end
	end
	local online_n, nearest = 0
	for id, st in pairs(db.storage) do
		local ok, n, counted, d = pcall(sweep_online_one, id, st)
		if ok and n then
			new = new + n
			if counted == 1 then
				online_n = online_n + 1
				nearest = nearest and math.min(nearest, d) or d
			end
		end
	end
	sweep_state.passes = sweep_state.passes + 1
	local periodic = scope == "online" or scope == "squads"
	if not periodic or new > 0 or online_n ~= sweep_state.online or now >= sweep_state.logged + SWEEP_HEARTBEAT_MS then
		sweep_state.online, sweep_state.logged = online_n, now
		say("sweep scope=%s new=%d squads=%d singles=%d kept=%d online=%d nearest=%.0f", scope, new,
			sweep_count.squads, sweep_count.singles, sweep_count.kept, online_n, nearest or -1)
	end
end

local function first_section(...)
	local ini = system_ini()
	for _, s in ipairs({...}) do
		if ini:section_exist(s) then return s end
	end
end

local function squad_npc_section(squad_section)
	local ini = system_ini()
	for _, key in ipairs({"npc", "npc_random"}) do
		local list = ini:line_exist(squad_section, key) and ini:r_string_ex(squad_section, key)
		local first = list and string.match(list, "[^,%s]+")
		if first then return first end
	end
end

local roles, role_ids = {}, {} -- id -> role, role -> id

local function tag(id, role)
	roles[id], role_ids[role] = role, id
	local se = alife():object(id)
	if se and se.set_character_name then se:set_character_name("qa_ai_" .. role) end
end

local function member_ids(sq)
	local ids = {}
	for k in sq:squad_members() do ids[#ids + 1] = k.id end
	table.sort(ids)
	return ids
end

-- A sim squad of count members at a vertex, held there: no simulation target (the squad's own __lock), so its members
-- idle in cover within ~15 m (xr_reach_task's fake target) instead of walking to a smart terrain, whose jobs would
-- teleport them to job spots on spawn.
local function spawn_squad(section, vid, count)
	local pos, gv = level.vertex_position(vid), db.actor:game_vertex_id()
	local sq = alife():create(section, pos, vid, gv)
	if not sq then
		say("error squad=%s not_created", section)
		return nil, {}
	end
	sq:create_npc(nil, pos, vid, gv)
	sq.__lock = true
	local ids = member_ids(sq)
	while #ids > count do
		sq:remove_npc(ids[#ids])
		table.remove(ids)
	end
	local npc_section = squad_npc_section(section)
	while #ids < count and npc_section do
		sq:add_squad_member(npc_section, pos, vid, gv)
		ids = member_ids(sq)
	end
	for _, id in ipairs(ids) do
		local se = alife():object(id)
		if se then SIMBOARD:setup_squad_and_group(se) end
	end
	return sq, ids
end

local function add_member(sq, section, vid)
	local pos, gv = level.vertex_position(vid), db.actor:game_vertex_id()
	local id = sq:add_squad_member(squad_npc_section(section), pos, vid, gv)
	local se = id and alife():object(id)
	if se then SIMBOARD:setup_squad_and_group(se) end
	return id
end

local function spawn_npc(section, vid)
	local se = alife():create(section, level.vertex_position(vid), vid, db.actor:game_vertex_id())
	return se and se.id
end

local function give(id, section, count)
	local se = alife():object(id)
	if not (se and section) then return end
	for _ = 1, count or 1 do
		alife():create(section, se.position, se.m_level_vertex_id, se.m_game_vertex_id, id)
	end
end

local function online(id) return id and level.object_by_id(id) end

-- Leaves the NPC only the given firearm (knives and grenades stay) and gives it that weapon with ammo.
local function arm_only(npc, weapon)
	local drop = {}
	npc:iterate_inventory(function(_, item)
		local class = npc:ai_weapon_class(item)
		if class ~= "" and class ~= "none" then drop[#drop + 1] = item:id() end
	end, npc)
	for _, id in ipairs(drop) do
		local se = alife():object(id)
		if se then alife():release(se, true) end
	end
	give(npc:id(), weapon, 1)
	local ammo = system_ini():r_string_ex(weapon, "ammo_class")
	give(npc:id(), ammo and string.match(ammo, "[^,%s]+"), 3)
end

local function hit_from_actor(npc, power)
	local h = hit()
	h.power = power
	h.impulse = 0
	h.draftsman = db.actor
	h.type = hit.fire_wound
	h.direction = vector():set(npc:position()):sub(db.actor:position()):normalize()
	h:bone("bip01_spine")
	npc:hit(h)
end

local SOURCES = {"seen", "heard_shot", "heard_step", "heard_reload", "heard_item", "heard_voice", "hit", "near_miss",
	"muzzle_flash", "flashlight", "report_voice", "report_hushed", "report_radio", "report_gesture", "report_pda",
	"report_hq", "witness", "script", "restored"}

local function source_name(s) return s == 0 and "none" or SOURCES[s] or tostring(s) end

-- Logs the NPC's belief on a target when its source or the NPC's own sight of it changes.
local beliefs = {}
local function poll_belief(npc, target, target_name)
	local key = npc:id() .. ":" .. target:id()
	local src, see = npc:ai_belief_source(target), npc:see(target)
	local last = beliefs[key]
	if last and last.src == src and last.see == see then return end
	beliefs[key] = {src = src, see = see}
	say("belief npc=%d role=%s comm=%s target=%s src=%s err=%.1f age=%d see=%d", npc:id(), roles[npc:id()] or "-",
		npc:character_community(), target_name, source_name(src), npc:ai_belief_error(target),
		math.floor(npc:ai_belief_age(target)), see and 1 or 0)
end

local function dump_belief(npc, target, target_name)
	say("final npc=%d role=%s comm=%s target=%s src=%s err=%.1f age=%d radio=%d", npc:id(), roles[npc:id()] or "-",
		npc:character_community(), target_name, source_name(npc:ai_belief_source(target)),
		npc:ai_belief_error(target), math.floor(npc:ai_belief_age(target)), npc:ai_has_radio() and 1 or 0)
end

local KINDS = {"contact", "update", "lost", "man_down", "leader_down", "taking_fire", "grenade", "reloading",
	"flanking", "cover_me", "fall_back", "attack", "help", "search_assign", "all_clear", "surrender"}
local CHANNELS = {"voice", "hushed", "radio", "gesture", "pda", "hq"}

local function name_of(obj)
	if not obj then return "-" end
	return obj:id() == 0 and "actor" or roles[obj:id()] or tostring(obj:id())
end

-- Every report the engine delivers (dead_air_x64_ai.subscribe_report): the receiver, kind, channel, whom it names,
-- the sender and how far apart the two stood. The line is formatted at delivery, stamped with that time, and printed
-- from the probe's own update: a log write (flushed per line with -force_flushlog) inside the delivery would be timed
-- as the AI's comms work (p_comms_us, P10).
local pending_reports = {}
local function on_report(receiver, target, kind, channel, sender)
	if not (t0 and receiver) or finished then return end
	pending_reports[#pending_reports + 1] = string.format("[qa-ai] t=%.2f ", t_now()) .. string.format(
		"report npc=%d role=%s comm=%s kind=%s channel=%s target=%s sender=%s d=%.0f", receiver:id(),
		roles[receiver:id()] or "-", receiver:character_community(), KINDS[(kind or -1) + 1] or tostring(kind),
		CHANNELS[(channel or -1) + 1] or tostring(channel), name_of(target), name_of(sender),
		sender and receiver:position():distance_to(sender:position()) or -1)
end

local function flush_reports()
	for i = 1, #pending_reports do printf("%s", pending_reports[i]) end
	pending_reports = {}
end

local reports_subscribed
local function subscribe_reports()
	local ok, ai = pcall(function() return dead_air_x64_ai end)
	if ok and ai and ai.subscribe_report then
		ai.subscribe_report(on_report)
		reports_subscribed = true
	end
end

-- Combat action ids by name, once, so the parser can read the action histograms.
local ACTIONS = {"action_get_item_to_kill", "action_find_item_to_kill", "action_make_item_killing", "action_find_ammo",
	"action_aim_enemy", "action_get_ready_to_kill", "action_kill_enemy", "action_retreat_from_enemy",
	"action_take_cover", "action_look_out", "action_hold_position", "action_get_distance", "action_detour_enemy",
	"action_search_enemy", "action_sudden_attack", "action_kill_enemy_if_not_visible", "action_reach_wounded_enemy",
	"action_prepare_wounded_enemy", "action_kill_wounded_enemy", "action_kill_if_player_on_the_path",
	"action_critically_wounded", "action_kill_if_enemy_critically_wounded", "action_post_combat_wait",
	"da_action_suppress_fire", "da_action_fall_back", "da_action_hold_ambush", "da_action_search_sweep",
	"da_action_search_overwatch", "da_action_return_fire", "da_action_flank"}

local function log_action_ids()
	for _, name in ipairs(ACTIONS) do
		local ok, id = pcall(function() return stalker_ids[name] end)
		if ok and type(id) == "number" then say("action_id name=%s id=%d", name, id) end
	end
end

-- The combat sub-planner's current action, nil outside combat.
local function combat_action(npc)
	local manager = npc:motivation_action_manager()
	if not (manager and manager:initialized()) or manager:current_action_id() ~= stalker_ids.action_combat_planner then
		return nil
	end
	local planner = cast_planner(manager:action(stalker_ids.action_combat_planner))
	return planner and planner:initialized() and planner:current_action_id() or nil
end

-- The counters of the running period to the log: ai_stat reads only that period, ai_log_period resets it.
local function flush_stats() cmd("ai_stats") end

-- Logs the NPC's morale level when it changes (0 panicked .. 4 heroic, 5 surrendered).
local morale_seen = {}
local function poll_morale(npc)
	local level = npc:ai_morale()
	if morale_seen[npc:id()] ~= level then
		morale_seen[npc:id()] = level
		say("morale npc=%d role=%s comm=%s level=%d fear=%.2f", npc:id(), roles[npc:id()] or "-",
			npc:character_community(), level, npc:ai_fear())
	end
end

-- The ammo section loaded in a weapon (its ammo_class entry by the loaded type), "-" when unknown: slugs widen a
-- shotgun's engage_max.
local function ammo_section(item)
	local ok, index = pcall(function() return item:get_ammo_type() end)
	local list = ok and index and system_ini():r_string_ex(item:section(), "ammo_class")
	if not list then return "-" end
	local i = 0
	for name in string.gmatch(list, "[^,%s]+") do
		if i == index then return name end
		i = i + 1
	end
	return "-"
end

-- Logs every round a watched NPC fires (its magazine count going down) with its sight of its best enemy. The poll
-- sees the enemy selected now, not at the round: switch=1 when the best enemy changed since the last poll, so the
-- distance may belong to another target than the round's (B3 judges the engine's t_shots_oob, this is a cross-check).
local ammo_seen = {}
local rounds_by = {} -- npc id -> rounds logged so far
local function poll_shots(npc, extra)
	local item = npc:active_item()
	local class = item and npc:ai_weapon_class(item) or ""
	if class == "" or class == "none" then return end
	local ammo = item:get_ammo_in_magazine()
	local last = ammo_seen[npc:id()]
	local enemy = npc:best_enemy()
	local target = enemy and enemy:id() or -1
	if last and last.id == item:id() and ammo < last.ammo then
		rounds_by[npc:id()] = (rounds_by[npc:id()] or 0) + last.ammo - ammo
		say("shot npc=%d role=%s comm=%s class=%s ammo=%s rounds=%d see=%d dist=%.0f target=%d switch=%d%s", npc:id(),
			roles[npc:id()] or "-", npc:character_community(), class, ammo_section(item), last.ammo - ammo,
			enemy and npc:see(enemy) and 1 or 0, enemy and npc:position():distance_to(enemy:position()) or -1,
			target, last.target ~= target and 1 or 0, extra or "")
	end
	ammo_seen[npc:id()] = {id = item:id(), ammo = ammo, target = target}
end

local function centre(ids)
	local x, y, z, n = 0, 0, 0, 0
	for _, id in ipairs(ids) do
		local npc = online(id)
		if npc and npc:alive() then
			local p = npc:position()
			x, y, z, n = x + p.x, y + p.y, z + p.z, n + 1
		end
	end
	return n > 0 and vector():set(x / n, y / n, z / n) or nil
end

-- One line per second: the worst of the required separations; a broken layout makes the run void.
local void_reason
local function check_layout(groups, pairs_needed, max_from_actor)
	local worst, worst_text = math.huge, "none"
	local where = {}
	for name, ids in pairs(groups) do where[name] = centre(ids) end
	local actor = db.actor:position()
	for name, p in pairs(where) do
		local d = flat_dist(p, actor)
		if d > max_from_actor and not void_reason then
			void_reason = string.format("%s_%.0fm_from_actor", name, d)
		end
	end
	for _, need in ipairs(pairs_needed) do
		local a, b = where[need[1]], where[need[2]]
		if a and b then
			local d = flat_dist(a, b)
			local margin = d - need[3] * 0.85
			if margin < worst then
				worst, worst_text = margin, string.format("%s-%s d=%.0f need=%d", need[1], need[2], d, need[3])
			end
			if margin < 0 and not void_reason then
				void_reason = string.format("%s-%s_%.0fm", need[1], need[2], d)
			end
		end
	end
	say("layout ok=%d worst=%s", void_reason and 0 or 1, worst_text)
end

local function unpause()
	device():pause(false)
end

------------------------------------------------------------------------------------------------------------------------
-- Scenario
------------------------------------------------------------------------------------------------------------------------
local FIGHTS = {
	{name = "F1", gap = 70, sides = {{"army", "army_sim_squad_advanced"}, {"bandit", "bandit_sim_squad_novice"}},
		shotguns = {"wpn_bm16", "wpn_toz34"}},
	{name = "F2", gap = 60,
		sides = {{"monolith", "monolith_sim_squad_advanced"}, {"stalker", "stalker_sim_squad_advanced"}}},
	-- 55 m, not the plan's 80: DA's combat_ignore caps (60-75 m between stalkers out of combat) kept the two squads
	-- from engaging at 80 m in the first K4 pass.
	{name = "F3", gap = 55, sides = {{"killer", "merc_sim_squad_advanced"}, {"dolg", "duty_sim_squad_advanced"}}},
}
local RADIUS, SQUAD_SIZE, DURATION = 130, 4, 85
-- A fight whose sides have not taken each other on by then gets one exchange of shots between its nearest pair.
local KICKOFF_S = 20

local stage = "wait"
local next_layout = 0
local sides = {} -- side name -> {ids = {...}, fight = name, enemy = other side name}
local side_of = {}
local next_sample, next_report, next_belief = 0, 0, 0
local histogram, samples, covered, in_combat_samples = {}, {}, {}, {}
-- B4's share: samples of a member fighting a living member of its paired side, neither broken nor given up
local fight_samples, fight_covered = {}, {}
-- ...and of those, the samples in contact: the member saw that enemy itself within CONTACT_MS (a firefight; searching
-- or withdrawing out of sight measures the search's and the rout's ground, not the doctrine's cover).
local CONTACT_MS = 3000
local contact_samples, contact_covered, last_contact = {}, {}, {}
local open_histogram = {} -- side -> combat action -> uncovered fight samples
local third_logged = {} -- "npc:enemy" -> true: a best enemy outside the probe's squads, logged once
local relations_forced = {}
local shotgun_given = {}
local engaged, kicked = {}, {} -- side name -> t of its first fight with its paired side; fight name -> true

local function side_ids(name) return sides[name] and sides[name].ids or {} end

-- The level vertex under a position, or nil when the graph has none within a cell of it.
local function vertex_at_pos(pos)
	local found = level.vertex_id(vector():set(pos.x, pos.y + 2, pos.z))
	if not (found and found < 4294967295 and level.valid_vertex_id(found)) then return nil end
	local p = level.vertex_position(found)
	if flat_dist(p, pos) > 3 then return nil end
	return found, p
end

-- One fight: a centre on the ring around the fight's sector bearing and the two sides `gap` metres apart across it,
-- in sight of each other, every spot inside the switch distance, 160 m from the other fights' spots and clear of the
-- level's own NPCs (crowd). Hills make one fixed ring bearing per fight far too rare, so the centre, its radius and
-- the axis of the pair are searched.
local function place_fight(fight, centre_deg, taken, need_los, crowd)
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local half = fight.gap / 2
	for _, r in ipairs({RADIUS, RADIUS - 10, RADIUS + 10, RADIUS - 20, RADIUS + 20, RADIUS - 30}) do
		for _, off in ipairs({0, 8, -8, 16, -16, 24, -24}) do
			local c_vid, c = vertex_at(actor_vid, centre_deg + off, r)
			for k = 0, (c_vid and 11 or -1) do
				local o = bearing(k * 15)
				local a_vid, a = vertex_at_pos(vector():set(c.x + o.x * half, c.y, c.z + o.z * half))
				local b_vid, b = vertex_at_pos(vector():set(c.x - o.x * half, c.y, c.z - o.z * half))
				if a_vid and b_vid and flat_dist(a, actor) <= 185 and flat_dist(b, actor) <= 185
					and far_from(a, taken, 160) and far_from(b, taken, 160)
					and far_from(a, crowd, CROWD_CLEAR_M) and far_from(b, crowd, CROWD_CLEAR_M)
					and (not need_los or los(a, b)) then
					return {{a_vid, a}, {b_vid, b}}
				end
			end
		end
	end
end

-- Every fight's spots, or nil: 36 ring bearings, the last one without the sight line between the sides.
local function place_fights(crowd)
	local start = math.random(0, 35) * 10
	for turn = 0, 35 do
		local base = start + turn * 10
		local placed, taken, ok = {}, {}, true
		for index, fight in ipairs(FIGHTS) do
			-- The two sides stand around the 130 m ring, `gap` metres apart, facing each other across open ground.
			local spot = place_fight(fight, base + (index - 1) * 120, taken, true, crowd)
			if not spot and turn == 35 then
				spot = place_fight(fight, base + (index - 1) * 120, taken, false, crowd)
				if spot then say("relaxed spot=%s los=0", fight.name) end
			end
			if not spot then
				ok = false
				break
			end
			placed[index] = spot
			taken[#taken + 1], taken[#taken + 2] = spot[1][2], spot[2][2]
		end
		if ok then return placed end
	end
end

local function layout()
	local actor = db.actor:position()
	local crowd = level_crowd()
	say("crowd npcs=%d clear=%d", #crowd, CROWD_CLEAR_M)
	local placed = place_fights(crowd)
	if not placed and #crowd > 0 then
		-- No layout keeps clear of the level's NPCs: fights that meet a third party are marked by the "third" lines.
		say("relaxed crowd=1")
		placed = place_fights({})
	end
	if not placed then return "no_open_ground" end
	for index, fight in ipairs(FIGHTS) do
		local spot = placed[index]
		say("fight name=%s gap=%.0f", fight.name, flat_dist(spot[1][2], spot[2][2]))
		for s = 1, 2 do
			local side, section = fight.sides[s][1], fight.sides[s][2]
			local vid, pos = spot[s][1], spot[s][2]
			local sq, ids = spawn_squad(section, vid, SQUAD_SIZE)
			if sq then own_ids[sq.id] = true end
			fight_spots[#fight_spots + 1] = vector():set(pos)
			sides[side] = {ids = ids, fight = fight.name, enemy = fight.sides[3 - s][1]}
			for i, id in ipairs(ids) do
				own_ids[id] = true
				side_of[id] = side
				tag(id, side .. i)
			end
			say("side name=%s fight=%s section=%s members=%d x=%.1f z=%.1f from_actor=%.0f", side, fight.name, section,
				#ids, pos.x, pos.z, flat_dist(pos, actor))
		end
	end
	return nil
end

-- The fights happen whatever the base relations say: each side is made the other's sworn enemy.
local function force_relations()
	for _, side in pairs(sides) do
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			if npc and not relations_forced[id] then
				local all = true
				for _, other_id in ipairs(side_ids(side.enemy)) do
					local other = online(other_id)
					if other then
						npc:force_set_goodwill(-1000, other)
					else
						all = false
					end
				end
				relations_forced[id] = all or nil
			end
		end
	end
end

local function give_shotguns()
	local bandits = side_ids("bandit")
	for i, weapon in ipairs(FIGHTS[1].shotguns) do
		local npc = online(bandits[i])
		weapon = first_section(weapon)
		if npc and weapon and not shotgun_given[i] then
			shotgun_given[i] = true
			arm_only(npc, weapon)
			say("armed role=%s weapon=%s", roles[npc:id()], weapon)
		end
	end
end

local open_value
local function cover_open_value()
	if not open_value then
		local ini = system_ini()
		open_value = ini:section_exist("ai_tactics") and ini:line_exist("ai_tactics", "cover_open_value")
			and ini:r_float_ex("ai_tactics", "cover_open_value") or 0.6
	end
	return open_value
end

local function sample()
	for name, side in pairs(sides) do
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			if npc and npc:alive() then
				local action = combat_action(npc)
				if action then
					histogram[name] = histogram[name] or {}
					histogram[name][action] = (histogram[name][action] or 0) + 1
					samples[name] = (samples[name] or 0) + 1
					local enemy = npc:best_enemy()
					if enemy then
						local dir = vector():set(enemy:position()):sub(npc:position())
						local vid = npc:level_vertex_id()
						in_combat_samples[name] = (in_combat_samples[name] or 0) + 1
						-- Level graph cover values run from 0 (covered) to 1 (open); a spot counts as cover when the
						-- lower of the two is at most the tactics' own threshold of an exposed spot.
						local is_covered = math.min(cover_in_direction(level.high_cover_in_direction, vid, dir),
							cover_in_direction(level.low_cover_in_direction, vid, dir)) <= cover_open_value()
						if is_covered then covered[name] = (covered[name] or 0) + 1 end
						-- B4 counts the fight itself: a living member of the paired side as the enemy, the member
						-- neither broken nor given up (a rout is B6's measure, a level NPC or a mutant another fight).
						local morale = npc:ai_morale()
						if side_of[enemy:id()] == side.enemy and enemy:alive() and morale ~= 0 and morale ~= 5 then
							fight_samples[name] = (fight_samples[name] or 0) + 1
							local now = time_global()
							if npc:see(enemy) then last_contact[id] = now end
							if last_contact[id] and now - last_contact[id] <= CONTACT_MS then
								contact_samples[name] = (contact_samples[name] or 0) + 1
								if is_covered then contact_covered[name] = (contact_covered[name] or 0) + 1 end
							end
							if is_covered then
								fight_covered[name] = (fight_covered[name] or 0) + 1
							else
								open_histogram[name] = open_histogram[name] or {}
								open_histogram[name][action] = (open_histogram[name][action] or 0) + 1
							end
						end
						local key = id .. ":" .. enemy:id()
						if not side_of[enemy:id()] and not third_logged[key] then
							third_logged[key] = true
							say("third npc=%d role=%s side=%s enemy=%d kind=%s d=%.0f", id, roles[id] or "-", name,
								enemy:id(), IsStalker(enemy) and enemy:character_community() or enemy:section(),
								npc:position():distance_to(enemy:position()))
						end
					end
				end
				-- Retreats are read from morale (0 broken, 5 surrendered) and the m_* counters, not from the planner
				-- action: a fanatic's sideways relocation runs through the same retreat action.
				poll_morale(npc)
				poll_shots(npc)
			end
		end
	end
end

local function report()
	for name, side in pairs(sides) do
		local alive, broken, surrendered = 0, 0, 0
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			if npc and npc:alive() then
				alive = alive + 1
				local morale = npc:ai_morale()
				broken = broken + (morale == 0 and 1 or 0)
				surrendered = surrendered + (morale == 5 and 1 or 0)
			end
		end
		local parts = {}
		for action, count in pairs(histogram[name] or {}) do parts[#parts + 1] = action .. ":" .. count end
		table.sort(parts)
		say("side_state side=%s alive=%d samples=%d combat=%d covered=%d fight=%d fight_covered=%d contact=%d "
			.. "contact_covered=%d broken=%d surrendered=%d actions=%s", name, alive, samples[name] or 0,
			in_combat_samples[name] or 0, covered[name] or 0, fight_samples[name] or 0, fight_covered[name] or 0,
			contact_samples[name] or 0, contact_covered[name] or 0, broken, surrendered,
			#parts > 0 and table.concat(parts, ",") or "-")
		-- The combat actions of the fight's uncovered samples: which ones stand in the open.
		local open = {}
		for action, count in pairs(open_histogram[name] or {}) do open[#open + 1] = action .. ":" .. count end
		table.sort(open)
		say("side_open side=%s actions=%s", name, #open > 0 and table.concat(open, ",") or "-")
	end
end

-- Every side against every side of another fight: the fights stay apart (150 m, less check_layout's margin); within
-- one fight the sides close in on each other. Broken or surrendered members are left out: their retreat is the
-- behaviour B6 measures, not a squad walking to its smart terrain.
local far_logged = {}
local function layout_groups()
	local groups, actor = {}, db.actor:position()
	for name, side in pairs(sides) do
		local ids = {}
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			local morale = npc and npc:alive() and npc:ai_morale()
			if morale == 0 or morale == 5 then
				local d = flat_dist(npc:position(), actor)
				if d > 200 and not far_logged[id] then
					far_logged[id] = true
					say("retreat_far npc=%d role=%s morale=%d d=%.0f", id, roles[id] or "-", morale, d)
				end
			else
				ids[#ids + 1] = id
			end
		end
		groups[name] = ids
	end
	return groups
end

local function layout_pairs()
	local list = {}
	for a, side_a in pairs(sides) do
		for b, side_b in pairs(sides) do
			if a < b and side_a.fight ~= side_b.fight then list[#list + 1] = {a, b, 150} end
		end
	end
	return list
end

local function beliefs_on_enemies()
	for _, side in pairs(sides) do
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			if npc and npc:alive() then
				for _, other_id in ipairs(side_ids(side.enemy)) do
					local other = online(other_id)
					if other then poll_belief(npc, other, roles[other_id] or tostring(other_id)) end
				end
			end
		end
	end
end

local function on_death(victim, who)
	local id = victim and victim:id()
	if id and side_of[id] then
		say("death npc=%d side=%s killer=%s", id, side_of[id], who and tostring(who:id()) or "-")
	end
end

-- A side fights its pair once one of its members has a member of the paired side as its enemy (not a level NPC).
local function track_engagement()
	for name, side in pairs(sides) do
		if not engaged[name] then
			local foes = {}
			for _, id in ipairs(side_ids(side.enemy)) do foes[id] = true end
			for _, id in ipairs(side.ids) do
				local npc = online(id)
				local enemy = npc and npc:alive() and npc:best_enemy()
				if enemy and foes[enemy:id()] then
					engaged[name] = t_now()
					say("engaged side=%s fight=%s enemy=%s npc=%s", name, side.fight, side.enemy, roles[id] or "-")
					break
				end
			end
		end
	end
end

local function nearest_pair(a_ids, b_ids)
	local best, a_best, b_best
	for _, a_id in ipairs(a_ids) do
		local a = online(a_id)
		if a and a:alive() then
			for _, b_id in ipairs(b_ids) do
				local b = online(b_id)
				if b and b:alive() then
					local d = a:position():distance_to(b:position())
					if not best or d < best then best, a_best, b_best = d, a, b end
				end
			end
		end
	end
	return a_best, b_best, best
end

-- One exchange of shots (weak scripted hits, each from the other's direction) opens a fight the layout left idle:
-- DA's combat_ignore distance caps (60 m under a surge, 75 m in rain or at night) can hold squads at the ring's gap.
local function kickoff()
	for _, fight in ipairs(FIGHTS) do
		local s1, s2 = fight.sides[1][1], fight.sides[2][1]
		if not kicked[fight.name] and sides[s1] and sides[s2] and not engaged[s1] and not engaged[s2] then
			local a, b, d = nearest_pair(side_ids(s1), side_ids(s2))
			if a and b then
				kicked[fight.name] = true
				for _, pair in ipairs({{a, b}, {b, a}}) do
					local h = hit()
					h.power = 0.05
					h.impulse = 0
					h.draftsman = pair[2]
					h.type = hit.fire_wound
					h.direction = vector():set(pair[1]:position()):sub(pair[2]:position()):normalize()
					h:bone("bip01_spine")
					pair[1]:hit(h)
				end
				say("kickoff fight=%s a=%s b=%s d=%.0f", fight.name, roles[a:id()] or "-", roles[b:id()] or "-", d)
			end
		end
	end
end

local radios_logged = {}
local function log_radios()
	for id, role in pairs(roles) do
		local npc = not radios_logged[id] and online(id)
		if npc then
			radios_logged[id] = true
			say("npc id=%d role=%s comm=%s rank=%d radio=%d leader=%d", id, role, npc:character_community(),
				npc:character_rank(), npc:ai_has_radio() and 1 or 0, npc:ai_is_leader() and 1 or 0)
		end
	end
end

-- Optional sampling profile of the fights (P10 attribution): -Globals 'qa_ai_profile = {start = 4, stop = 80,
-- hz = 1000, top = 300, callers = {"CEnemyManager::useful", ...}}' runs dar_profile_start/stop/callers in this run.
local profile_ok, profile_cfg = pcall(function() return qa_ai_profile end)
profile_cfg = profile_ok and type(profile_cfg) == "table" and profile_cfg or nil
local profile_stage = profile_cfg and 0 or 3
local function profile_step(t)
	if profile_stage == 0 and t >= (profile_cfg.start or 4) then
		cmd("dar_profile_start " .. tostring(profile_cfg.hz or 1000))
		say("profile start")
		profile_stage = 1
	elseif profile_stage == 1 and t >= (profile_cfg.stop or 80) then
		say("profile stop")
		cmd("dar_profile_stop " .. tostring(profile_cfg.top or 300))
		for _, needle in ipairs(profile_cfg.callers or {}) do cmd("dar_profile_callers " .. needle) end
		cmd("flush")
		profile_stage = 3
	end
end

-- The flash's line of sight as the engine traces it: from O's eye (FLASH_EYE_H, standing) to F's muzzle
-- (FLASH_MUZZLE_H) and back, and FLASH_LINE_DH above and below, every ray free of static geometry. level.ray_pick
-- culls back faces and was cast one way at 1.6 m; the engine's flash ray tests both faces (r4a9: that line passed,
-- O never got the flash, s_flash 0). A ray that hits anything, a bush included, rejects the pair: open ground.
local FLASH_EYE_H, FLASH_MUZZLE_H, FLASH_LINE_DH = 1.7, 1.4, 0.2
local function ray_free(from, to)
	local dir = vector():set(to):sub(from)
	local dist = dir:magnitude()
	if dist < 0.5 then return true end
	dir:normalize()
	return not level.ray_pick(from, dir, dist, rq_target.rqtStatic, rq_result(), nil)
end

local function flash_line(o, f)
	for _, dh in ipairs({0, FLASH_LINE_DH, -FLASH_LINE_DH}) do
		local eye = vector():set(o.x, o.y + FLASH_EYE_H + dh, o.z)
		local muzzle = vector():set(f.x, f.y + FLASH_MUZZLE_H + dh, f.z)
		if not (ray_free(eye, muzzle) and ray_free(muzzle, eye)) then return false end
	end
	return true
end

-- How squarely O's heading points at F (1 = straight; the flash counts only inside O's view cone) and how far apart
-- the two stand, logged when F's burst is ordered.
local function log_observer_view(o, f)
	if not (o and f and o:alive()) then return end
	local to = vector():set(f:position()):sub(o:position())
	local d = to:magnitude()
	to.y = 0
	local heading = vector():set(o:direction())
	heading.y = 0
	local dot = -2
	if to:magnitude() > 0.01 and heading:magnitude() > 0.01 then
		dot = to:normalize():dotproduct(heading:normalize())
	end
	say("observer_view dot=%.2f d=%.0f", dot, d)
end

------------------------------------------------------------------------------------------------------------------------
-- After the fights: B1's night muzzle flash (R1 placed no F in r1k; R4a9's F fired, O made no flash estimate)
------------------------------------------------------------------------------------------------------------------------
-- At DURATION, the fights' last report and counters printed, time jumps to 01:00 (as in ai_fair.lua) and an observer O
-- and a shooter F are spawned FLASH_D m apart: two level vertices with a flash line between them (both ways, three
-- heights), both inside 190 m of the actor, 40 m clear of him and FLASH_CLEAR m from every living member of the
-- probe's sides, and first also from the level's own online creatures; the distance tolerance widens over FLASH_TOLS
-- before the trial gives up ("error no_flash_pair", then the run ends). O stands under script control looking at F's
-- muzzle; F is held, armed, stripped of its torch and neutral to every probe stalker both ways (not a friend of O: a
-- friend's flash makes no estimate; nobody fights it), and at FLASH_FIRE_AT fires a scripted burst at a ground point
-- away from O and the actor. Logged as in ai_save.lua: "night", "spot name=O/F ... from_O=", "observer_view", F's
-- "shot" and "fire*" lines, O's "belief ... target=F src=..." lines. Parse-AiLog.py judges the fights only before the
-- "night" line and B1's flash from the lines after it.
local FLASH_READY_AT, FLASH_FIRE_AT, FLASH_DONE_AT = DURATION + 0.8, DURATION + 1.3, DURATION + 4.5
local FLASH_D, FLASH_TOLS, FLASH_CLEAR = 150, {8, 15, 25}, 80
local BURST, FIRE_WINDOW, FIRE_TRIES = 3, 2.5, 2
local flash = {} -- o_id, f_id, o_pos, f_pos, aim, weapon, state, tries, rounds0, rounds_first, stop_at, o_ready
local next_night_poll = 0

local function jump_to_night()
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local minutes = ((25 * 60) - (h * 60 + m)) % (24 * 60)
	level.change_game_time(0, math.floor(minutes / 60), minutes % 60)
	level.set_weather(level.get_weather(), true)
	say("night from=%02d:%02d added_min=%d", h, m, minutes)
end

-- One NPC of the given character section in its own held squad (a lone spawn walks off to a smart terrain); the
-- squad and its members are the probe's own (the sweep leaves them alone).
local function spawn_held(squad_section, npc_section, vid, role)
	local sq, list = spawn_squad(squad_section, vid, 1)
	if not sq then return nil end
	own_ids[sq.id] = true
	for _, old in ipairs(list) do own_ids[old] = true end
	local pos, gv = level.vertex_position(vid), db.actor:game_vertex_id()
	local id = npc_section and sq:add_squad_member(npc_section, pos, vid, gv)
	if id then
		own_ids[id] = true
		for _, old in ipairs(list) do sq:remove_npc(old) end
		local se = alife():object(id)
		if se then SIMBOARD:setup_squad_and_group(se) end
	else
		id = list[1]
	end
	if id then tag(id, role) end
	return id
end

-- The first pair of spots FLASH_D m (+-tolerance) apart with a flash line, O around the actor, F around O; clear of
-- the level's own online creatures first, of the probe's sides always.
local function flash_pair()
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local members = {}
	for _, side in pairs(sides) do
		for _, id in ipairs(side.ids) do
			local npc = online(id)
			if npc and npc:alive() then members[#members + 1] = npc:position() end
		end
	end
	local crowd = level_crowd()
	for _, clear_crowd in ipairs({true, false}) do
		local fine = function(q)
			local a = flat_dist(q, actor)
			return a <= 190 and a >= 40 and far_from(q, members, FLASH_CLEAR) and
				(not clear_crowd or far_from(q, crowd, FLASH_CLEAR))
		end
		local start = math.random(0, 17) * 20
		for _, tol in ipairs(FLASH_TOLS) do
			for _, r in ipairs({150, 120, 180, 90, 60}) do
				for k = 0, 17 do
					local o_vid, o = vertex_at(actor_vid, start + k * 20, r)
					if o_vid and fine(o) then
						for m = 0, 35 do
							for _, d in ipairs({FLASH_D, FLASH_D - tol * 0.6, FLASH_D + tol * 0.6}) do
								local f_vid, f = vertex_at(o_vid, m * 10, d)
								if f_vid and math.abs(flat_dist(f, o) - FLASH_D) <= tol and fine(f) and flash_line(o, f) then
									return o_vid, o, f_vid, f, tol, clear_crowd
								end
							end
						end
					end
				end
			end
		end
	end
end

-- A point d metres from `from` on the ground, pointing away from a and b (their bisector; sideways when the two lie on
-- opposite sides): where F aims, so no round flies toward O or the actor.
local function aim_away(from, a, b, d)
	local ux, uz = from.x - a.x, from.z - a.z
	local ul = math.max(math.sqrt(ux * ux + uz * uz), 0.01)
	local vx, vz = from.x - b.x, from.z - b.z
	local vl = math.max(math.sqrt(vx * vx + vz * vz), 0.01)
	local x, z = ux / ul + vx / vl, uz / ul + vz / vl
	local l = math.sqrt(x * x + z * z)
	if l < 0.2 then x, z, l = -uz / ul, ux / ul, 1 end
	x, z = x / l, z / l
	local p = vector():set(from.x + x * d, from.y + 0.3, from.z + z * d)
	local vid = level.vertex_id(vector():set(p.x, p.y + 3, p.z))
	if vid and vid < 4294967295 and level.valid_vertex_id(vid) then
		local ground = level.vertex_position(vid)
		if flat_dist(ground, p) <= 2 then p = vector():set(ground.x, ground.y + 0.3, ground.z) end
	end
	return p
end

-- Neutral to every probe stalker both ways (see ai_fair.lua's gunners); a remembered enemy forgotten.
local function neutralize(npc)
	for id in pairs(roles) do
		local other = id ~= npc:id() and online(id)
		if other and other:alive() and IsStalker(other) then
			npc:force_set_goodwill(0, other)
			other:force_set_goodwill(0, npc)
		end
	end
	local enemy = npc:best_enemy()
	if enemy then npc:enable_memory_object(enemy, false) end
end

local function strip_torches(npc)
	local torches = {}
	npc:iterate_inventory(function(_, item)
		if item:section() == "device_torch" then torches[#torches + 1] = item:id() end
	end, npc)
	for _, id in ipairs(torches) do
		local se = alife():object(id)
		if se then alife():release(se, true) end
	end
	return #torches
end

-- The weapon arm_only gave, else the NPC's best firearm; never a knife.
local function flash_weapon(npc)
	local weapon = flash.weapon and npc:object(flash.weapon)
	if weapon then return weapon end
	local best = npc:best_weapon()
	local class = best and npc:ai_weapon_class(best) or ""
	if class ~= "" and class ~= "none" then return best end
end

-- A fresh script command (see ai_fair.lua's command_gunner): hold (a walk to its own spot, weapon away), ready (the
-- weapon out), fire, idle; false when a weapon command finds no weapon.
local function command_flash(npc, action, look_at)
	local weapon = action ~= "hold" and flash_weapon(npc)
	if action ~= "hold" and not weapon then return false end
	neutralize(npc)
	if npc:get_script() then npc:script(false, "qa_ai") end
	npc:script(true, "qa_ai")
	local act = entity_action()
	if action == "hold" then
		act:set_action(anim(anim.danger))
		act:set_action(move(move.standing, move.walk, move.line, npc:position()))
		act:set_action(look(look.point, look_at))
	else
		act:set_action(anim(anim.danger))
		act:set_action(look(look.fire_point, vector():set(flash.aim.x, flash.aim.y, flash.aim.z)))
		local verb = action == "fire" and object.fire1 or action == "ready" and object.activate or object.idle
		act:set_action(object(weapon, verb, BURST))
	end
	act:set_action(cond(cond.time_end, 600000))
	npc:command(act, false)
	return true
end

-- O stands (body state stand) at its spot looking at the point.
local function command_observer(o, point)
	neutralize(o)
	if o:get_script() then o:script(false, "qa_ai") end
	o:script(true, "qa_ai")
	local act = entity_action()
	act:set_action(anim(anim.free))
	act:set_action(move(move.standing, move.walk, move.line, o:position()))
	act:set_action(look(look.point, point))
	act:set_action(cond(cond.time_end, 600000))
	o:command(act, false)
end

local function flash_setup()
	jump_to_night()
	local o_vid, o, f_vid, f, tol, clear_crowd = flash_pair()
	if not o_vid then
		say("error no_flash_pair tols=%d clear=%d", #FLASH_TOLS, FLASH_CLEAR)
		return false
	end
	local actor = db.actor:position()
	flash.o_pos, flash.f_pos, flash.state = o, f, "new"
	flash.aim = aim_away(f, o, actor, 15)
	flash.weapon = first_section("wpn_ak74")
	if not clear_crowd then say("relaxed spot=O,F crowd_near=1") end
	say("spot name=O x=%.1f y=%.1f z=%.1f from_actor=%.0f", o.x, o.y, o.z, flat_dist(o, actor))
	say("spot name=F x=%.1f y=%.1f z=%.1f from_O=%.0f tol=%d from_actor=%.0f", f.x, f.y, f.z, flat_dist(f, o), tol,
		flat_dist(f, actor))
	flash.o_id = spawn_held("stalker_sim_squad_veteran", first_section("sim_default_stalker_3", "sim_default_stalker_2"),
		o_vid, "O")
	flash.f_id = spawn_held("stalker_sim_squad_advanced", first_section("sim_default_stalker_2", "sim_default_stalker_1"),
		f_vid, "F")
	return true
end

local function flash_fire(npc, t)
	if not command_flash(npc, "fire") then
		say("fire_missed role=F why=no_weapon")
		flash.state = "done"
		return
	end
	flash.tries = (flash.tries or 0) + 1
	flash.rounds0 = rounds_by[npc:id()] or 0
	flash.rounds_first = flash.rounds_first or flash.rounds0
	flash.stop_at = t + FIRE_WINDOW
	flash.state = "fire"
	local weapon = flash_weapon(npc)
	say("fire role=F try=%d weapon=%s ammo=%d aim_d=%.0f", flash.tries, weapon:section(), weapon:get_ammo_in_magazine(),
		flat_dist(npc:position(), flash.aim))
end

-- O: held looking at F's muzzle once online. F: hold, ready, fire, idle once BURST rounds are logged (one retry).
local function update_flash(t, now)
	local o, f = online(flash.o_id), online(flash.f_id)
	if o and o:alive() and not flash.o_ready then
		flash.o_ready = true
		command_observer(o, vector():set(flash.f_pos.x, flash.f_pos.y + FLASH_MUZZLE_H, flash.f_pos.z))
		say("observer_facing target=F id=%d", o:id())
	end
	if f and f:alive() then
		local released = strip_torches(f)
		if released > 0 then say("torch_stripped id=%d role=F n=%d", f:id(), released) end
	end
	if f and f:alive() and flash.state ~= "done" then
		if flash.state == "new" then
			if flash.weapon then
				arm_only(f, flash.weapon)
				say("armed role=F weapon=%s", flash.weapon)
			end
			command_flash(f, "hold", vector():set(flash.aim.x, flash.aim.y + 1.3, flash.aim.z))
			flash.state = "hold"
		elseif flash.state == "hold" and t >= FLASH_READY_AT then
			if command_flash(f, "ready") then
				flash.state = "ready"
			elseif t >= FLASH_FIRE_AT + FIRE_WINDOW then
				say("fire_missed role=F why=no_weapon")
				flash.state = "done"
			end
		elseif flash.state == "ready" and t >= FLASH_FIRE_AT then
			log_observer_view(o, f)
			flash_fire(f, t)
		elseif flash.state == "fire" then
			local rounds = (rounds_by[f:id()] or 0) - flash.rounds0
			if rounds == 0 and t >= flash.stop_at and flash.tries < FIRE_TRIES then
				flash_fire(f, t)
			elseif rounds >= BURST or t >= flash.stop_at then
				command_flash(f, "idle")
				flash.state = "done"
				say("fire_end role=F rounds=%d tries=%d", (rounds_by[f:id()] or 0) - flash.rounds_first, flash.tries)
			end
		end
	elseif flash.state ~= "done" and t >= FLASH_FIRE_AT then
		say("fire_missed role=F why=%s", f and "dead" or "offline")
		flash.state = "done"
	end
	if now >= next_night_poll then
		next_night_poll = now + 100
		if f and f:alive() then poll_shots(f) end
		if o and f and o:alive() then poll_belief(o, f, "F") end
	end
end

local function step()
	local t, now = t_now(), time_global()
	if stage == "fight" then profile_step(t) end
	if stage == "wait" and t >= 1 then
		local failed = layout()
		if failed then
			finish("void " .. failed)
			return
		end
		log_action_ids()
		say("weather rain=%.2f hour=%d", level.rain_factor(), level.get_time_hours())
		sweep(now)
		stage = "fight"
	elseif stage == "fight" then
		sweep(now)
		force_relations()
		give_shotguns()
		log_radios()
		if now >= next_sample then
			next_sample = now + 250
			sample()
		end
		if now >= next_belief then
			next_belief = now + 500
			beliefs_on_enemies()
		end
		if now >= next_report then
			next_report = now + 5000
			report()
		end
		if now >= next_layout then
			next_layout = now + 1000
			check_layout(layout_groups(), layout_pairs(), 200)
			track_engagement()
			if t >= KICKOFF_S then kickoff() end
		end
		if t >= DURATION then
			report()
			flush_stats()
			stage = flash_setup() and "night" or "night_done"
		end
	elseif stage == "night" or stage == "night_done" then
		if stage == "night" then update_flash(t, now) end
		if t >= FLASH_DONE_AT or stage == "night_done" then
			finish(void_reason and ("void " .. void_reason) or "complete")
		end
	end
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	unpause()
	cmd("g_god 1")
	cmd("ai_ignore_actor 1")
	cmd("ai_log_period 5000")
	cmd("r__fps_log 5")
	t0 = time_global()
	say("start scenario=ai_factions level=%s save_time=%02d:%02d weather=%s", level.name(), level.get_time_hours(),
		level.get_time_minutes(), tostring(level.get_weather()))
	if not reports_subscribed then say("error no_report_subscription") end
end

local function on_update()
	unpause()
	if not t0 then return end
	if #pending_reports > 0 then flush_reports() end
	if quit_at then
		if time_global() >= quit_at then
			quit_at = nil
			cmd("quit")
		end
		return
	end
	if finished then return end
	local ok, err = pcall(step)
	if not ok then
		say("error lua=%s", tostring(err))
		finish("error")
	end
end

function on_game_start()
	subscribe_reports()
	RegisterScriptCallback("actor_on_first_update", on_first_update)
	RegisterScriptCallback("actor_on_update", on_update)
	RegisterScriptCallback("npc_on_death_callback", on_death)
end
